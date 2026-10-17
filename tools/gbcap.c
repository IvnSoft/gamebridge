// SPDX-License-Identifier: GPL-2.0
/*
 * gbcap - isochronous capture probe for the Adaptec GameBridge AVC-1410
 *         in capture state (03f3:009c).
 *
 * Step 0 of the streaming work: select a high-bandwidth alternate setting and
 * submit async isochronous transfers on EP 0x82, then report what comes back.
 * Deliberately sends NO initialisation commands -- the question this answers is
 * whether the FX2 firmware streams autonomously once it is running, or whether a
 * start command is required first. Every outcome is informative:
 *
 *   data flowing        -> firmware self-starts; go straight to frame parsing
 *   zero-filled packets -> USB path fine, decoder not producing pixels
 *   nothing at all      -> a start command is mandatory
 *
 * Build:  make
 * Usage:  sudo ./gbcap probe [seconds] [alt]
 *         sudo ./gbcap dump <file> [seconds] [alt]
 */

#define _GNU_SOURCE                 /* F_SETPIPE_SZ */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime under -std=c11 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <libusb-1.0/libusb.h>

#define VID_ADAPTEC 0x03f3
#define PID_CAPTURE 0x009c

#define EP_VIDEO    0x82   /* IN, isochronous on alts 1..5 */
#define EP_CMD      0x01   /* OUT, command channel         */
#define EP_STATUS   0x81   /* IN,  replies / status        */
#define IFACE       0

#define CMD_START   0x36
#define CMD_STOP    0x37

/* Tunables from the environment, CLAMPED: NUM_TRANSFERS indexes fixed arrays of
 * GB_MAX_XFERS, and this tool runs as root, so an unchecked GB_XFERS was a stack
 * overflow (2026-10-16 security review). */
#define GB_MAX_XFERS   64
#define GB_MAX_PKTS    128
static int env_count(const char *name, int def, int lo, int hi)
{
	const char *s = getenv(name);
	int v = s ? atoi(s) : def;

	return v < lo ? lo : v > hi ? hi : v;
}
#define NUM_TRANSFERS  env_count("GB_XFERS", 16, 1, GB_MAX_XFERS)  /* in flight at once                    */
#define PKTS_PER_XFER  env_count("GB_PKTS", 32, 1, GB_MAX_PKTS)    /* microframes per transfer (4 ms each) */
#define LEN_BUCKETS    16

struct stats {
	unsigned long packets, empty, zero_payload, data_packets;
	unsigned long long bytes;
	unsigned long errors, partial;
	/* actual_length histogram: sparse list of (len,count) */
	struct { int len; unsigned long count; } bucket[LEN_BUCKETS];
	int nbuckets;
	int shown;          /* how many sample packets already printed */
	int errstat, errshown;
};

static volatile sig_atomic_t stop_now;
static struct stats st;
static FILE *outfp;
static int pkt_size;
static unsigned char g_cmd[16];
static int g_cmdlen;          /* 0 = send nothing, probe only */
static const char *g_init_path;   /* captured bring-up to replay, or NULL */
static const char *g_session_path; /* faithful ordered session script, or NULL */
static const char *g_frames_path;  /* write decoded UYVY frames here, "-" = stdout */
static int g_pal;                  /* 576-line geometry and PAL standard */
static int g_progressive;          /* source repeats one field (240p) */
static int g_no_audio;             /* leave AFE_CTRL alone (audio path off)  */

/* Per-packet log, to tell a device-side gap from a lost URB. The raw dump is a
 * concatenation of packet payloads, so a discontinuity in it is invisible
 * afterwards: this records the byte offset each packet lands at, its status and
 * its length, which makes every gap in the dump attributable. */
static const char *g_pktlog;
static FILE *plog;
static unsigned long long stream_pos;   /* bytes written to the raw dump so far */

static void on_sigint(int sig) { (void)sig; stop_now = 1; }

static void note_len(int len)
{
	for (int i = 0; i < st.nbuckets; i++)
		if (st.bucket[i].len == len) { st.bucket[i].count++; return; }
	if (st.nbuckets < LEN_BUCKETS) {
		st.bucket[st.nbuckets].len = len;
		st.bucket[st.nbuckets].count = 1;
		st.nbuckets++;
	}
}

/* NOTE: every diagnostic in the streaming path writes to stderr, never stdout.
 * When --frames - is used, stdout carries raw frame data and a stray printf
 * lands in the middle of a frame. That has happened twice: once from the status
 * lines in run(), and once from this sampler, which put 763 bytes of packet
 * hexdump into the stream and made players reject the tail.
 *
 * Print the head of the first few non-empty packets. If there is a per-packet
 * header (usbtv and stk1160 both use a small field/scanline header) it should be
 * visible as a repeating pattern in the first handful of bytes. */
static void sample(const unsigned char *b, int len)
{
	if (st.shown >= 8)
		return;
	fprintf(stderr, "  pkt[%lu] len=%4d :", st.packets, len);
	for (int i = 0; i < 24 && i < len; i++)
		fprintf(stderr, " %02x", b[i]);
	fprintf(stderr, "\n");
	st.shown++;
}

/* libusb_get_max_iso_packet_size() searches the config descriptor for the first
 * endpoint with a given address and returns that one's size. EP 0x82 exists in
 * every altsetting of this device -- as BULK 512 in alt 0 -- so it returns 512
 * regardless of which alt is actually selected. Look the endpoint up in the
 * altsetting we chose, and apply the high-bandwidth multiplier ourselves. */
static int iso_packet_size(libusb_device *dev, int iface, int alt, int ep_addr)
{
	struct libusb_config_descriptor *cfg;
	int size = -1;

	if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
		return -1;

	for (int i = 0; i < cfg->bNumInterfaces && size < 0; i++)
		for (int a = 0; a < cfg->interface[i].num_altsetting; a++) {
			const struct libusb_interface_descriptor *id =
				&cfg->interface[i].altsetting[a];
			if (id->bInterfaceNumber != iface || id->bAlternateSetting != alt)
				continue;
			for (int e = 0; e < id->bNumEndpoints; e++) {
				const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
				if (ep->bEndpointAddress != ep_addr)
					continue;
				if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK)
				    != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
					break;   /* not isoc in this alt */
				size = (ep->wMaxPacketSize & 0x7ff) *
				       (((ep->wMaxPacketSize >> 11) & 3) + 1);
				break;
			}
			break;
		}

	libusb_free_config_descriptor(cfg);
	return size;
}

static const char *pkt_status(int s)
{
	switch (s) {
	case LIBUSB_TRANSFER_COMPLETED: return "completed";
	case LIBUSB_TRANSFER_ERROR:     return "error";
	case LIBUSB_TRANSFER_TIMED_OUT: return "timeout";
	case LIBUSB_TRANSFER_CANCELLED: return "cancelled";
	case LIBUSB_TRANSFER_STALL:     return "stall";
	case LIBUSB_TRANSFER_NO_DEVICE: return "no-device";
	case LIBUSB_TRANSFER_OVERFLOW:  return "overflow";
	default:                        return "?";
	}
}

/* ---------------------------------------------------------------- BT.656 ---
 * The device sends raw ITU-R BT.656: each line is preceded by a 4-byte timing
 * reference code FF 00 00 XY, with XY = 1 F V H PPPP (protection bits zeroed by
 * this device). SAV (H=0) with V=0 is followed by 1440 bytes of 4:2:2 -- 720
 * pixels. Field 1 supplies even output rows, field 2 odd.
 *
 * The scanner has to be incremental because a line straddles isochronous packet
 * boundaries: 1440 bytes of payload against 3030-byte packets never aligns.
 *
 * MOST EAVs on this device are followed by an ancillary-data packet occupying
 * the horizontal blanking interval:
 *
 *     00 FF FF  DID SDID DC  <4 * (DC & 0x0f) payload bytes>  <checksum>
 *
 * DID is always 0xbf; SDID is 0x41, 0x42 or 0x83; DC is 0x42 (15-byte block) or
 * 0x44 (23-byte block). Where a block is present it lands exactly on the next
 * SAV, so the length rule is exact -- but a line may carry NO block at all
 * (24% of them in one capture), so the header must be checked, not assumed.
 *
 * That payload is arbitrary bytes, and it contains runs of 0x00 and 0xff. It
 * therefore SPELLS FF 00 00 xy -- 177 times in that same capture, 6% of all
 * lines. Scanning it as if it were video invents timing codes with whatever F
 * and V bits the data happens to carry, which is the entire reason the F and V
 * bits looked unreliable: with the ancillary block skipped instead of scanned,
 * every vertical-blanking run in the capture is exactly 19 lines and the F bit
 * flips within one line of each true field boundary.
 */
/* The assembler itself now lives in driver/gb-bt656.c, and is compiled from that
 * one source into both this tool and the kernel module. The wire-format notes
 * above are restated there, next to the code they govern. What remains here is
 * the userspace policy around it: where frames go, how a slow consumer is
 * handled, and the diagnostics. */
#include "../driver/gb-bt656.h"

#define VID_W        GB_WIDTH
#define VID_LINE     GB_LINE_BYTES

