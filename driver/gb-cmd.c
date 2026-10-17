// SPDX-License-Identifier: GPL-2.0
/* The GameBridge's command channel: EP 0x01 OUT, EP 0x81 IN.
 *
 * The firmware dispatches on the first byte of an EP1 OUT packet -- 19 opcodes,
 * decoded from the 8051 image (docs/internals/README.md) -- and answers on EP1
 * IN. Two rules govern everything here, and both were learned expensively.
 *
 * EVERY COMMAND ARMS A REPLY, AND IT MUST BE DRAINED. The handler sets EP1INBC
 * before returning, so a reply is waiting whether the caller wants it or not.
 * Leaving one queued desynchronises everything after it: the next command's
 * caller reads the previous command's answer, and every subsequent read is off
 * by one. tools/gbcap.c says this plainly and it applies with more force in a
 * kernel driver, where the stale value ends up in a register the cx25840 subdev
 * then acts on.
 *
 * THE TRANSFER TYPE DEPENDS ON THE ALTSETTING. EP 0x01 and EP 0x81 are BULK at
 * alt 0 and INTERRUPT at alts 1..5. This is not a quirk to paper over; it has
 * bitten this project FOUR times, the last one fatally. It hid 97% of the
 * Windows driver's traffic from a usbmon parser; it is why the stop command
 * times out if sent before switching back to alt 0; and on 2026-08-26 it
 * panicked a machine, because gb_cmd() decided from gb->cur_alt -- a shadow
 * of the altsetting that is briefly WRONG while the altsetting is being
 * changed.
 *
 * So gb_cmd() now reads the ENDPOINT DESCRIPTOR, and gb_stream_up()/down() make
 * the switch under cmd_lock. The two cannot disagree because there is no longer
 * a second copy of the answer.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "gamebridge.h"

/* The bring-up prologue, and the answer to a question open since day one.
 *
 * On a freshly enumerated device the decoder answers its address but returns
 * 0x8a for EVERY register, including the ID at 0x0100/0x0101 -- the signature of
 * a part held in reset or unpowered. That is fatal for stage 5, because
 * cx25840_probe() reads the ID first thing and requires (id & 0xff00) == 0x8400.
 *
 * Measured 2026-08-24, on a device freshly replugged and never configured:
 *
 *     fresh enumeration            device ID reads 0x8a8a
 *     after the Windows session    device ID reads 0x8404
 *
 * and neither a bus resync nor the decoder reset pulse (command 0x83) changes
 * it. So something in the session is a precondition, and the session's own head
 * says what: five steps, after which its very next command is an I2C read that
 * works.
 *
 *     CLRHALT 0x01 ; CLRHALT 0x81 ; CMD de 00 ; CMD 85 ; CMD 0b
 *
 * Command 0xde is almost certainly the one that matters. Its handler at 0x00fa
 * calls two subroutines and then drives the FX2's port D:
 *
 *     00fa:  LCALL 0x1be8
 *     00fd:  LCALL 0x1d0f
 *     0100:  ANL IOD,#0xf7      ; clear port D bit 3
 *     0103:  ORL IOD,#0x10      ; set   port D bit 4
 *
 * Two GPIOs, asserted before anything touches I2C, on a board whose decoder is
 * not answering until they are. 0x0b reports the USB speed, and is sent anyway
 * because it costs nothing and the session is the only known-good ordering we
 * have.
 *
 * 0x85 IS THE INFRARED RECEIVER. This comment said "reads a counter ... looks
 * like a status poll" from day one and it was wrong for a reason worth keeping:
 * in the Windows capture 0x85 is sent 2563 times and returns only three distinct
 * replies, all beginning 0x07, because NOBODY PRESSED A BUTTON WHILE THE CAPTURE
 * WAS RECORDED. It looked like a heartbeat because the feature it belongs to was
 * absent from the recording -- the same trap that hid the audio path.
 * Pressing a button and watching it live settled it in ninety seconds. See
 * gb-ir.c and docs/internals/ir-receiver.md.
 */
int gb_send_prologue(struct gb_dev *gb)
{
	static const u8 cmd_de[] = { 0xde, 0x00 };
	static const u8 cmd_85[] = { 0x85 };
	static const u8 cmd_0b[] = { 0x0b };
	int ret;

	usb_clear_halt(gb->udev, usb_sndbulkpipe(gb->udev, GB_EP_CMD));
	usb_clear_halt(gb->udev, usb_rcvbulkpipe(gb->udev, GB_EP_STATUS & 0x0f));

	ret = gb_cmd(gb, cmd_de, sizeof(cmd_de), NULL, 0);
	if (ret < 0)
		return ret;
	ret = gb_cmd(gb, cmd_85, sizeof(cmd_85), NULL, 0);
	if (ret < 0)
		return ret;
	ret = gb_cmd(gb, cmd_0b, sizeof(cmd_0b), NULL, 0);
	return ret < 0 ? ret : 0;
}

