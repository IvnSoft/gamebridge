// SPDX-License-Identifier: GPL-2.0
/* The GameBridge as an ALSA capture device.
 *
 * ============================================================================
 *  This file was blamed for three hard lockups and was not responsible for any
 *  of them. The header below is kept because the mistake is more instructive
 *  than the code.
 * ============================================================================
 *
 * WHAT HAPPENED. Three times the machine died with the journal stopping
 * mid-line -- no oops, no warning, no panic -- always within moments of the
 * audio stream starting. Audio was made opt-in and this file was marked unfit
 * to enable.
 *
 * TWO REAL BUGS WERE FOUND HERE AND FIXED, AND NEITHER WAS THE CAUSE.
 *   1. gb_alsa_write()'s ring loop could fail to advance when the substream's
 *      buffer size was zero, spinning forever under spin_lock_irqsave().
 *   2. gb_pcm_trigger() took pcm_lock with plain spin_lock() while the URB
 *      completion handler takes the same lock -- a textbook deadlock, and one I
 *      introduced in the commit meant to fix (1), justified by a comment
 *      asserting a context guarantee I had not checked.
 * Both were genuine. It still hung. A third round of auditing this file would
 * have found a third thing and it would still have hung.
 *
 * WHERE IT ACTUALLY WAS. Under KASAN on a spare machine, on the first run that
 * reproduced it:
 *
 *     BUG: KASAN: vmalloc-out-of-bounds in gb_bt656_blank+0x1df [gamebridge]
 *       gb_bt656_feed <- gb_isoc_complete <- __usb_hcd_giveback_urb
 *
 * The VIDEO assembler, writing into vb2 planes that vb2 had already freed.
 * gb_stop_streaming() tears the stream down and then returns the buffers, which
 * is safe only because gb_stream_down() kills the URBs -- and it is REFCOUNTED,
 * so with audio capturing it just drops a reference and the ring stays live.
 * Fixed in gb_return_buffers(); see gb-video.c.
 *
 * SO: ENABLING AUDIO IS WHAT MADE THE BUG REACHABLE, NOT WHAT MADE IT WRONG.
 * Audio is the thing that holds the stream up past video's stop. The symptom
 * named the wrong file for two days, and the lesson is that co-occurrence is
 * not location -- "it only happens with X enabled" says X is on the path, not
 * that X is the defect.
 *
 * The design note below was written as a suspicion and is kept as a design
 * observation, since it is still true that this file enters the ALSA core from
 * the URB completion handler ~12000 times a second. It is not a known defect.
 * If a reason ever appears to change it, the shape is a kfifo in the handler
 * drained by a work item calling snd_pcm_period_elapsed() in process context.
 * There is no reason today, and nothing on this path is latency-critical --
 * the video is what has to be fast, and it already is at 0.07 ms.
 *
 * VERIFICATION behind turning audio on by default: the bug reproduced and was
 * named under KASAN on an EHCI test machine (kernel-debug), then 144 video start/stop
 * cycles under continuous audio and 23.1 GB with zero sanitizer reports; and
 * 164 traversals of the same window on the workstation that produced all three
 * lockups (xHCI, PipeWire holding the card), alive throughout.
 *
 * Structurally this is usbtv-audio.c,
 * with one difference that shapes the whole file: usbtv has a second endpoint
 * for audio, and this device does not.
 *
 * THE AUDIO COMES OFF THE VIDEO ENDPOINT. It rides in the horizontal blanking
 * of the BT.656 stream, as 48 kHz 16-bit big-endian stereo PCM inside ancillary
 * packets the frame assembler already has to skip. So there is nothing to start
 * separately: capturing sound means running the same isochronous ring that
 * carries the picture. Hence gb_stream_up()/gb_stream_down() being refcounted --
 * either subsystem can bring the endpoint up, and the last one down tears it
 * off. Nobody expects sound to arrive on a video endpoint, so it is worth
 * saying twice.
 *
 * S16_BE is not a typo and needs no conversion: the samples are big-endian on
 * the wire and ALSA has a format for exactly that.
 */
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "gamebridge.h"

#define GB_AUDIO_RATE		48000
#define GB_AUDIO_CHANNELS	2

