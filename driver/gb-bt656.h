/* SPDX-License-Identifier: GPL-2.0 */
/* The GameBridge's BT.656 frame assembler.
 *
 * This header and gb-bt656.c are compiled UNMODIFIED into two things: the
 * userspace capture tool (tools/gbcap.c) and the kernel module. That is
 * deliberate and it is the whole point of the file existing separately.
 *
 * The device is single-user, irreplaceable, and driven by a human being, so
 * hardware time is the scarcest resource in this project. The assembler is also
 * the only part of the driver with real algorithmic content and the only part
 * that has already been through four wrong versions. Keeping it in one
 * kernel-agnostic translation unit means every change to it can be replayed
 * against a saved capture in a second, with no device attached, under valgrind,
 * and without risking an oops in softirq context:
 *
 *     ./tools/gbcap decode captures/samples/bt656-osd-slice.raw --frames out.uyvy
 *
 * and checked against tools/bt656-decode.py, an independent implementation, for
 * byte-identical output. Two implementations agreeing is a far stronger check
 * than either one looking right.
 *
 * DEPENDENCIES ARE DELIBERATELY ALMOST NOTHING: memset and memcpy. No stdio, no
 * allocation, no floating point, no libc beyond <string.h>. The caller supplies
 * the frame buffer -- in userspace a calloc'd block, in the kernel a videobuf2
 * plane -- and receives completed frames and audio through callbacks.
 */
#ifndef GB_BT656_H
#define GB_BT656_H

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define GB_WIDTH        720
#define GB_LINE_BYTES   (GB_WIDTH * 2)          /* 4:2:2, two bytes per pixel */
#define GB_MAX_FIELD    288                     /* 625/50; 525/60 uses 240    */
#define GB_MAX_HEIGHT   (GB_MAX_FIELD * 2)
#define GB_MAX_FRAME    (GB_MAX_HEIGHT * GB_LINE_BYTES)

struct gb_bt656;

struct gb_bt656_ops {
	/* A frame boundary was reached. `accepted` is zero when the assembler's
	 * own guard rejected the frame as not being built from two plausible
	 * fields; the caller should still see it, because the rejection itself is
	 * the useful diagnostic. The pixels are in s->frame either way.
	 *
	 * Called from the URB completion handler in the kernel, so it must not
	 * sleep and must not block. */
	void (*frame_done)(void *ctx, struct gb_bt656 *s, int accepted);

	/* One ancillary block's worth of audio, already de-interleaved into
	 * ordinary interleaved stereo: 16-bit big-endian, 48 kHz, L then R.
	 * Same context rules as above. May be NULL. */
	void (*audio)(void *ctx, const unsigned char *pcm, int bytes);
};

struct gb_bt656 {
	/* ---- configuration, set by gb_bt656_init() ---- */
	int field_lines;             /* 240 on 525/60, 288 on 625/50           */
	int height;                  /* field_lines, doubled unless progressive */
	int progressive;             /* source repeats one field (240p)         */
	int pal;                     /* selects the interlace sign; see .c      */
	unsigned char *frame;        /* caller-owned, height * GB_LINE_BYTES    */
	const struct gb_bt656_ops *ops;
	void *ctx;

	/* ---- scanner state ----
	 * All of it lives here rather than on a stack because a 1440-byte line
	 * against 3030-byte packets never aligns: lines straddle packets and URBs,
	 * so the parser must be resumable at any byte. */
	int match;                   /* bytes of FF 00 00 matched so far        */
	int copying;                 /* bytes still to copy for this line       */
	int row;                     /* destination row within the frame        */
	int col;                     /* write position within the row           */
	int line_of;                 /* next line index within the current field */
	int cur_field;               /* field parity being filled now           */
	int started;                 /* first boundary has been seen            */
	int in_vblank;               /* previous timing code had V set          */

	/* ---- ancillary-data sub-state ---- */
	int anc;
	int anc_skip;                /* payload bytes left to skip              */
	int anc_keep;                /* payload bytes still wanted for audio    */
	int anc_did;
	unsigned char anc_pcm[64];   /* one block, before de-interleaving       */
	int anc_pcm_len;
	unsigned char pcm_out[64];   /* the same block, de-interleaved          */

	/* ---- per-frame accounting, read by the caller's diagnostics ---- */
	unsigned char rowmap[GB_MAX_HEIGHT / 8];  /* which rows this frame got  */
	int runs[4];                 /* lengths of the last four field runs     */
	int cur_run;                 /* lines in the field being filled now     */
	int lines;                   /* lines written into the current frame    */
	unsigned attempts;           /* frame boundaries seen                   */
	unsigned short_frames;       /* boundaries the guard rejected           */
};

/* `frame` must be at least field_lines * (progressive ? 1 : 2) * GB_LINE_BYTES
 * bytes. init() clears the state and blanks the frame. */
void gb_bt656_init(struct gb_bt656 *s, unsigned char *frame, int field_lines,
                   int progressive, int pal,
                   const struct gb_bt656_ops *ops, void *ctx);

/* Fill the frame with real black. NOT memset(0) -- see the comment in the .c. */
void gb_bt656_blank(struct gb_bt656 *s);

/* Feed one isochronous packet, or any other run of stream bytes. */
void gb_bt656_feed(struct gb_bt656 *s, const unsigned char *p, int n);

/* Force a frame boundary now, running the same accept/reject guard. For the end
 * of a capture, or stop_streaming(), where the frame in hand will otherwise
 * never be completed by an incoming F transition. */
void gb_bt656_flush(struct gb_bt656 *s);

#endif /* GB_BT656_H */
