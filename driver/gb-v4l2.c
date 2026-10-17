// SPDX-License-Identifier: GPL-2.0
/* The GameBridge's V4L2 node: standards, inputs, format and controls.
 *
 * Written before anything streamed, so that "the node is wrong" could be
 * debugged without URBs in flight. Streaming -- the vb2 queue and the URB
 * ring -- is gb-video.c.
 *
 * Almost nothing here decides anything. The CX25840 subdev owns the video
 * behaviour and has since 2005; this file is the translation layer between
 * V4L2's ioctls and v4l2_device_call_all(), plus the one thing the subdev cannot
 * know -- how many lines the bridge is going to assemble into a frame.
 */
#include <linux/module.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-event.h>
#include <media/v4l2-common.h>
#include <media/drv-intf/cx25840.h>
#include <media/tuner.h>          /* TUNER_XC2028, struct tuner_setup */
#include <media/dvb_frontend.h>   /* DVB_FRONTEND_COMPONENT_TUNER      */

#include "gamebridge.h"
#include "xc2028.h"	/* struct xc2028_ctrl -- see that file for why it is copied */
#include "gb-bt656.h"	/* GB_WIDTH, GB_LINE_BYTES -- one definition of the geometry */

/* An override for the CX25840 routing of the selected input. Which pin the
 * composite connector lands on was once unknown, and this parameter existed so
 * it could be swept against a live source. Adaptec's INF has since settled the
 * routing (the board table below), so -1, the default, uses that table and
 * this is a diagnostic only. */
static int vid_input = -1;
module_param(vid_input, int, 0644);
MODULE_PARM_DESC(vid_input,
		 "override the CX25840 routing for the selected input "
		 "(enum cx25840_video_input). -1, the default, uses the board table.");

/* The board's inputs, and the routing is NOT a guess: it comes out of Adaptec's
 * own INF, which carries the whole crossbar with its pin-type enum in a comment
 * above it. See docs/internals/vendor-inf-findings.md.
 *
 *     Pin 1  Composite in   InputMux 1
 *     Pin 2  S-Video in     InputMux 3 luma, 4 chroma
 *     Pin 0  Tuner In       InputMux 2       -- no subdev attached, so no row
 *
 * S-Video is the reason this was worth digging out rather than trying values.
 * Luma on mux 3 with chroma on mux 4 is 0x430, and that is not any of the
 * predefined combinations in cx25840.h -- CX25840_SVIDEO1/2/3 are 0x510, 0x620
 * and 0x730. Anyone adding S-Video by working through the three named constants
 * would have failed three times and concluded the input was dead.
 *
 * The audio route does not change between these two. The INF's RelatedPinIndex
 * ties Composite and S-Video both to pin 4, audio line in; only a tuner input
 * would switch to pin 3. So selecting an input here is purely a video routing
 * change, and the audio path is untouched.
 *
 * NOT YET TESTED AGAINST AN S-VIDEO SOURCE. The composite row is proven daily;
 * the S-Video row is documented-but-unverified, and is offered because the
 * evidence is the vendor's own table rather than an inference. If it turns out
 * wrong, the fix is one constant here. */
static const struct {
	const char	*name;
	u32		type;
	u32		route;
} gb_inputs[] = {
	{ "Composite", V4L2_INPUT_TYPE_CAMERA, CX25840_COMPOSITE1 },
	{ "S-Video",   V4L2_INPUT_TYPE_CAMERA,
	  CX25840_SVIDEO_LUMA3 | CX25840_SVIDEO_CHROMA4 },
	/* MUST STAY LAST. It is enumerated only when a tuner subdev has attached,
	 * by shortening the count -- so anything before it keeps its index whether
	 * the tuner is there or not. Adaptec's crossbar table gives the tuner
	 * InputMux 2, i.e. CX25840_COMPOSITE2, and ties it to audio pin 3 (tuner
	 * audio) rather than pin 4 (line in) -- see vendor-inf-findings.md. */
	{ "Tuner",     V4L2_INPUT_TYPE_TUNER,  CX25840_COMPOSITE2 },
};
#define GB_INPUT_TUNER	(ARRAY_SIZE(gb_inputs) - 1)

/* How many inputs to admit to. The tuner row exists in the table always and is
 * offered only when something is behind it. */
static unsigned int gb_num_inputs(struct gb_dev *gb)
{
	return gb->tuner_sd ? ARRAY_SIZE(gb_inputs) : ARRAY_SIZE(gb_inputs) - 1;
}

/* The routing for whichever input is selected, unless the module parameter is
 * overriding it. */
static u32 gb_input_route(struct gb_dev *gb)
{
	if (vid_input >= 0)
		return (u32)vid_input;
	return gb_inputs[gb->input].route;
}

void gb_set_geometry(struct gb_dev *gb, v4l2_std_id norm)
{
	gb->norm = norm;
	gb->is_pal = !(norm & V4L2_STD_525_60);
	/* 525/60 has 487 active lines and 625/50 has 576. The frame the bridge
	 * assembles is 480 or 576: the odd line on 525/60 is the half-line, and
	 * dropping it is correct. */
	gb->field_lines = gb->is_pal ? 288 : 240;
	gb->width  = GB_WIDTH;
	gb->height = gb->field_lines * 2;
}