/* Active line count differs by standard: NTSC has 240 lines per field (480
 * total), PAL 288 (576). Runtime-selectable because the CX25840 will do either
 * and the Windows driver only ever offered NTSC. */
static int vid_field = 240;
static int vid_height_fields = 2;   /* 2 = interlaced, 1 = progressive/240p */
#define VID_FIELD    vid_field
#define VID_H        (vid_field * vid_height_fields)
#define VID_FRAME    (VID_H * VID_LINE)

static struct gb_bt656 bt;      /* the shared assembler's state */
static unsigned char *framebuf; /* the frame it writes into, ours to allocate */
static unsigned frames_out;     /* frames actually delivered to the consumer */
static FILE *framefp;

/* Audio, lifted out of the ancillary blocks as the stream is scanned.
 *
 * The payload is 16-bit big-endian PCM at 48 kHz stereo, so it needs no
 * conversion -- `aplay -f S16_BE -r 48000 -c 2` plays it as it stands.
 *
 * It is written from inside the URB callback, so the same rule applies as to
 * frames: never block. A stalled audio consumer costs samples, not the capture.
 * The buffer batches the writes, because a block arrives on three lines in four
 * and one write per block would be twelve thousand syscalls a second. */
static const char *g_audio_path;
static int audio_fd = -1;
static unsigned char audio_buf[4096];
static size_t audio_len;
static unsigned long audio_bytes, audio_dropped;

static void audio_flush(void)
{
	if (audio_fd < 0 || !audio_len)
		return;
	ssize_t w = write(audio_fd, audio_buf, audio_len);
	if (w < 0)
		audio_dropped += audio_len;      /* EAGAIN, or consumer gone */
	else {
		audio_bytes += (unsigned long)w;
		if ((size_t)w < audio_len)
			audio_dropped += audio_len - (size_t)w;
	}
	audio_len = 0;
}

static void audio_put(unsigned char c)
{
	if (audio_fd < 0)
		return;
	audio_buf[audio_len++] = c;
	if (audio_len >= sizeof audio_buf)
		audio_flush();
}

/* The assembler hands us one block at a time, already de-interleaved into
 * ordinary L,R,L,R stereo -- that unpacking is format knowledge and lives with
 * the format, in driver/gb-bt656.c. All that is left here is buffering. */
static void gb_on_audio(void *ctx, const unsigned char *pcm, int bytes)
{
	(void)ctx;
	for (int i = 0; i < bytes; i++)
		audio_put(pcm[i]);
}

/* Frames are written from inside the isochronous completion callback. A blocking
 * write there is fatal: if the consumer (a player on the other end of a pipe)
 * cannot keep up, the write stalls, URBs stop being resubmitted, and the whole
 * stream dies -- which presents as the tool "running for a while and then
 * hanging". Drop frames instead. Video capture wants the newest frame, not every
 * frame, so a dropped frame is the right failure. */
static const char *g_framelog;    /* per-frame diagnostics, or NULL */
static FILE *flog;
static struct timespec t_start;
static unsigned dropped_frames;   /* skipped cleanly, stream stays aligned */
static unsigned torn_frames;      /* consumer stalled mid-frame; rare */

static void bt656_log(int accepted)
{
	/* Local copy so the compiler can see it is non-null past the guard; with the
	 * global it warns, since a call in between could in principle change it. */
	FILE *f = flog;
	if (!f) return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long us = (now.tv_sec - t_start.tv_sec) * 1000000L
	        + (now.tv_nsec - t_start.tv_nsec) / 1000;
	int rows = 0;
	for (int r = 0; r < VID_H; r++)
		if (bt.rowmap[r >> 3] & (1u << (r & 7))) rows++;
	fprintf(f, "%u %ld %d %d %d %d %d %d ",
	        bt.attempts, us, accepted, rows,
	        bt.runs[0], bt.runs[1], bt.runs[2], bt.runs[3]);
	for (int i = 0; i < (VID_H + 7) / 8; i++)
		fprintf(f, "%02x", bt.rowmap[i]);
	fputc('\n', f);
}

/* Called by the assembler at every frame boundary. The accept/reject decision
 * is made in driver/gb-bt656.c, because it is a property of the stream rather
 * than of what we happen to be doing with it; everything here is policy.
 *
 * Note that a rejected frame is still reported to the log -- the rejection is
 * one of the more useful diagnostics there is, and hiding it was how a bimodal
 * distribution stayed invisible for three rounds. */
static void gb_on_frame(void *ctx, struct gb_bt656 *s, int accepted)
{
	(void)ctx;
	bt656_log(accepted);
	if (!accepted) {
		if (getenv("GB_DEBUG") && s->attempts < 40)
			fprintf(stderr, "  boundary %u: %d lines, runs %d/%d\n",
			        s->attempts, s->lines, s->runs[3], s->cur_run);
		return;
	}
	/* Having nowhere to send it is not a defect in the frame. This used to be
	 * folded into the reject test, so `gbcap decode <raw>` with no --frames
	 * reported every frame as too short and read as a broken decoder. */
	if (!framefp)
		return;

	int fd = fileno(framefp);
	const unsigned char *p = s->frame;
	size_t left = (size_t)s->height * GB_LINE_BYTES;

	/* Drop at WHOLE-FRAME granularity or not at all. An earlier version bailed
	 * out on EAGAIN part-way through a frame, which left the consumer holding a
	 * partial frame and shifted every subsequent frame boundary permanently --
	 * the picture shears into diagonal noise and never recovers. So: if the
	 * very first write would block, discard this frame having written nothing.
	 * Once any byte is out, we are committed and must finish. */
	ssize_t first = write(fd, p, left);
	if (first < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			dropped_frames++;
			return;                       /* nothing written -- still aligned */
		}
		return;                           /* consumer went away */
	}
	p += first; left -= (size_t)first;

	while (left) {
		struct pollfd pfd = { .fd = fd, .events = POLLOUT };
		ssize_t w = write(fd, p, left);
		if (w > 0) { p += w; left -= (size_t)w; continue; }
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			/* Committed: wait for room rather than corrupt the stream. The
			 * timeout bounds the stall so a dead consumer cannot wedge the
			 * capture the way a plain blocking write did. */
			if (poll(&pfd, 1, 200) <= 0) {
				torn_frames++;
				return;
			}
			continue;
		}
		return;
	}
	frames_out++;
}

static const struct gb_bt656_ops gb_ops = {
	.frame_done = gb_on_frame,
	.audio      = gb_on_audio,
};


static void LIBUSB_CALL on_done(struct libusb_transfer *xfr)
{
	if (xfr->status != LIBUSB_TRANSFER_COMPLETED &&
	    xfr->status != LIBUSB_TRANSFER_TIMED_OUT) {
		if (xfr->status == LIBUSB_TRANSFER_CANCELLED ||
		    xfr->status == LIBUSB_TRANSFER_NO_DEVICE)
			return;                       /* shutting down */
		st.errors++;
	}

	for (int i = 0; i < xfr->num_iso_packets; i++) {
		struct libusb_iso_packet_descriptor *d = &xfr->iso_packet_desc[i];
		st.packets++;

		/* A packet can fail and still have delivered data. This endpoint is
		 * high-bandwidth isochronous -- three 1010-byte transactions per
		 * microframe -- and when fewer than three arrive the kernel returns
		 * -EXDEV, "ISO transfer only partially completed", which libusb maps
		 * to LIBUSB_TRANSFER_ERROR. The bytes that did arrive are perfectly
		 * good and sit at the front of the buffer.
		 *
		 * Discarding them was costing real picture: measured, 236 of 2358
		 * errored packets in a five-second capture carried a 1010-byte
		 * payload, and those partial packets are where the vertical-blanking
		 * intervals live -- the whole interval is under 600 bytes. Losing it
		 * loses the field anchor, and a field that loses its anchor is
		 * exactly the field that jumps. So: keep the data, count the error. */
		if (d->status != LIBUSB_TRANSFER_COMPLETED) {
			st.errors++;
			if (plog)
				fprintf(plog, "%llu %d %u\n", stream_pos,
				        (int)d->status, d->actual_length);
			if (st.errstat != (int)d->status) { st.errstat = d->status; st.errshown = 0; }
			if (!st.errshown) {
				/* Say what this means, because it is NOT a fault and it
				 * used to read like one. The device is allocated 24.24
				 * MB/s and produces 21.36, so short microframes are
				 * routine; the kernel calls a short high-bandwidth
				 * isochronous packet an error and we keep its data. */
				fprintf(stderr,
				        "  note: short isochronous packets (%s) -- normal "
				        "on this device, data is kept\n",
				        pkt_status(d->status));
				st.errshown = 1;
			}
			if (d->actual_length == 0)
				continue;
			st.partial++;
		} else {
			if (plog)
				fprintf(plog, "%llu 0 %u\n", stream_pos, d->actual_length);
			if (d->actual_length == 0)          { st.empty++;  continue; }
		}

		unsigned char *buf = libusb_get_iso_packet_buffer_simple(xfr, i);
		int len = d->actual_length;

		st.bytes += len;
		note_len(len);

		int nonzero = 0;
		for (int k = 0; k < len; k++)
			if (buf[k]) { nonzero = 1; break; }

		if (nonzero) {
			st.data_packets++;
			sample(buf, len);
		} else {
			st.zero_payload++;
		}

		if (outfp)
			fwrite(buf, 1, len, outfp);
		stream_pos += (unsigned long long)len;
		if (bt.frame)
			gb_bt656_feed(&bt, buf, len);
	}

	if (!stop_now && libusb_submit_transfer(xfr) != 0)
		st.errors++;
}

