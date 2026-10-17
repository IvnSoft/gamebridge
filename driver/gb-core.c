// SPDX-License-Identifier: GPL-2.0
/* Adaptec GameBridge AVC-1410 / 1510 -- USB bridge core.
 *
 * Binds both USB identities: the empty loader (03f3:009b), which gets the
 * firmware uploaded and re-enumerates, and the capture device (03f3:009c),
 * whose probe checks the endpoint geometry and brings up the I2C adapter, the
 * V4L2 node (gb-v4l2.c, gb-video.c), the ALSA card (gb-alsa.c) and the IR
 * receiver (gb-ir.c). Disconnect tears them down in the reverse order.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>	/* gb_release()'s belt-and-braces vfree() */
#include <media/v4l2-common.h>

#include "gamebridge.h"

/* Audio is ON by default. It was opt-in for one day, and the reason it stopped
 * being opt-in is worth keeping, because the reason it was opt-in was wrong.
 *
 * Audio hard-locked this machine three times, so the default was set to off and
 * gb-alsa.c was marked unfit. Three rounds of auditing that file found two real
 * locking bugs and fixed them, and it still hung. The bug was not in it: the
 * VIDEO assembler was writing into vb2 planes that vb2 had already freed, and
 * audio was merely what held the refcounted stream up long enough to reach the
 * window. See gb_return_buffers() in gb-video.c.
 *
 * PipeWire adopts the ALSA card the instant it appears and starts capturing on
 * its own, so registering it is not a passive act -- it exercises the whole path
 * within seconds of modprobe, with nobody having asked for sound. That is why
 * the default mattered, and it is also what makes the verification meaningful,
 * because it is exactly the configuration all three lockups happened in:
 *
 *   an EHCI test machine, kernel-debug with KASAN and lockdep
 *     the bug reproduced, was named, and after the fix: 144 video start/stop
 *     cycles under continuous audio, 23.1 GB, zero sanitizer reports
 *   this workstation, xHCI, the machine that locked three times
 *     164 traversals of the same window with PipeWire holding the card, alive
 *
 *     modprobe gamebridge            video and sound
 *     modprobe gamebridge audio=0    video only
 */
static bool audio = true;
module_param(audio, bool, 0444);
MODULE_PARM_DESC(audio,
		 "register the ALSA capture device (default on). Set audio=0 for "
		 "video only.");

static const struct usb_device_id gb_id_table[] = {
	{ USB_DEVICE(GB_VENDOR_ADAPTEC, GB_PRODUCT_LOADER),
	  .driver_info = GB_LOADER },
	{ USB_DEVICE(GB_VENDOR_ADAPTEC, GB_PRODUCT_CAPTURE),
	  .driver_info = GB_CAPTURE },
	{ }
};
MODULE_DEVICE_TABLE(usb, gb_id_table);

/* Find EP 0x82 in the STREAMING altsetting and record its real geometry.
 *
 * The altsetting matters more than it looks. EP 0x82 is bulk 512 at alt 0 and
 * high-bandwidth isochronous at alts 1..5, so a helper that walks to the first
 * endpoint with a matching address answers for alt 0 and reports 512 bytes --
 * about 4 MB/s, far below uncompressed NTSC, which invites the conclusion that
 * the device must compress on board. It does not: alt 1 carries 1010 x 3 =
 * 3030 bytes per microframe, 24.24 MB/s, and the stream is raw UYVY.
 */
