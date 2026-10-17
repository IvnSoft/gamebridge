// SPDX-License-Identifier: GPL-2.0
/* Streaming: the videobuf2 queue and the isochronous URB ring.
 *
 * The frame assembler this feeds is
 * driver/gb-bt656.c -- the same source tools/gbcap.c compiles, already verified
 * byte-identical against a saved capture and against an independent Python
 * implementation. Nothing in this file parses the stream; it moves bytes from
 * URBs into the assembler and completed frames out to userspace.
 *
 * The bytes go STRAIGHT into the vb2 buffer that the application has mmap'd.
 * There is no intermediate frame buffer and no copy: gb->bt.frame points at the
 * current buffer's virtual address, and the assembler writes pixels there as it
 * finds them.
 */
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <media/videobuf2-vmalloc.h>

#include "gamebridge.h"

/* Diagnostic: hand userspace the RAW isochronous payload instead of assembled
 * frames, so a kernel-side stream can be analysed with exactly the tools that
 * settled every previous wire-format question --
 *
 *     v4l2-ctl -d /dev/videoN --stream-mmap --stream-to=raw.bin
 *     ./tools/gbcap decode raw.bin --frames out.uyvy
 *
 * Replay beats hardware for iteration, and this makes the kernel's stream
 * replayable in the same way gbcap's dumps already are.
 *
 * CAVEAT, and it matters: this dump has HOLES. Packets arriving while no vb2
 * buffer is ready are discarded, so the file is not a contiguous capture. It
 * briefly produced a confident and wrong conclusion about the decoder's line
 * structure that was purely an artefact of those gaps. Use it for "what do the
 * bytes look like", never for "how far apart are the timing codes". */
static bool zero_buf;
module_param(zero_buf, bool, 0644);
MODULE_PARM_DESC(zero_buf, "scrub URB buffers before resubmit (diagnostic)");

static bool legacy_window;
module_param(legacy_window, bool, 0644);
MODULE_PARM_DESC(legacy_window,
		 "reproduce Adaptec's captured active window instead of the "
		 "subdev's computed one (diagnostic)");

static bool raw_dump;
module_param(raw_dump, bool, 0644);
MODULE_PARM_DESC(raw_dump,
		 "deliver raw isochronous bytes instead of assembled frames");

/* Point the assembler at the current buffer, or at the scratch area when
 * userspace has not given us one. It must always have somewhere valid to write:
 * the stream does not pause because nobody is listening.
 *
 * CALLER HOLDS buf_lock, AND THAT IS THE WHOLE POINT OF THIS FUNCTION.
 *
 * cur_buf and bt.frame are ONE piece of state: the second is derived from the
 * first and means nothing without it. Sampling cur_buf here and storing
 * bt.frame a moment later -- which is what this code used to do, unlocked --
 * leaves a window for gb_return_buffers() to hand that very buffer back to
 * userspace and park the assembler on the scratch, after which this store
 * silently re-points the assembler at the returned buffer. That is the KASAN
 * report quoted in gb_return_buffers() below, and it is reachable because the
 * stream is REFCOUNTED: an audio user keeps the URB ring live across video
 * STREAMOFF, so the completion handler and gb_return_buffers() really do run at
 * the same time on different CPUs.
 *
 * INVARIANT, and every store to bt.frame in this file obeys it: bt.frame is
 * written only with buf_lock held, and only from the cur_buf read in the same
 * critical section. gb_return_buffers() then wins by construction -- a publish
 * either precedes its critical section, in which case its park overwrites ours,
 * or follows it, in which case cur_buf is NULL and we park too.
 *
 * THAT INVARIANT IS ABOUT STORES TO bt.frame, AND IT IS NOT THE WHOLE STORY.
 * It says nothing about writes THROUGH the pointer, which is the hazard
 * gb_return_buffers() handles by waiting an RCU grace period before it hands
 * anything back; see the comment there.
 *
 * This function does not blank, and that is a change from gb_bt656_retarget():
 * the retarget-time blank is gone, which halves the blanking this driver does
 * in softirq. It does NOT mean there is no blanking in the handler.
 * gb_bt656_feed() still calls gb_bt656_blank() at every frame boundary
 * (gb-bt656.c:279, immediately after bt656_flush()), in softirq, 691200 bytes
 * a frame. That one predates round 1 and is still there. */
/* Does this buffer hold a whole frame at the geometry the ASSEMBLER latched?
 *
 * The assembler writes s->height rows into whatever bt.frame points at, and
 * s->height was fixed by gb_bt656_init() at stream-up, not by the geometry the
 * buffer was sized for. gb_s_std() keeps the two equal; this is the backstop
 * for the day it does not, so a mismatch costs a frame instead of a write past
 * a vb2 plane. */
static bool gb_buf_fits(struct gb_dev *gb, struct gb_buf *buf)
{
	return vb2_plane_size(&buf->vb.vb2_buf, 0) >=
	       (unsigned long)gb->bt.height * GB_LINE_BYTES;
}

static void gb_bt656_publish(struct gb_dev *gb)
{
	lockdep_assert_held(&gb->buf_lock);

	/* Too small for the assembler: park it on the scratch instead, and let
	 * gb_frame_done() hand the buffer back with an error. */
	if (gb->cur_buf && !gb_buf_fits(gb, gb->cur_buf)) {
		/* Not WARN: a backstop must not become a panic_on_warn crash. */
		dev_warn_once(&gb->intf->dev,
			      "buffer smaller than the frame; dropping it\n");
		gb->bt.frame = gb->scratch;
		return;
	}

	if (gb->cur_buf) {
		gb->bt.frame = vb2_plane_vaddr(&gb->cur_buf->vb.vb2_buf, 0);
		return;
	}

	/* No consumer: park the assembler on the scratch area and DO NOT blank it.
	 *
	 * Blanking is 691200 bytes, and a frame boundary happens inside the URB
	 * completion handler. With audio capturing and no video client -- which is
	 * the normal state once PipeWire adopts the ALSA card -- blanking there is
	 * ~20 MB/s of memset in softirq, on a buffer nobody will ever read, on top
	 * of scanning 24 MB/s of stream byte by byte. It was enough to make the
	 * completion handler overrun its isochronous ring:
	 *
	 *     xhci_hcd: WARN: buffer overrun event for slot 8 ep 4 on endpoint
	 *
	 * The scratch exists solely to give the assembler somewhere valid to write
	 * so it keeps its sync while nobody is listening. Its contents are never
	 * looked at. */
	gb->bt.frame = gb->scratch;
}

/* Take the next queued buffer, or NULL, and publish it. CALLER HOLDS buf_lock. */
static void gb_next_buffer_locked(struct gb_dev *gb)
{
	lockdep_assert_held(&gb->buf_lock);

	if (list_empty(&gb->bufs)) {
		gb->cur_buf = NULL;
	} else {
		gb->cur_buf = list_first_entry(&gb->bufs, struct gb_buf, list);
		list_del(&gb->cur_buf->list);
		/* A newly adopted buffer starts at the front of its plane, on
		 * every path. raw_off used to be reset only on completion in the
		 * raw_dump loop, in gb_return_buffers_locked() and in
		 * gb_stream_up(), which missed gb_frame_done() clearing cur_buf.
		 * raw_dump is writable at runtime (0644), so toggling it on, off
		 * and on again within one streaming session left the next dump
		 * starting at a stale offset. Here it cannot be missed. */
		gb->raw_off = 0;
	}
	gb_bt656_publish(gb);
}

/* The assembler reached a frame boundary. Interrupt context. */
static void gb_frame_done(void *ctx, struct gb_bt656 *s, int accepted)
{
	struct gb_dev *gb = ctx;
	struct gb_buf *buf;
	unsigned long flags;

	/* The caller is gb_isoc_complete()'s RCU read-side section, and the
	 * video stop fence depends on it: see gb_return_buffers_locked().
	 * Compiled out unless CONFIG_PROVE_RCU, which the debug-kernel test rigs have. */
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "gb_frame_done() outside gb_isoc_complete()'s RCU section");

	/* One critical section for the whole hand-off: find the buffer, complete
	 * it, release it and publish its successor. Reading cur_buf unlocked and
	 * completing it afterwards let gb_return_buffers() complete the same
	 * buffer in between, so vb2_buffer_done() ran TWICE on it. Taking the lock
	 * means exactly one of the two sees it.
	 *
	 * Everything in here is O(1) and irq-safe -- vb2_buffer_done() is a list
	 * add under vb2's own done_lock and a wake-up. (gb_return_buffers() no
	 * longer calls it under this lock: it detaches its buffers under buf_lock
	 * and completes them after a grace period, with no SPINLOCK of ours held
	 * -- the caller still holds stream_lock and vb_lock.
	 * Exactly-once still holds, because the detach is under buf_lock.)
	 *
	 * The 691200-byte blank must never be inside THIS critical section, and it
	 * is not. It has not left the path, though, and an earlier version of this
	 * comment wrongly said it had: bt656_flush() returns into gb_bt656_feed(),
	 * which blanks the newly published target immediately (gb-bt656.c:279) --
	 * in softirq, outside this lock, once per frame. Round 1 removed the
	 * SECOND, retarget-time blank, so this path blanks once per frame instead
	 * of twice. Not zero. */
	spin_lock_irqsave(&gb->buf_lock, flags);
	buf = gb->cur_buf;
	if (buf && !gb_buf_fits(gb, buf)) {
		/* gb_bt656_publish() never let the assembler write into it. */
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		gb->cur_buf = NULL;
	} else if (accepted && buf) {
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0,
				      (unsigned long)s->height * GB_LINE_BYTES);
		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.sequence = gb->sequence++;
		buf->vb.field    = V4L2_FIELD_INTERLACED;
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		gb->cur_buf = NULL;
	} else if (!buf) {
		gb->no_buffer++;
		/* A whole frame arrived and there was nowhere to put it. It still
		 * takes a sequence number: V4L2 defines sequence as a frame count
		 * so that an application can see drops as gaps. This used to skip
		 * the increment, and on the rig (2026-10-11) a consumer taking 50
		 * ms per frame saw 20 fps with "0 dropped" -- the drops were
		 * invisible to everything but this driver's own stream-down line. */
		if (accepted)
			gb->sequence++;
	}

	/* Keep parsing either way. Do NOT stall the assembler waiting for a
	 * consumer and do not stop resubmitting URBs: video capture wants the
	 * newest frame, not every frame, and a stalled assembler loses sync rather
	 * than merely losing a frame.
	 *
	 * With no current buffer, TRY TO ADOPT ONE -- every frame boundary, not
	 * only after completing a frame. This used to happen only in the branch
	 * above, so once cur_buf went NULL (one late re-queue, which at
	 * REQBUFS count=2 is routine) nothing ever took a buffer off gb->bufs
	 * again: every later QBUF sat there unconsumed, DQBUF blocked forever, and
	 * only STREAMOFF/STREAMON recovered. It predates the 2026-10-10 review and
	 * was not a race; it was unconditional.
	 *
	 * Adopting here is boundary-aligned, so it is NOT another mid-field
	 * publication: in the kernel this function is reached only from
	 * bt656_flush() at gb-bt656.c:278, which is followed immediately by the
	 * blank at :279 and s->lines = 0 at :280. (gb_bt656_flush() also reaches
	 * it, but only tools/gbcap.c calls that.)
	 *
	 * Gated on video_streaming because the ring outlives video: with audio
	 * holding it, this runs while vb2 is between buf_queue and
	 * ->start_streaming, and a buffer must not be completed on a queue vb2
	 * has not started.
	 *
	 * Otherwise re-publish rather than leave bt.frame alone: it is normally
	 * already right, but if gb_return_buffers() parked the assembler while
	 * this frame was being built, this is where the handler notices. */
	if (!gb->cur_buf && gb->video_streaming)
		gb_next_buffer_locked(gb);
	else
		gb_bt656_publish(gb);
	spin_unlock_irqrestore(&gb->buf_lock, flags);
}