/* Commands go to EP 0x01 OUT as a short packet; the firmware reads byte 0 as an
 * opcode and dispatches on it. EP 0x01 is bulk at alt 0 and interrupt at alts
 * 1..5, so pick the transfer type to match whichever alt is selected.
 *
 * Known from the firmware disassembly:
 *   0x36 <isoinpkts> <mode>   start streaming; must be exactly 3 bytes, <mode>
 *                             must be non-zero, and <isoinpkts> (1/2/3) is
 *                             written to EP2ISOINPKTS in high-speed mode
 *   0x37                      stop streaming
 */
static int send_cmd(libusb_device_handle *h, int alt,
                    const unsigned char *cmd, int len)
{
	int got = 0, rc;

	fprintf(stderr, "cmd ->");
	for (int i = 0; i < len; i++) fprintf(stderr, " %02x", cmd[i]);

	if (alt == 0)
		rc = libusb_bulk_transfer(h, EP_CMD, (unsigned char *)cmd, len,
		                          &got, 1000);
	else
		rc = libusb_interrupt_transfer(h, EP_CMD, (unsigned char *)cmd, len,
		                               &got, 1000);

	fprintf(stderr, "   rc=%s sent=%d\n", libusb_strerror(rc), got);
	return rc;
}



/* Replay a verbatim command stream captured from the Windows driver.
 *
 * The file holds one command per line as hex bytes, in the order the driver
 * sent them -- see tools/parse-usbmon.py and captures/full-command-replay.txt.
 * Replaying verbatim rather than reconstructing from decoded registers matters:
 * the driver opens with a prologue (0xde 0x00, 0x85, 0x0b) and interleaves
 * read-back commands with its writes, and the decoder evidently depends on that
 * sequence.
 *
 * After each command the firmware arms EP 0x81 IN with a reply, so drain it --
 * leaving replies queued desynchronises everything that follows.
 */
static int replay_stream(libusb_device_handle *h, const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");
	int n = 0, bad = 0, shown = 0;

	if (!f) { perror(path); return -1; }

	while (fgets(line, sizeof line, f)) {
		unsigned char cmd[32], rep[64];
		int k = 0, got = 0, rc;
		char *tok = strtok(line, " \t\r\n");

		if (!tok || tok[0] == '#')
			continue;
		while (tok && k < (int)sizeof cmd) {
			if (!isxdigit((unsigned char)tok[0]))
				break;
			cmd[k++] = (unsigned char)strtol(tok, NULL, 16);
			tok = strtok(NULL, " \t\r\n");
		}
		if (k == 0)
			continue;

		rc = libusb_bulk_transfer(h, EP_CMD, cmd, k, &got, 1000);
		if (rc != 0) {
			bad++;
			if (shown < 5) {
				fprintf(stderr, "  cmd 0x%02x (%d bytes) failed: %s\n",
				        cmd[0], k, libusb_strerror(rc));
				shown++;
			}
		}
		/* Drain the reply whether or not we care about it. */
		got = 0;
		libusb_bulk_transfer(h, EP_STATUS, rep, sizeof rep, &got, 300);
		n++;
	}
	fclose(f);
	fprintf(stderr, "replayed %d command(s), %d failure(s)\n", n, bad);

	/* Verify the writes actually reached the decoder, using the exact read form
	 * the Windows driver used: 09 <wcount=2> <rcount=1> <addr> <reg_hi> <reg_lo>.
	 * Register 0x0100 must read 0x04 (device ID low byte of 0x8404) and 0x0414
	 * was written as 0x17 by the sequence. */
	static const struct { int reg; const char *what; unsigned char expect; }
		checks[] = {
			{ 0x0100, "device ID low", 0x04 },
			{ 0x0414, "written by init", 0x17 },
		};
	for (unsigned i = 0; i < sizeof checks / sizeof checks[0]; i++) {
		unsigned char c[6] = { 0x09, 0x02, 0x01, 0x44,
		                       (unsigned char)(checks[i].reg >> 8),
		                       (unsigned char)(checks[i].reg & 0xff) };
		unsigned char rep[64] = { 0 };
		int got = 0;
		if (libusb_bulk_transfer(h, EP_CMD, c, 6, &got, 1000) != 0) {
			fprintf(stderr, "  verify 0x%04x: command failed\n", checks[i].reg);
			continue;
		}
		got = 0;
		int rc2 = libusb_bulk_transfer(h, EP_STATUS, rep, 2, &got, 1000);
		if (rc2 != 0 || got < 2) {
			fprintf(stderr, "  verify 0x%04x (%s): no reply (%s)\n",
			       checks[i].reg, checks[i].what, libusb_strerror(rc2));
			continue;
		}
		fprintf(stderr, "  verify 0x%04x (%s): status 0x%02x value 0x%02x  expect 0x%02x  %s\n",
		       checks[i].reg, checks[i].what, rep[0], rep[1], checks[i].expect,
		       rep[1] == checks[i].expect ? "OK" : "MISMATCH");
	}
	return bad ? -1 : 0;
}


/* Execute a faithful session script captured from the Windows driver.
 *
 *   CMD <hex bytes>   send on EP 0x01 OUT, then drain the reply on EP 0x81 IN
 *   SETIF <n>         SET_INTERFACE interface 0, alternate setting n
 *   CLRHALT <ep>      CLEAR_FEATURE(ENDPOINT_HALT)
 *
 * Order matters more than it looks: the driver switches to alt 1 part-way
 * through and does the *majority* of its configuration afterwards. EP 0x01 is
 * bulk at alt 0 and interrupt at alts 1..5, so the transfer type has to follow
 * whichever alt is currently selected.
 */
static int exec_session(libusb_device_handle *h, const char *path, int *final_alt)
{
	char line[1024];
	FILE *f = fopen(path, "r");
	int cur_alt = 0, ncmd = 0, bad = 0, shown = 0;

	if (!f) { perror(path); return -1; }

	while (fgets(line, sizeof line, f)) {
		char *p2 = line;
		while (*p2 == ' ' || *p2 == '\t') p2++;
		if (*p2 == '#' || *p2 == '\n' || *p2 == '\0')
			continue;

		if (!strncmp(p2, "SETIF", 5)) {
			int a = atoi(p2 + 5);
			int rc = libusb_set_interface_alt_setting(h, IFACE, a);
			if (rc != 0)
				fprintf(stderr, "  SETIF %d failed: %s\n", a, libusb_strerror(rc));
			else
				cur_alt = a;
			continue;
		}
		if (!strncmp(p2, "CLRHALT", 7)) {
			int ep = (int)strtol(p2 + 7, NULL, 0);
			libusb_clear_halt(h, (unsigned char)ep);
			continue;
		}
		if (strncmp(p2, "CMD", 3))
			continue;

		unsigned char cmd[64], rep[64];
		int k = 0, got = 0, rc;
		char *tok = strtok(p2 + 3, " \t\r\n");
		while (tok && k < (int)sizeof cmd) {
			if (!isxdigit((unsigned char)tok[0])) break;
			cmd[k++] = (unsigned char)strtol(tok, NULL, 16);
			tok = strtok(NULL, " \t\r\n");
		}
		if (!k)
			continue;

		if (cur_alt == 0)
			rc = libusb_bulk_transfer(h, EP_CMD, cmd, k, &got, 1000);
		else
			rc = libusb_interrupt_transfer(h, EP_CMD, cmd, k, &got, 1000);
		if (rc != 0) {
			bad++;
			if (shown < 5) {
				fprintf(stderr, "  cmd 0x%02x failed (alt %d): %s\n",
				        cmd[0], cur_alt, libusb_strerror(rc));
				shown++;
			}
		}
		got = 0;
		memset(rep, 0, sizeof rep);
		if (cur_alt == 0)
			libusb_bulk_transfer(h, EP_STATUS, rep, sizeof rep, &got, 200);
		else
			libusb_interrupt_transfer(h, EP_STATUS, rep, sizeof rep, &got, 200);
		/* Command 0x09 is a read; surface the answer when asked. Handy for
		 * inspecting registers the captured session never touches. */
		if (cmd[0] == 0x09 && k >= 6 && getenv("GB_DEBUG") && got > 0) {
			fprintf(stderr, "  read 0x%02x%02x ->", cmd[4], cmd[5]);
			for (int q = 0; q < got; q++) fprintf(stderr, " %02x", rep[q]);
			fprintf(stderr, "\n");
		}
		ncmd++;
	}
	fclose(f);
	fprintf(stderr, "session: %d command(s), %d failure(s), final alt %d\n",
	       ncmd, bad, cur_alt);
	if (final_alt) *final_alt = cur_alt;
	return bad;
}