int gb_cmd(struct gb_dev *gb, const u8 *cmd, int cmdlen, u8 *reply, int replylen)
{
	unsigned int sndpipe, rcvpipe;
	int ret, actual = 0;
	bool intr;

	if (cmdlen <= 0 || cmdlen > GB_CMD_MAX)
		return -EINVAL;
	if (replylen < 0 || replylen > GB_CMD_MAX)
		return -EINVAL;

	mutex_lock(&gb->cmd_lock);

	/* disconnect() clears udev under this lock, so a command racing a physical
	 * unplug gets -ENODEV instead of touching a dead usb_device. */
	if (!gb->udev) {
		ret = -ENODEV;
		goto out;
	}

	/* Ask the ENDPOINT what it is. Do not ask gb->cur_alt.
	 *
	 * This used to read `intr = gb->cur_alt != GB_ALT_IDLE`, and on 2026-08-26
	 * that panicked a machine. gb_stream_down() switches to alt 0 and updates
	 * cur_alt a few lines later; in between, the hardware has EP1 as BULK while
	 * cur_alt still says streaming. Nothing could ever observe that window until
	 * gb-ir.c started polling on its own schedule -- and then, within minutes:
	 *
	 *   usb 3-1.1: BOGUS urb xfer, pipe 1 != type 3
	 *   UBSAN: shift-out-of-bounds in drivers/usb/host/ehci-q.c:825:10
	 *   Workqueue: gamebridge-ir gb_ir_poll [gamebridge]
	 *   ... 14 s later ... Kernel panic - not syncing: Hard LOCKUP
	 *
	 * usb_bulk_msg() does NOT rescue this, which was the assumption that let it
	 * through review. It picks the URB *fill function* from the real endpoint
	 * descriptor, but passes the caller's `pipe` through untouched, so the
	 * mismatch reaches the HCD; usb_submit_urb() only warns. EHCI then routes on
	 * the pipe type, builds an interrupt QH for a bulk endpoint, and computes
	 * its polling interval from a bInterval of 0 -- a shift by -1.
	 *
	 * usb_pipe_endpoint() reads udev->ep_in/ep_out, which usb_set_interface()
	 * updates. Both that call and this read now happen under cmd_lock, so the
	 * type we build the pipe from is the type the endpoint actually has. cur_alt
	 * survives only as the thing gb_stream_*() sets; nothing decides with it. */
	{
		struct usb_host_endpoint *ep =
			usb_pipe_endpoint(gb->udev,
					  usb_sndbulkpipe(gb->udev, GB_EP_CMD));

		if (!ep) {
			ret = -ENODEV;
			goto out;
		}
		intr = usb_endpoint_xfer_int(&ep->desc);
	}
	sndpipe = intr ? usb_sndintpipe(gb->udev, GB_EP_CMD)
			: usb_sndbulkpipe(gb->udev, GB_EP_CMD);
	rcvpipe = intr ? usb_rcvintpipe(gb->udev, GB_EP_STATUS & 0x0f)
			: usb_rcvbulkpipe(gb->udev, GB_EP_STATUS & 0x0f);

	/* usb_bulk_msg()/usb_interrupt_msg() need a DMA-able buffer, so the
	 * caller's stack or rodata will not do. One scratch buffer per device,
	 * covered by the lock we already hold. */
	memcpy(gb->cmd_buf, cmd, cmdlen);

	if (intr)
		ret = usb_interrupt_msg(gb->udev, sndpipe, gb->cmd_buf, cmdlen,
					&actual, 1000);
	else
		ret = usb_bulk_msg(gb->udev, sndpipe, gb->cmd_buf, cmdlen,
				   &actual, 1000);
	if (ret) {
		dev_dbg(&gb->intf->dev, "cmd %02x: send failed: %d\n", cmd[0], ret);
		goto out;
	}
	if (actual != cmdlen) {
		dev_warn(&gb->intf->dev, "cmd %02x: short write %d of %d\n",
			 cmd[0], actual, cmdlen);
		ret = -EIO;
		goto out;
	}

	/* Drain. Not every command arms a reply, and the ones that do not must not
	 * be treated as failures.
	 *
	 * Command 0xde is the case that matters, because it is the first thing the
	 * bring-up prologue sends: its handler at 0x00fa drives two GPIOs and jumps
	 * straight to the dispatcher exit without ever touching EP1INBUF or
	 * EP1INBC. Measured -- `cmd -> de 00  sent=2 rc=Success` followed by a
	 * reply that never comes. Command 0x0b is conditional in the same way: it
	 * returns without arming anything when internal RAM 0x4c is zero.
	 *
	 * An earlier version of this function required the drain to succeed, which
	 * would have failed probe on the prologue's very first command.
	 *
	 * So: a caller that WANTS data gets a real timeout treated as an error,
	 * because a read that produced nothing is a failed read. A caller that does
	 * not is only draining for synchronisation, and a timeout there means there
	 * was nothing queued -- which is exactly the desired state. Short timeout in
	 * that case, since we expect to wait it out on every such command. */
	actual = 0;
	if (intr)
		ret = usb_interrupt_msg(gb->udev, rcvpipe, gb->cmd_buf,
					replylen ? replylen : GB_CMD_MAX,
					&actual, replylen ? 500 : 50);
	else
		ret = usb_bulk_msg(gb->udev, rcvpipe, gb->cmd_buf,
				   replylen ? replylen : GB_CMD_MAX,
				   &actual, replylen ? 500 : 50);
	if (ret == -ETIMEDOUT && !replylen) {
		ret = 0;		/* nothing was armed; the channel is in sync */
		goto out;
	}
	if (ret) {
		/* A timeout when data WAS expected usually means the 8051 is wedged
		 * rather than that the command failed -- see gb-i2c.c.
		 * Do not retry from inside the lock; let the caller decide. */
		dev_dbg(&gb->intf->dev, "cmd %02x: no reply: %d\n", cmd[0], ret);
		goto out;
	}

	if (reply && replylen)
		memcpy(reply, gb->cmd_buf, min(actual, replylen));
	ret = actual;
out:
	mutex_unlock(&gb->cmd_lock);
	return ret;
}