static int gb_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct gb_dev *gb = video_drvdata(file);

	strscpy(cap->driver, "gamebridge", sizeof(cap->driver));
	strscpy(cap->card, "Adaptec GameBridge AVC-1410", sizeof(cap->card));
	/* From the copy made at registration, never from gb->udev: this ioctl
	 * can be running when disconnect clears it. See gb_dev.bus_info. */
	strscpy(cap->bus_info, gb->bus_info, sizeof(cap->bus_info));
	return 0;
}

/* One pixel format, no scaling, no cropping. UYVY is the only format string in
 * avcgbdr.sys, and the BT.656 byte order out of the CX25840 is Cb Y Cr Y --
 * which is UYVY as it stands, with no swap anywhere. */
static int gb_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_UYVY;
	return 0;
}

static void gb_fill_fmt(struct gb_dev *gb, struct v4l2_pix_format *pix)
{
	pix->width        = gb->width;
	pix->height       = gb->height;
	pix->pixelformat  = V4L2_PIX_FMT_UYVY;
	pix->field        = V4L2_FIELD_INTERLACED;
	pix->bytesperline = gb->width * 2;
	pix->sizeimage    = pix->bytesperline * gb->height;
	pix->colorspace   = V4L2_COLORSPACE_SMPTE170M;
}

static int gb_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	gb_fill_fmt(video_drvdata(file), &f->fmt.pix);
	return 0;
}

/* TRY_FMT clamps to the one size the current standard allows rather than
 * accepting a request and silently ignoring it. An application that asks for
 * 640x480 gets told 720x480, which is the honest answer and is what lets it
 * adapt. */
static int gb_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	gb_fill_fmt(video_drvdata(file), &f->fmt.pix);
	return 0;
}

static int gb_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	return gb_try_fmt(file, priv, f);
}

static int gb_enum_framesizes(struct file *file, void *priv,
			      struct v4l2_frmsizeenum *fsize)
{
	struct gb_dev *gb = video_drvdata(file);

	if (fsize->index || fsize->pixel_format != V4L2_PIX_FMT_UYVY)
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width  = gb->width;
	fsize->discrete.height = gb->height;
	return 0;
}

/* ------------------------------------------------------------ standards --- */

static int gb_g_std(struct file *file, void *priv, v4l2_std_id *norm)
{
	*norm = ((struct gb_dev *)video_drvdata(file))->norm;
	return 0;
}

/* The decoder's own detector, and this is worth having early rather than late.
 * This project once spent a whole episode chasing a PAL-M hypothesis
 * while register 0x040d said NTSC-M all along -- the shear being blamed on the
 * standard was this project's own frame assembly. A working QUERYSTD would have
 * ended it in one command. */
static int gb_querystd(struct file *file, void *priv, v4l2_std_id *norm)
{
	struct gb_dev *gb = video_drvdata(file);

	*norm = V4L2_STD_ALL;
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, querystd, norm);
	return 0;
}