static int run(libusb_context *ctx, int seconds, int alt, const char *path)
{
	struct libusb_transfer *xfr[GB_MAX_XFERS] = { 0 };
	unsigned char *bufs[GB_MAX_XFERS] = { 0 };
	int rc = 1;

	libusb_device_handle *h =
		libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
	if (!h) {
		fprintf(stderr, "no device at 03f3:009c.\n"
		        "load firmware first: sudo ./gbfw load ../firmware/gb-loader-fw.bin\n");
		return 1;
	}
	libusb_set_auto_detach_kernel_driver(h, 1);

	if (libusb_claim_interface(h, IFACE) != 0) {
		fprintf(stderr, "cannot claim interface %d\n", IFACE);
		goto out;
	}
	if (g_session_path) {
		int fa = 0;
		libusb_set_interface_alt_setting(h, IFACE, 0);
		fprintf(stderr, "executing session script %s\n", g_session_path);
		exec_session(h, g_session_path, &fa);
		alt = fa;                       /* stream at whatever the script left */

		/* PAL needs a register write the captured session never makes, so
		 * --pal has to do more than change the geometry.
		 *
		 * 0x0400 = 0x04 selects PAL-BDGHI in bits 3:0 AND leaves ACFG_DIS
		 * (bit 5) clear. The session writes 0x21 -- NTSC-M with ACFG_DIS
		 * SET, which pins registers 0x470-0x47F to the NTSC active window.
		 * Changing only the format nibble to 0x24 restores horizontal lock
		 * and keeps a 244-line window on a 288-line signal, which is what
		 * made every early PAL attempt look half-broken. Clearing the bit
		 * lets the chip configure its own 625/50 timing.
		 *
		 * 0x401..0x403 keep the values the session left (0xe0, 0x04, 0x00).
		 *
		 * Note the chip latches the first standard it detects and holds it
		 * until power is cycled, so if this was NOT a fresh plug the write
		 * may be accepted and ignored. Read 0x040d back to see what it
		 * actually locked to: bits 3:0 are the DETECTED format, 4 = PAL. */
		if (g_pal) {
			static const unsigned char pal[] = {
				0x08, 0x44, 0x06, 0x04, 0x00, 0x04, 0xe0, 0x04, 0x00
			};
			fprintf(stderr, "PAL: writing 0x0400 = 0x04 "
			                "(PAL-BDGHI, ACFG_DIS clear)\n");
			send_cmd(h, alt, pal, sizeof pal);
		}

		/* Analogue front end: 0x0104 = 0x0704dc00.
		 *
		 * This is the one register the captured session never writes, and
		 * without it the audio ADC digitises nothing. Bits 7 and 8 of
		 * 0x0104 are VGA_SEL_CH2 and VGA_SEL_CH3, which choose whether the
		 * analogue channels carry video or audio -- mainline's
		 * cx25840-core.c calls the register AFE_CTRL and says so in as many
		 * words. Left at its reset default, the audio path is simply not
		 * connected.
		 *
		 * The value is what the Windows driver writes the moment an audio
		 * capture pin is opened, which is why 3171 steps of a video-only
		 * VirtualDub session never contained it. Measured with music fed
		 * into the input, one write either side:
		 *
		 *              distinct sample values   range    RMS
		 *   without                     10-18     +-10    1.7
		 *   with                         2011  +-1100  255.4
		 *
		 * and the two channels correlate at +0.982, which is stereo music
		 * and not noise. Video is unaffected: 480 of 480 rows either way. */
		if (!g_no_audio) {
			static const unsigned char afe[] = {
				0x08, 0x44, 0x06, 0x01, 0x04, 0x00, 0xdc, 0x04, 0x07
			};
			fprintf(stderr, "audio: writing 0x0104 = 0x0704dc00 "
			                "(AFE_CTRL, connects the audio path)\n");
			send_cmd(h, alt, afe, sizeof afe);
		}
	} else if (g_init_path) {
		libusb_set_interface_alt_setting(h, IFACE, 0);
		fprintf(stderr, "replaying captured bring-up from %s\n", g_init_path);
		replay_stream(h, g_init_path);
	}
	if (!g_session_path && libusb_set_interface_alt_setting(h, IFACE, alt) != 0) {
		fprintf(stderr, "cannot select alt %d (bandwidth refused?)\n", alt);
		goto out_release;
	}

	pkt_size = iso_packet_size(libusb_get_device(h), IFACE, alt, EP_VIDEO);
	if (pkt_size <= 0) {
		fprintf(stderr, "EP 0x%02x is not isochronous at alt %d\n", EP_VIDEO, alt);
		goto out_release;
	}
	fprintf(stderr, "alt %d: EP 0x%02x, %d bytes/microframe (%.2f MB/s ceiling)\n",
	       alt, EP_VIDEO, pkt_size, pkt_size * 8000.0 / 1e6);
	if (g_cmdlen)
		send_cmd(h, alt, g_cmd, g_cmdlen);
	else
		fprintf(stderr, "no init command sent (probe mode)\n");
	fprintf(stderr, "submitting %d transfers x %d packets\n\n",
	       NUM_TRANSFERS, PKTS_PER_XFER);

	if (path) {
		outfp = fopen(path, "wb");
		if (!outfp) { perror(path); goto out_release; }
	}
	if (g_audio_path) {
		/* O_NONBLOCK on the open too: a FIFO with no reader yet would
		 * otherwise block here forever, before a single frame is read. */
		audio_fd = open(g_audio_path, O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK,
		                0644);
		if (audio_fd < 0)
			/* Not fatal. A FIFO whose reader has not arrived yet gives
			 * ENXIO, and losing the sound is no reason to lose the
			 * picture as well. */
			fprintf(stderr, "audio: cannot open %s (%s) -- continuing "
			                "without sound\n", g_audio_path, strerror(errno));
		else {
			/* Deepen the FIFO for the same reason the frame pipe is
			 * deepened: the writer cannot block, so anything the reader
			 * is briefly too slow for is lost outright. A default 64 KB
			 * pipe is a third of a second of 48 kHz stereo, and a
			 * measured 0.8% of samples went missing at that size. Fails
			 * harmlessly with EINVAL on a regular file. */
			if (fcntl(audio_fd, F_SETPIPE_SZ, 1 << 18) == -1 &&
			    errno != EINVAL && errno != EBADF && errno != ESPIPE)
				fprintf(stderr, "note: could not enlarge audio pipe (%s)\n",
				        strerror(errno));
			fprintf(stderr, "audio -> %s (48000 Hz, 2ch, S16_BE)\n",
			        g_audio_path);
		}
	}
	if (g_pal)
		vid_field = 288;
	if (g_progressive)
		vid_height_fields = 1;
	if (g_frames_path) {
		framefp = !strcmp(g_frames_path, "-") ? stdout : fopen(g_frames_path, "wb");
		if (!framefp) { perror(g_frames_path); goto out_release; }
		framebuf = calloc(1, VID_FRAME);
		if (!framebuf) { fprintf(stderr, "out of memory\n"); goto out_release; }
		gb_bt656_init(&bt, framebuf, VID_FIELD, g_progressive, g_pal,
		              &gb_ops, NULL);
		/* Non-blocking, so a slow consumer costs frames rather than the
		 * whole capture. See gb_on_frame(). */
		int ffd = fileno(framefp);
		int fl = fcntl(ffd, F_GETFL, 0);
		if (fl != -1)
			fcntl(ffd, F_SETFL, fl | O_NONBLOCK);
		/* A default 64 KB pipe cannot hold a 691 KB frame, so the first write
		 * would always be partial and the all-or-nothing drop above could never
		 * fire cleanly. Ask for a frame-sized buffer; harmless if refused. */
		if (fcntl(ffd, F_SETPIPE_SZ, VID_FRAME + (VID_FRAME / 2)) == -1 &&
		    errno != EINVAL && errno != EBADF && errno != ESPIPE)
			fprintf(stderr, "note: could not enlarge pipe buffer (%s)\n",
			        strerror(errno));
		clock_gettime(CLOCK_MONOTONIC, &t_start);
		if (g_framelog) {
			flog = fopen(g_framelog, "w");
			if (!flog)
				perror(g_framelog);
			else
				fprintf(flog, "# seq us accepted rows run-3 run-2 run-1 run0 rowbitmap\n");
		}
		fprintf(stderr, "decoding BT.656 -> %dx%d UYVY frames\n", VID_W, VID_H);
	}

	if (g_pktlog) {
		plog = fopen(g_pktlog, "w");
		if (!plog)
			perror(g_pktlog);
		else
			fprintf(plog, "# stream_offset status actual_length\n");
	}

	size_t bufsz = (size_t)pkt_size * PKTS_PER_XFER;
	for (int i = 0; i < NUM_TRANSFERS; i++) {
		bufs[i] = malloc(bufsz);
		xfr[i] = libusb_alloc_transfer(PKTS_PER_XFER);
		if (!bufs[i] || !xfr[i]) { fprintf(stderr, "alloc failed\n"); goto out_free; }

		libusb_fill_iso_transfer(xfr[i], h, EP_VIDEO, bufs[i], bufsz,
		                         PKTS_PER_XFER, on_done, NULL, 1000);
		libusb_set_iso_packet_lengths(xfr[i], pkt_size);

		int e = libusb_submit_transfer(xfr[i]);
		if (e != 0) {
			fprintf(stderr, "submit %d failed: %s\n", i, libusb_strerror(e));
			goto out_free;
		}
	}

	struct timespec t0, now;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (!stop_now) {
		struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
		libusb_handle_events_timeout_completed(ctx, &tv, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - t0.tv_sec >= seconds)
			break;
	}
	stop_now = 1;

	for (int i = 0; i < NUM_TRANSFERS; i++)
		if (xfr[i]) libusb_cancel_transfer(xfr[i]);
	for (int i = 0; i < 8; i++) {
		struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
		libusb_handle_events_timeout_completed(ctx, &tv, NULL);
	}

	double secs = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
	fprintf(stderr, "\n--- %.1f s on alt %d ---\n", secs, alt);
	fprintf(stderr, "packets       %lu\n", st.packets);
	fprintf(stderr, "  empty       %lu\n", st.empty);
	fprintf(stderr, "  all-zero    %lu\n", st.zero_payload);
	fprintf(stderr, "  with data   %lu\n", st.data_packets);
	fprintf(stderr, "errors        %lu  (%lu carried data and were kept)\n",
	        st.errors, st.partial);
	fprintf(stderr, "bytes         %llu  (%.2f MB/s)\n",
	       st.bytes, secs > 0 ? st.bytes / secs / 1e6 : 0.0);
	if (st.nbuckets) {
		fprintf(stderr, "packet lengths seen:\n");
		for (int i = 0; i < st.nbuckets; i++)
			fprintf(stderr, "  %5d bytes  x%lu\n", st.bucket[i].len, st.bucket[i].count);
	}
	if (outfp)
		fprintf(stderr, "wrote %s\n", path);
	if (framefp) {
		/* Restore blocking for the last frame: a torn tail makes players
		 * complain ("packet size 763 < expected frame_size"), and at shutdown
		 * there is no stream left to protect from a stall. */
		int ffd = fileno(framefp);
		int fl = fcntl(ffd, F_GETFL, 0);
		if (fl != -1)
			fcntl(ffd, F_SETFL, fl & ~O_NONBLOCK);
		gb_bt656_flush(&bt);
		fprintf(stderr, "decoded %u complete frame(s); %u boundaries, %u rejected as partial",
		        frames_out, bt.attempts, bt.short_frames);
		if (dropped_frames)
			fprintf(stderr, "; %u dropped (consumer too slow)", dropped_frames);
		if (torn_frames)
			fprintf(stderr, "; %u TORN (consumer stalled mid-frame)", torn_frames);
		fprintf(stderr, "\n");
		/* Rejecting EVERY boundary is a diagnosis, not a failure, and it has
		 * one overwhelmingly likely cause: the source is not the standard we
		 * configured. A 625/50 signal decoded as 525/60 gives field runs of
		 * roughly 161 and 120 lines instead of 243 and 244, and every frame
		 * fails the short-field test. Say so, because a silent zero sends
		 * people looking for a bug in the assembler. */
		if (frames_out == 0 && bt.attempts > 8)
			fprintf(stderr,
			        "  every frame was rejected -- the usual cause is a "
			        "standard mismatch.\n"
			        "  A PAL source needs %s; an NTSC source needs %s.\n",
			        g_pal ? "what you gave (--pal)" : "--pal",
			        g_pal ? "no --pal" : "what you gave (no --pal)");
		if (framefp != stdout) fclose(framefp);
		if (flog) { fclose(flog); fprintf(stderr, "frame log -> %s\n", g_framelog); }
	}

	fprintf(stderr, "\nverdict: ");
	if (st.data_packets)
		fprintf(stderr, "DATA IS FLOWING -- firmware streams without an init command.\n");
	else if (st.zero_payload)
		fprintf(stderr, "packets arrive but are all zero -- USB path works, "
		       "decoder is not producing pixels.\n");
	else
		fprintf(stderr, "no payload at all -- a start command is required.\n");

	rc = 0;

out_free:
	/* ALWAYS stop the stream, not just when we were the one who started it with
	 * an explicit `go` command.
	 *
	 * This used to be `if (g_cmdlen && g_cmd[0] == CMD_START)`, which is false
	 * for the `session` path -- the one gbplay.sh and gb-up.sh actually use --
	 * so 0x37 was never sent and the device kept streaming after every capture.
	 * Dropping to alt 0 below releases the BUS bandwidth, so nothing else on the
	 * machine suffers and the leak is invisible from the host; but the firmware's
	 * own streaming flag stays set, the CX25840 keeps digitising, and the front
	 * LED keeps flashing until the device is unplugged. The operator noticed the
	 * LED. Nothing in the host-side numbers could have.
	 *
	 * It is sent AFTER the switch to alt 0 below, not before. Stopping the
	 * stream and then releasing the bandwidth looks like the natural order and
	 * does not work: measured, `cmd -> 37 rc=Operation timed out sent=0`, every
	 * time. EP 0x01 is interrupt at alts 1..5 and bulk only at alt 0, and the
	 * interrupt endpoint will not take the command with isochronous transfers
	 * still unwinding. At alt 0 the same byte goes through first time. */
	for (int i = 0; i < NUM_TRANSFERS; i++) {
		if (xfr[i]) libusb_free_transfer(xfr[i]);
		free(bufs[i]);
	}
	if (outfp) fclose(outfp);
	if (audio_fd >= 0) {
		audio_flush();
		close(audio_fd);
		audio_fd = -1;
		fprintf(stderr, "audio: %lu bytes (%.2f s)",
		        audio_bytes, audio_bytes / 4.0 / 48000.0);
		if (audio_dropped)
			fprintf(stderr, ", %lu dropped (consumer too slow)", audio_dropped);
		fprintf(stderr, "\n");
	}
	if (plog) { fclose(plog); plog = NULL; }
	/* Release the bandwidth reservation; leaving a high-bandwidth isoc alt
	 * selected steals bus bandwidth from every other device. */
	libusb_set_interface_alt_setting(h, IFACE, 0);
	{
		unsigned char stop[1] = { CMD_STOP }, rep[8];
		int got = 0;
		send_cmd(h, 0, stop, 1);
		/* Drain the reply. Every command arms one on EP 0x81, and leaving it
		 * queued desynchronises whatever runs next against this device. */
		libusb_bulk_transfer(h, EP_STATUS, rep, sizeof rep, &got, 200);
	}
out_release:
	libusb_release_interface(h, IFACE);
out:
	libusb_close(h);
	return rc;
}