static void gb_audio_block(void *ctx, const unsigned char *pcm, int bytes)
{
	struct gb_dev *gb = ctx;

	gb->audio_calls++;
	gb->audio_bytes += bytes;
	gb_alsa_write(gb, pcm, bytes);
}

static const struct gb_bt656_ops gb_bt656_ops = {
	.frame_done = gb_frame_done,
	.audio      = gb_audio_block,
};

/* Board-specific decoder configuration the subdev does not do.
 *
 * mainline's cx25840 driver configures the chip for its own generic
 * expectations. Everything board-specific in it -- the video output format, the
 * I2S wiring, the audio path -- sits inside `if (is_cx2388x(state))` or
 * `else if (is_cx231xx(state))`, and this is a PLAIN CX25840, which is neither.
 * So the subdev leaves these registers at their reset values and the bridge owns
 * them. That is a clean division rather than a conflict, and it is the same
 * division already found for AFE_CTRL.
 *
 * These four values are not guesses. They are the difference, measured register
 * by register, between the configuration the Windows session produces (which
 * streams correctly) and the one the subdev alone produces (which does not):
 *
 *     0x0404  Video Out Control 1          0x31 -> 0x36
 *     0x0405  Video Out Control 2          0x25 -> 0x29
 *     0x0407  Video Out Control 4          0x00 -> 0x04
 *     0x040b  Ancillary Audio Output Ctrl  0x00 -> 0x0f
 *     0x0104  AFE_CTRL                     connects the audio path
 *
 * Without them the decoder streams UYVY that is not framed the way this bridge
 * expects: measured, 349 MB arrived and the assembler synced, but every field
 * came out 43 lines instead of 240 and not one ancillary block appeared, so
 * there was no audio and not a single complete frame.
 *
 * NOT copied from the session, deliberately: 0x0400 bit 5, ACFG_DIS. Adaptec
 * sets it and writes the active-window registers by hand; leaving it CLEAR lets
 * the chip compute that window from the selected standard, which is what makes
 * PAL correct here rather than merely locked. That is the driver's advantage
 * over the replay and it should not be given away by copying a captured value
 * wholesale. */
static int gb_dec_write(struct gb_dev *gb, u16 reg, const u8 *val, int len)
{
	u8 buf[6];
	struct i2c_msg msg = {
		.addr = GB_I2C_ADDR_DECODER, .flags = 0,
		.len = 2 + len, .buf = buf,
	};

	if (len > 4)
		return -EINVAL;
	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	memcpy(buf + 2, val, len);
	return i2c_transfer(&gb->i2c_adap, &msg, 1) == 1 ? 0 : -EIO;
}

static int gb_apply_board_config(struct gb_dev *gb)
{
	static const u8 vout1[]   = { 0x36 };
	static const u8 vout2[]   = { 0x29 };
	/* Pixel-port pin configuration. THIS is what makes the difference between a
	 * stream that fits the endpoint and one that does not.
	 *
	 * Left to the subdev, the decoder emits a COMPLETE BT.656 line: 1444 bytes
	 * of active video plus 272 of horizontal blanking, 1716 in all, which is
	 * 858 pixels. At 15734 lines/s that is 27.0 MB/s against an endpoint
	 * ceiling of 24.24, so the FX2's FIFO overflows and simply loses lines.
	 * Measured: 218 active lines per field instead of 243, and 24.24/27.0 =
	 * 0.898 against 218/243 = 0.897.
	 *
	 * Adaptec's configuration suppresses the blanking, sending only the 23-byte
	 * ancillary block between EAV and the next SAV -- 1471 bytes per line, which
	 * fits. Measured on a working session: EAV->SAV gaps of 27 bytes, against
	 * 272 here. */
	static const u8 pinctl1[] = { 0x40 };	/* 0x114 Pin Control 1       */
	static const u8 pinctl5[] = { 0x02 };	/* 0x118 Pin Control 5       */
	static const u8 pincfg5[] = { 0x01 };	/* 0x120 Pin Configuration 5 */
	static const u8 vout4[]   = { 0x04 };
	static const u8 anc_aud[] = { 0x0f };
	/* AFE_CTRL, 32-bit at 0x0104, little-endian value 0x0704dc00. Bits 7 and 8
	 * are VGA_SEL_CH2/VGA_SEL_CH3, which choose whether the analogue channels
	 * carry video or audio; left at reset the ADC runs, is clocked, and
	 * digitises nothing. */
	static const u8 afe[]     = { 0x00, 0xdc, 0x04, 0x07 };
	/* Path 1 select, 32-bit at 0x08d0, little-endian 0x01011012. Mainline names
	 * both values this register can take: 0x01011012 is "Set Path1 to Serial
	 * Audio Input" and 0x1f063870 is "Set Path1 to Analog Demod Main Channel".
	 *
	 * The subdev leaves it on the demod value, which on this board is silence:
	 * there is no tuner signal to demodulate, and the audio arrives over I2S
	 * from an AK5357 wired to the line inputs. Forcing the demod value as the
	 * last write was measured at RMS 0.00 -- total silence -- which is what
	 * settled what that chip is on the board for.
	 *
	 * Without this the decoder still emits ancillary blocks, because 0x040b
	 * enables them, but they carry a repeating placeholder: measured, 4376 of
	 * 4376 consecutive payloads byte-identical, against 0 of 3349 on a working
	 * stream. With no audio clock to pace them it emits one per line rather
	 * than one per four sample-frames, which is why the extracted audio
	 * measured 61 kHz against an expected 48. */
	static const u8 path1[]  = { 0x12, 0x10, 0x01, 0x01 };
	static const u8 i2s_in[] = { 0xa0 };	/* 0x914 I2S_IN_CTL  */
	static const u8 i2s_out[] = { 0xa0 };	/* 0x918 I2S_OUT_CTL */
	/* The sample-rate converters, and this is where the RATE lives.
	 *
	 * 0x900/0x904/0x90c are SRC3/4/6 Phase Increment -- the resampling ratio --
	 * and 0x910 is Output SRC Source Select, which decides which converter
	 * feeds the output at all. The session writes the increments TWICE: first
	 * 0x08014faa, then 0x08015555 once the serial path is selected. The subdev
	 * leaves us on the first, so the output is resampled at the wrong ratio and
	 * the ancillary blocks are paced accordingly -- measured at 60 kHz against
	 * an expected 48. */
	static const u8 src3[]   = { 0x55, 0x55, 0x01, 0x08 };	/* 0x900 */
	static const u8 src4[]   = { 0x55, 0x55, 0x01, 0x08 };	/* 0x904 */
	static const u8 src6[]   = { 0x55, 0x55, 0x01, 0x08 };	/* 0x90c */
	static const u8 srcsel[] = { 0xd8, 0x00, 0xb0, 0x12 };	/* 0x910 */
	static const u8 phasefix[] = { 0x00, 0x00, 0x10, 0x00 };	/* 0x8cc */
	/* Adaptec's own active-window constants, used only when legacy_window is
	 * set. Reading them back from a working session: 0x0470 = 7e 00 2d 5b,
	 * 0x0474 = 19 70 1e 1e, 0x0478 = 1f 02 58 66, 0x047c = 1f 7c 08 00. */
	static const u8 win70[] = { 0x7e, 0x00, 0x2d, 0x5b };
	static const u8 win74[] = { 0x19, 0x70, 0x1e, 0x1e };
	static const u8 win78[] = { 0x1f, 0x02, 0x58, 0x66 };
	static const u8 win7c[] = { 0x1f, 0x7c, 0x08, 0x00 };
	static const u8 acfg[]  = { 0x21 };
	int ret;

	ret = gb_dec_write(gb, 0x0114, pinctl1, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x0118, pinctl5, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x0120, pincfg5, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x0404, vout1, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x0405, vout2, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x0407, vout4, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x040b, anc_aud, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x0104, afe, 4);
	if (!ret)
		ret = gb_dec_write(gb, 0x0914, i2s_in, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x0918, i2s_out, 1);
	if (!ret)
		ret = gb_dec_write(gb, 0x08cc, phasefix, 4);
	if (!ret)
		ret = gb_dec_write(gb, 0x0900, src3, 4);
	if (!ret)
		ret = gb_dec_write(gb, 0x0904, src4, 4);
	if (!ret)
		ret = gb_dec_write(gb, 0x090c, src6, 4);
	if (!ret)
		ret = gb_dec_write(gb, 0x0910, srcsel, 4);
	/* Path 1 LAST, because the subdev's own audio setup runs before this and
	 * ends on the demod value. */
	if (!ret)
		ret = gb_dec_write(gb, 0x08d0, path1, 4);

	/* Diagnostic: reproduce Adaptec's active window exactly, ACFG_DIS and all.
	 * The subdev's computed window is the better long-term answer -- it is what
	 * makes PAL correct rather than merely locked -- so this exists to find out
	 * whether the window is the reason fields come out short, not as the fix. */
	if (!ret && legacy_window) {
		ret = gb_dec_write(gb, 0x0470, win70, 4);
		if (!ret) ret = gb_dec_write(gb, 0x0474, win74, 4);
		if (!ret) ret = gb_dec_write(gb, 0x0478, win78, 4);
		if (!ret) ret = gb_dec_write(gb, 0x047c, win7c, 4);
		if (!ret) ret = gb_dec_write(gb, 0x0400, acfg, 1);
	}
	return ret;
}