static int gb_s_std(struct file *file, void *priv, v4l2_std_id norm)
{
	struct gb_dev *gb = video_drvdata(file);
	int ret;

	/* Setting the standard that is ALREADY selected is a no-op and must
	 * succeed even with buffers allocated -- and even mid-stream.
	 * v4l2-compliance's testCanSetSameTimings checks exactly this, and it is
	 * the one thing this driver failed on its first compliance run. So this
	 * fast path stays ahead of every busy test below.
	 *
	 * Only a CHANGE is refused, because a change alters the frame height and
	 * both the allocated buffers and the assembler are sized for the old one. */
	if (norm == gb->norm)
		return 0;

	/* THE STREAM BEING UP AT ALL REFUSES A CHANGE -- by any user, video or
	 * audio. vb2_is_busy() alone is not that test, and the gap between the
	 * two was a write past the end of a videobuf2 plane, reachable from
	 * ordinary userspace with no misbehaving hardware involved.
	 *
	 * The assembler latches its geometry once, in the gb_bt656_init() call
	 * inside gb_stream_up()'s FIRST-user branch. Later users deliberately do
	 * not re-run it: resetting the assembler's state underneath a user that
	 * is already running is precisely what the refcount exists to prevent
	 * (see the comment on gb_stream_up() in gb-video.c). The geometry
	 * therefore has to be frozen for as long as anyone holds the stream, and
	 * the only place that can be enforced is here.
	 *
	 * How the overflow went, before this check existed:
	 *
	 *   1. arecord starts audio. gb_stream_up() runs as the first user, so
	 *      gb_bt656_init() latches the current standard -- say PAL, 576
	 *      lines.
	 *   2. No video buffers have been allocated, so vb2_is_busy() is false.
	 *      VIDIOC_S_STD to NTSC is accepted and gb_set_geometry() drops
	 *      gb->height to 480, gb->field_lines to 240.
	 *   3. Video starts. gb_queue_setup() sizes the plane from the NEW
	 *      geometry: 720 * 2 * 480 = 691200 bytes. gb_stream_up() returns
	 *      early at the refcount, so the assembler is still on PAL.
	 *   4. At the next frame boundary gb_bt656_blank(), called from
	 *      gb_bt656_feed() at gb-bt656.c:279, blanks through the ASSEMBLER's
	 *      height: 720 * 2 * 576 = 829440 bytes into a 691200-byte plane,
	 *      138240 bytes past the end of it -- and the rows the assembler then
	 *      writes go past it the same way. (When this was found the first
	 *      blank came from gb_bt656_retarget(), since renamed
	 *      gb_bt656_publish() and no longer blanking; the overflow does not
	 *      depend on which function issues it.)
	 *
	 * HELD ACROSS THE WHOLE CHANGE, not just the test. stream_lock is taken
	 * here and released only after gb_set_geometry(), because the first
	 * version of this fix read stream_users under the lock, RELEASED it, and
	 * then spent milliseconds in cx25840_s_std()'s I2C sequence before
	 * changing the geometry. An ALSA trigger takes neither vdev_lock nor
	 * vb_lock, so gb_pcm_work() could become the FIRST stream user inside that
	 * window, latch the OLD geometry in gb_bt656_init(), and the overflow
	 * above came straight back. A check whose lock is released before the
	 * thing it protects is a snapshot, not a guarantee. Found by adversarial
	 * review on 2026-10-10; the snapshot version shipped in no release.
	 *
	 * Holding it means gb_stream_up() -- from STREAMON or from gb_pcm_work() --
	 * blocks until the new geometry is in place, and then latches the new one.
	 * The cost is that an audio start waits for an S_STD in flight: a few
	 * milliseconds, on an ioctl nobody issues mid-session.
	 *
	 * NO INVERSION -- but NOT for the reason an earlier version of this
	 * comment gave. It claimed the order vdev_lock -> stream_lock "is the
	 * order STREAMON already takes them in". STREAMON DOES NOT TAKE vdev_lock.
	 * This driver uses the SPLIT-LOCK arrangement: it sets both gb->vdev.lock
	 * and gb->vb_vidq.lock, and v4l2_ioctl_get_lock() hands an INFO_FL_QUEUE
	 * ioctl -- REQBUFS, QBUF, DQBUF, STREAMON, STREAMOFF and the rest --
	 * vdev->queue->lock, which is gb->vb_lock, and everything else, S_STD
	 * included, vdev->lock. One or the other, never both.
	 *
	 * AND THAT SPLIT IS WHY THIS FUNCTION ALSO TAKES vb_lock. Under vdev_lock
	 * alone, the vb2_is_busy() test below froze nothing: REQBUFS, QBUF and
	 * STREAMON run under vb_lock and never touch stream_lock, so a second
	 * thread could allocate and queue buffers while this one sat in
	 * cx25840_s_std()'s I2C sequence. gb_queue_setup() and gb_buf_prepare()
	 * then sized and checked them against the OLD height; STREAMON's
	 * gb_buf_queue() blanked them through the NEW one, 720*2*576 into a
	 * 691200-byte plane, and the assembler went on to write 576 rows into
	 * them. The same overflow as the one above, by a second door. Found by
	 * a security review on 2026-10-16, before the repository was public.
	 * Holding vb_lock makes the busy test mean what it says: no buffer can
	 * be allocated, prepared or queued until the new geometry is in place.
	 *
	 * So the orders in play are
	 *     vdev_lock -> vb_lock -> stream_lock   (here, S_STD)
	 *     vb_lock   -> stream_lock   (STREAMON/STREAMOFF -> gb_stream_up(),
	 *                                 gb_stop_streaming())
	 * vdev_lock is taken by the V4L2 core on ioctl entry, and by
	 * gb_disconnect() and gb_fop_release(), always with nothing of ours
	 * held; nothing in this driver takes vdev_lock or vb_lock while
	 * holding stream_lock, and nothing takes vdev_lock while holding vb_lock.
	 * There is no cycle. A reader blocked in DQBUF or read() does not hold
	 * vb_lock while it waits (wait_prepare on 6.x, the vb2 core on 7.x), so
	 * an S_STD is not stuck behind it.
	 *
	 * Holding stream_lock across the subdev call extends the chain: s_std
	 * reaches cmd_lock through the I2C adapter, giving
	 *     vdev_lock -> vb_lock -> stream_lock -> i2c adapter lock -> cmd_lock
	 * which is the order gb_stream_up() already uses when it configures the
	 * decoder (stream_lock, then I2C, then cmd_lock). Checked on 2026-10-10:
	 * every taker of stream_lock -- gb_pcm_work(), STREAMON/STREAMOFF under
	 * vb_lock, disconnect -- holds
	 * neither cmd_lock nor the adapter lock when it does, and the IR poller
	 * takes cmd_lock alone. If that ever changes, this is where it deadlocks. */
	mutex_lock(&gb->vb_lock);
	mutex_lock(&gb->stream_lock);
	if (gb->stream_users) {
		ret = -EBUSY;
		goto out;
	}

	/* And buffers that are merely ALLOCATED are sized for the old height as
	 * well, so a busy queue still refuses even with the stream down. The two
	 * tests cover different states and neither implies the other. */
	if (vb2_is_busy(&gb->vb_vidq)) {
		ret = -EBUSY;
		goto out;
	}

	/* Let the subdev reject what it cannot do rather than second-guessing it
	 * here. It has known which standards a CX2584x supports since 2005. */
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_std, norm);

	/* The geometry must follow, and this is the bridge's job alone: the subdev
	 * configures the chip's active window, but only we know that the assembler
	 * weaves two fields into one frame and how tall that frame is.
	 *
	 * This is also where the driver beats the userspace replay. gbcap has to
	 * write one standard's captured constants into 0x0470-0x047c, so PAL keeps
	 * NTSC's 244-line window and leaves noise outside the picture. The subdev
	 * COMPUTES those from the standard, so the kernel driver is expected to
	 * produce a cleaner PAL picture than the replay can, not merely an
	 * equivalent one. */
	gb_set_geometry(gb, norm);
	ret = 0;
