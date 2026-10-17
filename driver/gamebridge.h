/* SPDX-License-Identifier: GPL-2.0 */
/* Adaptec GameBridge AVC-1410 / 1510 -- shared definitions.
 *
 * The device is a Cypress EZ-USB FX2LP bridging a Conexant CX25840 analogue
 * video decoder, with an Xceive XC2028 tuner and an AK5357 audio ADC also on
 * the I2C bus. It ships no firmware: the FX2's EEPROM supplies a VID/PID and
 * nothing else, so the 8051 boots held in reset and the host must upload an
 * image before the device is anything at all.
 */
#ifndef GAMEBRIDGE_H
#define GAMEBRIDGE_H

#include <linux/usb.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-dev.h>
#include <media/videobuf2-v4l2.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <linux/workqueue.h>

#include "gb-bt656.h"

#define GB_VENDOR_ADAPTEC	0x03f3
#define GB_PRODUCT_LOADER	0x009b	/* empty 8051, generic Cypress descriptors */
#define GB_PRODUCT_CAPTURE	0x009c	/* firmware running, the real device       */

/* 03f3:0099 / 03f3:009a are the same loader/capture pair for the older
 * AVC-1400/1500 -- both INFs say so. */

enum gb_model {
	GB_LOADER,
	GB_CAPTURE,
};

/* ---------------------------------------------------------------- FX2LP --- */

#define FX2_REQ_FIRMWARE_LOAD	0xa0	/* the built-in "write to RAM" vendor request */
#define FX2_REG_CPUCS		0xe600	/* bit 0 is 8051RES; 1 holds the core in reset */
#define FX2_RAM_SIZE		0x4000	/* 16 KB of internal RAM on an FX2LP          */
#define FX2_LOAD_CHUNK		1024

/* The image is extracted from a copyrighted Adaptec driver binary and cannot be
 * redistributed. The module must therefore fail cleanly and say how to build it,
 * rather than leaving the device at 009b looking like a driver bug. */
#define GB_FIRMWARE_NAME	"gamebridge/gb-loader-fw.bin"

/* ------------------------------------------------------------ endpoints --- */

#define GB_EP_CMD	0x01	/* OUT: commands                                */
#define GB_EP_STATUS	0x81	/* IN:  command replies                         */
#define GB_EP_VIDEO	0x82	/* IN:  video, isochronous at alts 1..5         */

/* EP 0x01 and 0x81 are BULK at alt 0 and INTERRUPT at alts 1..5. This is not a
 * detail. Assuming bulk is what once hid 97% of the Windows driver's traffic
 * from a usbmon parse, and it is also why the stop command must be sent after
 * switching back to alt 0 rather than before. Any
 * code that touches these endpoints must know the current altsetting. */
#define GB_ALT_IDLE	0
#define GB_ALT_STREAM	1

/* ------------------------------------------------------------- commands --- */

#define GB_CMD_I2C_WRITE	0x08	/* 08 <addr7> <len> <payload...>        */
#define GB_CMD_I2C_XFER		0x09	/* 09 <wcount> <rcount> <addr7> <data>  */
#define GB_CMD_START		0x36	/* 36 <isoinpkts> <apply>               */
#define GB_CMD_STOP		0x37
#define GB_CMD_DECODER_RESET	0x83	/* low-high pulse on an FX2 GPIO        */

/* 0x87 is a board reset, and it is the ONLY way to reach the tuner's reset line.
 *
 * Read out of the 8051 at 0x0109: it clears port A, re-initialises port D to
 * 0x87 with OED 0x9f, strobes port A bit 3 three times, then clears port D bit 0
 * and pulses bit 7 LOW-delay-HIGH before restoring the LED bits. Port D bit 1 is
 * already known to be the CX25840's reset (command 0x83), so bit 7 is the other
 * resettable part on this board: the XC2028.
 *
 * The parameterised GPIO command 0x0e cannot do this. Its handler at 0x0379
 * bounds-checks the bit number to 3..6 (`SUBB A,#0x03 / JC` then
 * `SETB C / SUBB A,#0x06 / JNC`), and of those only 3 and 4 are outputs under
 * OED 0x9f -- so 0x0e is an LED command and bit 7 is out of its reach. */