static const struct snd_pcm_hardware gb_pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP_VALID,
	.formats          = SNDRV_PCM_FMTBIT_S16_BE,
	.rates            = SNDRV_PCM_RATE_48000,
	.rate_min         = GB_AUDIO_RATE,
	.rate_max         = GB_AUDIO_RATE,
	.channels_min     = GB_AUDIO_CHANNELS,
	.channels_max     = GB_AUDIO_CHANNELS,
	.buffer_bytes_max = 128 * 1024,
	.period_bytes_min = 1024,
	.period_bytes_max = 32 * 1024,
	.periods_min      = 2,
	.periods_max      = 32,
};

/* Called from the URB completion handler, with one ancillary block's worth of
 * already-de-interleaved stereo. Interrupt context: no sleeping, no allocation.
 *
 * A block is four sample frames, so this runs about 12000 times a second.
 *
 * MUST BE CALLED INSIDE gb_isoc_complete()'s RCU read-side section, which is
 * the only caller today (gb_audio_block() <- the assembler <- the handler):
 * gb_pcm_sync_stop()'s grace period is what makes the unlocked use of `sub`
 * below safe. Non-static and declared in gamebridge.h, so a future caller could
 * skip the section silently; the RCU_LOCKDEP_WARN() says so instead, on any
 * kernel with CONFIG_PROVE_RCU (the debug-kernel test rigs), and compiles out
 * elsewhere. */
void gb_alsa_write(struct gb_dev *gb, const unsigned char *pcm, int bytes)
{
	struct snd_pcm_substream *sub;
	struct snd_pcm_runtime *rt;
	unsigned int buf_bytes, period_bytes;
	unsigned long flags;
	bool elapsed = false;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "gb_alsa_write() outside gb_isoc_complete()'s RCU section");

	if (bytes <= 0)
		return;

	spin_lock_irqsave(&gb->pcm_lock, flags);
	sub = gb->pcm_sub;
	if (!gb->pcm_running || !sub || !sub->runtime) {
		spin_unlock_irqrestore(&gb->pcm_lock, flags);
		return;
	}
	rt = sub->runtime;
	buf_bytes    = snd_pcm_lib_buffer_bytes(sub);
	period_bytes = snd_pcm_lib_period_bytes(sub);

	/* THIS DRIVER HUNG A MACHINE HERE, and both ways are worth stating.
	 *
	 * The loop below advances by n = min(bytes, buf_bytes - pcm_write). If
	 * buf_bytes is zero -- a substream that is running but whose buffer is not
	 * set up -- then n is zero, `bytes` never decreases, and this spins forever
	 * with a spinlock held and interrupts DISABLED. That is a hard lockup: the
	 * machine dies without logging a word, which is exactly what happened. The
	 * last thing in the journal was the first URB completion.
	 *
	 * And if pcm_write ever exceeds buf_bytes, `buf_bytes - pcm_write` is
	 * UNSIGNED and wraps to something enormous, so n becomes `bytes` and the
	 * memcpy runs off the end of the ALSA buffer.
	 *
	 * So: validate everything before the loop, clamp the cursor, and make the
	 * loop structurally incapable of not advancing. A bad state drops samples,
	 * which is the correct failure -- audio capture wants the newest samples,
	 * and no sound is infinitely better than no computer. */
	if (!rt->dma_area || !buf_bytes || !period_bytes) {
		spin_unlock_irqrestore(&gb->pcm_lock, flags);
		return;
	}
	if (gb->pcm_write >= buf_bytes)
		gb->pcm_write = 0;

	while (bytes > 0) {
		unsigned int space = buf_bytes - gb->pcm_write;
		unsigned int n = min_t(unsigned int, (unsigned int)bytes, space);

		if (!n)			/* cannot happen now; refuse to spin if it does */
			break;

		memcpy(rt->dma_area + gb->pcm_write, pcm, n);
		gb->pcm_write += n;
		if (gb->pcm_write >= buf_bytes)
			gb->pcm_write = 0;	/* ring wrap */
		pcm   += n;
		bytes -= n;

		gb->pcm_period += n;
		while (gb->pcm_period >= period_bytes) {
			gb->pcm_period -= period_bytes;
			elapsed = true;
		}
	}
	spin_unlock_irqrestore(&gb->pcm_lock, flags);

	/* Outside the lock, and it must be: snd_pcm_period_elapsed() takes the
	 * substream stream lock and can re-enter this driver -- ops->pointer()
	 * always, and ops->trigger(STOP) on an xrun -- and both take pcm_lock.
	 * Holding pcm_lock across this call is a same-CPU self-deadlock with
	 * interrupts already off, which is bug 2 in this file's header.
	 *
	 * SO `sub` IS DEREFERENCED HERE WITH NO LOCK HELD, and clearing gb->pcm_sub
	 * under pcm_lock elsewhere orders nothing for a handler that is already
	 * past the unlock above. What makes that safe is gb_pcm_sync_stop(): ALSA
	 * calls it between the trigger STOP and the free of sub->runtime, and it
	 * waits an RCU grace period, which cannot end while this call -- inside
	 * gb_isoc_complete()'s read-side section -- is still running. See the
	 * comment there -- it is where the reasoning lives, because this is the
	 * line it exists to protect.
	 *
	 * The memcpy into rt->dma_area above does NOT depend on that hook: it runs
	 * inside pcm_lock behind the gb->pcm_running test, and snd_pcm_drop()'s
	 * trigger(STOP) clears pcm_running under pcm_lock before do_hw_free()
	 * releases the managed buffer. That half was already fenced. */
	if (elapsed)
		snd_pcm_period_elapsed(sub);
}