/* Synchronous bulk read on a given endpoint at alt 0. Cross-check for the isoc
 * result: a device that is simply idle should time out here, whereas a stall or
 * an immediate error says something different about the endpoint's state. */
static int bulk_probe(libusb_context *ctx, int ep, int alt)
{
	unsigned char buf[512];
	int got = 0, rc;

	libusb_device_handle *h =
		libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
	if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); return 1; }
	libusb_set_auto_detach_kernel_driver(h, 1);

	if (libusb_claim_interface(h, IFACE) != 0) {
		fprintf(stderr, "cannot claim interface\n");
		libusb_close(h); return 1;
	}
	libusb_set_interface_alt_setting(h, IFACE, alt);

	fprintf(stderr, "bulk read EP 0x%02x at alt %d, 5 attempts, 500 ms each\n", ep, alt);
	for (int i = 0; i < 5; i++) {
		rc = libusb_bulk_transfer(h, ep, buf, sizeof buf, &got, 500);
		fprintf(stderr, "  [%d] rc=%s got=%d", i, libusb_strerror(rc), got);
		if (got > 0) {
			fprintf(stderr, " :");
			for (int k = 0; k < 24 && k < got; k++) fprintf(stderr, " %02x", buf[k]);
		}
		fprintf(stderr, "\n");
	}

	libusb_set_interface_alt_setting(h, IFACE, 0);
	libusb_release_interface(h, IFACE);
	libusb_close(h);
	return 0;
}

/* Command 0x09 is the firmware's I2C passthrough. From the disassembly at
 * 0x02c5, the packet layout is:
 *
 *   byte[0] = 0x09
 *   byte[1] = number of data bytes to write   (stored to 0x53 as the count)
 *   byte[2] = reply length - 1                (EP1INBC is set to byte[2]+1)
 *   byte[3] = 7-bit slave address             (reaches R4, doubled to 8-bit)
 *   byte[4..] = data
 *
 * The helper at 0x17b8 asserts START, writes addr<<1 to I2DAT, then polls I2CS
 * checking BERR (bit 2) and ACK (bit 1). The reply on EP 0x81 IN carries the
 * resulting count/status in byte 0, so a device that ACKs is distinguishable
 * from one that does not -- which is enough to scan the bus.
 */