static int gb_probe_iso_geometry(struct gb_dev *gb, struct usb_interface *intf)
{
	struct usb_host_interface *alt;
	int i;

	/* By VALUE, not by position. intf->altsetting[] is an array indexed by
	 * position, and usb_set_interface() in gb-video.c selects by
	 * bAlternateSetting value; indexing here agrees with that only while the
	 * descriptors happen to arrive ordered 0,1,2... with matching values.
	 * Reading geometry from one altsetting while the URBs are submitted
	 * against another is how a pipe ends up built for the wrong endpoint
	 * type -- a machine panicked for exactly that reason on 2026-08-26. */
	alt = usb_altnum_to_altsetting(intf, GB_ALT_STREAM);
	if (!alt) {
		dev_err(&intf->dev,
			"no altsetting with bAlternateSetting %d (%d altsetting(s) present)\n",
			GB_ALT_STREAM, intf->num_altsetting);
		return -ENODEV;
	}

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

		if (ep->bEndpointAddress != GB_EP_VIDEO)
			continue;
		if (!usb_endpoint_xfer_isoc(ep)) {
			dev_err(&intf->dev,
				"EP %02x at alt %d is not isochronous\n",
				GB_EP_VIDEO, GB_ALT_STREAM);
			return -ENODEV;
		}
		gb->iso_maxp = usb_endpoint_maxp(ep);
		gb->iso_mult = usb_endpoint_maxp_mult(ep);
		gb->iso_packet_size = gb->iso_maxp * gb->iso_mult;
		return 0;
	}

	dev_err(&intf->dev, "EP %02x missing from alt %d\n",
		GB_EP_VIDEO, GB_ALT_STREAM);
	return -ENODEV;
}

/* Check the command pair in every altsetting the driver selects.
 *
 * gb_cmd() builds both pipes from EP 0x01's type -- bulk at alt 0, interrupt
 * at alt 1 on the real device -- and applies it to EP 0x81 as well. A device
 * that answers to 03f3:009c with any other arrangement (0x81 of a different
 * type, 0x01 isochronous, either missing or pointing the wrong way) would make
 * the pipe disagree with the endpoint, and usb_submit_urb() reports that with
 * a WARN: a crash on a panic_on_warn machine, from nothing but a USB
 * descriptor. Refuse such a device here instead. Written out rather than with
 * usb_check_bulk_endpoints(), which RHEL 9's 5.14 does not have. Found by the
 * 2026-10-16 security review. */
static int gb_check_cmd_endpoints(struct usb_interface *intf)
{
	static const int alts[] = { GB_ALT_IDLE, GB_ALT_STREAM };
	int a, i;

	for (a = 0; a < ARRAY_SIZE(alts); a++) {
		struct usb_host_interface *alt = usb_altnum_to_altsetting(intf, alts[a]);
		struct usb_endpoint_descriptor *out = NULL, *in = NULL;

		if (!alt)
			goto bad;
		for (i = 0; i < alt->desc.bNumEndpoints; i++) {
			struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

			if (ep->bEndpointAddress == GB_EP_CMD)
				out = ep;
			else if (ep->bEndpointAddress == GB_EP_STATUS)
				in = ep;
		}
		if (!out || !in || usb_endpoint_type(out) != usb_endpoint_type(in) ||
		    !(usb_endpoint_xfer_bulk(out) || usb_endpoint_xfer_int(out)))
			goto bad;
		continue;
bad:
		dev_err(&intf->dev,
			"EP %02x/%02x at alt %d are not a matching bulk or interrupt pair\n",
			GB_EP_CMD, GB_EP_STATUS, alts[a]);
		return -ENODEV;
	}
	return 0;
}

/* Log every altsetting's video endpoint. Stage 3 has no other output, and this
 * is the thing worth seeing: it is the bandwidth ladder, and it is the evidence
 * that the descriptors after firmware bear no resemblance to the ones before. */
static void gb_log_endpoint_map(struct usb_interface *intf)
{
	int a, i;

	for (a = 0; a < intf->num_altsetting; a++) {
		struct usb_host_interface *alt = &intf->altsetting[a];

		for (i = 0; i < alt->desc.bNumEndpoints; i++) {
			struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;
			unsigned int maxp = usb_endpoint_maxp(ep);
			unsigned int mult = usb_endpoint_maxp_mult(ep);
			const char *type = usb_endpoint_xfer_isoc(ep) ? "isoc" :
					   usb_endpoint_xfer_bulk(ep) ? "bulk" :
					   usb_endpoint_xfer_int(ep)  ? "intr" : "ctrl";

			if (ep->bEndpointAddress != GB_EP_VIDEO)
				continue;
			dev_info(&intf->dev,
				 "  alt %d  EP %02x %s %4u x%u = %5u B/uframe (%u.%02u MB/s)\n",
				 a, ep->bEndpointAddress, type, maxp, mult,
				 maxp * mult,
				 (maxp * mult * 8000) / 1000000,
				 ((maxp * mult * 8000) / 10000) % 100);
		}
	}
}