/* ------------------------------------------------------------------ URBs --- */

static void gb_isoc_complete(struct urb *urb)
{
	struct gb_dev *gb = urb->context;
	int i, ret;

	switch (urb->status) {
	case 0:
	case -EXDEV:		/* partial isochronous transfer -- normal here */
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* Killed. The only kill in this driver is usb_kill_urb() in
		 * gb_free_urbs() -- teardown: the last stream user leaving,
		 * gb_stream_up()'s error paths, disconnect -- so this runs once
		 * per URB per teardown, not on every stop. Returning without
		 * parsing and without resubmitting is what lets that kill end the
		 * ring: a killed URB's partial payload is never fed to the
		 * assembler, and the URB does not put itself back in flight.
		 *
		 * Outside the RCU section below, because it touches nothing the
		 * section protects. */
		return;
	default:
		gb->urb_errors++;
		break;		/* fall through and resubmit anyway */
	}

	/* THE READ SIDE OF BOTH STOP FENCES.
	 *
	 * Every dereference a stop path retires happens inside this section:
	 * gb_bt656_feed()'s per-byte writes through gb->bt.frame
	 * (gb-bt656.c:349), the frame-boundary gb_frame_done() and the blank
	 * through bt.frame right after it (:278 and :279), the raw_dump copy into
	 * cur_buf's plane, and gb_audio_block() -> gb_alsa_write() up to and
	 * including its unlocked snd_pcm_period_elapsed(sub).
	 * gb_return_buffers_locked() and gb_pcm_sync_stop() first publish the
	 * state that makes a NEW handler harmless, then call synchronize_rcu(),
	 * which returns only once every section that began before the call has
	 * ended. That is the whole fence. The ring is not stopped, so unlike the
	 * usb_kill_urb()-and-re-arm fences it replaces, it discards nothing.
	 *
	 * Explicit, although on a conforming HCD it adds nothing:
	 * usb_hcd_giveback_urb() runs this handler from the HCD's hardirq handler
	 * or from the BH workqueue in softirq, both non-preemptible, and
	 * synchronize_rcu() has waited for such regions since the RCU flavour
	 * consolidation (its kerneldoc in kernel/rcu/tree.c says so). What
	 * the explicit section buys is that the guarantee no longer depends on
	 * how giveback is scheduled, and that the RCU_LOCKDEP_WARN()s in
	 * gb_frame_done() and gb_alsa_write() can check it -- they test
	 * rcu_read_lock_held(), which a disabled-preemption region does not
	 * satisfy. Its compiler barriers would also keep gb-bt656.c's plain loads
	 * of s->frame inside the section if gb_bt656_feed() were ever inlined
	 * here; that file cannot use READ_ONCE, since tools/gbcap compiles it.
	 *
	 * Nothing inside may sleep, and nothing does: buf_lock and pcm_lock are
	 * spinlocks taken irqsave, and snd_pcm_period_elapsed() takes the
	 * substream stream lock, which is a spinlock as long as pcm->nonatomic
	 * stays false (this driver never sets it). On PREEMPT_RT those become
	 * sleeping locks, which a preemptible RCU reader may block on.
	 *
	 * The killed-status return above and the zero_buf scrub and resubmit
	 * below are outside: they touch only the URB, which no fence retires. The
	 * loop has no return, only continue and break, so the unlock always runs. */
	rcu_read_lock();
	for (i = 0; i < urb->number_of_packets; i++) {
		struct usb_iso_packet_descriptor *d = &urb->iso_frame_desc[i];
		unsigned int len;

		/* ACTUAL_LENGTH DECIDES, NEVER STATUS.
		 *
		 * This is the single largest defect this project ever had, in its
		 * libusb form. EP 0x82 is high-bandwidth isochronous -- three
		 * 1010-byte transactions per microframe -- and when fewer than three
		 * arrive the transfer is reported as an error while the bytes that
		 * did arrive sit at the front of the buffer, perfectly good.
		 *
		 * Where those bytes land is why it mattered so much: a whole
		 * vertical-blanking interval is under 600 bytes, so it fits INSIDE
		 * one discarded packet, and discarding it takes the field's anchor
		 * with it. Fixing this took worst frame-to-frame vertical movement
		 * from 2.04 rows to 0.236.
		 *
		 * Dropped microframes are also normal and must not be logged: the
		 * device produces 21.4 of a possible 24.2 MB/s, so roughly one
		 * microframe in eight carries nothing, and a driver that logs each
		 * one emits about 2000 lines a second. Count them and move on. */
		if (d->actual_length == 0) {
			gb->pkt_drops++;
			continue;
		}

		/* CLAMP IT. actual_length comes from the host controller and is
		 * the only number in this loop that is not ours; d->length is,
		 * and it is the per-packet size gb_alloc_urbs() actually
		 * allocated at d->offset. A controller that reports more than
		 * that would walk the assembler, or the raw_dump memcpy,
		 * straight off the end of the coherent buffer.
		 *
		 * Not hypothetical here: the zero_buf parameter below exists
		 * precisely because over-reported lengths are a live suspicion
		 * on this device, and it only makes the stale tail BENIGN. This
		 * makes an impossible length harmless. Counted as a URB error,
		 * which is what it is -- a transfer the host described wrongly
		 * -- and the count is the only way it will ever be noticed. */
		len = d->actual_length;
		if (unlikely(len > d->length)) {
			gb->urb_errors++;
			len = d->length;
		}

		gb->fed_bytes += len;
		if (len == 3030)      gb->len_hist[0]++;
		else if (len == 2020) gb->len_hist[1]++;
		else if (len == 1010) gb->len_hist[2]++;
		else                  gb->len_hist[3]++;

		if (raw_dump) {
			const u8 *src = (u8 *)urb->transfer_buffer + d->offset;
			unsigned int cap = gb->width * 2 * gb->height;
			unsigned int left = len;
			unsigned long flags;

			/* Carry the remainder across the buffer boundary rather
			 * than dropping it. An earlier version copied only what
			 * fitted and discarded the rest, which put a hole of up
			 * to one packet at every frame boundary -- about 40 of
			 * them in a 27 MB capture. That is invisible in the file
			 * and it corrupts exactly the measurement this mode
			 * exists to make, since a hole looks like a short line.
			 * The dump is only worth having if it is contiguous.
			 *
			 * UNDER buf_lock, for the reason gb_bt656_publish()
			 * gives: this path reads cur_buf, writes through its
			 * plane and completes it, and gb_return_buffers() can
			 * complete that same buffer from process context
			 * meanwhile. Unlike the blank, the copy is short enough
			 * to belong inside the lock -- at most one packet,
			 * 3030 bytes -- so the whole step is taken atomically
			 * rather than guarded by a re-check that would still
			 * leave a window. */
			while (left) {
				struct gb_buf *buf;
				unsigned int n;

				spin_lock_irqsave(&gb->buf_lock, flags);
				/* Same dead end as gb_frame_done() had: adopt
				 * here or a single dry moment ends the dump for
				 * good. The adopted buffer starts at offset 0
				 * because gb_next_buffer_locked() resets raw_off
				 * on every adoption. (An earlier version of this
				 * comment said raw_off was 0 whenever cur_buf was
				 * NULL; gb_frame_done() clears cur_buf without
				 * touching it, so that was false.) */
				if (!gb->cur_buf && gb->video_streaming)
					gb_next_buffer_locked(gb);
				buf = gb->cur_buf;
				if (!buf) {
					spin_unlock_irqrestore(&gb->buf_lock,
							       flags);
					gb->no_buffer++;
					break;
				}
				/* The plane, not the current geometry, is
				 * the bound on what may be written into it. */
				cap = min_t(unsigned long, cap,
					    vb2_plane_size(&buf->vb.vb2_buf, 0));
				if (gb->raw_off >= cap)
					gb->raw_off = 0;
				n = min_t(unsigned int, left, cap - gb->raw_off);
				memcpy((u8 *)vb2_plane_vaddr(&buf->vb.vb2_buf, 0) +
				       gb->raw_off, src, n);
				gb->raw_off += n;
				src  += n;
				left -= n;

				if (gb->raw_off >= cap) {
					vb2_set_plane_payload(&buf->vb.vb2_buf,
							      0, cap);
					buf->vb.vb2_buf.timestamp = ktime_get_ns();
					buf->vb.sequence = gb->sequence++;
					buf->vb.field = V4L2_FIELD_NONE;
					vb2_buffer_done(&buf->vb.vb2_buf,
							VB2_BUF_STATE_DONE);
					gb->cur_buf = NULL;
					gb->raw_off = 0;
					gb_next_buffer_locked(gb);
				}
				spin_unlock_irqrestore(&gb->buf_lock, flags);
			}
			continue;
		}

		gb_bt656_feed(&gb->bt,
			      (unsigned char *)urb->transfer_buffer + d->offset,
			      len);
	}
	rcu_read_unlock();

	/* DIAGNOSTIC (zero_buf=1): scrub the transfer buffer before resubmitting.
	 * If the host controller is over-reporting actual_length, the tail of each
	 * packet is stale content from this URB's previous use -- which contains
	 * old timing codes, and those appear mid-line as spurious short lines.
	 * Zeroing replaces them with 0x00, which is not a valid code, so the
	 * spurious gaps must vanish if that is what is happening. */
	if (zero_buf)
		memset(urb->transfer_buffer, 0, urb->transfer_buffer_length);

	/* -EPERM and -ENODEV are not transfer errors and must not be counted.
	 *
	 * usb_submit_urb() answers -EPERM while urb->reject is set, and the only
	 * thing in this driver that sets it is usb_kill_urb() in gb_free_urbs():
	 * an URB whose completion was already running when teardown started
	 * lands here. That is once per teardown -- the stop fences are RCU grace
	 * periods now and never kill the ring -- but every LAST-USER stream-down
	 * is a teardown, so without this filter urb_errors would still gain a
	 * spurious count each time the stream ends. -ENODEV is the same for an
	 * unplug. */
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0 && ret != -EPERM && ret != -ENODEV)
		gb->urb_errors++;
}