#define GB_CMD_BOARD_RESET	0x87
#define GB_CMD_GPIO		0x0e	/* 0e <bit 3..6> <0|1> -- the LEDs      */

#define GB_I2C_ADDR_DECODER	0x44	/* CX25840  */
#define GB_I2C_ADDR_EEPROM	0x50	/* 24C02    */
#define GB_I2C_ADDR_TUNER	0x61	/* XC2028   */

/* Reply byte 0 from a command: */
#define GB_I2C_ACK	0x08
#define GB_I2C_NACK	0x07

/* --------------------------------------------------------------- infrared --- */

/* 0x85 is the IR receiver, not the "counter" gb_send_prologue() calls it. It
 * answers five bytes: the ACK/NACK flag above, then a decoded NEC frame that is
 * only valid on ACK. See gb-ir.c -- the ACK check is the whole feature, because
 * ignoring it re-reports the last key at the poll rate instead of failing. */
#define GB_CMD_IR_POLL		0x85
#define GB_IR_REPLY_LEN		5
#define GB_IR_REPEAT		0x80	/* set in byte 3 while a key is held */

/* --------------------------------------------------------------- device --- */

/* The longest command the firmware will accept is NOT known. What is known:
 * the FX2's EP1 buffers are 64 bytes, and the Windows driver demonstrably
 * sends 64-byte commands -- `08 <addr> <len>` followed by 61 payload bytes,
 * during the CX25840 audio microcode download. So 61 is the
 * arithmetic ceiling and is evidence-backed at that value, but it has never been
 * walked up from our side. */
#define GB_CMD_MAX		64
#define GB_I2C_MAX_LEN		61

/* 8 x 32 x 3030 = 776 KB in flight, about 32 ms of stream, a little under two
 * NTSC fields.
 *
 * Depth buys nothing here and the smallest workable depth is the right choice:
 * queue depths from 8x64 through 64x8 were measured in userspace and ALL gave
 * the same 21.36 MB/s and the same microframe loss. Since depth does not help
 * throughput, the latency budget decides, and this hardware exists to put a
 * console on a monitor. 8x32 is the smallest of the shapes that was tested. */
#define GB_ISOC_URBS		8
#define GB_ISOC_PACKETS		32

struct gb_buf {
	struct vb2_v4l2_buffer	vb;	/* MUST be first */
	struct list_head	list;
};

/* Only gb-ir.c needs the definition; a pointer is all the rest of the driver
 * sees, so <media/rc-core.h> stays out of every other translation unit. */
struct rc_dev;

struct gb_dev {
	struct usb_device	*udev;
	struct usb_interface	*intf;

	/* One lock for the whole command channel, held across the command AND its
	 * reply. Every command arms a reply on EP 0x81, and leaving one queued
	 * desynchronises everything that follows -- a driver that skips the drain
	 * on a write reads that stale reply as the answer to the next read. */
	struct mutex		cmd_lock;
	u8			*cmd_buf;	/* DMA-able scratch, under cmd_lock */

	struct i2c_adapter	i2c_adap;
	bool			i2c_registered;

	/* THE V4L2 DEVICE'S REFCOUNT OWNS THIS WHOLE STRUCT.
	 *
	 * struct video_device vdev and struct vb2_queue vb_vidq below are
	 * EMBEDDED here, and video_unregister_device() does not wait for open file
	 * handles. Unplug the device while ffplay holds /dev/videoN and the close()
	 * that follows still runs vb2_fop_release() -> mutex_lock(&gb->vb_lock) ->
	 * vb2_core_queue_release(&gb->vb_vidq) -- verified in the shipped
	 * videobuf2-v4l2.ko, where _vb2_fop_release() takes q->lock (q+0x20) and
	 * calls vb2_core_queue_release() whenever q->owner is NULL or is this file.
	 * So gb_dev's lifetime is this kref, NOT gb_disconnect()'s stack frame.
	 *
	 * gb_release() in gb-core.c is the release callback and the only place
	 * gb_dev is freed. Three references exist: v4l2_device_register()'s own,
	 * one __video_register_device() takes for the node (verified in the shipped
	 * videodev.ko: it does a kref_get on vdev->v4l2_dev immediately before
	 * device_register(), and v4l2_device_release() drops it after calling
	 * vdev->release), and one gb_alsa_register() takes for the ALSA card --
	 * which outlives disconnect too, because gb_alsa_unregister() uses
	 * snd_card_free_when_closed(). The pattern is usbtv's; see
	 * usbtv_release()/usbtv_video_free() in the kernel's
	 * drivers/media/usb/usbtv/usbtv-video.c.
	 *
	 * CONSEQUENCE: anything reachable after gb_disconnect() returns must not be
	 * torn down in it. That is why vb_lock is never mutex_destroy'd, why
	 * stream_lock and vdev_lock are destroyed in gb_release(), and why the
	 * device-facing teardown (the altsetting, GB_CMD_STOP, the i2c adapter)
	 * must all still happen inside disconnect, while gb->udev is valid. */
	struct v4l2_device	v4l2_dev;
	bool			v4l2_registered;
	struct v4l2_subdev	*sd;		/* the CX25840, or NULL */
	struct v4l2_ctrl_handler ctrl_handler;	/* merged from the subdev  */
	struct video_device	vdev;
	bool			vdev_registered;
	struct mutex		vdev_lock;	/* video_device serialisation */
	bool			vdev_lock_ready; /* mutex_init() has run       */