static int gb_pcm_open(struct snd_pcm_substream *sub)
{
	struct gb_dev *gb = snd_pcm_substream_chip(sub);

	unsigned long flags;

	sub->runtime->hw = gb_pcm_hw;
	spin_lock_irqsave(&gb->pcm_lock, flags);
	gb->pcm_sub = sub;
	gb->pcm_running = false;
	gb->pcm_write = 0;
	gb->pcm_period = 0;
	spin_unlock_irqrestore(&gb->pcm_lock, flags);
	return 0;
}

static int gb_pcm_close(struct snd_pcm_substream *sub)
{
	struct gb_dev *gb = snd_pcm_substream_chip(sub);
	unsigned long flags;

	spin_lock_irqsave(&gb->pcm_lock, flags);
	gb->pcm_sub = NULL;
	gb->pcm_running = false;
	spin_unlock_irqrestore(&gb->pcm_lock, flags);
	return 0;
}

/* THE AUDIO STOP FENCE: wait until no completion handler can still be using a
 * pre-stop `sub`.
 *
 * gb_alsa_write() reads gb->pcm_sub under pcm_lock, drops the lock, and calls
 * snd_pcm_period_elapsed(sub) outside it -- deliberately, because that function
 * takes the substream stream lock and can re-enter trigger(), which takes
 * pcm_lock. Clearing pcm_sub under pcm_lock in gb_pcm_close() orders nothing for
 * a handler that has ALREADY read the pointer and passed the unlock: ALSA then
 * frees substream->runtime on that same syscall, with no wait of its own, and
 * snd_pcm_period_elapsed() dereferences sub->runtime. It is the same mistake
 * round 1 made about gb->bt.frame, in a different file -- a store cannot retire
 * a pointer another CPU has already loaded.
 *
 * ALSA calls this hook between the stop and that free, for precisely this
 * purpose. Verified in the shipped snd-pcm.ko of the running kernel:
 * snd_pcm_release_substream() calls snd_pcm_drop(), then do_hw_free(), then
 * ops->close(), then snd_pcm_detach_substream(); and do_hw_free() calls
 * snd_pcm_sync_stop() FIRST, before ops->hw_free and before
 * snd_pcm_lib_free_pages(). snd_pcm_sync_stop() invokes ops->sync_stop only when
 * runtime->stop_operating is set, and clears it as it does so, so this runs ONCE
 * per stop -- but from whichever of its FIVE callers comes first, not only from
 * the close path (sound/core, v6.16): snd_pcm_hw_params(), do_hw_free(),
 * snd_pcm_do_prepare(), snd_pcm_suspend_all() and snd_pcm_dev_disconnect().
 *
 * HOW: synchronize_rcu(), and nothing else. gb_isoc_complete() runs its packet
 * loop -- and with it every gb_alsa_write(), from the `sub = gb->pcm_sub` load
 * through snd_pcm_period_elapsed(sub) returning -- inside rcu_read_lock(). A
 * grace period ends only after every read-side section that began before it has
 * ended, so when this returns no handler that loaded a pre-stop `sub` is still
 * running. Completions may well be running again by then; the claim is not "no
 * gb_alsa_write() in flight" but that every one in flight started after the
 * stop was published -- see the next paragraph. That is the role
 * synchronize_irq(card->sync_irq) plays for a driver with no sync_stop -- a WAIT
 * for in-flight handlers, not a teardown -- and it is the right model here
 * because the audio is assembled inside the VIDEO completion handler.
 *
 * PUBLISH BEFORE WAIT, the precondition everything above rests on: a handler
 * that starts after the grace period begins must find pcm_running false. The
 * core sets stop_operating, which is what makes this run at all, only right
 * after calling our trigger:
 *   - snd_pcm_do_stop() and snd_pcm_undo_start() call trigger(STOP), then set
 *     it (pcm_native.c:1519-1528 and :1455-1461). That covers snd_pcm_drop()
 *     ahead of do_hw_free() on close, xrun, drain, a failed start, and
 *     snd_pcm_dev_disconnect()'s stop of a running substream, and so the
 *     hw_params, prepare and disconnect callers that consume it.
 *   - snd_pcm_do_suspend() (pcm_native.c:1702-1712) calls trigger(SUSPEND),
 *     IGNORES THE RESULT, and sets it; snd_pcm_suspend_all() then calls this
 *     (:1783). So gb_pcm_trigger() handles SUSPEND exactly as STOP. Until it
 *     did, it returned -EINVAL for SUSPEND and left pcm_running true, so for
 *     that caller the precondition was false -- for the old kill fence just as
 *     much as for this one. That hole was LIVE, on an ABORTED system suspend.
 *     On a completed one it was harmless: gb_driver has no .suspend/.resume,
 *     so usb_suspend() force-unbinds the interface
 *     (unbind_no_pm_drivers_interfaces(), drivers/usb/core/driver.c) and
 *     gb_disconnect() tears the ring down before userspace thaws. But the PCM
 *     device has its own PM op (do_pcm_suspend(), sound/core/pcm.c:595-611)
 *     and suspends FIRST, so if the transition then aborts -- a pending wakeup
 *     event, or any later device failing to suspend
 *     (drivers/base/power/main.c:1812-1814) -- there is no forced unbind.
 *     Userspace thaws with the stream SUSPENDED, stop_operating already
 *     consumed and pcm_running still true. close() then runs snd_pcm_drop(),
 *     which does not call trigger for a SUSPENDED stream, frees the managed
 *     buffer in do_hw_free(), and only after that does ops->close clear
 *     pcm_running: the handler copied into a freed dma_area meanwhile. A
 *     use-after-free predating the 2026-10-10 review, never exercised.
 * Every trigger that publishes clears pcm_running under pcm_lock, and
 * gb_alsa_write() tests it under pcm_lock BEFORE it loads `sub`, so a later
 * handler returns early -- ordered by the lock itself, not only by RCU's
 * memory-ordering guarantee. Once a stop is consumed, only trigger(START) sets
 * pcm_running again.
 *
 * WHAT CHANGED, AND WHAT IT NO LONGER COSTS. This used to kill the isochronous
 * ring and re-arm it (gb_stream_sync(), now deleted). Both costs that carried
 * are GONE:
 *   - STOPPING AUDIO NO LONGER COSTS THE VIDEO STREAM A FRAME. The kill
 *     discarded up to one ring depth -- 8 x 32 x 3030 bytes, ~32 ms, about one
 *     NTSC frame -- and the frame in progress was then rejected as short. This
 *     function does not touch the ring.
 *   - IT TAKES NO LOCK OF OURS. The kill needed stream_lock, which made the
 *     unplug path register_mutex -> pcm->open_mutex -> stream_lock, the close
 *     path pcm->open_mutex -> stream_lock (snd_pcm_release() holds open_mutex;
 *     do_hw_free() takes no buffer_mutex), and the hw_params, hw_free and
 *     prepare ioctls buffer_mutex -> stream_lock -- so every sound card
 *     on the machine could wait on this device's USB traffic, at up to 1000 ms
 *     per gb_cmd() send. That dependency is CLOSED: no gb_cmd() timeout,
 *     gb_send_stop() retry or I2C sequence can extend this function, and both
 *     lock orders are gone. It does not even dereference the chip pointer.
 *
 * WHAT IT STILL COSTS, plainly:
 *   - One normal RCU grace period per stop, on whichever caller consumes it --
 *     close, hw_free, hw_params or prepare after a stop (xrun recovery
 *     included), suspend, unplug. Measured at 5-48 ms on a debug
 *     kernel, 2026-10-11; the pr_debug below reports it. UNCONDITIONAL, because
 *     it cannot be skipped safely when the ring is down: gb->streaming means
 *     something only under stream_lock, and gb_stream_down_locked() clears it
 *     BEFORE gb_free_urbs() kills the ring, so a lockless read could see false
 *     while a handler is still inside gb_alsa_write(). With the ring really
 *     down the wait is pointless but correct.
 *   - On unplug, snd_pcm_dev_disconnect() calls this holding register_mutex --
 *     file-scope in sound/core/pcm.c, SHARED BY EVERY PCM DEVICE ON THE
 *     MACHINE -- and pcm->open_mutex, so for one grace period other cards' PCM
 *     registration, unplug and enumeration (including reads of
 *     /proc/asound/pcm) can wait on our account. Not their opens: PCM open
 *     does not take register_mutex in v6.16, which is taken only at
 *     sound/core/pcm.c:94, 119, 1054, 1093, 1145 and 1170. That NARROWS the
 *     old priority inversion; it
 *     does not close it. The wait is bounded by RCU across the whole system,
 *     not by this device or any lock it holds, and it is the same kind of wait
 *     as ALSA's own synchronize_irq() fallback at that call site, though
 *     typically longer.
 * Plain synchronize_rcu(), not synchronize_rcu_expedited(): the expedited form
 * IPIs every CPU on the machine, which saving milliseconds on a stop path that
 * no sample deadline depends on does not justify.
 *
 * Called in process context with NO SPINLOCK HELD, so sleeping in
 * synchronize_rcu() is legal -- the same requirement the previous
 * usb_kill_urb() version had (commit 6e2fa0a, never released). And the
 * substream stream lock specifically is never held here, which matters more
 * than ever: the handler ACQUIRES that lock inside
 * snd_pcm_period_elapsed(), within its RCU section, so a thread holding it while
 * waiting for the grace period would wait for ever -- the shape of bug 2 in this
 * file's header. Checked rather than assumed. In the shipped snd-pcm.ko,
 * snd_pcm_release_substream() calls snd_pcm_drop() -- which takes and RELEASES
 * the stream lock -- and only then do_hw_free(). In the v6.16 source:
 * snd_pcm_hw_params() and snd_pcm_hw_free() release it after their state
 * check, before calling this; snd_pcm_do_prepare() runs under
 * snd_pcm_action_nonatomic(), which holds snd_pcm_link_rwsem for reading and
 * buffer_mutex, and no stream lock; snd_pcm_suspend_all() calls this after
 * snd_pcm_suspend() has released it; snd_pcm_dev_disconnect() releases it
 * before its sync_stop loop.
 *
 * DEPENDENCY: pcm->nonatomic must stay false (we never set it). With a nonatomic
 * PCM the substream stream lock becomes a mutex, and snd_pcm_period_elapsed()
 * could sleep -- which it can do neither from the URB completion handler nor
 * inside the rcu_read_lock() section that this fence waits for. That holds on
 * PREEMPT_RT as well: there, a reader may block only "when acquiring spinlocks
 * that are subject to priority inheritance" (rcu_read_lock() kerneldoc,
 * include/linux/rcupdate.h, 6.16), and a nonatomic PCM's stream lock is a
 * struct mutex, not one of those.
 *
 * Reachable as: stop arecord while ffplay keeps running. An early KASAN run
 * was taken to refute this, but that test was a USB unbind during live audio,
 * which goes through gb_alsa_unregister(), a different path. The close() path
 * itself was tested on 2026-10-11: arecord started and stopped 40 times under
 * live video, with KASAN, lockdep and PROVE_RCU, and no report. */