/* Tear the ring down. TEARDOWN, not a fence: usb_kill_urb() returns only once
 * the URB's completion has finished and cannot run again, so after this loop no
 * handler is running and none will -- which is also why a stop path that finds
 * the ring already gone has nothing to wait for. The stop fences that used to
 * kill and re-arm a ring still in use are RCU grace periods now
 * (gb_return_buffers_locked(), gb_pcm_sync_stop()); this is the only kill left.
 *
 * SLEEPS. CALLER HOLDS stream_lock, and must hold neither buf_lock nor pcm_lock,
 * which the handler takes. */
static void gb_free_urbs(struct gb_dev *gb)
{
	int i;

	lockdep_assert_held(&gb->stream_lock);

	for (i = 0; i < GB_ISOC_URBS; i++) {
		struct urb *urb = gb->urbs[i];

		if (!urb)
			continue;
		usb_kill_urb(urb);
		usb_free_coherent(gb->udev,
				  GB_ISOC_PACKETS * gb->iso_packet_size,
				  urb->transfer_buffer, urb->transfer_dma);
		usb_free_urb(urb);
		gb->urbs[i] = NULL;
	}
}

static int gb_alloc_urbs(struct gb_dev *gb)
{
	unsigned int pktsz = gb->iso_packet_size;
	int i, j;

	for (i = 0; i < GB_ISOC_URBS; i++) {
		struct urb *urb;
		void *buf;

		urb = usb_alloc_urb(GB_ISOC_PACKETS, GFP_KERNEL);
		if (!urb)
			goto err;
		gb->urbs[i] = urb;

		buf = usb_alloc_coherent(gb->udev, GB_ISOC_PACKETS * pktsz,
					 GFP_KERNEL, &urb->transfer_dma);
		if (!buf)
			goto err;

		urb->dev			= gb->udev;
		urb->pipe			= usb_rcvisocpipe(gb->udev,
						  GB_EP_VIDEO & 0x0f);
		urb->transfer_flags		= URB_ISO_ASAP |
						  URB_NO_TRANSFER_DMA_MAP;
		urb->transfer_buffer		= buf;
		urb->transfer_buffer_length	= GB_ISOC_PACKETS * pktsz;
		urb->complete			= gb_isoc_complete;
		urb->context			= gb;
		urb->interval			= 1;
		urb->number_of_packets		= GB_ISOC_PACKETS;

		for (j = 0; j < GB_ISOC_PACKETS; j++) {
			urb->iso_frame_desc[j].offset = j * pktsz;
			/* The MICROFRAME payload, 3030 -- not the per-transaction
			 * wMaxPacketSize of 1010. The USB core drives the three
			 * high-bandwidth transactions itself. */
			urb->iso_frame_desc[j].length = pktsz;
		}
	}
	return 0;
err:
	gb_free_urbs(gb);
	return -ENOMEM;
}

/* ------------------------------------------------------------------ vb2 --- */