	/* Geometry follows the standard, and the interlace sign follows the parity
	 * of the chip's active-line count -- 487 on 525/60 is ODD, 576 on 625/50 is
	 * even. See driver/gb-bt656.c. */
	v4l2_std_id		norm;
	unsigned int		width, height;
	unsigned int		field_lines;
	bool			is_pal;
	unsigned int		input;
	/* QUERYCAP's bus_info, computed once at registration while gb->udev is
	 * certainly valid, so that the ioctl never dereferences udev: a QUERYCAP
	 * that passed the v4l2 core's video_is_registered() test could otherwise
	 * race disconnect clearing it. Same idea as ir_phys below. 32 bytes is
	 * sizeof(struct v4l2_capability.bus_info). */
	char			bus_info[32];

	/* ---- streaming ---- */
	struct vb2_queue	vb_vidq;
	bool			vb2_ready;	/* gb_vb2_init() completed   */
	struct mutex		vb_lock;	/* vb2 queue serialisation   */
	struct list_head	bufs;		/* queued, not yet filled    */
	spinlock_t		buf_lock;	/* taken from the URB handler */
	struct gb_buf		*cur_buf;	/* being filled now, or NULL */
	/* VIDEO IS STREAMING: the completion handler may adopt a queued buffer.
	 * Under buf_lock. Raised in gb_start_streaming(), lowered in
	 * gb_return_buffers_locked(). It is NOT the same as "the ring is armed"
	 * (that is `streaming` below): audio keeps the ring armed with video
	 * stopped, and vb2_core_streamon() calls buf_queue for every pre-queued
	 * buffer BEFORE ->start_streaming, so without this gate the handler
	 * could complete a buffer on a queue vb2 has not started yet. */
	bool			video_streaming;
	unsigned char		*scratch;	/* somewhere to park the assembler
						 * when no buffer is available  */
	struct urb		*urbs[GB_ISOC_URBS];
	/* THE RING IS ARMED. Read under stream_lock, where that is exactly what it
	 * means: set true after the last usb_submit_urb() and false before
	 * gb_free_urbs(), both under that lock. It is the predicate
	 * gb_return_buffers_locked() uses to decide whether there can be a
	 * completion handler to wait for: false means no handler can be running,
	 * because the ring was never armed (before the first gb_stream_up(), or
	 * after one that failed) or because a gb_free_urbs() kill has run.
	 * MEANINGLESS WITHOUT stream_lock -- it goes false before
	 * that kill, so a lockless reader can see false while a handler is still
	 * running, which is why gb_pcm_sync_stop() does not consult it. Note the
	 * limit of the claim: "armed", not "delivering" -- a resubmit that fails
	 * inside gb_isoc_complete() (counted in urb_errors, unless it is the
	 * -EPERM of a teardown or the -ENODEV of an unplug) silently drops that
	 * URB out of the ring, so this can be true of a short or dead ring. */
	bool			streaming;
	u32			sequence;

	struct gb_bt656		bt;

	/* Three counters, not one. They diagnose entirely different problems and
	 * merging them hides all three: pkt_drops is the device having nothing to
	 * send, short_frames is the assembler rejecting a frame built from
	 * implausible fields, and no_buffer is userspace not keeping up. */
	unsigned long		pkt_drops;
	unsigned long		no_buffer;
	unsigned long		urb_errors;
	unsigned long		audio_bytes;
	unsigned long		audio_calls;
	ktime_t			t_start;

