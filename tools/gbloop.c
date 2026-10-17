// SPDX-License-Identifier: GPL-2.0
/*
 * gbloop - feed decoded GameBridge frames into a v4l2loopback output node.
 *
 * Stage 2 of the driver plan (docs/internals/README.md): make the AVC-1410
 * usable by ordinary applications -- browsers, OBS, mpv, anything that opens a
 * /dev/videoN capture node -- while the wire format is still being refined and
 * no kernel module exists yet.
 *
 * gbcap already does the hard part: isochronous capture on EP 0x82 and BT.656
 * line assembly into whole UYVY frames. It writes those frames to stdout as a
 * bare byte stream with no container and no framing, which is exactly what
 * v4l2loopback's write() interface wants, so the whole of this program is:
 * open the loopback node, agree a format with VIDIOC_S_FMT, then copy frames
 * from stdin to the device one write() per frame.
 *
 *     sudo ./gbcap session <script> 3600 --progressive --frames - \
 *       | ./gbloop --width 720 --height 240 /dev/video9
 *
 * One write() call is one frame.
 * v4l2loopback's write handler treats each call as a complete buffer: it clamps
 * count to the negotiated frame size, copies, and immediately marks the buffer
 * ready for consumers. A frame split across two write() calls therefore becomes
 * two frames, the first torn and the second mostly stale. So a partial write is
 * counted and the remainder is DISCARDED rather than resumed -- resuming would
 * turn one glitched frame into a permanent half-frame offset for the rest of
 * the session. See write_frame() below.
 *
 * NOT TESTED END TO END. This program has never fed a real loopback node in
 * this project: the capture device is single-user and driven by the operator,
 * and loading v4l2loopback needs root.
 * It compiles clean under -Wall -Wextra -std=c11 and
 * the ioctl sequence follows the V4L2 output-device contract as documented.
 *
 * Build:  make
 * Usage:  ./gbloop [options] /dev/videoN
 */

#define _POSIX_C_SOURCE 200809L   /* clock_gettime, sigaction under -std=c11 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/videodev2.h>

#define DEF_WIDTH   720
#define DEF_FORMAT  "UYVY"

static volatile sig_atomic_t stop_now;

static void on_signal(int sig) { (void)sig; stop_now = 1; }

/* ------------------------------------------------------------------ util --- */

/* ioctls are interruptible; a signal arriving mid-ioctl is not a failure. */
static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR && !stop_now);
	return r;
}

static const char *fourcc_str(unsigned f, char out[5])
{
	out[0] = (char)( f        & 0xff);
	out[1] = (char)((f >>  8) & 0xff);
	out[2] = (char)((f >> 16) & 0xff);
	out[3] = (char)((f >> 24) & 0x7f);   /* bit 31 is the "big-endian" flag */
	out[4] = 0;
	return out;
}

/* Accept a 1..4 character fourcc, case-insensitively, space-padded like V4L2
 * does ("RGB3" and "GREY" are already 4). YUY2 is the Windows/DirectShow name
 * for what V4L2 calls YUYV and turns up in the Adaptec documentation, so it is
 * translated rather than rejected. */
static unsigned parse_fourcc(const char *s)
{
	char f[4] = { ' ', ' ', ' ', ' ' };
	size_t n = strlen(s);
	if (n == 0 || n > 4) return 0;
	for (size_t i = 0; i < n; i++) f[i] = (char)toupper((unsigned char)s[i]);
	if (!memcmp(f, "YUY2", 4)) return V4L2_PIX_FMT_YUYV;
	return v4l2_fourcc(f[0], f[1], f[2], f[3]);
}

/* Bytes per pixel for the packed formats gbcap can actually emit. 0 means
 * "unknown to this program", in which case the driver's sizeimage is trusted
 * instead of being cross-checked. */
static unsigned bytes_per_pixel(unsigned fourcc)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_UYVY:
	case V4L2_PIX_FMT_YUYV:
	case V4L2_PIX_FMT_VYUY:
	case V4L2_PIX_FMT_YVYU:
	case V4L2_PIX_FMT_RGB565: return 2;
	case V4L2_PIX_FMT_BGR24:
	case V4L2_PIX_FMT_RGB24:  return 3;
	case V4L2_PIX_FMT_BGR32:
	case V4L2_PIX_FMT_RGB32:  return 4;
	case V4L2_PIX_FMT_GREY:   return 1;
	default:                  return 0;
	}
}