/* Read one CX25840 register through the adapter we just registered.
 *
 * 16-bit big-endian register address, one byte back -- the same two-message
 * shape cx25840_read() builds, deliberately, because the point of this check is
 * to exercise the exact path the subdev will use rather than a convenient
 * approximation of it. */
static int gb_decoder_read(struct gb_dev *gb, u16 reg, u8 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msgs[2] = {
		{ .addr = GB_I2C_ADDR_DECODER, .flags = 0,        .len = 2, .buf = addr },
		{ .addr = GB_I2C_ADDR_DECODER, .flags = I2C_M_RD, .len = 1, .buf = val  },
	};
	int ret = i2c_transfer(&gb->i2c_adap, msgs, 2);

	return ret == 2 ? 0 : (ret < 0 ? ret : -EIO);
}

/* The known-answer control for the whole command-and-I2C path.
 *
 * The decoder's ID registers must read 0x8404, and mainline's own test is
 * (id & 0xff00) == 0x8400. This is the right check to run and the EEPROM is
 * not: a 24C02 takes a ONE-byte address, so reading it requires wcount=1, and
 * wcount=1 is precisely the shape that wedges the firmware. The EEPROM served
 * as this project's known-answer control for two days and, being the only
 * device on the bus that forces the broken shape, is what made "reads wedge"
 * look like a property of reading. */
static void gb_check_decoder(struct gb_dev *gb)
{
	u8 lo = 0, hi = 0;
	int ret;

	ret = gb_decoder_read(gb, 0x0100, &lo);
	if (!ret)
		ret = gb_decoder_read(gb, 0x0101, &hi);
	if (ret) {
		dev_warn(&gb->intf->dev,
			 "decoder ID read failed (%d) -- the cx25840 subdev will not bind\n",
			 ret);
		return;
	}

	dev_info(&gb->intf->dev, "CX25840 device ID 0x%04x%s\n",
		 (hi << 8) | lo,
		 ((((hi << 8) | lo) & 0xff00) == 0x8400) ? " (CX2584x, as expected)"
							 : " -- UNEXPECTED");
}

/* THE LAST REFERENCE IS GONE, AND THIS IS THE ONLY PLACE gb_dev IS FREED.
 *
 * gb_dev's storage is owned by the v4l2_device refcount, not by gb_disconnect().
 * struct video_device vdev and struct vb2_queue vb_vidq are EMBEDDED in it, and
 * video_unregister_device() does not wait for open file handles: unplug with
 * ffplay holding /dev/videoN and the close() that follows still runs
 * vb2_fop_release() -> mutex_lock(&gb->vb_lock) ->
 * vb2_core_queue_release(&gb->vb_vidq) -> v4l2_fh_release() ->
 * spin_lock(&gb->vdev.fh_lock), every one of them inside this struct. kfree(gb)
 * from disconnect() made all of that a use-after-free, and then video_put()
 * dropped the last reference and called the release callback through freed
 * storage as well.
 *
 * The struct v4l2_device kref exists for exactly this and was unused. The
 * pattern is usbtv's usbtv_release(), in the kernel's
 * drivers/media/usb/usbtv/usbtv-video.c. Verified in the shipped
 * videodev.ko of the running kernel: __video_register_device() does a kref_get
 * on vdev->v4l2_dev immediately before device_register(), and
 * v4l2_device_release() -- the struct device release for vdev->dev -- calls
 * vdev->release and then, when v4l2_dev->release is set, v4l2_device_put(). So
 * the last reference standing frees us, whichever side it belongs to.
 *
 * CONTEXT: process. Possibly long after disconnect() returned, possibly from a
 * userspace close(), possibly from the ALSA card's private_free. gb->udev IS
 * ALREADY NULL and the usb_device reference was dropped in disconnect(), so
 * NOTHING HERE MAY TOUCH USB. gb->intf is still safe to dereference only
 * because probe took usb_get_intf() on it, which this function balances.
 *
 * It must also be correct on a HALF-BUILT gb_dev, because probe's error path
 * calls it directly when there is no kref to drop yet. Every step is: the vb2
 * release has its vb2_ready guard, vdev_lock_ready is a kzalloc'd false,
 * vfree(NULL) and kfree(NULL) are fine, and cmd_lock and stream_lock are
 * mutex_init'd as probe's first statements precisely so that this is true.
 *
 * MODULE UNLOAD, since this hangs a callback in module text off a userspace fd:
 * rmmod cannot run while either descriptor is open -- snd_pcm_open() does
 * try_module_get(pcm->card->module) and the video node's cdev carries
 * vdev->fops->owner = THIS_MODULE -- so this cannot be called into freed text.
 * That is an assumption about two other subsystems, which is why it is written
 * down rather than left implicit. */