static int gb_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
			  unsigned int *nplanes, unsigned int sizes[],
			  struct device *alloc_devs[])
{
	struct gb_dev *gb = vb2_get_drv_priv(q);
	unsigned int size = gb->width * 2 * gb->height;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int gb_buf_prepare(struct vb2_buffer *vb)
{
	struct gb_dev *gb = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int size = gb->width * 2 * gb->height;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

/* Clear a buffer to real black. NOT memset(0), which is bright green in UYVY:
 * these are the same four bytes gb_bt656_blank() writes, and they are written
 * here rather than by calling it because that function blanks through
 * gb->bt.frame -- the shared pointer this must not touch from this context. */
static void gb_blank_plane(void *vaddr, unsigned int bytes)
{
	u8 *p = vaddr;
	unsigned int i;

	/* Four times fewer stores, and this is 691200 bytes per QBUF -- but
	 * memset32() wants an aligned destination and a VB2_USERPTR plane can
	 * start at any byte offset into its first page. */
	if (IS_ALIGNED((unsigned long)p, 4)) {
		memset32((u32 *)p, (__force u32)cpu_to_le32(0x10801080),
			 bytes / 4);
		return;
	}
	for (i = 0; i + 4 <= bytes; i += 4) {
		p[i + 0] = 0x80;	/* Cb */
		p[i + 1] = 0x10;	/* Y  */
		p[i + 2] = 0x80;	/* Cr */
		p[i + 3] = 0x10;	/* Y  */
	}
}

static void gb_buf_queue(struct vb2_buffer *vb)
{
	struct gb_dev *gb = vb2_get_drv_priv(vb->vb2_queue);
	struct gb_buf *buf = container_of(to_vb2_v4l2_buffer(vb),
					  struct gb_buf, vb);
	unsigned long flags;

	/* Refuse buffers on a device that has gone away -- but ONLY on a queue
	 * that is really running. usbtv refuses unconditionally (usbtv_buf_queue(),
	 * drivers/media/usb/usbtv/usbtv-video.c), and copying that was wrong here:
	 * vb2_start_streaming() hands every queued buffer to ->buf_queue BEFORE it
	 * sets start_streaming_called (videobuf2-core.c, v6.16), so on a dead
	 * device an unconditional refusal error-completed them all onto vb2's
	 * done_list, gb_start_streaming()'s -ENODEV path then found nothing to
	 * return, and vb2 WARNed on the non-empty done_list. Before streaming has
	 * started, the buffer goes onto gb->bufs like any other and the failing
	 * start hands it back QUEUED. Once it has started -- vb2_core_qbuf() calls
	 * ->buf_queue only then -- a dead device means nothing will ever fill the
	 * buffer, and completing it with an error is right.
	 *
	 * gb_dev's storage is safe here whatever happens -- the v4l2_device kref
	 * owns it. Read without a lock: it is a hint, not a fence, and the thing
	 * that makes a late QBUF SAFE is the refcount, not this test. */
	if (!gb->udev && vb2_start_streaming_called(vb->vb2_queue)) {
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		return;
	}

	/* Blank HERE, in process context, before this buffer is reachable.
	 *
	 * Any row the decoder fails to fill must be black rather than whatever
	 * the previous user of this buffer left there -- but the clear used to
	 * happen in the completion handler, which is both the wrong context for
	 * a 691200-byte memset and unfixably racy: it had to be done outside
	 * buf_lock to keep the critical section short, and anything written
	 * outside that lock can land in a buffer gb_return_buffers() has just
	 * handed back to userspace.
	 *
	 * On this side of the fence the race cannot exist. The buffer is ours
	 * alone: userspace has queued it and this driver has not yet put it on
	 * gb->bufs, so neither the handler nor gb_return_buffers() can see it.
	 * The cost lands on the caller of QBUF, which is where it belongs.
	 *
	 * WHAT THIS BLANK IS FOR, because it looks redundant and is not. The
	 * assembler blanks every target IT publishes, at a frame boundary, a few
	 * instructions after frame_done hands the frame over (gb-bt656.c:278-279,
	 * the gb_bt656_blank() right after bt656_flush()) -- and that includes the
	 * adoption gb_frame_done() makes when cur_buf is NULL, which is reached
	 * only from there. Publications that are NOT at a BT.656 frame boundary,
	 * all of them, as of 2026-10-10 (grep gb_next_buffer_locked to re-check):
	 *
	 *   1. gb_start_streaming() at STREAMON -- mid-field. THIS is what the
	 *      blank is for. That buffer is delivered at the next boundary with
	 *      whatever rows the decoder did not reach, and this blank is the only
	 *      thing that makes those rows black.
	 *   2. and 3. the raw_dump loop in gb_isoc_complete(), on completion and
	 *      when it finds no buffer -- at byte-count boundaries, with
	 *      gb_bt656_feed() never running in that mode. Immaterial here,
	 *      because raw_dump writes the plane contiguously from offset 0 to
	 *      cap; but it means a raw_dump buffer that is ever delivered short of
	 *      cap would carry this blank in its tail, not stale pixels.
	 *
	 * And it really can be delivered -- the accept guard does not save it.
	 * bt656_flush() accepts on `runs in range && s->lines >= s->field_lines`
	 * (gb-bt656.c:134), and s->lines is incremented per written row
	 * (gb-bt656.c:322) and reset ONLY at a boundary (gb-bt656.c:280). So with
	 * audio already holding the ring -- the normal state -- the assembler is
	 * in steady state at STREAMON: runs[] are nominal and lines is already
	 * around 2*field_lines from rows written into the scratch, the first
	 * boundary accepts, and the buffer goes out carrying the previous
	 * session's pixels in every row written before the switch. Without this
	 * blank those rows would be all-zero UYVY, which is the bright green
	 * gb-bt656.c:42-46 exists to warn about.
	 *
	 * It is also sufficient, which is the part worth checking: a buffer is
	 * blanked exactly once per queue cycle here, before it is reachable, and
	 * every target the assembler publishes at a boundary it blanks itself. So
	 * nothing is delivered uncleared, and the duplicate blank that used to run
	 * at retarget time on every frame is gone. */
	/* Never more than the plane holds. gb_buf_prepare() guarantees it today,
	 * but it checked at QBUF and this runs at STREAMON, and the 2026-10-16
	 * S_STD race (gb_s_std()) lived exactly in the gap between the two. */
	gb_blank_plane(vb2_plane_vaddr(vb, 0),
		       min_t(unsigned long, vb2_plane_size(vb, 0),
			     (unsigned long)gb->width * 2 * gb->height));

	spin_lock_irqsave(&gb->buf_lock, flags);
	list_add_tail(&buf->list, &gb->bufs);
	spin_unlock_irqrestore(&gb->buf_lock, flags);
}

/* Hand every buffer back to vb2, and none while a completion handler could
 * still be writing into it.
 *
 * CALLER HOLDS stream_lock. SLEEPS, in synchronize_rcu(). Process context only,
 * and never with buf_lock or pcm_lock held: the handler takes both inside the
 * RCU section this waits for, so a caller holding either would wait for ever
 * on a reader it is blocking.
 *
 * ROUND 1's PARK WAS ONLY HALF THE FIX, AND THIS IS THE OTHER HALF.
 *
 * Round 1 made the STORE of bt.frame atomic: it is written only under buf_lock,
 * only from the cur_buf read in the same critical section. That closed two real
 * races -- a publish undoing the park, and vb2_buffer_done() running twice on
 * one buffer. It did NOT close the one that hung the machine, because that one
 * is not about stores to the pointer at all. It is about writes THROUGH it.
 *
 * gb_bt656_feed() writes s->frame one byte at a time (gb-bt656.c:349) and
 * gb_bt656_blank() memsets 691200 bytes through it (gb-bt656.c:279), both in
 * softirq, holding no lock, for as long as a 3030-byte packet takes. s->frame is
 * a plain struct member, so the compiler may legitimately keep it in a register
 * across that whole byte loop -- the window is a packet wide, not an
 * instruction. Once this function returns nothing stops userspace freeing these
 * vmalloc planes -- REQBUFS(0), or close() -- and the stream is REFCOUNTED, so
 * with an audio user gb_stream_down() only dropped a reference and the ring was
 * still live. That is the KASAN report:
 *
 *     BUG: KASAN: vmalloc-out-of-bounds in gb_bt656_blank+0x1df
 *     Write of size 1 at addr ffffc9000a6fd51d by task swapper/0/0
 *       gb_bt656_feed <- gb_isoc_complete <- __usb_hcd_giveback_urb
 *
 * A store cannot retire a pointer another CPU has already loaded; only waiting
 * for that CPU to finish with it can. So, in this order:
 *
 *   1. PUBLISH, under buf_lock: lower video_streaming, detach gb->bufs and
 *      cur_buf onto a local list, park bt.frame on the scratch, reset raw_off.
 *      From here no handler can reach a detached buffer through anything of
 *      ours.
 *   2. WAIT, holding no spinlock: synchronize_rcu(). gb_isoc_complete() runs
 *      its whole packet loop inside rcu_read_lock(), so this returns only once
 *      every handler that began before the call -- including one that loaded
 *      bt.frame before the park and is still writing through it -- has left
 *      that loop.
 *   3. HAND BACK: vb2_buffer_done(state) on each detached buffer, with no lock
 *      of ours held; it takes vb2's own done_lock.
 *
 * A HANDLER THAT BEGINS AFTER STEP 2 STARTS IS HARMLESS, for two different
 * reasons depending on what it touches. Its buf_lock-protected decisions --
 * gb_frame_done() and the raw_dump loop -- see step 1 through the lock: cur_buf
 * NULL and video_streaming false, so it parks rather than adopts. Its mid-frame
 * writes go through a PLAIN, UNLOCKED load of s->frame in gb-bt656.c, which
 * buf_lock does not order at all. What makes that load see the scratch is
 * synchronize_rcu()'s memory-ordering guarantee, stated in its kerneldoc
 * (kernel/rcu/tree.c, read for v6.16): each CPU whose read-side section
 * extends beyond the return from synchronize_rcu() has executed a full memory
 * barrier after synchronize_rcu() began and before that section began. The
 * park is stored before the call, so every handler still running when step 3
 * starts loads the scratch. That store and that load remain a data race in the
 * KCSAN sense -- a plain store under a lock against a plain load outside it --
 * exactly as they were under the old kill fence and as they are at
 * gb_start_streaming()'s publication. It is benign: an aligned pointer, and
 * both values are valid inside the section. It is not annotated because
 * gb-bt656.c cannot use READ_ONCE (tools/gbcap compiles it), and nothing here
 * claims KCSAN would be quiet about it.
 *
 * WHY DETACH, WAIT, THEN COMPLETE -- not complete, then wait. Completing first
 * would be memory-safe, but NOT for the reason an earlier version of this
 * comment gave ("vb2_core_reqbufs() returns -EBUSY while q->streaming"). That
 * rule covers REQBUFS only: with VB2_USERPTR, which this queue offers, a QBUF
 * at a changed address releases the buffer's old mapping mid-stream
 * (__prepare_userptr() -> put_userptr, videobuf2-core.c, v6.16). What makes
 * completing first safe HERE is that vb_lock -- q->lock -- is held across
 * ->stop_streaming, so no QBUF, and so no remap, can run until this function
 * returns; STREAMOFF and close() both hold it on the way in. But
 * vb2_buffer_done() is
 * documented (include/media/videobuf2-core.h) as the point after which "the
 * driver cannot use this buffer anymore until it is queued back to it", and
 * completing first would let an in-flight handler keep writing into a buffer
 * vb2 already holds as returned. Detaching first makes the property local and
 * true by construction -- nothing is handed back while a handler could still
 * write into it -- and leaves the vb2 facts as a second line of defence instead
 * of the only one. It costs a list head.
 *
 * EXACTLY ONCE. The handler may still complete cur_buf itself, DONE, in
 * gb_frame_done() or the raw_dump loop, before step 1. Both sides hold buf_lock,
 * so either it did -- and step 1 finds cur_buf NULL, or a successor adopted
 * while video_streaming was still true, and takes that -- or step 1 detached it
 * and the handler never sees it.
 *
 * WHEN THERE IS NOTHING TO WAIT FOR. The wait is conditional on gb->streaming,
 * read under stream_lock, which is the only lock it is written under. False
 * means NO HANDLER CAN BE RUNNING, for one of two reasons: the ring was never
 * armed (before the first gb_stream_up(), or after one that failed -- the
 * gb_start_streaming() error path reaches here that way), or
 * gb_stream_down_locked() has run gb_free_urbs(), whose usb_kill_urb() is a
 * teardown that proved it. Either way no ring can be armed again while we hold
 * stream_lock. gb_stream_up()'s
 * intermediate state -- URBs submitted, streaming not yet true -- is invisible
 * here for the same reason: it holds stream_lock throughout. So the common case,
 * video as the last user, pays no grace period.
 *
 * WHAT IT COSTS. The kill/re-arm fence this replaces discarded up to one ring
 * depth on every video stop with audio running, which put a gap in the audio.
 * That cost is GONE: the ring is never stopped here and nothing is discarded.
 * What remains, only when audio holds the ring, is one normal RCU grace period
 * spent holding vb_lock and stream_lock -- measured at 5-48 ms on a debug
 * kernel, 2026-10-11; the dev_dbg below reports it. Plain
 * synchronize_rcu(), not the expedited form, which would IPI every CPU on the
 * machine to save milliseconds on a path no frame deadline depends on.
 *
 * THE INVARIANT THIS ESTABLISHES, and any new code must keep it: writes through
 * a vb2 plane happen only (a) from the isochronous completion handler, inside
 * its RCU section, or (b) in gb_buf_queue(), before the buffer is reachable.
 *
 * NOT "planes are handed back to vb2 only here" -- an earlier version said so
 * and it was false. vb2_buffer_done() is also called by the handler itself, in
 * softirq with the ring live (gb_frame_done() and the raw_dump loop, with
 * VB2_BUF_STATE_DONE), and by gb_buf_queue()'s dead-device branch (ERROR, on a
 * buffer never put on gb->bufs). What is true is narrower:
 *   - THIS is the only place gb->bufs is drained, cur_buf is cleared from
 *     process context, and bt.frame is parked; and nothing drained here is
 *     handed back until every handler that could hold a pointer into it has
 *     finished.
 *   - A plane the handler completes ITSELF is safe because THE HANDLER NEVER
 *     TOUCHES A BUFFER AFTER vb2_buffer_done(). An earlier version of this
 *     comment said instead that such a plane "cannot be freed until the queue
 *     has stopped", citing the REQBUFS -EBUSY rule. That is FALSE for
 *     VB2_USERPTR: a DONE buffer can be dequeued, queued again at a new
 *     address, and have its old mapping released mid-stream by
 *     __prepare_userptr(), without passing through ->stop_streaming. No
 *     runtime defect followed from the wrong reason, because the real one
 *     holds: gb_frame_done() completes the buffer and publishes its successor
 *     in ONE buf_lock section; the blank at gb-bt656.c:279 reloads s->frame
 *     after the indirect frame_done() call; the raw_dump loop copies and
 *     completes in one section; and the handler's invocations never overlap
 *     -- gb->bt is unlocked state that already assumes so, which holds
 *     because the HCD gives back one endpoint's URBs one at a time
 *     (usb_giveback_urb_bh() drains its list serially, drivers/usb/core/hcd.c).
 * Any new path that drains gb->bufs or parks the assembler must come through
 * here.
 *
 * There is deliberately no WARN_ON for "no handler holds a plane": it has no
 * local witness. The assertions that ARE cheap are lockdep_assert_held() and
 * might_sleep() here, and RCU_LOCKDEP_WARN() on the reader side, in
 * gb_frame_done() and gb_alsa_write().
 *
 * scratch is NULL only when gb_stream_down() really did tear the stream down,
 * and in that case the URBs are freed and nothing will read it. */
static void gb_return_buffers_locked(struct gb_dev *gb,
				     enum vb2_buffer_state state)
{
	struct gb_buf *buf, *tmp;
	LIST_HEAD(done);
	unsigned long flags;

	lockdep_assert_held(&gb->stream_lock);
	might_sleep();

	spin_lock_irqsave(&gb->buf_lock, flags);
	/* Lower the adoption gate first. Since the handler adopts at every frame
	 * boundary where cur_buf is NULL, the "a later publisher finds cur_buf
	 * NULL and parks" argument below holds only while this is false: with
	 * audio keeping the ring alive, the next STREAMON's buf_queue calls land
	 * on gb->bufs before ->start_streaming, and nothing may adopt them until
	 * gb_start_streaming() raises this again. */
	gb->video_streaming = false;

	/* DETACH, do not complete: see the comment above. Reusing cur_buf->list
	 * is legal because gb_next_buffer_locked() list_del'd it off gb->bufs
	 * when it was adopted, and list_add_tail() rewrites the poisoned links.
	 * Order does not matter to vb2 for ERROR or QUEUED. */
	list_splice_tail_init(&gb->bufs, &done);
	if (gb->cur_buf) {
		list_add_tail(&gb->cur_buf->list, &done);
		gb->cur_buf = NULL;
	}

	/* Park the assembler on the scratch under the same lock that publishes
	 * cur_buf. A bare pointer store, and never a blank: 691200 bytes with this
	 * lock held and interrupts off is not acceptable, which is why the blank
	 * happens at QBUF time instead (gb_buf_queue()).
	 *
	 * This store is what stops a LATER handler from writing into a detached
	 * buffer -- one that starts afterwards finds cur_buf NULL and, with
	 * video_streaming lowered above, parks on the scratch too rather than
	 * adopting, and its unlocked loads of s->frame see this store by the
	 * ordering argument above. The grace period below is what stops an
	 * EARLIER one that is already past the load. Both are needed and neither
	 * substitutes for the other. */
	gb->bt.frame = gb->scratch;

	/* raw_off is an offset into cur_buf's plane, and it was only ever reset in
	 * gb_stream_up(). Leaving it non-zero across a stop makes the first buffer
	 * of the next raw_dump session start filling mid-plane, which looks exactly
	 * like a short line and would corrupt the one measurement that mode exists
	 * to make. */
	gb->raw_off = 0;
	spin_unlock_irqrestore(&gb->buf_lock, flags);

	/* WAIT, after the publish and with no spinlock held. */
	if (gb->streaming) {
		ktime_t t0 = ktime_get();

		synchronize_rcu();
		dev_dbg(&gb->intf->dev, "video stop: grace period %lld us\n",
			ktime_us_delta(ktime_get(), t0));
	}

	/* HAND BACK. The detached buffers are reachable only through `done`. */
	list_for_each_entry_safe(buf, tmp, &done, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

/* The same, for a caller that does not already hold stream_lock. */
static void gb_return_buffers(struct gb_dev *gb, enum vb2_buffer_state state)
{
	mutex_lock(&gb->stream_lock);
	gb_return_buffers_locked(gb, state);
	mutex_unlock(&gb->stream_lock);
}

/* Reset both command endpoints after an altsetting change: CLEAR_FEATURE
 * (ENDPOINT_HALT) on EP 0x01 and 0x81, which resets the data toggle on BOTH
 * sides. Caller holds cmd_lock, so nothing is in flight (see gb_stream_up()).
 *
 * EVERY alt switch needs this, not only the one into streaming.
 * usb_set_interface() resets the HOST's toggles to DATA0; this firmware does
 * not reset its own on SET_INTERFACE. So after a switch the device expects
 * whatever its toggle was, and if that is DATA1 the next OUT packet is taken
 * for a retransmission: ACKed and silently discarded. The host sees success,
 * usbmon shows the packet accepted, and the 8051 never sees it.
 *
 * That was the front-LED bug, open from August to 2026-10-11. Stream-down
 * switched to alt 0 and sent 0x37 with no reset, so the stop was lost exactly
 * when an ODD number of command packets had gone out on EP1 during the stream
 * -- 25 in every capture on the rig, 2026-10-11, and the LED flashed after
 * every cycle; on a host where the IR
 * poller adds commands the parity varies, which is why it looked
 * intermittent. A lone 0x37 sent afterwards from userspace, on the toggle the
 * device was expecting, stopped it at once; a 100 ms delay before the driver's
 * 0x37 did not. */
static void gb_reset_cmd_endpoints(struct gb_dev *gb)
{
	lockdep_assert_held(&gb->cmd_lock);

	usb_clear_halt(gb->udev, usb_sndbulkpipe(gb->udev, GB_EP_CMD));
	usb_clear_halt(gb->udev, usb_rcvbulkpipe(gb->udev, GB_EP_STATUS & 0x0f));
}

/* Tell the device to stop digitising, and MEAN IT.
 *
 * Factored out of gb_stream_down() so that gb_stream_up()'s error path can send
 * the same command the same way; it is the only command in this driver whose
 * failure has no symptom a program can see, so the retries and the logging are
 * not optional decoration and must not be reimplemented at half strength in a
 * second place.
 *
 * Sleeps, and takes cmd_lock via gb_cmd(), so the caller must hold neither
 * cmd_lock nor any spinlock. Must be called AFTER the switch back to alt 0 --
 * see the ordering comment in gb_stream_down(). */
static void gb_send_stop(struct gb_dev *gb)
{
	u8 stop[1] = { GB_CMD_STOP };
	int ret;

	/* CHECK THE RESULT. This was `gb_cmd(...)` with the return value dropped on
	 * the floor, and it is the one command in this driver whose failure has no
	 * symptom a program can see -- the device just keeps digitising, and the only
	 * evidence is the front LED still flashing.
	 *
	 * It also has a known timing sensitivity: 0x37 is
	 * refused while isochronous transfers are still unwinding, which is why it is
	 * sent after the switch to alt 0 rather than before. "After" may not be far
	 * enough after -- tools/gbcap stop demonstrably quiets a device that this
	 * path has just failed to quiet, and it sends the same single byte. So retry
	 * rather than assume, and say so either way. */
	lockdep_assert_held(&gb->stream_lock);

	ret = gb_cmd(gb, stop, sizeof(stop), NULL, 0);
	/* During disconnect a failure means the device is unreachable, and
	 * retrying only produces a warning that blames the decoder. That is the
	 * unplug case: gb_cmd() returns -ENODEV, and on the rig (2026-10-11) every
	 * physical unplug logged four tries and "the decoder is still running".
	 * An unbind with the device attached delivers it: the usb_set_interface()
	 * to alt 0 above re-enables EP1 even without .soft_unbind. gone is stable
	 * here: it is written under stream_lock, which every caller holds. One
	 * attempt, no retry. */
	if (ret < 0 && gb->gone) {
		dev_dbg(&gb->intf->dev, "stop command not delivered during disconnect: %d\n",
			ret);
		return;
	}
	if (ret < 0) {
		int try;

		for (try = 0; try < 3 && ret < 0; try++) {
			usleep_range(2000, 3000);
			ret = gb_cmd(gb, stop, sizeof(stop), NULL, 0);
		}
		if (ret < 0)
			dev_warn(&gb->intf->dev,
				 "stop command (0x%02x) failed after %d tries: %d -- the "
				 "decoder is still running; `gbcap stop` will quiet it\n",
				 GB_CMD_STOP, try + 1, ret);
		else
			dev_info(&gb->intf->dev,
				 "stop command needed %d retries\n", try);
	}
}

/* Bring the shared isochronous stream up. Refcounted -- see gb_dev.stream_lock.
 *
 * The first user does the work; later ones just take a reference. That matters
 * because the assembler's state and the decoder's configuration must NOT be
 * reset underneath a user that is already running. */
int gb_stream_up(struct gb_dev *gb)
{
	u8 start[3] = { GB_CMD_START, 0x03, 0x00 };
	bool started = false;
	int i, ret = 0;

	mutex_lock(&gb->stream_lock);
	/* Before the refcount, so a second user cannot take a reference on a
	 * ring that is being torn down either. See gb_dev.gone. */
	if (gb->gone) {
		mutex_unlock(&gb->stream_lock);
		return -ENODEV;
	}
	if (gb->stream_users++) {
		mutex_unlock(&gb->stream_lock);
		return 0;
	}

	/* REFUSE ON A DEAD DEVICE, and this is not decoration.
	 *
	 * gb_dev now outlives gb_disconnect() -- both the video node and the ALSA
	 * card can hold it -- so a late gb_pcm_work() really can run after
	 * disconnect has cleared gb->udev under cmd_lock. Everything below needs a
	 * live usb_device: usb_set_interface(), usb_clear_halt(),
	 * usb_alloc_coherent() and usb_submit_urb() all dereference it directly.
	 * gb_cmd() has its own -ENODEV check, but that is three sleeping USB calls
	 * too late. This is the one new crash the deferred-free design creates.
	 *
	 * gb->gone, tested at the top of this function, is now the PRIMARY
	 * refusal on the disconnect path: it is raised first thing in
	 * gb_disconnect(), long before udev is cleared, so it also stops a user
	 * that arrives while the rest of the teardown is still running. This
	 * udev test is the BACKSTOP, kept because it costs nothing and does not
	 * depend on disconnect's ordering.
	 *
	 * Read under stream_lock rather than cmd_lock: disconnect clears udev after
	 * it has dropped both stream users, so once it is NULL it stays NULL, and a
	 * racing read can only be stale in the harmless direction -- the USB calls
	 * below then fail with -ENODEV of their own. */
	if (!gb->udev) {
		ret = -ENODEV;
		goto err;
	}

	gb->sequence = 0;
	gb->raw_off = 0;
	memset(gb->len_hist, 0, sizeof(gb->len_hist));
	gb->fed_bytes = gb->pkt_drops = gb->no_buffer = gb->urb_errors = gb->audio_bytes = 0;
	gb->audio_calls = 0;
	gb->t_start = ktime_get();

	gb->scratch = vmalloc(GB_MAX_FRAME);
	if (!gb->scratch) {
		ret = -ENOMEM;
		goto err;
	}

	/* Alt 1 ONLY. If the host refuses the bandwidth, fail and say so -- do not
	 * walk down the ladder. Alts 2..5 top out at 17.4 MB/s and below, against
	 * the 20.7 MB/s that 720x480 UYVY at 30 fps needs, so degrading to them
	 * silently produces a driver that "works" and drops a third of every
	 * frame. */
	/* The alt switch happens UNDER cmd_lock, not before it.
	 *
	 * EP 0x01/0x81 change between bulk and interrupt across this call, so for as
	 * long as the switch and the state that describes it are separate steps,
	 * there is a window where a command built for the wrong endpoint type can be
	 * submitted. That window panicked a machine on 2026-08-26 once gb-ir.c began
	 * issuing commands asynchronously -- see the comment in gb_cmd(). Holding
	 * cmd_lock across the switch makes it unobservable. */
	mutex_lock(&gb->cmd_lock);
	ret = usb_set_interface(gb->udev, 0, GB_ALT_STREAM);
	if (!ret) {
		gb->cur_alt = GB_ALT_STREAM;

		/* CLEAR_FEATURE(ENDPOINT_HALT) on both command endpoints,
		 * between the alt switch and the start command, matching the
		 * captured ordering. It also resets the data toggle and flushes
		 * the FIFO.
		 *
		 * Inside the lock, and that matters: resetting an endpoint that
		 * has a transfer in flight makes EHCI warn that the queue head
		 * still holds qTDs --
		 *
		 *   WARNING: drivers/usb/host/ehci-hcd.c:1069
		 *            at ehci_endpoint_reset
		 *            usb_clear_halt <- gb_stream_up
		 *
		 * -- which showed up once in 120 stream cycles on 2026-08-26.
		 * Before gb-ir.c nothing else ever used EP1 during a transition,
		 * so this was unreachable for the same reason the 2026-08-26 panic was.
		 * gb_cmd() holds cmd_lock across the command AND its reply, so
		 * holding it here means nothing is in flight. */
		gb_reset_cmd_endpoints(gb);
	}
	mutex_unlock(&gb->cmd_lock);
	if (ret) {
		dev_err(&gb->intf->dev,
			"cannot select alt %d (%d)%s\n", GB_ALT_STREAM, ret,
			ret == -ENOSPC ? " -- the host refused the isochronous "
					 "bandwidth; try another port or unplug "
					 "other USB devices" : "");
		goto err_free;
	}

	gb_bt656_init(&gb->bt, gb->scratch, gb->field_lines,
		      0 /* interlaced */, gb->is_pal, &gb_bt656_ops, gb);

	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_stream, 1);

	/* AFTER the subdev, because s_std and s_stream touch these. */
	ret = gb_apply_board_config(gb);
	if (ret) {
		dev_err(&gb->intf->dev, "board configuration failed: %d\n", ret);
		goto err_stop;
	}

	ret = gb_cmd(gb, start, sizeof(start), NULL, 0);
	if (ret < 0) {
		dev_err(&gb->intf->dev, "start command failed: %d\n", ret);
		goto err_stop;
	}
	/* From here on the DEVICE is digitising, whatever happens on our side, and
	 * only GB_CMD_STOP ends that. err_stop keys off this flag. */
	started = true;

	ret = gb_alloc_urbs(gb);
	if (ret)
		goto err_stop;

	for (i = 0; i < GB_ISOC_URBS; i++) {
		ret = usb_submit_urb(gb->urbs[i], GFP_KERNEL);
		if (ret) {
			dev_err(&gb->intf->dev, "cannot submit URB %d: %d\n",
				i, ret);
			gb_free_urbs(gb);
			goto err_stop;
		}
	}

	gb->streaming = true;
	dev_info(&gb->intf->dev, "stream up: %d URBs x %d packets x %u bytes\n",
		 GB_ISOC_URBS, GB_ISOC_PACKETS, gb->iso_packet_size);
	mutex_unlock(&gb->stream_lock);
	return 0;

err_stop:
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_stream, 0);
	mutex_lock(&gb->cmd_lock);
	if (!usb_set_interface(gb->udev, 0, GB_ALT_IDLE))
		gb_reset_cmd_endpoints(gb);	/* or the stop below may be lost */
	gb->cur_alt = GB_ALT_IDLE;
	mutex_unlock(&gb->cmd_lock);
	/* AND SEND THE STOP. Failing to bring the stream up does not stop the
	 * device: once GB_CMD_START is acknowledged the CX25840 digitises into a
	 * FIFO, and a failed gb_alloc_urbs() or usb_submit_urb() left it doing
	 * exactly that with nobody draining it and nothing to show for it but the
	 * front LED. s_stream(0) and alt 0 do not end it -- that is
	 * the whole reason gb_stream_down() sends this command with retries -- so
	 * the error path has to send it too.
	 *
	 * Only when START actually got through: before that the device was never
	 * told to run, cmd_lock is released above as gb_send_stop() requires, and
	 * the ordering is the same as gb_stream_down()'s -- alt 0 first, then the
	 * command. */
	if (started)
		gb_send_stop(gb);
err_free:
	vfree(gb->scratch);
	gb->scratch = NULL;
err:
	gb->stream_users--;
	mutex_unlock(&gb->stream_lock);
	return ret;
}

/* Drop one reference on the shared ring, and tear it down if it was the last.
 *
 * Split out of gb_stream_down() so that gb_stop_streaming() can hold
 * stream_lock across the WHOLE video stop -- the decrement, the detach and park,
 * the grace period and the hand-off -- rather than as separate critical
 * sections. Between the decrement and the detach a pcm_work-driven
 * gb_stream_up() could otherwise run, re-run gb_bt656_init() and submit a fresh
 * ring whose handler adopts from gb->bufs while video_streaming is still true.
 *
 * CALLER HOLDS stream_lock. Sleeps: usb_kill_urb() (the teardown, when this is
 * the last user), usb_set_interface() and up to four gb_cmd() attempts in
 * gb_send_stop(), so on the order of 9 ms. */
static void gb_stream_down_locked(struct gb_dev *gb)
{
	lockdep_assert_held(&gb->stream_lock);

	if (!gb->stream_users || --gb->stream_users)
		return;

	gb->streaming = false;

	/* Blocks until the completion handler is not running, so hold nothing it
	 * needs. */
	gb_free_urbs(gb);

	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_stream, 0);

	/* Alt 0 FIRST, then the stop command. The natural order is the other way
	 * round and it does not work: EP 0x01 is interrupt at alts 1..5 and bulk
	 * only at alt 0, and the interrupt endpoint will not take the command while
	 * isochronous transfers are still unwinding.
	 *
	 * Sending it at all matters more than it looks: the device has no idea its
	 * host went away, so a stream left running keeps the CX25840 digitising
	 * into a FIFO nobody drains, with only the front LED to show for it.
	 *
	 * The udev test is DEFENCE IN DEPTH, not a fix for a known path. The
	 * invariant is that stream_users is 0 before gb_disconnect() sets
	 * gb->udev = NULL: gone is raised first, so no user can join;
	 * gb_alsa_unregister() drops the reference ALSA may hold; and
	 * gb_v4l2_unregister() -> vb2_video_unregister_device() stops video
	 * streaming under q->lock while udev is still valid (see the comment
	 * there). After that, a close() of a surviving fd does not reach
	 * ->stop_streaming at all: __vb2_queue_cancel() calls it only if
	 * start_streaming_called (videobuf2-core.c, v6.16). So this function
	 * should never run with udev NULL; if a future change breaks the
	 * invariant, usb_set_interface(NULL, ...) would oops, and this test turns
	 * that into a skipped alt switch. Tested under cmd_lock, the lock
	 * disconnect clears udev under. gb_send_stop() below needs no guard of its
	 * own: gb_cmd() makes the same test and returns -ENODEV. (The
	 * usb_set_interface() on gb_stream_up()'s error path needs none either: it
	 * runs under stream_lock with gb->gone false, and disconnect sets gone
	 * under stream_lock BEFORE it clears udev.) */
	mutex_lock(&gb->cmd_lock);
	/* Then reset the command endpoints, or the stop below is silently
	 * discarded whenever the stream sent an odd number of commands -- see
	 * gb_reset_cmd_endpoints(). Only after a successful switch: after a
	 * failed one the endpoints are not in a state worth resetting. */
	if (gb->udev && !usb_set_interface(gb->udev, 0, GB_ALT_IDLE))
		gb_reset_cmd_endpoints(gb);
	gb->cur_alt = GB_ALT_IDLE;
	mutex_unlock(&gb->cmd_lock);
	/* CHECK THE RESULT -- which is why this is a call and not a bare gb_cmd().
	 * The result checking, the retries and the warning are all in
	 * gb_send_stop(), together with the evidence for why they are needed;
	 * gb_stream_up()'s error path sends the same command through the same
	 * function rather than a weaker copy of it. */
	gb_send_stop(gb);

	vfree(gb->scratch);
	gb->scratch = NULL;

	dev_info(&gb->intf->dev,
		 "stream down: %u frames; %lu fed, %lu idle, %lu no-buffer, %lu urb err, %lu audio\n",
		 gb->sequence, gb->fed_bytes, gb->pkt_drops, gb->no_buffer,
		 gb->urb_errors, gb->audio_bytes);
	{
		s64 ms = ktime_ms_delta(ktime_get(), gb->t_start);
		unsigned long fa = gb->audio_bytes / 4;

		dev_info(&gb->intf->dev,
			 "audio: %lu sample frames in %lld ms = %lu Hz (expect 48000)\n",
			 fa, ms, ms ? (unsigned long)(fa * 1000 / ms) : 0);
	}
	dev_info(&gb->intf->dev,
		 "assembler: %u boundaries, %u short, field runs %d/%d/%d/%d\n",
		 gb->bt.attempts, gb->bt.short_frames,
		 gb->bt.runs[0], gb->bt.runs[1], gb->bt.runs[2], gb->bt.runs[3]);
}

void gb_stream_down(struct gb_dev *gb)
{
	mutex_lock(&gb->stream_lock);
	gb_stream_down_locked(gb);
	mutex_unlock(&gb->stream_lock);
}

/* Called by gb_disconnect() after both stream holders -- the ALSA card and the
 * vb2 queue -- have been unregistered, so stream_users should already be 0 and
 * this should do nothing. It exists because the driver sets .soft_unbind: on an
 * unbind with the device still attached, usb_unbind_interface() no longer
 * disables the interface before ->disconnect, so nothing in USB core kills the
 * ring for us any more. If a future change breaks the invariant, a live ring
 * outliving disconnect would complete into a gb_dev nobody owns. Say so loudly,
 * and tear it down while udev is still valid. */
void gb_stream_kill(struct gb_dev *gb)
{
	mutex_lock(&gb->stream_lock);
	if (WARN_ONCE(gb->stream_users, "gamebridge: %u stream user(s) left at disconnect\n",
		      gb->stream_users)) {
		gb->stream_users = 1;
		gb_stream_down_locked(gb);
	}
	mutex_unlock(&gb->stream_lock);
}

static int gb_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct gb_dev *gb = vb2_get_drv_priv(q);
	unsigned long flags;
	int ret;