static int gb_pcm_sync_stop(struct snd_pcm_substream *sub)
{
	ktime_t t0 = ktime_get();

	synchronize_rcu();
	/* pr_debug, not dev_dbg: a device pointer would mean dereferencing the
	 * chip or the card, and this function deliberately touches neither. */
	pr_debug("gamebridge: pcm sync_stop: grace period %lld us\n",
		 ktime_us_delta(ktime_get(), t0));
	return 0;
}

static int gb_pcm_prepare(struct snd_pcm_substream *sub)
{
	struct gb_dev *gb = snd_pcm_substream_chip(sub);
	unsigned long flags;

	spin_lock_irqsave(&gb->pcm_lock, flags);
	gb->pcm_write = 0;
	gb->pcm_period = 0;
	spin_unlock_irqrestore(&gb->pcm_lock, flags);
	return 0;
}

/* Reconcile the endpoint against what the PCM wants. Process context.
 *
 * A single work item, so the kernel serialises it against itself and
 * pcm_holds_stream needs no lock of its own. */
static void gb_pcm_work(struct work_struct *w)
{
	struct gb_dev *gb = container_of(w, struct gb_dev, pcm_work);
	/* A DISCONNECTED CARD NEVER WANTS THE ENDPOINT, and this is what makes
	 * every late run of this item monotonic -- it can only take the stream
	 * DOWN, never bring it up. gb_alsa_unregister() clears gb->snd_card as its
	 * first act, and snd_card_free_when_closed() means a trigger racing
	 * disconnect can still queue this work after the flush_work() there.
	 * Without this test such a run would call gb_stream_up() on a device whose
	 * udev is about to be NULL. gb_stream_up() would refuse it anyway --
	 * gb->gone, raised first thing in gb_disconnect(), is the primary refusal,
	 * and its !gb->udev check is the backstop -- so this test makes the
	 * refusal happen here, without taking stream_lock, rather than being the
	 * only line of defence. */
	bool want = READ_ONCE(gb->pcm_want_stream) && READ_ONCE(gb->snd_card);

	if (want && !gb->pcm_holds_stream) {
		if (!gb_stream_up(gb))
			gb->pcm_holds_stream = true;
	} else if (!want && gb->pcm_holds_stream) {
		gb_stream_down(gb);
		gb->pcm_holds_stream = false;
	}
}