static void gb_release(struct v4l2_device *v4l2_dev)
{
	struct gb_dev *gb = container_of(v4l2_dev, struct gb_dev, v4l2_dev);

	/* Both are unreachable today, which is exactly why a WARN is worth more
	 * here than a comment: disconnect drops both stream users before its
	 * final put, and every gb_stream_up() error path vfrees the scratch. If
	 * either ever stops being true, this says so instead of leaking. */
	WARN_ON(gb->stream_users);
	vfree(gb->scratch);

	/* A guarded no-op on every path that exists today -- gb_v4l2_unregister()
	 * clears vb2_ready in both of its branches -- and kept so that the release
	 * callback is the backstop if a future path forgets. */
	gb_vb2_release(gb);
	/* The empty handler gb_v4l2_unregister() leaves for late close()s. */
	v4l2_ctrl_handler_free(&gb->ctrl_handler);

	if (gb->vdev_lock_ready) {
		gb->vdev_lock_ready = false;
		mutex_destroy(&gb->vdev_lock);
	}
	/* stream_lock, not vb_lock. A late gb_pcm_work() takes stream_lock (and
	 * gb_pcm_sync_stop(), which used to, takes no lock of ours any more), so
	 * the release callback is the only correct place for
	 * it; vb_lock is deliberately never destroyed, because _vb2_fop_release()
	 * takes it on a close() that can arrive after disconnect (see
	 * gb_vb2_release()). */
	mutex_destroy(&gb->stream_lock);
	kfree(gb->cmd_buf);
	mutex_destroy(&gb->cmd_lock);
	usb_put_intf(gb->intf);
	kfree(gb);
}