	/* A dead device fails through the SAME path as a failed stream-up, so the
	 * queued buffers go back QUEUED either way. That takes BOTH halves:
	 * gb_buf_queue() no longer refuses a buffer while start_streaming has not
	 * been called, so on a dead device the buffers vb2 enqueued just before
	 * this call are on gb->bufs; and this path hands them back. The bare
	 * `return -ENODEV` this used to be -- as usbtv_start_streaming() has it,
	 * drivers/media/usb/usbtv/usbtv-video.c -- combined with buf_queue's
	 * unconditional refusal to leave them error-completed on vb2's done_list,
	 * and vb2_start_streaming() WARNs on that (videobuf2-core.c, v6.16). The
	 * udev test is only a fast path: once disconnect has begun, gb_stream_up()
	 * refuses every user, not only the first (gb_dev.gone). */
	ret = gb->udev ? gb_stream_up(gb) : -ENODEV;
	if (ret) {
		/* Hand the queued buffers back through the same function the stop
		 * path uses, so they get the same detach, wait and hand-off.
		 *
		 * The wait here is DEFENCE IN DEPTH, not the fix for a race. The
		 * handler cannot adopt from gb->bufs on this path: it adopts only
		 * while video_streaming is true, and video_streaming is false here
		 * -- false from kzalloc, lowered at every stop, and raised only at
		 * the end of a gb_start_streaming() that succeeded. gb_stream_up()
		 * fails either for the FIRST user, having freed or never allocated
		 * the ring and dropped stream_users back to 0, or for ANY user once
		 * the device is gone, having never taken a reference. A grace
		 * period is paid only if gb->streaming is true when we take
		 * stream_lock: in the first case, because a gb_pcm_work() brought
		 * the ring up in between; in the gone case, because ALSA still
		 * holds it -- gone is raised before gb_alsa_unregister() drops
		 * that reference. Otherwise nothing is waited for. */
		gb_return_buffers(gb, VB2_BUF_STATE_QUEUED);
		return ret;
	}