	/* ---- the shared endpoint ----
	 * Video and audio come off ONE isochronous endpoint, because the audio
	 * rides in the video stream's horizontal blanking. So the URB ring is a
	 * shared resource with two independent users, and it is refcounted: either
	 * STREAMON or an ALSA trigger brings it up, and the last one down tears it
	 * off. Nobody reading this driver will expect sound to come off the video
	 * endpoint, which is exactly why it needs saying.
	 *
	 * One consequence has teeth: because the ring outlives video STREAMOFF,
	 * stopping video is NOT enough to stop the completion handler, and neither
	 * is stopping audio. So anything the handler dereferences is retired in
	 * two steps: publish the state that makes a new handler harmless, then
	 * wait an RCU grace period -- gb_isoc_complete() runs its packet loop
	 * inside rcu_read_lock() -- before freeing or handing anything back. The
	 * two places that do it are gb_return_buffers_locked() and
	 * gb_pcm_sync_stop(). Neither stops the ring; only teardown, in
	 * gb_free_urbs(), kills it.
	 *
	 * stream_lock is mutex_init'd in gb_probe_capture(), not in gb_vb2_init(),
	 * so that gb_release() can destroy it on every path including a probe that
	 * failed before the video node existed. */
	struct mutex		stream_lock;
	unsigned int		stream_users;
	/* THE DEVICE IS GOING AWAY: gb_stream_up() refuses every new user. Under
	 * stream_lock; set once, first thing in gb_disconnect(), never cleared.
	 * Testing gb->udev is not enough, because disconnect clears that only at
	 * its END: in between, vb2_fop_read() does not re-check registration after
	 * taking q->lock (videobuf2-v4l2.c), so a read() blocked across a sysfs
	 * unbind could start fileio after gb_v4l2_unregister() and bring the ring
	 * up on an interface being torn down. */
	bool			gone;

	/* ---- ALSA ---- */
	struct snd_card		*snd_card;
	struct snd_pcm_substream *pcm_sub;
	spinlock_t		pcm_lock;
	unsigned int		pcm_write;	/* byte offset into dma_area   */
	unsigned int		pcm_period;	/* bytes since period_elapsed  */
	bool			pcm_running;
	/* trigger() is called in ATOMIC context with the PCM stream lock held, and
	 * gb_stream_up() sleeps in half a dozen places -- mutex, usb_set_interface,
	 * vmalloc, usb_alloc_coherent, gb_cmd. So the trigger records what it wants
	 * and a work item reconciles it. Getting this wrong is a "scheduling while
	 * atomic" splat, which is what the first version produced. */
	struct work_struct	pcm_work;
	bool			pcm_want_stream;
	bool			pcm_holds_stream;
	int			dumped;
	unsigned long		fed_bytes;
	unsigned int		raw_off;
	unsigned long		len_hist[4];	/* 3030 / 2020 / 1010 / other */

	/* ---- infrared ----
	 * The receiver decodes NEC in hardware, so this is an RC_DRIVER_SCANCODE
	 * device with no raw path. There is no unsolicited delivery -- EP 0x81
	 * only ever answers a command -- so a work item polls 0x85 and the device
	 * is consequently never idle while the poller runs. */
	/* ---- tuner ----
	 * NULL until a tuner subdev attaches, and everything keys off that: the
	 * Tuner input row is not enumerated, V4L2_CAP_TUNER is not advertised, and
	 * the tuner ioctls return -ENOTTY. An input that claims to exist and
	 * produces nothing is worse than one honest about its scope. */
	struct v4l2_subdev	*tuner_sd;
	u32			tuner_freq;	/* 62.5 kHz units, as V4L2 counts */

	struct rc_dev		*rc;
	struct workqueue_struct	*ir_wq;		/* not system_wq: polls block */
	struct delayed_work	ir_work;
	bool			ir_polling;	/* cleared BEFORE cancelling */
	u32			ir_last_scancode;
	unsigned long		ir_events;
	unsigned long		ir_bad_frames;
	char			ir_phys[64];