/* ATOMIC CONTEXT. The PCM stream lock is held and interrupts are off, so
 * nothing here may sleep -- which rules out bringing the isochronous endpoint
 * up directly, since that takes a mutex, changes the altsetting, allocates
 * coherent DMA and sends a command. The first version of this function called
 * gb_stream_up() straight from here and produced
 *
 *     BUG: scheduling while atomic: pipewire/2803
 *       gb_stream_up+0x111 [gamebridge]
 *       gb_pcm_trigger+0x28 [gamebridge]
 *
 * So record the intent and let a work item do the work. The audio path itself
 * copes with the gap: gb_alsa_write() drops samples while pcm_running is false,
 * and the ring simply starts filling a few milliseconds later. */
/* EVERY acquisition of pcm_lock must disable interrupts, without exception.
 *
 * gb_alsa_write() takes this lock from the URB completion handler. A lock that
 * is taken in interrupt context and acquired anywhere else with plain
 * spin_lock() is a deadlock waiting for the interrupt to land on the same CPU:
 * that CPU then spins forever with interrupts already disabled, so the machine
 * dies without logging a word.
 *
 * An earlier version of this function used spin_lock() here, justified by a
 * comment asserting that trigger() is always atomic. That assertion was the
 * bug. It hung the machine, and the reasoning error is the part worth keeping:
 * "this context already has interrupts off" is a claim about someone else's
 * code, and the cost of being wrong is a hard lockup with no evidence. Use
 * irqsave and stop needing to be right. */