	/* Grab a buffer HERE, not in gb_stream_up(). The stream is refcounted, so
	 * when audio already has it up, gb_stream_up() takes a reference and
	 * returns immediately -- and if the buffer hand-off lived in there, video
	 * would stream into the scratch area forever and never deliver a frame.
	 *
	 * Measured exactly that way: with PipeWire holding the ALSA capture open,
	 * every VIDIOC_DQBUF blocked indefinitely at any buffer count. Bringing up
	 * video first and audio second worked, which is the asymmetry that gives it
	 * away -- a shared resource whose second user gets a different code path.
	 *
	 * And reset the frame counter HERE for the same reason. V4L2 wants the
	 * sequence to start at zero for each streaming session on this queue, but
	 * gb_stream_up() only reaches its body for the FIRST user of the endpoint.
	 * With audio holding the stream up -- the normal state now that audio is on
	 * by default -- video's streamon took a reference and left the counter
	 * running, and v4l2-compliance said so:
	 *
	 *     warn: got sequence number 50, expected 0
	 *
	 * The counter belongs to the video queue, not to the shared endpoint.
	 *
	 * ALL THREE IN ONE CRITICAL SECTION -- reset the counter, open the adoption
	 * gate, adopt the first buffer. The order used to be adopt, unlock, then
	 * reset outside buf_lock. With audio holding the ring the handler is live
	 * right here, so it could complete the just-adopted buffer with
	 * `sequence++` under the lock before the unlocked reset landed: the first
	 * frame of a session carried the previous session's count and the second
	 * carried 0. Under the lock the handler takes, it sees none of this or all
	 * of it.
	 *
	 * This adoption is the one publication that is NOT at a frame boundary;
	 * gb_buf_queue()'s blank exists for it. */
	spin_lock_irqsave(&gb->buf_lock, flags);
	gb->sequence = 0;
	gb->video_streaming = true;
	gb_next_buffer_locked(gb);
	spin_unlock_irqrestore(&gb->buf_lock, flags);
	return 0;
}