	/* Isochronous geometry, read from the alt-1 descriptor rather than
	 * hardcoded. Never compute this from alt 0, where EP 0x82 is bulk 512:
	 * libusb_get_max_iso_packet_size() walks to the first matching address and
	 * answers for the wrong altsetting, which cost this project real time. */
	unsigned int		iso_packet_size;	/* bytes per microframe */
	unsigned int		iso_maxp;		/* per transaction      */
	unsigned int		iso_mult;		/* transactions/uframe  */

	/* Bookkeeping and logging ONLY. Nothing decides anything from this.
	 * gb_cmd() used to pick its transfer type from it and panicked a machine in
	 * the window where it disagrees with the hardware; it now
	 * reads the endpoint descriptor. Written under cmd_lock, together with the
	 * usb_set_interface() call it describes. */
	int			cur_alt;
};

/* gb-fw.c */
int gb_upload_firmware(struct usb_interface *intf);

/* gb-cmd.c -- the EP 0x01 OUT / EP 0x81 IN command channel.
 *
 * Sends one command and drains its reply, both under cmd_lock. `reply` may be
 * NULL, in which case the reply is still read and discarded -- it must be,
 * because the firmware always arms one. Returns the number of reply bytes
 * received, or negative errno. May sleep; never call it from a URB completion
 * handler. */
int gb_cmd(struct gb_dev *gb, const u8 *cmd, int cmdlen, u8 *reply, int replylen);

/* The five-step bring-up prologue the Windows driver opens with. Without it the
 * decoder returns 0x8a for every register and the cx25840 subdev cannot bind.
 * Must run before the I2C adapter is used for anything real. */
int gb_send_prologue(struct gb_dev *gb);

/* gb-i2c.c */
int gb_i2c_register(struct gb_dev *gb);
void gb_i2c_unregister(struct gb_dev *gb);

/* gb-video.c */
int gb_vb2_init(struct gb_dev *gb);
void gb_vb2_release(struct gb_dev *gb);

/* Bring the shared isochronous stream up or down. Refcounted: safe to call from
 * both the vb2 and the ALSA sides, in any order. */
int gb_stream_up(struct gb_dev *gb);
void gb_stream_down(struct gb_dev *gb);
/* Disconnect's backstop: the ring must be dead when disconnect returns (the
 * driver sets .soft_unbind). Warns if any user was left holding the stream. */
void gb_stream_kill(struct gb_dev *gb);

/* gb-alsa.c */
int gb_alsa_register(struct gb_dev *gb);
void gb_alsa_unregister(struct gb_dev *gb);
/* Only from inside gb_isoc_complete()'s RCU read-side section; see gb-alsa.c. */
void gb_alsa_write(struct gb_dev *gb, const unsigned char *pcm, int bytes);

/* gb-ir.c -- the infrared receiver on command 0x85.
 *
 * Registering starts a poller that runs until unregister; unregister is safe to
 * call on a device that never registered one.
 *
 * Optional: RHEL 9 builds its kernels without CONFIG_RC_CORE, and the remote is
 * the one feature a capture device can do without. Kbuild leaves gb-ir.o out
 * when the kernel has no rc-core, and these stubs take its place. */
#if IS_ENABLED(CONFIG_RC_CORE)
int gb_ir_register(struct gb_dev *gb);
void gb_ir_unregister(struct gb_dev *gb);

/* The remote's keymap is a GLOBAL rc-core registration, not a per-device one, so
 * it is done once from module init rather than from probe -- registering it per
 * device would add a duplicate list entry for every GameBridge attached. */
#define GB_IR_MAP_NAME	"rc-gamebridge"
int gb_ir_map_register(void);
void gb_ir_map_unregister(void);
#else
static inline int gb_ir_register(struct gb_dev *gb)
{
	dev_info(&gb->intf->dev, "no IR receiver: kernel built without CONFIG_RC_CORE\n");
	return 0;
}
static inline void gb_ir_unregister(struct gb_dev *gb) { }
static inline int gb_ir_map_register(void) { return 0; }
static inline void gb_ir_map_unregister(void) { }
#endif

/* gb-v4l2.c */
int gb_v4l2_register(struct gb_dev *gb);
void gb_v4l2_unregister(struct gb_dev *gb);
void gb_set_geometry(struct gb_dev *gb, v4l2_std_id norm);

#endif /* GAMEBRIDGE_H */