static int gb_probe_capture(struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct gb_dev *gb;
	int ret;

	gb = kzalloc(sizeof(*gb), GFP_KERNEL);
	if (!gb)
		return -ENOMEM;

	gb->udev = usb_get_dev(udev);
	/* A REFERENCE, not a borrowed pointer, and it is new with gb_release().
	 *
	 * Once gb_dev outlives gb_disconnect(), every dev_warn(&gb->intf->dev, ...)
	 * reachable afterwards is a dereference of a usb_interface whose last
	 * reference the core may have dropped -- gb_stream_down_locked()'s three
	 * dev_info() reports from a late gb_pcm_work() are a concrete instance.
	 * v4l2_device_register() does take get_device(&intf->dev) (verified in
	 * the shipped videodev.ko), but
	 * v4l2_device_unregister() gives it back inside disconnect, so it does not
	 * cover the deferred window. One usb_get_intf() makes all of them rest on a
	 * reference instead of on an inference; gb_release() does the put.
	 *
	 * It deliberately does not disturb gb->udev, which disconnect still NULLs
	 * under cmd_lock so that a racing gb_cmd() gets -ENODEV. */
	gb->intf = usb_get_intf(intf);
	gb->cur_alt = GB_ALT_IDLE;
	/* Both before anything that can fail. gb_release() destroys them on every
	 * path, including a probe that fails on the next line, and destroying a
	 * mutex that is still all zeros trips DEBUG_MUTEXES. stream_lock used to be
	 * initialised in gb_vb2_init(), which a failing probe may never reach. */
	mutex_init(&gb->cmd_lock);
	mutex_init(&gb->stream_lock);
	usb_set_intfdata(intf, gb);

	gb->cmd_buf = kzalloc(GB_CMD_MAX, GFP_KERNEL);
	if (!gb->cmd_buf) {
		ret = -ENOMEM;
		goto err;
	}

	ret = gb_probe_iso_geometry(gb, intf);
	if (ret)
		goto err;
	ret = gb_check_cmd_endpoints(intf);
	if (ret)
		goto err;

	dev_info(&intf->dev, "GameBridge AVC-1410 (%s)\n",
		 udev->serial ? udev->serial : "no serial");
	dev_info(&intf->dev, "video endpoint map:\n");
	gb_log_endpoint_map(intf);
	dev_info(&intf->dev,
		 "streaming alt %d: %u x %u = %u bytes/microframe\n",
		 GB_ALT_STREAM, gb->iso_maxp, gb->iso_mult, gb->iso_packet_size);

	/* BEFORE the register, because the register is what makes the kref live and
	 * the callback is what the kref calls. From here on gb_dev's storage
	 * belongs to that refcount and the error path below drops a reference
	 * instead of freeing by hand. */
	gb->v4l2_dev.release = gb_release;
	ret = v4l2_device_register(&intf->dev, &gb->v4l2_dev);
	if (ret) {
		dev_err(&intf->dev, "v4l2_device_register failed: %d\n", ret);
		goto err;
	}
	gb->v4l2_registered = true;

	/* Before anything touches I2C. See gb_send_prologue() -- without it the
	 * decoder answers its address but returns 0x8a for every register. */
	ret = gb_send_prologue(gb);
	if (ret) {
		dev_err(&intf->dev, "bring-up prologue failed: %d\n", ret);
		goto err;
	}

	ret = gb_i2c_register(gb);
	if (ret) {
		dev_err(&intf->dev, "cannot register the I2C adapter: %d\n", ret);
		goto err;
	}
	dev_info(&intf->dev, "I2C adapter registered as %s\n",
		 dev_name(&gb->i2c_adap.dev));

	gb_check_decoder(gb);

	/* The bet this whole project has been making.
	 *
	 * If mainline's cx25840 subdev binds at 0x44, then standards handling,
	 * input muxing, brightness/contrast/saturation/hue, the sync slicer, the
	 * PLL and -- most valuable of all -- the COMPUTED active-window timing come
	 * from a driver that has been maintained since 2005. The alternative is a
	 * bridge-private register table replayed from the captured Windows session:
	 * it works, it is what produces video today, and it is a distinctly worse
	 * driver, because it throws away every one of those.
	 *
	 * cx25840_probe() reads 0x101 then 0x100 one byte at a time, which is the
	 * wcount=2/rcount=1 shape our adapter handles perfectly, and it makes no
	 * large writes. The prologue above is what makes those reads return real
	 * data rather than 0x8a. */
	gb->sd = v4l2_i2c_new_subdev(&gb->v4l2_dev, &gb->i2c_adap,
				     "cx25840", GB_I2C_ADDR_DECODER, NULL);
	if (!gb->sd) {
		/* Not fatal here, deliberately: the bridge is perfectly usable
		 * without the subdev and saying exactly what failed is worth more
		 * than refusing to probe. A NULL return almost always means the
		 * cx25840 driver's own ID read did not return 0x8404 -- so if the
		 * check above printed the right ID and this still failed, suspect
		 * the module being absent rather than the chip. */
		dev_warn(&intf->dev,
			 "cx25840 subdev did not bind -- is the cx25840 module available?\n");
	} else {
		dev_info(&intf->dev, "cx25840 subdev bound: %s\n", gb->sd->name);
	}

	ret = gb_v4l2_register(gb);
	if (ret)
		goto err;

	/* Not fatal: a device that captures pictures but not sound is far more
	 * useful than one that refuses to probe. */
	if (!audio)
		dev_info(&intf->dev, "audio disabled by module parameter\n");
	else if (gb_alsa_register(gb))
		dev_warn(&intf->dev, "no ALSA device -- video only\n");

	/* Also not fatal, and for a second reason beyond the one above: this is
	 * the only feature in the driver with no vendor implementation to check
	 * ourselves against, so it is the one most likely to be wrong. It must
	 * not be able to take the capture device down with it. */
	gb_ir_register(gb);

	/* Stage 6 stops here: the node describes the device but hands out no
	 * buffers. Stage 7 adds videobuf2 and the isochronous URB ring. */
	dev_info(&intf->dev, "probe complete\n");
	return 0;

err:
	/* This order must stay in step with gb_disconnect(): one teardown path
	 * that differs from the other is how a latent teardown bug survives
	 * review. v4l2_device_unregister() comes BEFORE gb_i2c_unregister()
	 * because the cx25840 subdev is an i2c client of that adapter and is
	 * listed on the v4l2_device; dropping the adapter first destroys the
	 * client while the device that lists it is still registered. It happens
	 * to be survivable today only because cx25840_remove() calls
	 * v4l2_device_unregister_subdev() itself, which is not a guarantee this
	 * driver gets to rely on. */
	gb_ir_unregister(gb);
	gb_alsa_unregister(gb);
	gb_v4l2_unregister(gb);
	if (gb->v4l2_registered)
		v4l2_device_unregister(&gb->v4l2_dev);
	gb_i2c_unregister(gb);

	/* The same three steps as gb_disconnect()'s tail, in the same order. This
	 * path used to drop the reference without clearing gb->udev, which left
	 * gb_release()'s documented precondition -- "gb->udev IS ALREADY NULL ...
	 * NOTHING HERE MAY TOUCH USB" -- false for one of its two callers, with a
	 * pointer whose reference this driver had already given back. Harmless
	 * while nothing in gb_release() reads it; a trap for whatever is added
	 * there next. cmd_lock is safe to take: mutex_init() precedes every
	 * goto err above. */
	mutex_lock(&gb->cmd_lock);
	gb->udev = NULL;
	mutex_unlock(&gb->cmd_lock);
	usb_set_intfdata(intf, NULL);
	usb_put_dev(interface_to_usbdev(intf));

	/* ONE FREE PATH. With the v4l2_device registered the kref owns gb_dev and
	 * may still be held by the ALSA card, so drop a reference and let
	 * gb_release() run when the count reaches zero; without it there is no kref
	 * to drop, so call the same body directly. Either way every teardown step
	 * happens exactly once and in one place.
	 *
	 * That holds even when video_register_device() fails INSIDE
	 * device_register(), after __video_register_device() has already taken
	 * its own v4l2_device reference. A 2026-10-10 review claimed that
	 * reference leaks; it does not. v6.16 calls put_device(&vdev->dev) on that
	 * failure, the video_device release then calls v4l2_device_put() --
	 * because v4l2_dev.release is set (gb_release, above), and only then --
	 * and vdev->release is video_device_release_empty, so nothing else runs.
	 * The count is back to probe's single reference by the time this put
	 * drops it. Checked against drivers/media/v4l2-core/v4l2-dev.c, not
	 * inferred: the put is a call into the release path, which is why a
	 * search for a second inline atomic in the disassembly found none. */
	if (gb->v4l2_registered)
		v4l2_device_put(&gb->v4l2_dev);
	else
		gb_release(&gb->v4l2_dev);
	return ret;
}