static int gb_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct gb_dev *gb = snd_pcm_substream_chip(sub);
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		spin_lock_irqsave(&gb->pcm_lock, flags);
		gb->pcm_write = 0;
		gb->pcm_period = 0;
		gb->pcm_running = true;
		spin_unlock_irqrestore(&gb->pcm_lock, flags);
		WRITE_ONCE(gb->pcm_want_stream, true);
		schedule_work(&gb->pcm_work);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	/* SUSPEND IS A STOP HERE, and not for tidiness. snd_pcm_do_suspend()
	 * (pcm_native.c:1702-1712, v6.16) calls this with SUSPEND, ignores
	 * what it returns, and sets stop_operating, so snd_pcm_suspend_all()
	 * goes on to run gb_pcm_sync_stop(). That fence only works if
	 * pcm_running was cleared FIRST -- see publish-before-wait there --
	 * and this used to return -EINVAL for SUSPEND and leave it true.
	 * That was LIVE, on an aborted system suspend: the PCM suspends
	 * through its own PM op before usb_suspend() force-unbinds the
	 * interface, so when the transition aborts in between there is no
	 * unbind, and a later close() frees the DMA buffer while the handler
	 * still sees pcm_running true. See gb_pcm_sync_stop() for the
	 * sequence. On a completed suspend the forced unbind made it
	 * harmless.
	 * The work item it schedules is covered exactly as STOP's is: a run
	 * racing the forced unbind is flushed by gb_alsa_unregister(), any
	 * later one can only take the stream down, and gb_snd_private_free()
	 * cancels it before gb_dev's reference goes. No RESUME: the PCM does
	 * not advertise SNDRV_PCM_INFO_RESUME, so userspace re-prepares. */
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock_irqsave(&gb->pcm_lock, flags);
		gb->pcm_running = false;
		spin_unlock_irqrestore(&gb->pcm_lock, flags);
		WRITE_ONCE(gb->pcm_want_stream, false);
		schedule_work(&gb->pcm_work);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t gb_pcm_pointer(struct snd_pcm_substream *sub)
{
	struct gb_dev *gb = snd_pcm_substream_chip(sub);
	unsigned int pos;
	unsigned long flags;

	spin_lock_irqsave(&gb->pcm_lock, flags);
	pos = gb->pcm_write;
	spin_unlock_irqrestore(&gb->pcm_lock, flags);
	return bytes_to_frames(sub->runtime, pos);
}