out:
	mutex_unlock(&gb->stream_lock);
	mutex_unlock(&gb->vb_lock);
	return ret;
}

/* ------------------------------------------------------- tuner attachment ---
 *
 * OFF BY DEFAULT, and deliberately so. Every other feature here was verified
 * against hardware before its default flipped; this one has never seen an RF
 * signal, and its firmware upload is 66 KB pushed over an I2C bridge whose write
 * ceiling has never been walked up. Audio spent a day as opt-in for exactly this
 * reason. Turn it on with tuner=1.
 */
static bool tuner;
module_param(tuner, bool, 0444);
MODULE_PARM_DESC(tuner,
		 "attach the XC2028 TV tuner (default off -- UNVERIFIED, no RF "
		 "source has ever been connected). Needs xc3028-v27.fw.");


/* THE TUNER CANNOT WORK WITHOUT THIS. xc2028's firmware loader does
 *
 *     rc = do_tuner_callback(fe, XC2028_TUNER_RESET, 0);
 *     if (rc < 0) goto fail;
 *
 * before every base-firmware load, and do_tuner_callback() returns -EINVAL when
 * the bridge supplied no callback. So a bridge without one cannot load firmware
 * into an XC2028 at all -- it is not an optional refinement.
 *
 * The reset itself is command 0x87. Read out of the 8051: it pulses port D bit 7
 * low-then-high, and bit 7 is the only reset line on this board that is not the
 * decoder's (bit 1, command 0x83). The parameterised GPIO command 0x0e bounds its
 * bit number to 3..6 and cannot reach bit 7, so 0x87 is the only route.
 *
 * UNVERIFIED that bit 7 is the tuner's reset. It is inference from the firmware
 * plus elimination, not measurement, and it is the most likely thing on this
 * page to be wrong. If the tuner never leaves XC2028_NO_FIRMWARE, suspect this
 * first.
 *
 * Note 0x87 is heavier than a tuner reset: it also re-initialises port D, strobes
 * port A bit 3 and resets the LED bits. That is tolerable because firmware load
 * happens once, before streaming, but it is why this is not called casually.
 */
static int gb_tuner_callback(void *dev, int component, int cmd, int arg)
{
	static const u8 board_reset[] = { GB_CMD_BOARD_RESET };
	struct gb_dev *gb = dev;
	int ret;

	if (!gb)
		return -EINVAL;
	if (component != DVB_FRONTEND_COMPONENT_TUNER)
		return -EINVAL;

	switch (cmd) {
	case XC2028_TUNER_RESET:
		ret = gb_cmd(gb, board_reset, sizeof(board_reset), NULL, 0);
		if (ret < 0) {
			dev_warn(&gb->intf->dev,
				 "tuner reset (cmd %02x) failed: %d\n",
				 GB_CMD_BOARD_RESET, ret);
			return ret;
		}
		/* The handler's own delays are in the 8051; give the part time to
		 * come out of reset before the loader starts pushing blocks. */
		usleep_range(10000, 12000);
		return 0;
	case XC2028_RESET_CLK:
	case XC2028_I2C_FLUSH:
		/* Nothing on this bridge corresponds to either. Returning 0 says
		 * "done" rather than "cannot", which is what the loader needs --
		 * an error here aborts the firmware load. */
		return 0;
	default:
		return -EINVAL;
	}
}

static void gb_tuner_attach(struct gb_dev *gb)
{
	struct tuner_setup setup = {
		.addr		= GB_I2C_ADDR_TUNER,
		.type		= TUNER_XC2028,
		.mode_mask	= T_ANALOG_TV,
		.config		= NULL,
		.tuner_callback	= gb_tuner_callback,
	};
	struct v4l2_subdev *sd;

	if (!tuner)
		return;

	/* Bound through tuner-core ("tuner"), NOT by instantiating an xc2028 i2c
	 * client directly: tuner-core is what turns TUNER_XC2028 into an
	 * xc2028_attach() with the right xc2028_config, and it is what the
	 * g_tuner/s_frequency subdev ops land on. It passes no xc2028_ctrl, so we
	 * get the driver's own defaults -- firmware "xc3028-v27.fw" and
	 * max_len 13, comfortably under this bridge's evidence-backed 61-byte
	 * ceiling. That is why the I2C write ceiling does NOT block the tuner. */
	sd = v4l2_i2c_new_subdev(&gb->v4l2_dev, &gb->i2c_adap,
				 "tuner", GB_I2C_ADDR_TUNER, NULL);
	if (!sd) {
		dev_warn(&gb->intf->dev,
			 "tuner subdev did not bind at 0x%02x -- is the `tuner` "
			 "module available?\n", GB_I2C_ADDR_TUNER);
		return;
	}

	/* Tells tuner-core which part it is and hands over the callback. Until
	 * this lands, tuner-core has a client and no idea what to do with it. */
	v4l2_subdev_call(sd, tuner, s_type_addr, &setup);

	/* AND THIS IS NOT OPTIONAL EITHER. The XC2028 requests its firmware from
	 * exactly one place -- xc2028_set_config() -- and xc2028_attach() reaches
	 * it only `if (cfg->ctrl)`. tuner-core attaches with cfg.ctrl = NULL, so
	 * without this call the part never leaves XC2028_NO_FIRMWARE:
	 * check_firmware() returns -EAGAIN out of check_device_status() before it
	 * logs a thing. Measured exactly that way on the EHCI test machine before this existed.
	 *
	 * max_len is the reason the I2C write ceiling does not gate the tuner. The
	 * driver's own default is 13 and we keep it: comfortably under this
	 * bridge's evidence-backed 61-byte limit, and raising it is an
	 * optimisation to make once something works, not before. */
	{
		struct xc2028_ctrl ctrl = {
			.fname		= XC2028_DEFAULT_FIRMWARE,
			.max_len	= 13,
			.demod		= XC3028_FE_DEFAULT,
			.type		= XC2028_AUTO,
			/* Skips the firmware-version and hwmodel comparisons after
			 * the read-back. Harmless when the reads are real and
			 * required when they are faked -- this bridge cannot read
			 * the tuner, so nothing that comes back deserves to be
			 * compared against anything. */
			.read_not_reliable = 1,
		};
		struct v4l2_priv_tun_config cfg = {
			.tuner	= TUNER_XC2028,
			.priv	= &ctrl,
		};

		v4l2_subdev_call(sd, tuner, s_config, &cfg);
	}

	gb->tuner_sd = sd;
	dev_info(&gb->intf->dev,
		 "tuner subdev bound: %s (XC2028 at 0x%02x) -- UNVERIFIED path\n",
		 sd->name, GB_I2C_ADDR_TUNER);
}