static int gb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	/* Only interface 0 exists on this device, but bind defensively: a probe
	 * that fires for an unexpected interface and proceeds anyway is how a
	 * driver ends up owning something it cannot drive. */
	if (intf->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	if (id->driver_info == GB_LOADER) {
		dev_info(&intf->dev, "loader identity %04x:%04x -- 8051 is in reset\n",
			 GB_VENDOR_ADAPTEC, GB_PRODUCT_LOADER);
		return gb_upload_firmware(intf);
	}

	return gb_probe_capture(intf);
}

static void gb_disconnect(struct usb_interface *intf)
{
	struct gb_dev *gb = usb_get_intfdata(intf);

	/* The loader instance never allocated anything: it uploads and goes away,
	 * and disconnect fires for it as soon as the device renumerates. */
	if (!gb)
		return;

	/* Order matters: the node and the stream go first, then the subdevs, then
	 * the adapter, so no subdev can start a transfer while the command channel
	 * is torn down; then udev is cleared under cmd_lock, so any transfer
	 * already in flight finishes with -ENODEV rather than touching a dead
	 * usb_device. The locking for each step is commented where it is taken. */
	/* Order: the node first, so no new open() can succeed and nothing can call
	 * into the subdev while it is being taken away; then the subdev's own
	 * adapter; then the v4l2_device. */
	/* The IR poller goes first, and it must: it is the only thing in this
	 * driver that issues commands on its own schedule rather than in
	 * response to userspace, so anything torn down before it is torn down
	 * underneath a live gb_cmd().
	 *
	 * Before even that, and not a teardown step: close the door on new stream
	 * users. Everything below can still take the stream DOWN -- the ALSA
	 * card's held reference, video's stop_streaming -- but from here nothing
	 * may bring it UP, including a read() that vb2 lets start fileio after
	 * the video node is unregistered. Taking stream_lock also waits out any
	 * gb_stream_up() already in progress, so none can finish arming a ring
	 * behind this. See gb_dev.gone. */
	mutex_lock(&gb->stream_lock);
	gb->gone = true;
	mutex_unlock(&gb->stream_lock);

	gb_ir_unregister(gb);
	gb_alsa_unregister(gb);

	/* vdev_lock HELD from unregistering the node until the subdevs are
	 * unbound. The V4L2 core tests video_is_registered() once, on
	 * ioctl entry, under vdev->lock -- and every non-queue ioctl runs under
	 * that lock: controls, S_INPUT, QUERYSTD, the tuner ioctls, LOG_STATUS,
	 * all of which reach the cx25840 subdev. video_unregister_device() only
	 * clears the flag; it does not wait for an ioctl already past the test.
	 * Without this, one could still be inside cx25840's control ops or
	 * walking the subdev list when v4l2_device_unregister() unbound the
	 * subdev and freed its control handler underneath it. Taking the lock
	 * waits such an ioctl out, and every later one sees the node gone.
	 * stk1160_disconnect() does the same. Found by the second whole-repo
	 * review, 2026-10-16.
	 *
	 * Order vdev_lock -> vb_lock -> stream_lock -> i2c -> cmd_lock, as in
	 * gb_s_std(): vb2_video_unregister_device() takes vb_lock and stops the
	 * stream from inside it. gb_dev cannot be freed while this is held:
	 * disconnect owns a v4l2_device reference until its last line. */
	if (gb->vdev_lock_ready)
		mutex_lock(&gb->vdev_lock);
	gb_v4l2_unregister(gb);
	/* Both stream holders are gone, so the ring should be too. With
	 * .soft_unbind nothing else guarantees it -- see gb_stream_kill(). Here,
	 * not later: tearing down sends s_stream through the subdev and the stop
	 * command through gb_cmd(), so the subdev, the adapter and udev must all
	 * still exist. */
	gb_stream_kill(gb);
	if (gb->v4l2_registered)
		v4l2_device_unregister(&gb->v4l2_dev);
	/* Released BEFORE the adapter goes. With the node unregistered and the
	 * subdevs unbound, nothing under vdev_lock can reach the adapter any
	 * more -- and i2c_del_adapter() waits, without limit, for every holder
	 * of /dev/i2c-N to close it. Held across that, one root process with
	 * the chardev open would park every ioctl and close() on this node
	 * behind it (third review, 2026-10-16). */
	if (gb->vdev_lock_ready)
		mutex_unlock(&gb->vdev_lock);
	gb_i2c_unregister(gb);

	mutex_lock(&gb->cmd_lock);
	gb->udev = NULL;
	mutex_unlock(&gb->cmd_lock);

	usb_set_intfdata(intf, NULL);
	usb_put_dev(interface_to_usbdev(intf));
	dev_info(&intf->dev, "disconnected\n");

	/* NO kfree(gb) HERE, and nothing may dereference gb after this line.
	 *
	 * The video node holds a reference on this kref and so does the ALSA card,
	 * so this either frees gb_dev now or leaves it to the last close(). That is
	 * the whole of defect B; gb_release() above says why. v4l2_device_disconnect()
	 * needs no separate call: v4l2_device_unregister() calls it first (verified
	 * in the shipped videodev.ko -- it reaches the same put_device(v4l2_dev->dev)
	 * and clears v4l2_dev->dev).
	 *
	 * WHAT IS DELIBERATELY *NOT* DEFERRED, because it looks like an oversight
	 * and is not: v4l2_device_unregister() and gb_i2c_unregister() stay above,
	 * inside disconnect. Deferring them into gb_release() would leave an unplug
	 * with a descriptor held carrying a live i2c-N adapter and a still-bound
	 * cx25840 client, in sysfs and reachable through i2c-dev, answering
	 * transfers for hardware that is physically gone -- and the adapter's
	 * dev.parent is &gb->intf->dev, which the driver core is about to
	 * device_del(). A USB disconnect must remove its bus children while the
	 * hardware is still addressable. Only memory and locks may be deferred. */
	v4l2_device_put(&gb->v4l2_dev);
}