static const struct snd_pcm_ops gb_pcm_ops = {
	.open      = gb_pcm_open,
	.close     = gb_pcm_close,
	.prepare   = gb_pcm_prepare,
	.trigger   = gb_pcm_trigger,
	.pointer   = gb_pcm_pointer,
	.sync_stop = gb_pcm_sync_stop,
};

/* The card outlives gb_disconnect() now -- that is the point of
 * snd_card_free_when_closed() below -- and its substream ops reach gb_dev
 * through pcm->private_data on every call, so a deferred close() runs
 * gb_pcm_close() with gb as its chip pointer. gb_dev must
 * therefore outlive the ALSA card as well as the video node, and the v4l2_device
 * kref that owns it (gb_release(), gb-core.c) is the right instrument: the card
 * simply holds one reference and drops it here, from the callback
 * snd_card_do_free() runs after the last ops->close(). Without this, unplugging
 * with PipeWire holding the PCM open frees gb_dev at the last VIDEO close and
 * leaves PipeWire's close() calling into it. */
static void gb_snd_private_free(struct snd_card *card)
{
	struct gb_dev *gb = card->private_data;

	/* The work item holds a bare gb_dev pointer that the card's own refcount
	 * does not cover, so it has to be provably gone before the reference is
	 * dropped. Legal here: private_free runs in process context holding none of
	 * this driver's locks, and the worker takes stream_lock and cmd_lock -- so
	 * cancelling from anywhere that held either would deadlock, and this is not
	 * such a place. */
	cancel_work_sync(&gb->pcm_work);
	v4l2_device_put(&gb->v4l2_dev);
}

int gb_alsa_register(struct gb_dev *gb)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	INIT_WORK(&gb->pcm_work, gb_pcm_work);

	ret = snd_card_new(&gb->intf->dev, SNDRV_DEFAULT_IDX1, "GameBridge",
			   THIS_MODULE, 0, &card);
	if (ret < 0)
		return ret;

	strscpy(card->driver, "gamebridge", sizeof(card->driver));
	strscpy(card->shortname, "GameBridge", sizeof(card->shortname));
	strscpy(card->longname, "Adaptec GameBridge AVC-1410 line input",
		sizeof(card->longname));

	ret = snd_pcm_new(card, "GameBridge", 0, 0, 1, &pcm);
	if (ret < 0)
		goto err;
	pcm->private_data = gb;
	strscpy(pcm->name, "GameBridge line input", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &gb_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	/* PUBLISH BEFORE REGISTERING. snd_card_register() makes the PCM node
	 * openable, and PipeWire opens a new card within milliseconds of it
	 * appearing. gb_pcm_work() decides on pcm_want_stream && gb->snd_card,
	 * so when gb->snd_card was set only AFTER registration, a trigger landing
	 * in between ran the worker with snd_card still NULL, it decided "no
	 * stream", and nothing queued it again: silent audio until the next
	 * trigger. The card is now never reachable while gb->snd_card is NULL.
	 *
	 * The reference and private_free go in together, and that is what keeps
	 * the count symmetric on every path. The card owns a reference on
	 * gb_dev's storage from here on. If snd_card_register() fails, the
	 * snd_card_free(card) at err: runs private_free, whose v4l2_device_put()
	 * drops exactly the reference taken here; its cancel_work_sync() is a
	 * no-op, because an unregistered card has no users to trigger anything.
	 * If snd_pcm_new() failed earlier, neither was installed and
	 * snd_card_free() drops nothing. */
	v4l2_device_get(&gb->v4l2_dev);
	card->private_data = gb;
	card->private_free = gb_snd_private_free;
	WRITE_ONCE(gb->snd_card, card);

	ret = snd_card_register(card);
	if (ret < 0)
		goto err;

	dev_info(&gb->intf->dev, "ALSA capture device registered (card %d)\n",
		 card->number);
	return 0;
err:
	/* Unpublish first, so the probe error path's gb_alsa_unregister() finds
	 * nothing to tear down a second time. */
	WRITE_ONCE(gb->snd_card, NULL);
	snd_card_free(card);
	return ret;
}