/* --------------------------------------------------------------- inputs --- */

static int gb_enum_input(struct file *file, void *priv, struct v4l2_input *inp)
{
	struct gb_dev *gb = video_drvdata(file);

	/* Composite and S-Video always; Tuner only once a tuner subdev has
	 * attached. See gb_num_inputs(). */
	if (inp->index >= gb_num_inputs(gb))
		return -EINVAL;

	strscpy(inp->name, gb_inputs[inp->index].name, sizeof(inp->name));
	inp->type = gb_inputs[inp->index].type;
	inp->std  = gb->vdev.tvnorms;
	inp->capabilities = V4L2_IN_CAP_STD;
	/* A tuner input must name the tuner it belongs to. We have exactly one. */
	if (inp->type == V4L2_INPUT_TYPE_TUNER)
		inp->tuner = 0;

	/* Report signal presence honestly rather than always claiming a lock. */
	inp->status = 0;
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, g_input_status, &inp->status);
	return 0;
}

static int gb_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = ((struct gb_dev *)video_drvdata(file))->input;
	return 0;
}

static int gb_s_input(struct file *file, void *priv, unsigned int i)
{
	struct gb_dev *gb = video_drvdata(file);

	if (i >= gb_num_inputs(gb))
		return -EINVAL;
	gb->input = i;
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_routing,
			     gb_input_route(gb), 0, 0);
	return 0;
}

/* ----------------------------------------------------------------- tuner ---
 *
 * All four return -ENOTTY while nothing is attached, which is what V4L2 expects
 * from a device without a tuner and is what v4l2-compliance checks for. They are
 * present unconditionally so that attaching a tuner later changes nothing here.
 */

static int gb_g_tuner(struct file *file, void *priv, struct v4l2_tuner *t)
{
	struct gb_dev *gb = video_drvdata(file);

	if (!gb->tuner_sd || t->index)
		return -ENOTTY;
	strscpy(t->name, "Television", sizeof(t->name));
	t->type = V4L2_TUNER_ANALOG_TV;
	t->capability = V4L2_TUNER_CAP_NORM;
	v4l2_device_call_all(&gb->v4l2_dev, 0, tuner, g_tuner, t);
	return 0;
}

static int gb_s_tuner(struct file *file, void *priv, const struct v4l2_tuner *t)
{
	struct gb_dev *gb = video_drvdata(file);

	if (!gb->tuner_sd || t->index)
		return -ENOTTY;
	v4l2_device_call_all(&gb->v4l2_dev, 0, tuner, s_tuner, t);
	return 0;
}

static int gb_g_frequency(struct file *file, void *priv,
			  struct v4l2_frequency *f)
{
	struct gb_dev *gb = video_drvdata(file);

	if (!gb->tuner_sd || f->tuner)
		return -ENOTTY;
	f->type = V4L2_TUNER_ANALOG_TV;
	f->frequency = gb->tuner_freq;
	v4l2_device_call_all(&gb->v4l2_dev, 0, tuner, g_frequency, f);
	return 0;
}

static int gb_s_frequency(struct file *file, void *priv,
			  const struct v4l2_frequency *f)
{
	struct gb_dev *gb = video_drvdata(file);

	if (!gb->tuner_sd || f->tuner)
		return -ENOTTY;
	v4l2_device_call_all(&gb->v4l2_dev, 0, tuner, s_frequency, f);
	gb->tuner_freq = f->frequency;
	return 0;
}

/* ---------------------------------------------------------------- parms --- */