static struct usb_driver gb_driver = {
	.name		= "gamebridge",
	.id_table	= gb_id_table,
	.probe		= gb_probe,
	.disconnect	= gb_disconnect,
	/* Keep the endpoints alive through ->disconnect on an unbind or rmmod
	 * with the device still plugged in. Without this, usb_unbind_interface()
	 * disables the interface first (drivers/usb/core/driver.c, identical in
	 * v5.14 and v6.16), so the decoder's s_stream(0) -- I2C over EP1 at the
	 * streaming altsetting -- can never arrive: every unbind on the rig,
	 * 2026-10-11, failed it with -ENOENT, and the CX25840 was never told to
	 * stop. (The stop command 0x37 got through regardless: it is sent after
	 * the switch to alt 0, which re-enables EP1.) A physical unplug is
	 * unaffected: the core still disables a NOTATTACHED device. The price is
	 * that the core no longer kills our URBs, so gb_disconnect() must leave
	 * none alive; gb_stream_kill() is the backstop for that. */
	.soft_unbind	= 1,
};

/* Not module_usb_driver(). The remote's keymap is a global rc-core registration
 * with a lifetime tied to the MODULE, not to any device, so it has to be put in
 * place before the first probe can reference it by name and taken out after the
 * last disconnect. Registering it from probe would add a duplicate entry per
 * attached device; leaving it registered after unload would leave rc-core
 * holding a pointer into freed module text. */
static int __init gb_init(void)
{
	int ret = gb_ir_map_register();

	if (ret) {
		pr_warn("gamebridge: cannot register the IR keymap: %d\n", ret);
		/* Not fatal: gb_ir_register() falls back to rc-empty, and a
		 * capture device without a preloaded keymap is still a capture
		 * device. */
	}

	ret = usb_register(&gb_driver);
	if (ret)
		gb_ir_map_unregister();
	return ret;
}

static void __exit gb_exit(void)
{
	usb_deregister(&gb_driver);
	gb_ir_map_unregister();
}

module_init(gb_init);
module_exit(gb_exit);

MODULE_DESCRIPTION("Adaptec GameBridge AVC-1410/1510 analogue capture bridge. "
		   "Needs an 8051 image extracted from Adaptec's own Windows "
		   "driver, which cannot be redistributed -- see the README.");
MODULE_AUTHOR("Ivan Barrera");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(GB_FIRMWARE_NAME);