static int parse_field(const char *s, enum v4l2_field *out)
{
	if (!strcmp(s, "none"))         *out = V4L2_FIELD_NONE;
	else if (!strcmp(s, "interlaced")) *out = V4L2_FIELD_INTERLACED;
	else if (!strcmp(s, "tb"))      *out = V4L2_FIELD_INTERLACED_TB;
	else if (!strcmp(s, "bt"))      *out = V4L2_FIELD_INTERLACED_BT;
	else if (!strcmp(s, "top"))     *out = V4L2_FIELD_TOP;
	else if (!strcmp(s, "bottom"))  *out = V4L2_FIELD_BOTTOM;
	else return -1;
	return 0;
}

static const char *field_str(unsigned f)
{
	switch (f) {
	case V4L2_FIELD_NONE:          return "none";
	case V4L2_FIELD_TOP:           return "top";
	case V4L2_FIELD_BOTTOM:        return "bottom";
	case V4L2_FIELD_INTERLACED:    return "interlaced";
	case V4L2_FIELD_INTERLACED_TB: return "interlaced-tb";
	case V4L2_FIELD_INTERLACED_BT: return "interlaced-bt";
	case V4L2_FIELD_ANY:           return "any";
	default:                       return "?";
	}
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- stream --- */

/* Read exactly `want` bytes unless EOF or a signal intervenes. Returns the
 * number of bytes actually read, or -1 on a real error. A short read is not an
 * error in itself: gbcap's stdout is a pipe and the kernel hands over whatever
 * is in it, so a 691200-byte frame routinely arrives in 64 KB instalments. */
static ssize_t read_full(int fd, unsigned char *buf, size_t want)
{
	size_t got = 0;
	while (got < want) {
		ssize_t n = read(fd, buf + got, want - got);
		if (n == 0) break;                       /* clean EOF */
		if (n < 0) {
			if (errno == EINTR) {
				if (stop_now) break;
				continue;
			}
			return -1;
		}
		got += (size_t)n;
	}
	return (ssize_t)got;
}

/* Write one frame. Returns 0 written whole, 1 short (torn), -1 error/aborted.
 *
 * The remainder of a short write is deliberately dropped -- see the file header.
 * A device with stream rather than frame semantics would want a resume loop
 * here instead, but v4l2loopback is not that device. */
static int write_frame(int fd, const unsigned char *buf, size_t len, size_t *wrote)
{
	for (;;) {
		ssize_t n = write(fd, buf, len);
		if (n < 0) {
			if (errno == EINTR) {
				if (stop_now) return -1;
				continue;                        /* not a failure, retry */
			}
			return -1;
		}
		*wrote = (size_t)n;
		return ((size_t)n == len) ? 0 : 1;
	}
}

/* ------------------------------------------------------------------ main --- */

static void usage(void)
{
	fprintf(stderr,
"gbloop - pipe raw UYVY frames into a v4l2loopback output node\n"
"\n"
"usage: gbloop [options] /dev/videoN\n"
"\n"
"  --width  N     frame width in pixels        (default %d)\n"
"  --height N     frame height in pixels       (default 480; 240 for a 240p source)\n"
"  --format CC    fourcc of the input frames   (default %s)\n"
"  --field  F     none|interlaced|tb|bt|top|bottom (default none)\n"
"  --fps    N     declared frame rate; default 60 at <=288 lines, else 30\n"
"  --input  PATH  read frames from PATH instead of stdin ('-' is stdin)\n"
"  --quiet        no periodic progress line on stderr\n"
"  --help\n"
"\n"
"The frame size must match what the producer emits exactly; there is no framing\n"
"in the stream to resynchronise from, so a wrong --height shears the picture\n"
"permanently rather than failing.\n"
"\n"
"example:\n"
"  sudo ./gbcap session ../captures/full-session-replay.txt 3600 \\\n"
"       --progressive --frames - | ./gbloop --height 240 /dev/video9\n",
	        DEF_WIDTH, DEF_FORMAT);
}

int main(int argc, char **argv)
{
	const char *dev_path = NULL, *in_path = "-";
	unsigned width = DEF_WIDTH, height = 480, fps = 0;
	unsigned fourcc = parse_fourcc(DEF_FORMAT);
	enum v4l2_field field = V4L2_FIELD_NONE;
	int quiet = 0, rc = 1;
	int fd = -1, in_fd = -1;
	unsigned char *frame = NULL;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int has_next = (i + 1 < argc);

		if (!strcmp(a, "--help") || !strcmp(a, "-?")) { usage(); return 0; }
		else if ((!strcmp(a, "--width")  || !strcmp(a, "-w")) && has_next)
			width = (unsigned)strtoul(argv[++i], NULL, 0);
		else if ((!strcmp(a, "--height") || !strcmp(a, "-H")) && has_next)
			height = (unsigned)strtoul(argv[++i], NULL, 0);
		else if ((!strcmp(a, "--format") || !strcmp(a, "-f")) && has_next) {
			fourcc = parse_fourcc(argv[++i]);
			if (!fourcc) { fprintf(stderr, "gbloop: bad fourcc '%s'\n", argv[i]); return 64; }
		}
		else if (!strcmp(a, "--field") && has_next) {
			if (parse_field(argv[++i], &field)) {
				fprintf(stderr, "gbloop: bad --field '%s'\n", argv[i]);
				return 64;
			}
		}
		else if (!strcmp(a, "--fps") && has_next)
			fps = (unsigned)strtoul(argv[++i], NULL, 0);
		else if ((!strcmp(a, "--input") || !strcmp(a, "-i")) && has_next)
			in_path = argv[++i];
		else if (!strcmp(a, "--quiet") || !strcmp(a, "-q"))
			quiet = 1;
		else if (a[0] == '-' && a[1] && strcmp(a, "-")) {
			fprintf(stderr, "gbloop: unknown option '%s'\n", a);
			return 64;
		}
		else if (!dev_path)
			dev_path = a;
		else {
			fprintf(stderr, "gbloop: unexpected argument '%s'\n", a);
			return 64;
		}
	}

	if (!dev_path) { usage(); return 64; }
	if (!width || !height) {
		fprintf(stderr, "gbloop: width and height must be non-zero\n");
		return 64;
	}
	/* 240p sources give 60 whole frames a second, 480i gives 30; gbcap's
	 * --progressive switch is the same distinction on the other end. */
	if (!fps) fps = (height <= 288) ? 60 : 30;

	/* SIGINT/SIGTERM installed WITHOUT SA_RESTART on purpose: the point is for
	 * the blocking read() to return EINTR so the loop notices stop_now and
	 * shuts down. signal() would give BSD semantics (SA_RESTART set), the read
	 * would silently restart, and ^C would appear to do nothing until the next
	 * frame arrived -- which, with no video present, is never. */
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* ---- input ---- */
	if (!strcmp(in_path, "-")) {
		in_fd = STDIN_FILENO;
	} else {
		in_fd = open(in_path, O_RDONLY);
		if (in_fd < 0) { perror(in_path); return 1; }
	}

	/* ---- the loopback node ----
	 * O_RDWR rather than O_WRONLY: v4l2loopback wants a read/write handle for
	 * the producer side, and with exclusive_caps=1 it is the open itself that
	 * flips the node into capture-only mode for everyone else. */
	fd = open(dev_path, O_RDWR);
	if (fd < 0) { perror(dev_path); goto out; }

	struct v4l2_capability cap;
	memset(&cap, 0, sizeof cap);
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		if (errno == ENOTTY)
			fprintf(stderr, "gbloop: %s is not a V4L2 device\n", dev_path);
		else
			perror("VIDIOC_QUERYCAP");
		goto out;
	}
	unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
	              ? cap.device_caps : cap.capabilities;
	/* A warning, not a refusal. Under exclusive_caps=1 the advertised caps of a
	 * loopback node change depending on who has it open, so a missing
	 * VIDEO_OUTPUT bit here is suggestive rather than conclusive; S_FMT below
	 * is the real test. */
	if (!(caps & V4L2_CAP_VIDEO_OUTPUT))
		fprintf(stderr,
		        "gbloop: warning: %s (%s) does not advertise VIDEO_OUTPUT "
		        "(caps 0x%08x) -- wrong node, or already claimed?\n",
		        dev_path, cap.card, caps);

	/* ---- format ---- */
	unsigned bpp = bytes_per_pixel(fourcc);
	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width        = width;
	fmt.fmt.pix.height       = height;
	fmt.fmt.pix.pixelformat  = fourcc;
	fmt.fmt.pix.field        = field;
	fmt.fmt.pix.bytesperline = bpp ? width * bpp : 0;
	fmt.fmt.pix.sizeimage    = bpp ? width * height * bpp : 0;
	/* SMPTE 170M is the colorimetry of 525-line analogue NTSC, which is what
	 * the CX25840 is digitising. Consumers mostly ignore it, but declaring
	 * BT.709 here would be a lie that a colour-managed one would act on. */
	fmt.fmt.pix.colorspace   = V4L2_COLORSPACE_SMPTE170M;

	char cc[5], got_cc[5];
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		if (errno == EBUSY)
			fprintf(stderr,
			        "gbloop: the node is busy -- a consumer that already "
			        "negotiated a format pins it; close it and retry.\n");
		goto out;
	}

	/* S_FMT is allowed to return something other than what was asked for, and
	 * silently feeding UYVY bytes into a node that has decided it is something
	 * else produces a picture that is wrong in a way nobody can debug from the
	 * far end. Refuse instead. */
	if (fmt.fmt.pix.pixelformat != fourcc ||
	    fmt.fmt.pix.width  != width ||
	    fmt.fmt.pix.height != height) {
		fprintf(stderr,
		        "gbloop: driver refused the format: asked %ux%u %s, got %ux%u %s\n",
		        width, height, fourcc_str(fourcc, cc),
		        fmt.fmt.pix.width, fmt.fmt.pix.height,
		        fourcc_str(fmt.fmt.pix.pixelformat, got_cc));
		fprintf(stderr,
		        "gbloop: v4l2loopback restricts geometry when it is loaded with "
		        "max_width/max_height set, and pins the format once a consumer "
		        "has negotiated one.\n");
		goto out;
	}
	if (fmt.fmt.pix.field != (unsigned)field)
		fprintf(stderr, "gbloop: note: asked field '%s', driver chose '%s'\n",
		        field_str(field), field_str(fmt.fmt.pix.field));

	/* The driver's sizeimage is authoritative -- it, not this program, decides
	 * how many bytes constitute one buffer, and a mismatch here is exactly the
	 * shearing failure the usage text warns about. */
	size_t frame_size = fmt.fmt.pix.sizeimage;
	if (bpp && frame_size != (size_t)width * height * bpp)
		fprintf(stderr,
		        "gbloop: note: driver sizeimage %zu != %ux%u*%u = %zu; "
		        "using the driver's value\n",
		        frame_size, width, height, bpp, (size_t)width * height * bpp);
	if (frame_size == 0) {
		fprintf(stderr, "gbloop: driver reports a zero frame size, giving up\n");
		goto out;
	}

	/* Declared frame rate. Nothing enforces it: frames are written at whatever
	 * rate the producer supplies them, and this only sets what a consumer sees
	 * from G_PARM. Non-fatal if the driver has no opinion. */
	struct v4l2_streamparm parm;
	memset(&parm, 0, sizeof parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	parm.parm.output.capability   = V4L2_CAP_TIMEPERFRAME;
	parm.parm.output.timeperframe.numerator   = 1;
	parm.parm.output.timeperframe.denominator = fps;
	if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
		fprintf(stderr, "gbloop: note: VIDIOC_S_PARM failed (%s); "
		                "frame rate left at the driver default\n", strerror(errno));

	frame = malloc(frame_size);
	if (!frame) { fprintf(stderr, "gbloop: out of memory\n"); goto out; }

	fprintf(stderr, "gbloop: %s [%s] %ux%u %s %s, %u fps, %zu bytes/frame\n",
	        dev_path, cap.card, width, height,
	        fourcc_str(fourcc, cc), field_str(fmt.fmt.pix.field), fps, frame_size);
	fprintf(stderr, "gbloop: reading frames from %s\n",
	        in_fd == STDIN_FILENO ? "stdin" : in_path);

	/* ---- the copy loop ---- */
	unsigned long frames = 0, torn = 0;
	unsigned long long bytes = 0;
	double t0 = now_s(), t_last = t0;
	unsigned long f_last = 0;

	while (!stop_now) {
		ssize_t got = read_full(in_fd, frame, frame_size);
		if (got < 0) { perror("read"); goto out_stats; }
		if (got == 0) break;                     /* producer finished */
		if ((size_t)got < frame_size) {
			/* A truncated tail means the producer was killed mid-frame.
			 * Publishing it would show half a frame of video over half a frame
			 * of whatever was in the buffer before. */
			fprintf(stderr,
			        "gbloop: short read at end of stream (%zd of %zu bytes), "
			        "discarding partial frame\n", got, frame_size);
			break;
		}

		size_t wrote = 0;
		int w = write_frame(fd, frame, frame_size, &wrote);
		if (w < 0) {
			if (stop_now) break;
			perror("write");
			goto out_stats;
		}
		if (w > 0) {
			torn++;
			if (torn <= 5)
				fprintf(stderr, "gbloop: short write (%zu of %zu bytes), "
				                "frame %lu is torn\n", wrote, frame_size, frames);
		}
		frames++;
		bytes += wrote;

		if (!quiet) {
			double t = now_s();
			if (t - t_last >= 5.0) {
				fprintf(stderr, "gbloop: %lu frames, %.1f fps, %.1f MB\n",
				        frames, (frames - f_last) / (t - t_last),
				        bytes / 1048576.0);
				t_last = t;
				f_last = frames;
			}
		}
	}
	rc = 0;

out_stats:
	{
		double dt = now_s() - t0;
		fprintf(stderr,
		        "gbloop: %lu frames in %.1f s (%.2f fps avg), %.1f MB, %lu torn%s\n",
		        frames, dt, dt > 0 ? frames / dt : 0.0,
		        bytes / 1048576.0, torn, stop_now ? ", stopped by signal" : "");
	}

out:
	free(frame);
	if (fd >= 0) close(fd);
	if (in_fd >= 0 && in_fd != STDIN_FILENO) close(in_fd);
	return rc;
}