static int gb_g_parm(struct file *file, void *priv, struct v4l2_streamparm *p)
{
	struct gb_dev *gb = video_drvdata(file);

	if (p->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	p->parm.capture.readbuffers = 2;
	p->parm.capture.timeperframe.numerator   = gb->is_pal ? 1 : 1001;
	p->parm.capture.timeperframe.denominator = gb->is_pal ? 25 : 30000;
	return 0;
}

/* Dump both halves: our merged control values, and then the subdev's own view of
 * the world. The CX25840's log_status reports the standard it has DETECTED, the
 * input it is routed to and the state of its audio path -- which is exactly the
 * set of questions this project kept answering by guesswork. It costs one
 * command and it is the first thing to run when a picture looks wrong. */
static int gb_log_status(struct file *file, void *priv)
{
	struct gb_dev *gb = video_drvdata(file);

	v4l2_ctrl_handler_log_status(&gb->ctrl_handler, "gamebridge");
	v4l2_device_call_all(&gb->v4l2_dev, 0, core, log_status);
	return 0;
}

static const struct v4l2_ioctl_ops gb_ioctl_ops = {
	.vidioc_querycap		= gb_querycap,
	.vidioc_enum_fmt_vid_cap	= gb_enum_fmt,
	.vidioc_g_fmt_vid_cap		= gb_g_fmt,
	.vidioc_s_fmt_vid_cap		= gb_s_fmt,
	.vidioc_try_fmt_vid_cap		= gb_try_fmt,
	.vidioc_enum_framesizes		= gb_enum_framesizes,
	.vidioc_g_std			= gb_g_std,
	.vidioc_s_std			= gb_s_std,
	.vidioc_querystd		= gb_querystd,
	.vidioc_enum_input		= gb_enum_input,
	.vidioc_g_input			= gb_g_input,
	.vidioc_s_input			= gb_s_input,
	.vidioc_g_tuner			= gb_g_tuner,
	.vidioc_s_tuner			= gb_s_tuner,
	.vidioc_g_frequency		= gb_g_frequency,
	.vidioc_s_frequency		= gb_s_frequency,
	.vidioc_g_parm			= gb_g_parm,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
	.vidioc_log_status		= gb_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

/* close() under vdev_lock, so it can never land inside gb_disconnect()'s
 * teardown. With a control event subscribed, close() unlinks the subscription
 * through OUR handler from a control the cx25840 subdev OWNS. Between
 * disconnect freeing our handler and v4l2_device_unregister() freeing the
 * subdev's, it would find nothing, skip the unlink, and kvfree a subscription
 * still on that control's list -- which the subdev's free then walked: a
 * use-after-free (third whole-repo review, 2026-10-16). Under the lock it
 * runs wholly before that teardown or wholly after it, and after it the
 * subdev's free has already unlinked the subscription.
 *
 * ONLY THE UNLINK is under the lock, not the whole of vb2_fop_release().
 * The unlink is the only part that touches the control handlers; holding
 * vdev_lock across the queue release as well made close() wait on vb_lock
 * with vdev_lock held, so one stuck stream stop (a wedged device's USB
 * timeouts, or a read() whose copy_to_user() faults on a FUSE page that never
 * comes) parked every other file's non-queue ioctls behind it. Narrowed on
 * the fourth review's advice, 2026-10-17. vb2_fop_release() unsubscribes
 * again through v4l2_fh_exit(); by then there is nothing left to unlink.
 *
 * The V4L2 core calls ->release without vdev->lock (v4l2_release(), v6.16 and
 * 5.14), and gb_dev cannot be freed under us: the core drops the file's
 * video_device reference only after ->release returns. */
static int gb_fop_release(struct file *file)
{
	struct gb_dev *gb = video_drvdata(file);

	mutex_lock(&gb->vdev_lock);
	v4l2_event_unsubscribe_all(file->private_data);
	mutex_unlock(&gb->vdev_lock);
	return vb2_fop_release(file);
}

static const struct v4l2_file_operations gb_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= gb_fop_release,
	.read		= vb2_fop_read,
	.mmap		= vb2_fop_mmap,
	.poll		= vb2_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
};

int gb_v4l2_register(struct gb_dev *gb)
{
	int ret;

	mutex_init(&gb->vdev_lock);
	gb->vdev_lock_ready = true;
	gb_set_geometry(gb, V4L2_STD_NTSC_M);
	gb->input = 0;
	usb_make_path(gb->udev, gb->bus_info, sizeof(gb->bus_info));

	/* The bridge has NO controls of its own. Everything a user can turn is on
	 * the CX25840 -- brightness, contrast, saturation, hue -- and the subdev
	 * already registered them in its own handler. Merging that handler here and
	 * pointing the video_device at it means the V4L2 core services QUERYCTRL,
	 * G_CTRL and S_CTRL with no vidioc_* callbacks from us at all. */
	ret = gb_vb2_init(gb);
	if (ret) {
		dev_err(&gb->intf->dev, "vb2_queue_init failed: %d\n", ret);
		/* Delegate, do NOT hand-roll. See the note above err_ctrl. */
		gb_v4l2_unregister(gb);
		return ret;
	}

	v4l2_ctrl_handler_init(&gb->ctrl_handler, 0);
	if (gb->sd && gb->sd->ctrl_handler) {
		ret = v4l2_ctrl_add_handler(&gb->ctrl_handler,
					    gb->sd->ctrl_handler, NULL, true);
		if (ret) {
			dev_err(&gb->intf->dev,
				"cannot merge the subdev's controls: %d\n", ret);
			goto err_ctrl;
		}
	}
	if (gb->ctrl_handler.error) {
		ret = gb->ctrl_handler.error;
		goto err_ctrl;
	}
	gb->v4l2_dev.ctrl_handler = &gb->ctrl_handler;

	strscpy(gb->vdev.name, "gamebridge", sizeof(gb->vdev.name));
	gb->vdev.v4l2_dev     = &gb->v4l2_dev;
	gb->vdev.fops         = &gb_fops;
	gb->vdev.ioctl_ops    = &gb_ioctl_ops;
	/* The core's own way of saying "this struct is embedded, do not free it",
	 * and verbatim what usbtv does (usbtv_video_init() in the kernel's
	 * drivers/media/usb/usbtv/usbtv-video.c). It replaces an empty stub whose
	 * comment claimed disconnect() owns gb_dev -- the wrong ownership model,
	 * and the whole of defect B.
	 *
	 * What actually frees gb_dev is gb_release() in gb-core.c, the
	 * v4l2_device release callback. The sequence is not obvious and is worth
	 * naming: video_unregister_device() -> the last put_device() on
	 * vdev->dev -> v4l2_device_release(), which calls vdev->release (this
	 * no-op) and then, because gb->v4l2_dev.release is set, drops the
	 * v4l2_device reference __video_register_device() took for the node.
	 * Verified in the shipped videodev.ko. */
	gb->vdev.release      = video_device_release_empty;
	gb->vdev.ctrl_handler = &gb->ctrl_handler;
	gb->vdev.lock         = &gb->vdev_lock;
	/* Let the subdev reject standards it cannot do rather than filtering here. */
	gb->vdev.tvnorms      = V4L2_STD_ALL;
	gb->vdev.queue        = &gb->vb_vidq;
	/* Attach before device_caps is computed: V4L2_CAP_TUNER must not be
	 * advertised unless a tuner really is behind it, and v4l2-compliance
	 * checks that the capability, the input type and the ioctls all agree. */
	gb_tuner_attach(gb);

	gb->vdev.device_caps  = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
				V4L2_CAP_READWRITE;
	if (gb->tuner_sd)
		gb->vdev.device_caps |= V4L2_CAP_TUNER;
	video_set_drvdata(&gb->vdev, gb);

	ret = video_register_device(&gb->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&gb->intf->dev, "video_register_device failed: %d\n", ret);
		goto err_ctrl;
	}
	gb->vdev_registered = true;

	/* Bring the decoder into a known state now that something can ask it
	 * questions: select the standard we claim in G_FMT, and route the input.
	 *
	 * The V4L2_STD_UNKNOWN call first is NOT redundant, and leaving it out is a
	 * bug we shipped for about ten minutes.
	 *
	 * cx25840_probe() sets its own state->std to V4L2_STD_NTSC_M, and
	 * cx25840_s_std() early-outs when the requested standard equals the cached
	 * one. So asking for NTSC-M here writes nothing at all: the subdev believes
	 * the chip is in NTSC-M, the chip is still in its power-on auto-detect mode,
	 * and our G_STD cheerfully reports NTSC-M to userspace. Caught by
	 * VIDIOC_LOG_STATUS, which asks the chip rather than the driver and answered
	 * "Specified standard: automatic detection".
	 *
	 * Setting UNKNOWN first makes the second call a real change, so the register
	 * write actually happens and the driver's model matches the hardware. Two
	 * extra I2C writes at probe, once.
	 *
	 * The general lesson is worth more than the fix: a peer driver's cached
	 * state is not the hardware's state, and the only way to tell them apart is
	 * to ask the hardware. */
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_std, V4L2_STD_UNKNOWN);
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_std, gb->norm);
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_routing,
			     gb_input_route(gb), 0, 0);

	/* Leave the decoder POWERED DOWN until somebody actually wants a picture.
	 *
	 * Probing has to configure the CX25840 -- standard, routing, board pin
	 * setup -- and that leaves it digitising. Nothing was streaming and no
	 * endpoint was up, but the chip ran anyway: the front LED flashed for as
	 * long as the module was loaded, and the case stayed warm rather than
	 * cooling. On a part discontinued in 2004 that cannot be replaced, running
	 * it around the clock to produce frames nobody reads is the wrong default.
	 *
	 * s_stream(0) is the lever available. mainline's cx25840 has no s_power --
	 * it was dropped -- but for a plain CX2584x cx25840_s_stream() clears the
	 * output-enable and clock bits in 0x115/0x116, which is the power control
	 * that matters here.
	 *
	 * gb_stream_up() already calls s_stream(1), so the wake path is in place
	 * and this only closes the idle one. It is the same asymmetry that hid two
	 * other bugs today: the teardown path was written and the never-started-yet
	 * path was assumed. */
	v4l2_device_call_all(&gb->v4l2_dev, 0, video, s_stream, 0);

	dev_info(&gb->intf->dev, "registered as /dev/video%d (%ux%u UYVY, %s)\n",
		 gb->vdev.num, gb->width, gb->height,
		 gb->is_pal ? "625/50" : "525/60");
	return 0;