void gb_alsa_unregister(struct gb_dev *gb)
{
	struct snd_card *card = gb->snd_card;
	unsigned long flags;

	if (!card)
		return;

	/* CLEARED FIRST, before anything else in this function. gb_pcm_work()
	 * reads it as "nobody wants the stream", which is what makes every run of
	 * that item from this moment on monotonic. The old code cleared it after
	 * the free, which left a trigger racing disconnect able to bring the ring
	 * back up; snd_card_free_when_closed() below makes that window real rather
	 * than theoretical. */
	gb->snd_card = NULL;

	/* Stop the URB handler touching the substream. Clearing pcm_sub under the
	 * lock the handler reads it under is what makes gb_alsa_write() bail out;
	 * it does NOT retire the pointer for a handler already past that unlock,
	 * and this path does not need it to. What retires it is reached from
	 * snd_card_free_when_closed() below: snd_pcm_dev_disconnect() stops a
	 * running substream and then calls gb_pcm_sync_stop(), whose grace
	 * period waits for that handler before anything is freed. */
	spin_lock_irqsave(&gb->pcm_lock, flags);
	gb->pcm_running = false;
	gb->pcm_sub = NULL;
	spin_unlock_irqrestore(&gb->pcm_lock, flags);

	/* snd_card_free_when_closed() can run card->private_free SYNCHRONOUSLY
	 * when nobody has the card open, and that drops the gb_dev reference the
	 * card holds. Hold one of our own across the rest of this function, so the
	 * lines after the call cannot be touching freed storage whatever the
	 * reference count happens to be. Two atomics, and it removes an inference
	 * instead of documenting one. */
	v4l2_device_get(&gb->v4l2_dev);

	/* NOT snd_card_free(). That one "waits until the all resources are
	 * properly released" -- it blocks until every userspace descriptor on the
	 * card is closed -- and this function runs in the USB disconnect callback,
	 * on the hub's workqueue. Unplug the device while PipeWire holds the PCM
	 * open (the normal state, since PipeWire adopts the card seconds after
	 * modprobe) and that thread sits there until PipeWire notices, with every
	 * other USB event on the hub queued behind it. The waiting is the defect,
	 * not the feature, and the comment that used to be here had it backwards.
	 *
	 * snd_card_free_when_closed() does the half that must be synchronous and
	 * only that: snd_card_disconnect() marks the card dead, stops any running
	 * substream -- which issues our trigger STOP synchronously, so the
	 * flush_work() below still has something definite to wait for -- and swaps
	 * the fops of every open file, so no new open() or trigger can reach us.
	 * The card itself is freed when its last descriptor closes.
	 *
	 * What keeps gb_dev alive until then is the reference gb_alsa_register()
	 * took, dropped by gb_snd_private_free(). Switching to _when_closed()
	 * WITHOUT that reference would turn this fix into a fresh use-after-free of
	 * gb_dev, which is why defects B and J are one change and not two. */
	snd_card_free_when_closed(card);

	/* Then drain the worker and drop the endpoint reference if it holds one, or
	 * disconnect would leave the stream up for ever. Still correct after the
	 * disconnect above, and now also backed by the monotonicity rule: a trigger
	 * that queues the work after this flush can only take the stream down, and
	 * gb_snd_private_free() cancel_work_syncs before gb_dev's reference goes. */
	WRITE_ONCE(gb->pcm_want_stream, false);
	flush_work(&gb->pcm_work);
	if (gb->pcm_holds_stream) {
		gb_stream_down(gb);
		gb->pcm_holds_stream = false;
	}

	v4l2_device_put(&gb->v4l2_dev);
}