static void gb_stop_streaming(struct vb2_queue *q)
{
	struct gb_dev *gb = vb2_get_drv_priv(q);

	/* ONE critical section for the whole video stop, and that is the point.
	 *
	 * Dropping video's reference and detaching, waiting for and returning the
	 * buffers must not be separable: between the decrement and the detach an
	 * ALSA trigger could run gb_pcm_work() -> gb_stream_up(), which re-runs
	 * gb_bt656_init() and submits a fresh ring whose handler can adopt from
	 * gb->bufs while video_streaming is still true.
	 *
	 * The order inside matters too. gb_stream_down_locked() goes FIRST so that
	 * the common case -- video is the last user -- tears the ring down
	 * outright; gb_return_buffers_locked() then sees gb->streaming false and
	 * has nothing to wait for, because the teardown kill already proved no
	 * handler is running. Between the two the handler may still be writing
	 * into cur_buf, which is still a valid plane. When audio holds the ring,
	 * gb->streaming is still true, the ring keeps running, and the grace
	 * period inside gb_return_buffers_locked() is what makes the hand-off
	 * safe.
	 *
	 * That grace period is waited for HOLDING vb_lock and stream_lock -- two
	 * mutexes, which the handler never takes, so the wait cannot deadlock --
	 * and no spinlock. The price is that a concurrent S_STD, STREAMON or
	 * gb_pcm_work() waits one grace period longer; none of them runs under an
	 * ALSA mutex.
	 *
	 * Called by vb2 with vb_lock (q->lock) held, so the order is
	 * vb_lock -> stream_lock -> buf_lock. Nothing takes vb_lock while holding
	 * stream_lock, and buf_lock is a leaf, so there is no cycle. */
	mutex_lock(&gb->stream_lock);
	gb_stream_down_locked(gb);
	gb_return_buffers_locked(gb, VB2_BUF_STATE_ERROR);
	mutex_unlock(&gb->stream_lock);
}

static const struct vb2_ops gb_vb2_ops = {
	.queue_setup     = gb_queue_setup,
	.buf_prepare     = gb_buf_prepare,
	.buf_queue       = gb_buf_queue,
	.start_streaming = gb_start_streaming,
	.stop_streaming  = gb_stop_streaming,
#ifdef GB_HAVE_VB2_WAIT_PREPARE
	/* Release q->lock while a reader waits. On 7.x the core does this
	 * itself and the callbacks no longer exist; see driver/Kbuild. */
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
#endif
};

int gb_vb2_init(struct gb_dev *gb)
{
	struct vb2_queue *q = &gb->vb_vidq;
	int ret;

	INIT_LIST_HEAD(&gb->bufs);
	spin_lock_init(&gb->buf_lock);
	spin_lock_init(&gb->pcm_lock);
	/* stream_lock is NOT initialised here. It is mutex_init'd in
	 * gb_probe_capture(), next to cmd_lock, because gb_release() destroys it
	 * and gb_release() runs for a probe that failed before this function was
	 * ever reached. Destroying a mutex that is still all zeros trips
	 * DEBUG_MUTEXES -- that is the bug gb->vdev_lock_ready exists for, and the
	 * cheaper answer here is to make the lock always exist rather than to add
	 * a second flag. */
	mutex_init(&gb->vb_lock);

	q->type            = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes        = VB2_MMAP | VB2_USERPTR | VB2_READ;
	q->drv_priv        = gb;
	q->buf_struct_size = sizeof(struct gb_buf);
	q->ops             = &gb_vb2_ops;
	q->mem_ops         = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock            = &gb->vb_lock;
	/* ONE, deliberately. The whole point of this hardware is a one-frame
	 * latency budget, and the end-state presenter wants REQBUFS count = 2.
	 * Anything higher here forbids that. Two buffers is tight -- a stall in the
	 * consumer drops frames rather than delaying them -- and that is the
	 * correct trade here. Do not "fix" it. */
	q->min_queued_buffers = 1;

	ret = vb2_queue_init(q);
	if (ret)
		return ret;
	gb->vb2_ready = true;
	return 0;
}

void gb_vb2_release(struct gb_dev *gb)
{
	/* Releasing a queue that was never initialised is not a no-op: it is a
	 * NULL dereference. vb2_queue_release() -> __vb2_queue_cancel() ->
	 * __wake_up() on a wait_queue_head that is still all zeros.
	 *
	 * That is reachable from probe's error path, which calls
	 * gb_v4l2_unregister() for ANY failure after v4l2_device_register() --
	 * including gb_send_prologue() failing, which happens whenever the device
	 * arrives with a wedged command endpoint. KASAN caught it on the EHCI test machine,
	 * 2026-08-26:
	 *
	 *   gamebridge: bring-up prologue failed: -110
	 *   KASAN: null-ptr-deref in range [0x0..0x7]
	 *   __wake_up_common <- __vb2_queue_cancel <- vb2_core_queue_release
	 *     <- gb_vb2_release <- gb_v4l2_unregister <- gb_probe_capture
	 *
	 * The bug predates the tuner work by every stage since 7. It needed a
	 * probe that FAILS to expose it, and until a wedged device turned up
	 * there had never been one. */
	/* vb_lock is NOT destroyed here, and not in gb_release() either.
	 *
	 * _vb2_fop_release() takes q->lock after gb_disconnect() has returned --
	 * verified in the shipped videobuf2-v4l2.ko, where it loads vdev->queue,
	 * takes q->lock (q+0x20) and calls vb2_core_queue_release() whenever
	 * q->owner (q+0x28) is NULL or is this file. vb2_video_unregister_device()
	 * clears q->owner, so a late close() takes exactly that branch. The
	 * mutex_destroy() that used to be on the next line therefore handed
	 * userspace's close() a destroyed mutex.
	 *
	 * Leaving it alive costs nothing: mutex_destroy() is a DEBUG_MUTEXES
	 * annotation, and the storage is about to be kfree'd by gb_release(). */
	if (!gb->vb2_ready)
		return;
	gb->vb2_ready = false;
	vb2_queue_release(&gb->vb_vidq);
}