err_ctrl:
	/* ONE teardown path, and this function must not grow a second one.
	 *
	 * These failure returns used to free the handler, release the queue and
	 * mutex_destroy(&gb->vdev_lock) by hand -- while leaving
	 * gb->vdev_lock_ready true. probe's error path then calls
	 * gb_v4l2_unregister() for ANY gb_v4l2_register() failure, its guard saw
	 * the flag still set, and destroyed the same mutex a SECOND time. Under
	 * DEBUG_MUTEXES that is the exact WARN the flag was added to prevent, so
	 * the local cleanup was re-creating the bug the guarded cleanup fixes.
	 *
	 * Delegating means the flag is owned by exactly one piece of code, and it
	 * costs nothing: gb_v4l2_unregister() is idempotent by construction --
	 * every step is behind a flag or is safe to repeat -- which it has to be
	 * anyway, because probe's error path calls it again after we return.
	 *
	 * That ONE piece of code is no longer this file: vdev_lock is now destroyed
	 * by gb_release() in gb-core.c, still behind vdev_lock_ready, because
	 * vdev->lock can be reached after gb_disconnect() has returned. The rule is
	 * unchanged -- exactly one owner -- and the owner moved. */
	gb_v4l2_unregister(gb);
	return ret;
}

void gb_v4l2_unregister(struct gb_dev *gb)
{
	if (gb->vdev_registered) {
		gb->vdev_registered = false;
		/* vb2_video_unregister_device(), NOT video_unregister_device().
		 * videobuf2-v4l2.h says a driver whose ->release is
		 * vb2_fop_release() must use this helper, and here is why it
		 * matters: verified in the shipped videobuf2-v4l2.ko, it
		 * unregisters the node and then, under q->lock, calls
		 * vb2_core_queue_release() and clears q->owner. That queue
		 * release is what CANCELS STREAMING -- __vb2_queue_cancel() ->
		 * gb_stop_streaming() -> gb_stream_down_locked() -- and it has to
		 * happen HERE, while gb->udev is still valid, because
		 * gb_stream_down_locked() calls usb_set_interface(gb->udev, ...)
		 * and usb_free_coherent(gb->udev, ...). It must NOT be deferred to
		 * gb_release(), which can run long after disconnect has cleared
		 * udev.
		 *
		 * It is also idempotent: it returns early unless
		 * V4L2_FL_REGISTERED is set (vdev->flags bit 0). */
		vb2_video_unregister_device(&gb->vdev);
		/* The queue is released, so gb_vb2_release() must not release it a
		 * second time from gb_release(). */
		gb->vb2_ready = false;
	} else {
		/* Probe failed before the node existed, so nobody can hold a
		 * handle on it and the queue can be released right here. */
		gb_vb2_release(gb);
	}

	gb->v4l2_dev.ctrl_handler = NULL;
	/* Unguarded ON PURPOSE -- do not "fix" this by adding a flag.
	 *
	 * This runs twice whenever gb_v4l2_register() fails: once from its
	 * err_ctrl, and again from probe's error path. It can also run on a
	 * handler v4l2_ctrl_handler_init() never touched, when gb_vb2_init() is
	 * what failed. Both are safe: v4l2_ctrl_handler_free() returns
	 * immediately when hdl->buckets is NULL, the first call sets buckets
	 * NULL, and gb_dev arrives kzalloc'd so a never-initialised handler has
	 * it NULL already. Verified against the kernel this builds on, v6.16.
	 * (Since 2026-10-17 the handler is re-initialised right after the free,
	 * below; each init is still paired with exactly one free, the last in
	 * gb_release().)
	 *
	 * AND IT DELIBERATELY STAYS HERE rather than moving to gb_release() with
	 * the rest of the teardown, which looks like the opposite of this whole
	 * change and is not. Our handler holds v4l2_ctrl_refs to controls OWNED by
	 * the cx25840 subdev, and v4l2_device_unregister() unbinds that subdev a
	 * few lines later in gb_disconnect() (gb_i2c_unregister() on probe's
	 * error path). Freeing our handler first drops those refs, so no later
	 * v4l2_ctrl_find() on it can follow one into storage the subdev has
	 * already kvfree'd; the re-initialised handler below is empty. usbtv frees its handler from the
	 * release callback, but usbtv has no i2c subdevs to outlive. */
	v4l2_ctrl_handler_free(&gb->ctrl_handler);

	/* ...AND THEN RE-INITIALISE IT, EMPTY. Freeing it is not the end of its
	 * use: an open file with a control event subscribed reaches it again on
	 * close(), v4l2_fh_exit() -> v4l2_ctrl_del_event() -> find_ref_lock(),
	 * which LOCKS hdl->lock -- a mutex v4l2_ctrl_handler_free() has just
	 * destroyed. A DEBUG_MUTEXES warning, a crash under panic_on_warn. Found
	 * by the third whole-repo review, 2026-10-16. Empty, it answers every
	 * late lookup with "no such control" through a live mutex; gb_release()
	 * frees it for good once the last file is closed. The other half of that
	 * finding -- a close() landing between this free and the subdev's --
	 * is gb_fop_release()'s job. */
	v4l2_ctrl_handler_init(&gb->ctrl_handler, 0);

	/* vdev_lock is NOT destroyed here any more: it is vdev->lock, and
	 * gb_dev now outlives disconnect. gb_release() destroys it, still behind
	 * vdev_lock_ready, for the same reason that flag exists -- probe's error
	 * path reaches this function for ANY failure after v4l2_device_register(),
	 * including one before gb_v4l2_register() ran mutex_init(). */
}