static int i2c_xfer(libusb_device_handle *h, int addr7,
                    const unsigned char *data, int ndata,
                    int nread, unsigned char *reply, int rlen)
{
	unsigned char cmd[24];
	int got = 0, rc, n = 0;

	cmd[n++] = 0x09;
	cmd[n++] = (unsigned char)ndata;
	cmd[n++] = (unsigned char)nread;
	cmd[n++] = (unsigned char)addr7;
	for (int i = 0; i < ndata && n < (int)sizeof cmd; i++)
		cmd[n++] = data[i];

	rc = libusb_bulk_transfer(h, EP_CMD, cmd, n, &got, 500);
	if (rc != 0)
		return rc;

	/* The firmware sets EP1INBC to nread + 1: one status byte then the data. */
	int want = nread + 1;
	if (want > rlen)
		want = rlen;
	got = 0;
	rc = libusb_bulk_transfer(h, EP_STATUS, reply, want, &got, 500);
	return rc != 0 ? rc : got;
}

/* The firmware's I2C read path leaves the interrupt state machine in a state
 * that the next read's precondition check (0x1af1: "state must be 0") rejects,
 * after which the busy-wait at 0x1cb5 spins forever and wedges the 8051. So
 * exactly ONE read succeeds, and the next one hangs.
 *
 * Empirically the only thing that clears it is a full address sweep. A single
 * write, an ACKing write, and simply waiting all fail to recover it; the
 * 112-transaction sweep works every time. It is slow (~1-2 s) but completely
 * reproducible, which is enough to read the handful of registers that matter.
 *
 * TWO CORRECTIONS from disassembling the firmware (2026-08-24):
 *
 *   - These are NOT "write-only transactions". 0x17b8 treats a zero write count
 *     as "skip the write phase" and falls straight through to the read phase, so
 *     the sweep is 112 READS. It recovers by driving the state machine to a
 *     terminal state, not by writing anything.
 *
 *   - The failed recovery attempts used command 0x08, which is a COMPLETELY
 *     SEPARATE I2C implementation -- polled, with the interrupt disabled, never
 *     touching state variable 0x1b. They could not have recovered anything, so
 *     "only a full sweep works" does not follow from them.
 *
 * MEASURED SINCE (2026-08-24, `gbcap recovertest`): a single zero-count
 * command 0x09 is enough, and reads never wedged at all -- one transaction
 * SHAPE does. wcount=2/rcount=1 reads correctly 100 times running with no
 * resync between them; wcount=1, and any rcount > 1, wedge. driver/gb-i2c.c
 * has the full matrix. This function keeps the historical sweep because the
 * subcommands below were written against it; GB_RECOVER sets its length.
 */
static void i2c_sweep(libusb_device_handle *h, int addr, int count)
{
	unsigned char reply[8];
	for (int i = 0; i < count; i++)
		i2c_xfer(h, addr < 0 ? 0x08 + (i % 0x70) : addr,
		         NULL, 0, 0, reply, sizeof reply);
}

static void i2c_recover(libusb_device_handle *h)
{
	i2c_sweep(h, -1, 0x70);          /* the historical 112-address sweep */
}

/* Read one CX2584x register. The chip uses 16-bit big-endian register
 * addressing: write addr_hi, addr_lo, then read the value back. Same convention
 * as cx25840_read() in drivers/media/i2c/cx25840/cx25840-core.c. */
static int cx_read(libusb_device_handle *h, int reg, int *val)
{
	unsigned char w[2] = { (unsigned char)(reg >> 8), (unsigned char)(reg & 0xff) };
	unsigned char r[8] = { 0 };
	int got = i2c_xfer(h, 0x44, w, 2, 1, r, sizeof r);

	if (got < 0)
		return got;
	if (got < 2 || r[0] != 0x08)
		return -1000 - r[0];
	*val = r[1];
	return 0;
}

static int cmd_i2cscan(libusb_context *ctx)
{
	unsigned char reply[64];
	int found = 0;

	libusb_device_handle *h =
		libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
	if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); return 1; }
	libusb_set_auto_detach_kernel_driver(h, 1);
	if (libusb_claim_interface(h, IFACE) != 0) {
		fprintf(stderr, "cannot claim interface\n"); libusb_close(h); return 1;
	}
	libusb_set_interface_alt_setting(h, IFACE, 0);

	fprintf(stderr, "scanning I2C bus via command 0x09 (7-bit addresses 0x08..0x77)\n");
	fprintf(stderr, "addr7  addr8   reply\n");
	for (int a = 0x08; a <= 0x77; a++) {
		memset(reply, 0xAA, sizeof reply);
		int r = i2c_xfer(h, a, NULL, 0, 0, reply, 8);
		if (r < 0) {
			fprintf(stderr, " 0x%02x   0x%02x    transfer error: %s\n",
			       a, a << 1, libusb_strerror(r));
			continue;
		}
		fprintf(stderr, " 0x%02x   0x%02x    got=%d :", a, a << 1, r);
		for (int i = 0; i < r && i < 8; i++)
			fprintf(stderr, " %02x", reply[i]);
		fprintf(stderr, "\n");
		found++;
	}
	fprintf(stderr, "\n%d addresses answered the command\n", found);

	libusb_release_interface(h, IFACE);
	libusb_close(h);
	return 0;
}

/* Run a saved raw capture through the very same assembler the live path uses.
 *
 * The offline decoder in tools/bt656-decode.py is a second implementation and
 * has disagreed with this one before, which makes it useless as a check on it.
 * This subcommand needs no device, so every change to gb_bt656_feed() can be
 * measured against captures/samples/bt656-slice.raw before anyone is asked to
 * plug a camera in. */
static int cmd_decode(const char *raw)
{
	FILE *f = fopen(raw, "rb");
	if (!f) { perror(raw); return 1; }

	if (g_pal)         vid_field = 288;
	if (g_progressive) vid_height_fields = 1;

	framebuf = calloc(1, VID_FRAME);
	if (!framebuf) { fprintf(stderr, "out of memory\n"); fclose(f); return 1; }
	gb_bt656_init(&bt, framebuf, VID_FIELD, g_progressive, g_pal, &gb_ops, NULL);
	if (g_frames_path) {
		framefp = !strcmp(g_frames_path, "-") ? stdout : fopen(g_frames_path, "wb");
		if (!framefp) { perror(g_frames_path); fclose(f); return 1; }
	}
	if (g_audio_path) {
		audio_fd = open(g_audio_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (audio_fd < 0) { perror(g_audio_path); fclose(f); return 1; }
	}
	clock_gettime(CLOCK_MONOTONIC, &t_start);
	if (g_framelog) {
		flog = fopen(g_framelog, "w");
		if (!flog) perror(g_framelog);
		else fprintf(flog, "# seq us accepted rows run-3 run-2 run-1 run0 rowbitmap\n");
	}

	/* Feed it in packet-sized bites so the incremental scanner is exercised
	 * across buffer boundaries exactly as it is on the wire. */
	unsigned char buf[3072];
	size_t got, total = 0;
	while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
		gb_bt656_feed(&bt, buf, (int)got);
		total += got;
	}
	fclose(f);
	if (framefp && framefp != stdout) fclose(framefp);
	if (flog) fclose(flog);
	if (audio_fd >= 0) {
		audio_flush();
		close(audio_fd);
		audio_fd = -1;
		fprintf(stderr, "audio -> %s: %lu bytes (%.2f s, 48000 Hz 2ch S16_BE)\n",
		        g_audio_path, audio_bytes, audio_bytes / 4.0 / 48000.0);
	}

	/* Report what the ASSEMBLER did, not what was written. Those used to be the
	 * same number, because having no --frames destination was folded into the
	 * reject test -- so `gbcap decode <raw>` answered "0 frames, 7 too short"
	 * and read as a decoder that could not assemble anything.
	 * The guard now lives in driver/gb-bt656.c and knows nothing about output
	 * files, so the two can be told apart. */
	fprintf(stderr, "%zu bytes -> %u boundaries, %u frames, %u too short",
	        total, bt.attempts, bt.attempts - bt.short_frames, bt.short_frames);
	if (g_frames_path)
		fprintf(stderr, "; %u written", frames_out);
	fputc('\n', stderr);
	return 0;
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	int seconds = 3, alt = 1, rc;
	const char *path = NULL;

	if (argc < 2) {
		fprintf(stderr,
		        "usage: %s probe [seconds] [alt]\n"
		        "       %s dump <file> [seconds] [alt]\n", argv[0], argv[0]);
		return 64;
	}
	if (!strcmp(argv[1], "probe")) {
		if (argc > 2) seconds = atoi(argv[2]);
		if (argc > 3) alt     = atoi(argv[3]);
	} else if (!strcmp(argv[1], "dump") && argc >= 3) {
		path = argv[2];
		if (argc > 3) seconds = atoi(argv[3]);
		if (argc > 4) alt     = atoi(argv[4]);
	} else if (!strcmp(argv[1], "go") && argc >= 4) {
		/* go <alt> <seconds> <cmd bytes...> [-o file] */
		alt     = atoi(argv[2]);
		seconds = atoi(argv[3]);
		for (int i = 4; i < argc && g_cmdlen < (int)sizeof g_cmd; i++) {
			if (!strcmp(argv[i], "-o") && i + 1 < argc) { path = argv[++i]; continue; }
			g_cmd[g_cmdlen++] = (unsigned char)strtol(argv[i], NULL, 0);
		}
	} else if (!strcmp(argv[1], "i2c") && argc >= 4) {
		/* i2c <addr7> <replylen> [data bytes...] */
		unsigned char data[12], reply[64];
		int addr = (int)strtol(argv[2], NULL, 0);
		int rlen = (int)strtol(argv[3], NULL, 0);
		int nd = 0;
		for (int i = 4; i < argc && nd < (int)sizeof data; i++)
			data[nd++] = (unsigned char)strtol(argv[i], NULL, 0);

		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		libusb_claim_interface(h, IFACE);
		libusb_set_interface_alt_setting(h, IFACE, 0);

		memset(reply, 0, sizeof reply);
		int r = i2c_xfer(h, addr, data, nd, rlen, reply, sizeof reply);
		printf("i2c addr 0x%02x (8-bit 0x%02x), wrote %d byte(s) ->", addr, addr << 1, nd);
		if (r < 0) printf(" error: %s\n", libusb_strerror(r));
		else {
			printf(" got %d :", r);
			for (int i = 0; i < r; i++) printf(" %02x", reply[i]);
			printf("   [byte0 status: 08=ACK 07=NACK]\n");
		}
		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	} else if (!strcmp(argv[1], "cxid")) {
		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		libusb_claim_interface(h, IFACE);
		libusb_set_interface_alt_setting(h, IFACE, 0);

		int lo = 0, hi = 0;
		printf("recovering I2C state (address sweep)...\n");
		i2c_recover(h);
		int a = cx_read(h, 0x0100, &lo);
		printf("  reg 0x0100 rc=%d val=0x%02x\n", a, lo);
		printf("recovering again...\n");
		i2c_recover(h);
		int b = cx_read(h, 0x0101, &hi);
		printf("  reg 0x0101 rc=%d val=0x%02x\n", b, hi);
		if (a || b) {
			printf("CX2584x ID read failed (0x100 rc=%d, 0x101 rc=%d)\n", a, b);
		} else {
			int id = (hi << 8) | lo;
			printf("CX2584x device ID = 0x%04x\n", id);
			if ((id & 0xff00) == 0x8400)      printf("  -> CX2584x family\n");
			else if ((id & 0xff00) == 0x8300) printf("  -> CX2583x family\n");
			else if ((id & 0xfff0) == 0x5a30) printf("  -> CX25836/7\n");
			else                              printf("  -> unrecognised\n");
		}
		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	} else if (!strcmp(argv[1], "regdump") && argc >= 4) {
		/* regdump <addr7> <count> -- 8-bit register addressing, one byte at a
		 * time with an I2C recovery sweep before each read. Writing a single
		 * byte only sets the sub-address pointer, so this cannot alter the chip. */
		int a7 = (int)strtol(argv[2], NULL, 0);
		int cnt = (int)strtol(argv[3], NULL, 0);
		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		libusb_claim_interface(h, IFACE);
		libusb_set_interface_alt_setting(h, IFACE, 0);

		int do_reset = (argc > 4 && !strcmp(argv[4], "reset"));
		printf("8-bit register dump, I2C 0x%02x (8-bit 0x%02x)%s\n",
		       a7, a7 << 1, do_reset ? ", decoder reset before each read" : "");
		for (int r = 0; r < cnt; r++) {
			unsigned char w[1] = { (unsigned char)r }, rep[8] = { 0 };
			if (do_reset) {
				unsigned char rst[1] = { 0x83 };
				int sent = 0;
				libusb_bulk_transfer(h, EP_CMD, rst, 1, &sent, 1000);
				libusb_bulk_transfer(h, EP_STATUS, rep, 8, &sent, 1000);
				memset(rep, 0, sizeof rep);
			}
			i2c_recover(h);
			int got = i2c_xfer(h, a7, w, 1, 1, rep, sizeof rep);
			if (got == 2 && rep[0] == 0x08)
				printf("  reg 0x%02x = 0x%02x\n", r, rep[1]);
			else if (got < 0)
				printf("  reg 0x%02x : %s\n", r, libusb_strerror(got));
			else
				printf("  reg 0x%02x : status 0x%02x (got %d)\n", r, rep[0], got);
		}
		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	} else if (!strcmp(argv[1], "cmd") && argc >= 3) {
		/* cmd <bytes...> -- send a raw command on EP 0x01 OUT at alt 0 and show
		 * the reply from EP 0x81 IN. */
		unsigned char c[16], rep[64] = { 0 };
		int n = 0, got = 0;
		for (int i = 2; i < argc && n < (int)sizeof c; i++)
			c[n++] = (unsigned char)strtol(argv[i], NULL, 0);
		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		libusb_claim_interface(h, IFACE);
		libusb_set_interface_alt_setting(h, IFACE, 0);
		printf("cmd ->");
		for (int i = 0; i < n; i++) printf(" %02x", c[i]);
		rc = libusb_bulk_transfer(h, EP_CMD, c, n, &got, 1000);
		printf("   sent=%d rc=%s\n", got, libusb_strerror(rc));
		got = 0;
		rc = libusb_bulk_transfer(h, EP_STATUS, rep, 8, &got, 1000);
		printf("reply <- rc=%s got=%d :", libusb_strerror(rc), got);
		for (int i = 0; i < got; i++) printf(" %02x", rep[i]);
		printf("\n");
		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	} else if (!strcmp(argv[1], "irwatch")) {
		/* irwatch [seconds] -- poll command 0x85 and print only what CHANGES.
		 *
		 * Why 0x85. In the Windows capture it is sent 2563 times across 279
		 * seconds -- about nine times a second, for the whole session -- and it
		 * reads back five bytes. Nothing else in the command set behaves that
		 * way: every other opcode fires a handful of times during bring-up. A
		 * steady low-rate poll returning a small fixed payload is what reading a
		 * remote-control receiver looks like, and this unit has an IR window on
		 * the front and shipped with a remote.
		 *
		 * Why the existing capture cannot answer it. It only ever shows three
		 * distinct replies -- 07 01 00 10 00, 07 01 00 00 00, 07 00 00 00 00 --
		 * because nobody pressed a button while it was recorded. That is the
		 * same trap as the audio session: a replay is only as complete as the
		 * capture, and this capture does not contain the feature. So this has to
		 * be run live, with somebody pressing buttons.
		 *
		 * It may equally turn out to be a signal-status poll, in which case the
		 * bytes will not move no matter what is pressed. That is a real answer
		 * too, and cheaper than guessing. */
		int secs = argc >= 3 ? atoi(argv[2]) : 30;
		unsigned char c = 0x85, rep[64], last[8];
		int lastn = -1, polls = 0, changes = 0, got;
		struct timespec t0, tn;

		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		libusb_claim_interface(h, IFACE);
		libusb_set_interface_alt_setting(h, IFACE, 0);

		printf("polling 0x85 for %d s at ~20 Hz -- PRESS BUTTONS ON THE REMOTE\n", secs);
		printf("only changed replies are printed; aim it at the window on the front\n\n");
		clock_gettime(CLOCK_MONOTONIC, &t0);
		memset(last, 0, sizeof last);
		for (;;) {
			double el;
			clock_gettime(CLOCK_MONOTONIC, &tn);
			el = (tn.tv_sec - t0.tv_sec) + (tn.tv_nsec - t0.tv_nsec) / 1e9;
			if (el >= secs) break;

			got = 0;
			if (libusb_bulk_transfer(h, EP_CMD, &c, 1, &got, 500) != 0) continue;
			got = 0;
			memset(rep, 0, sizeof rep);
			if (libusb_bulk_transfer(h, EP_STATUS, rep, 8, &got, 500) != 0) continue;
			polls++;

			if (got != lastn || memcmp(rep, last, got ? got : 1) != 0) {
				if (lastn >= 0) {          /* skip the first, it is not a change */
					printf("  t=%7.3fs  ", el);
					for (int i = 0; i < got; i++) printf("%02x ", rep[i]);
					printf("\n");
					fflush(stdout);
					changes++;
				}
				memcpy(last, rep, got > (int)sizeof last ? (int)sizeof last : got);
				lastn = got;
			}
			usleep(50000);                 /* ~20 Hz, twice Windows' rate */
		}
		printf("\n%d polls, %d changes\n", polls, changes);
		if (!changes)
			printf("nothing moved: 0x85 is not carrying button events, or the\n"
			       "receiver is read some other way entirely.\n");
		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	} else if (!strcmp(argv[1], "cxreg") && argc >= 3) {
		/* cxreg <reg16> [order] -- read one CX25840 register (16-bit big-endian
		 * addressing, per cx25840_write/read in the mainline driver).
		 * order: "plain"  recover, then read
		 *        "reset"  recover, pulse decoder reset (cmd 0x83), then read
		 *        "rstrec" pulse reset, then recover, then read              */
		int reg = (int)strtol(argv[2], NULL, 0);
		const char *order = argc > 3 ? argv[3] : "plain";
		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		libusb_claim_interface(h, IFACE);
		libusb_set_interface_alt_setting(h, IFACE, 0);

		unsigned char rst[1] = { 0x83 }, junk[8];
		int sent = 0;
		if (!strcmp(order, "rstrec")) {
			libusb_bulk_transfer(h, EP_CMD, rst, 1, &sent, 1000);
			libusb_bulk_transfer(h, EP_STATUS, junk, 8, &sent, 1000);
		}
		i2c_recover(h);
		if (!strcmp(order, "reset")) {
			libusb_bulk_transfer(h, EP_CMD, rst, 1, &sent, 1000);
			libusb_bulk_transfer(h, EP_STATUS, junk, 8, &sent, 1000);
		}
		int val = 0;
		int r = cx_read(h, reg, &val);
		if (r == 0) printf("[%s] CX25840 reg 0x%04x = 0x%02x\n", order, reg, val);
		else        printf("[%s] CX25840 reg 0x%04x : failed rc=%d\n", order, reg, r);

		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	} else if (!strcmp(argv[1], "cxseq") && argc >= 3) {
		/* cxseq <reg16>... -- read a list of CX25840 registers in ONE session,
		 * one recovery transaction between each, and print them in order.
		 *
		 * The point is to test whether a single-byte read returns the CURRENT
		 * byte or the PREVIOUS one. The ISR stores a byte on the state-3
		 * interrupt and again on the state-2 interrupt, so a read that gets only
		 * one interrupt hands back stale data. If the values lag by one
		 * position, that is the answer -- and it would also explain the
		 * long-standing "every register reads 0x8a", which would be a stale
		 * constant rather than a property of the chip.
		 *
		 * Reads only. */
		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		libusb_claim_interface(h, IFACE);
		libusb_set_interface_alt_setting(h, IFACE, 0);

		i2c_recover(h);
		for (int i = 2; i < argc; i++) {
			int reg = (int)strtol(argv[i], NULL, 0), v = 0;
			int rc2 = cx_read(h, reg, &v);
			printf("  0x%04x -> rc=%d val=0x%02x\n", reg, rc2, v);
			/* GB_RECOVER=0 tests whether any resync is needed between reads
			 * at all; the default of 1 is the cheap resync this experiment
			 * established is sufficient. */
			i2c_sweep(h, -1, getenv("GB_RECOVER")
			                 ? atoi(getenv("GB_RECOVER")) : 1);
		}
		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	} else if (!strcmp(argv[1], "recovertest")) {
		/* recovertest [count] [addr7] -- how much recovery does a wedged read
		 * actually need?
		 *
		 * An early note recorded that only a full 112-address sweep clears a
		 * wedged read, and that a single write does not. But disassembly showed
		 * those write tests used command 0x08, which is a SEPARATE polled I2C
		 * implementation that never touches the state machine involved -- so the
		 * conclusion did not follow from the evidence. The sweep itself is 112
		 * command-0x09 transactions with a zero write count, i.e. 112 reads.
		 *
		 * If the sweep works by driving the read state machine to a terminal
		 * state, ONE transaction should be enough, and recovery drops from 1-2 s
		 * to about a millisecond -- which makes the whole problem survivable in a
		 * kernel driver.
		 *
		 * Everything here is a READ. Nothing is written to any device. */
		int count = argc > 2 ? atoi(argv[2]) : 1;
		int addr  = argc > 3 ? (int)strtol(argv[3], NULL, 0) : -1;
		int v;

		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		libusb_claim_interface(h, IFACE);
		libusb_set_interface_alt_setting(h, IFACE, 0);

		printf("full 112-address sweep, then read 0x0100 (expect 0x04)\n");
		i2c_recover(h);
		printf("  read 1: rc=%d val=0x%02x\n", cx_read(h, 0x0100, &v), v);

		printf("read again with NO recovery (expect the wedge)\n");
		v = 0;
		printf("  read 2: rc=%d val=0x%02x\n", cx_read(h, 0x0100, &v), v);

		if (addr < 0)
			printf("now %d transaction(s), addresses walking from 0x08\n", count);
		else
			printf("now %d transaction(s) to 0x%02x\n", count, addr);
		i2c_sweep(h, addr, count);

		v = 0;
		int rc3 = cx_read(h, 0x0100, &v);
		printf("  read 3: rc=%d val=0x%02x   -> %s\n", rc3, v,
		       rc3 == 0 && v == 0x04 ? "RECOVERED" : "still wedged");

		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return 0;
	} else if (!strcmp(argv[1], "stop")) {
		/* stop -- tell the device to stop streaming, and drop to alt 0.
		 *
		 * Needed as a standalone command because the device has no idea its host
		 * went away: killing a capture, or a capture that forgot to send 0x37,
		 * leaves the firmware's streaming flag set and the decoder running. The
		 * visible symptom is the front LED flashing indefinitely; the invisible
		 * one is a CX25840 digitising into a FIFO nobody is draining.
		 *
		 * Only a replug cleared this before. It is also exactly what the kernel
		 * driver's stop_streaming() will have to do. */
		unsigned char stop[1] = { CMD_STOP }, rep[8];
		int got = 0;

		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		libusb_device_handle *h =
			libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_CAPTURE);
		if (!h) { fprintf(stderr, "no device at 03f3:009c\n"); libusb_exit(ctx); return 1; }
		libusb_set_auto_detach_kernel_driver(h, 1);
		if (libusb_claim_interface(h, IFACE) != 0) {
			fprintf(stderr, "cannot claim interface -- is a capture still running?\n");
			libusb_close(h); libusb_exit(ctx); return 1;
		}
		/* Whatever alt we inherited, alt 0 makes EP 0x01 bulk and releases any
		 * high-bandwidth isochronous reservation still held. */
		libusb_set_interface_alt_setting(h, IFACE, 0);
		rc = send_cmd(h, 0, stop, 1);
		libusb_bulk_transfer(h, EP_STATUS, rep, sizeof rep, &got, 200);
		if (rc == 0)
			printf("stop sent; the LED should go steady.\n");
		else
			printf("stop failed: %s\n", libusb_strerror(rc));
		libusb_release_interface(h, IFACE);
		libusb_close(h);
		libusb_exit(ctx);
		return rc == 0 ? 0 : 1;
	} else if (!strcmp(argv[1], "i2cscan")) {
		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		rc = cmd_i2cscan(ctx);
		libusb_exit(ctx);
		return rc;
	} else if (!strcmp(argv[1], "live") && argc >= 4) {
		/* live <alt> <seconds> <initfile> [-o dump] -- full sequence:
		 * replay the captured CX25840 bring-up, SET_INTERFACE to the streaming
		 * alt, then read isochronous video off EP 0x82. */
		alt      = atoi(argv[2]);
		seconds  = atoi(argv[3]);
		g_init_path = argv[4];
		for (int i = 5; i < argc; i++)
			if (!strcmp(argv[i], "-o") && i + 1 < argc)
				path = argv[++i];
	} else if (!strcmp(argv[1], "session") && argc >= 4) {
		/* session <script> <seconds> [-o dump] */
		g_session_path = argv[2];
		seconds = atoi(argv[3]);
		alt = 1;
		for (int i = 4; i < argc; i++) {
			if (!strcmp(argv[i], "-o") && i + 1 < argc)
				path = argv[++i];
			else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
				g_frames_path = argv[++i];
			else if (!strcmp(argv[i], "--framelog") && i + 1 < argc)
				g_framelog = argv[++i];
			else if (!strcmp(argv[i], "--pktlog") && i + 1 < argc)
				g_pktlog = argv[++i];
			else if (!strcmp(argv[i], "--pal"))
				g_pal = 1;
			else if (!strcmp(argv[i], "--no-audio"))
				g_no_audio = 1;
			else if (!strcmp(argv[i], "--audio") && i + 1 < argc)
				g_audio_path = argv[++i];
			else if (!strcmp(argv[i], "--progressive") || !strcmp(argv[i], "--240p"))
				g_progressive = 1;
		}
	} else if (!strcmp(argv[1], "decode") && argc >= 3) {
		/* decode <rawfile> [--frames f] [--framelog f] [--pal] [--progressive] */
		const char *raw = argv[2];
		for (int i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--frames") && i + 1 < argc)
				g_frames_path = argv[++i];
			else if (!strcmp(argv[i], "--framelog") && i + 1 < argc)
				g_framelog = argv[++i];
			else if (!strcmp(argv[i], "--audio") && i + 1 < argc)
				g_audio_path = argv[++i];
			else if (!strcmp(argv[i], "--pal"))
				g_pal = 1;
			else if (!strcmp(argv[i], "--progressive") || !strcmp(argv[i], "--240p"))
				g_progressive = 1;
		}
		return cmd_decode(raw);
	} else if (!strcmp(argv[1], "bulk")) {
		int ep = (argc > 2) ? (int)strtol(argv[2], NULL, 0) : EP_VIDEO;
		int a  = (argc > 3) ? atoi(argv[3]) : 0;
		signal(SIGINT, on_sigint);
		if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
		rc = bulk_probe(ctx, ep, a);
		libusb_exit(ctx);
		return rc;
	} else {
		fprintf(stderr, "unknown command\n");
		return 64;
	}

	signal(SIGINT, on_sigint);
	if (libusb_init(&ctx) != 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }
	rc = run(ctx, seconds, alt, path);
	libusb_exit(ctx);
	return rc;
}
