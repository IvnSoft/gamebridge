// SPDX-License-Identifier: GPL-2.0
/* The GameBridge's infrared receiver, as an rc-core device.
 *
 * WHAT THIS IS, AND WHY IT IS NOT PORTED FROM ANYWHERE
 *
 * The unit has an IR window on the front and shipped with a 28-button remote.
 * Adaptec's own Windows driver has NO IR support anywhere in it -- the INF
 * declares no IR interface, and the binary contains no decoder (see
 * docs/internals/vendor-inf-findings.md). Whatever read that remote under
 * Windows sat above the driver, in the GameBridge application, and we do not
 * have it. So unlike every other feature in this driver, none of this was
 * recovered from a capture of working software. It was measured directly.
 *
 * WHAT COMMAND 0x85 ACTUALLY IS
 *
 * 0x85 has been in gb_send_prologue() since day one, described there as
 * "reads a counter ... looks like a status poll". That was wrong, and the
 * reason it survived is worth keeping: in the Windows capture 0x85 is sent
 * 2563 times across 279 seconds and returns only three distinct replies, all
 * of them beginning 0x07. It looked inert because NOBODY PRESSED A BUTTON
 * WHILE THE CAPTURE WAS RECORDED. A replay is only as complete as its capture,
 * which is the same trap that hid the audio path for three rounds.
 *
 * Measured live on 2026-08-26, with a remote in hand, it is an NEC receiver:
 *
 *     byte 0   0x08 = a frame is latched and these bytes are IT
 *              0x07 = nothing new; bytes 1..4 are STALE and must be ignored
 *     byte 1   address low                   0x82 on this remote
 *     byte 2   address high                  0xf1 on this remote
 *     byte 3   command, OR 0x80 if this is a REPEAT of the held key
 *     byte 4   ~command   (byte3 & 0x7f) ^ byte4 == 0xff, always
 *
 * Byte 0 is not an IR field at all. GB_I2C_ACK/GB_I2C_NACK are already 0x08 and
 * 0x07 in gamebridge.h: it is the firmware's generic "this reply carries data"
 * flag, and reading the command clears it. That is the single most important
 * thing in this file. Trusting bytes 1..4 on a NACK reply does not fail
 * visibly -- it re-reports the last key forever, at the poll rate.
 *
 * 0x82/0xf1 are not complementary (0x82 ^ 0xff is 0x7d), so this is EXTENDED
 * NEC and the scancode rc-core expects is addr_lo << 16 | addr_hi << 8 | cmd,
 * matching how ir-nec-decoder.c composes RC_PROTO_NECX.
 *
 * WHY POLLING, AND WHY IT IS NOT FREE
 *
 * There is no unsolicited path: EP 0x81 answers commands and nothing else, so
 * the only way to see a keypress is to ask. Adaptec's application asked about
 * nine times a second. NEC repeat frames arrive every 108 ms, so anything
 * slower than ~9 Hz starts dropping them; the default here is 50 ms, which is
 * twice that and was the rate the receiver was characterised at.
 *
 * The cost is that the device is never idle again while this module is loaded.
 * It is a command on EP1, not isochronous -- the altsetting stays at 0 and
 * nothing streams -- but after the front-LED investigation this driver does
 * not get to assume that costs nothing. It is behind ir= for exactly that
 * reason, and the LED and temperature with it running are unmeasured.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/input.h>	/* usb_to_input_id() */
#include <media/rc-core.h>

#include "gamebridge.h"

static bool ir = true;
module_param(ir, bool, 0444);
MODULE_PARM_DESC(ir,
		 "register the infrared receiver (default on). Set ir=0 to stop "
		 "polling the device entirely.");

static unsigned int ir_poll_ms = 50;
module_param(ir_poll_ms, uint, 0644);
MODULE_PARM_DESC(ir_poll_ms,
		 "IR poll interval in ms (default 50, floor 10). NEC repeats "
		 "arrive every 108 ms, so values above ~100 drop held keys.");

/* Writable at runtime, so it is clamped at every use rather than once at
 * registration. The floor is not cosmetic: gb_cmd() can block for up to 1.5 s on
 * a wedged device, and a 1 ms request would queue polls faster than they drain. */
static unsigned long gb_ir_delay(void)
{
	return msecs_to_jiffies(clamp(ir_poll_ms, 10u, 1000u));
}

/* ------------------------------------------------------------- the keymap ---
 *
 * The 28 buttons of the remote that shipped with the unit, established by
 * pressing every one of them in reading order and reading the codes back --
 * twice, on two machines, through two entirely different code paths:
 *
 *   tools/gbcap irwatch, libusb, this workstation, xHCI, kernel 6.16
 *   this driver,         in-kernel, test machine,  EHCI, kernel 6.19+debug
 *
 * Both runs returned the same 28 codes in the same order. That is why this is a
 * table and not a guess.
 *
 * TWO STRUCTURAL CHECKS AGREE WITH IT, and they are worth recording because
 * neither was arranged: the five buttons the remote prints on a blue background
 * -- OK, CH/PG +/-, VOL +/- -- are exactly the complete 0x01..0x05 bank, nothing
 * else in it and nothing missing. And the digit columns run consecutively IN
 * BCD: 3/6/9/MUTE are 0x10,0x11,0x12,0x13, and 2/5/8/0 are 0x19,0x20,0x21,0x22,
 * which is +7 in hex but +1 in decimal. So these are BCD scan positions, and the
 * runs follow the physical columns -- which is how a matrix keypad is wired. It
 * also explains the "missing" values (0x06..0x09, 0x14, 0x24, 0x29, 0x32): they
 * are BCD-invalid or unpopulated, not holes in our data.
 *
 * The one genuinely uncertain entry is START. Every other button has an obvious
 * Linux keycode; START launched Adaptec's application, which we do not have, so
 * KEY_MEDIA -- "launch the media application" -- is a judgement call rather than
 * a measurement. Anyone who disagrees can override it with ir-keytable without
 * recompiling, which is most of the reason this is an rc-core map at all.
 */
#define GB_IR_ADDR	0x82f100	/* NECX, constant for this remote */

static struct rc_map_table gb_ir_table[] = {
	/* transport and navigation */
	{ GB_IR_ADDR | 0x33, KEY_MEDIA },		/* START            */
	{ GB_IR_ADDR | 0x36, KEY_UP },
	{ GB_IR_ADDR | 0x34, KEY_BACK },		/* the curved arrow */
	{ GB_IR_ADDR | 0x30, KEY_PLAYPAUSE },
	{ GB_IR_ADDR | 0x38, KEY_LEFT },
	{ GB_IR_ADDR | 0x03, KEY_OK },
	{ GB_IR_ADDR | 0x39, KEY_RIGHT },
	{ GB_IR_ADDR | 0x23, KEY_STOP },
	{ GB_IR_ADDR | 0x31, KEY_REWIND },
	{ GB_IR_ADDR | 0x37, KEY_DOWN },
	{ GB_IR_ADDR | 0x15, KEY_FASTFORWARD },
	{ GB_IR_ADDR | 0x35, KEY_RECORD },

	/* the keypad */
	{ GB_IR_ADDR | 0x26, KEY_NUMERIC_1 },
	{ GB_IR_ADDR | 0x19, KEY_NUMERIC_2 },
	{ GB_IR_ADDR | 0x10, KEY_NUMERIC_3 },
	{ GB_IR_ADDR | 0x27, KEY_NUMERIC_4 },
	{ GB_IR_ADDR | 0x20, KEY_NUMERIC_5 },
	{ GB_IR_ADDR | 0x11, KEY_NUMERIC_6 },
	{ GB_IR_ADDR | 0x28, KEY_NUMERIC_7 },
	{ GB_IR_ADDR | 0x21, KEY_NUMERIC_8 },
	{ GB_IR_ADDR | 0x12, KEY_NUMERIC_9 },
	{ GB_IR_ADDR | 0x22, KEY_NUMERIC_0 },
	{ GB_IR_ADDR | 0x16, KEY_SUBTITLE },		/* CC   */
	{ GB_IR_ADDR | 0x13, KEY_MUTE },

	/* the five blue ones */
	{ GB_IR_ADDR | 0x01, KEY_CHANNELUP },
	{ GB_IR_ADDR | 0x05, KEY_CHANNELDOWN },
	{ GB_IR_ADDR | 0x04, KEY_VOLUMEUP },
	{ GB_IR_ADDR | 0x02, KEY_VOLUMEDOWN },
};

static struct rc_map_list gb_ir_map = {
	.map = {
		.scan     = gb_ir_table,
		.size     = ARRAY_SIZE(gb_ir_table),
		.rc_proto = RC_PROTO_NECX,
		.name     = GB_IR_MAP_NAME,
	}
};

static bool gb_ir_map_registered;

/* Called from module init/exit, NOT from probe: rc_map_register() adds to a
 * global list, so doing it per-device would add a duplicate entry for every
 * GameBridge plugged in. */
int gb_ir_map_register(void)
{
	int ret;

	if (!ir)
		return 0;
	ret = rc_map_register(&gb_ir_map);
	if (ret)
		return ret;
	gb_ir_map_registered = true;
	return 0;
}

void gb_ir_map_unregister(void)
{
	if (gb_ir_map_registered) {
		rc_map_unregister(&gb_ir_map);
		gb_ir_map_registered = false;
	}
}

/* One poll. Returns nothing: a failed command here is not interesting enough to
 * propagate, and the next tick will try again. */
static void gb_ir_poll(struct work_struct *work)
{
	struct gb_dev *gb = container_of(to_delayed_work(work),
					 struct gb_dev, ir_work);
	static const u8 cmd85[] = { 0x85 };
	u8 rep[GB_IR_REPLY_LEN] = { 0 };
	u32 scancode;
	u8 command;
	int ret;

	ret = gb_cmd(gb, cmd85, sizeof(cmd85), rep, sizeof(rep));
	if (ret < (int)sizeof(rep))
		goto again;			/* unplugged, wedged, or short */

	/* The whole file turns on this line. See the header comment. */
	if (rep[0] != GB_I2C_ACK)
		goto again;

	command = rep[3] & 0x7f;
	if ((command ^ rep[4]) != 0xff) {
		/* Not a valid NEC frame. Never observed, but a garbled reply
		 * that we forwarded would arrive in userspace as a real
		 * keypress, so it is dropped and counted rather than trusted. */
		gb->ir_bad_frames++;
		goto again;
	}

	scancode = (u32)rep[1] << 16 | (u32)rep[2] << 8 | command;

	if ((rep[3] & GB_IR_REPEAT) && scancode == gb->ir_last_scancode) {
		/* The device tells us this is a held key, so say so rather than
		 * inferring it from arrival times. rc-core extends its own keyup
		 * timer; a new keydown here would look like a second press. */
		rc_repeat(gb->rc);
	} else {
		rc_keydown(gb->rc, RC_PROTO_NECX, scancode, 0);
		gb->ir_last_scancode = scancode;
	}
	gb->ir_events++;
	dev_dbg(&gb->intf->dev, "IR %s scancode %06x\n",
		(rep[3] & GB_IR_REPEAT) ? "repeat" : "key   ", scancode);

again:
	if (READ_ONCE(gb->ir_polling))
		queue_delayed_work(gb->ir_wq, &gb->ir_work, gb_ir_delay());
}

int gb_ir_register(struct gb_dev *gb)
{
	struct rc_dev *rc;
	int ret;

	if (!ir) {
		dev_info(&gb->intf->dev, "IR disabled by module parameter\n");
		return 0;
	}

	/* Its own workqueue, not the system one. Every poll blocks on USB, and
	 * gb_cmd() will wait 1 s for the send and 500 ms for the reply if the
	 * 8051 stops answering -- occupying a shared system_wq worker 20 times a
	 * second to do it. Ordered, so a poll can never overlap its successor. */
	gb->ir_wq = alloc_ordered_workqueue("gamebridge-ir", 0);
	if (!gb->ir_wq)
		return -ENOMEM;

	rc = rc_allocate_device(RC_DRIVER_SCANCODE);
	if (!rc) {
		destroy_workqueue(gb->ir_wq);
		gb->ir_wq = NULL;
		return -ENOMEM;
	}

	usb_make_path(gb->udev, gb->ir_phys, sizeof(gb->ir_phys));
	strlcat(gb->ir_phys, "/input0", sizeof(gb->ir_phys));

	rc->device_name		= "Adaptec GameBridge AVC-1410 IR";
	rc->input_phys		= gb->ir_phys;
	usb_to_input_id(gb->udev, &rc->input_id);
	rc->dev.parent		= &gb->intf->dev;
	rc->driver_name		= "gamebridge";
	rc->priv		= gb;

	/* NECX only. The receiver decodes in hardware and hands us a composed
	 * frame, so we are RC_DRIVER_SCANCODE and there is no raw path to offer
	 * other protocols on: claiming them would be a lie the sysfs
	 * protocols file then repeats. */
	rc->allowed_protocols	= RC_PROTO_BIT_NECX;
	/* Falls back to rc-empty if the map failed to register, which still gives
	 * a working scancode receiver that ir-keytable can load a map into. */
	rc->map_name		= gb_ir_map_registered ? GB_IR_MAP_NAME : RC_MAP_EMPTY;

	ret = rc_register_device(rc);
	if (ret) {
		dev_warn(&gb->intf->dev, "cannot register the IR device: %d\n", ret);
		rc_free_device(rc);
		destroy_workqueue(gb->ir_wq);
		gb->ir_wq = NULL;
		return ret;
	}
	gb->rc = rc;

	INIT_DELAYED_WORK(&gb->ir_work, gb_ir_poll);
	WRITE_ONCE(gb->ir_polling, true);
	queue_delayed_work(gb->ir_wq, &gb->ir_work, gb_ir_delay());

	dev_info(&gb->intf->dev,
		 "IR receiver registered as %s, polling %02x every %u ms\n",
		 dev_name(&rc->dev), GB_CMD_IR_POLL, clamp(ir_poll_ms, 10u, 1000u));
	return 0;
}

void gb_ir_unregister(struct gb_dev *gb)
{
	if (!gb->rc) {
		/* register() can fail after the queue exists. */
		if (gb->ir_wq) {
			destroy_workqueue(gb->ir_wq);
			gb->ir_wq = NULL;
		}
		return;
	}

	/* Clear the flag BEFORE cancelling. cancel_delayed_work_sync() waits for
	 * a running instance and drops a pending one, but the instance it waits
	 * for re-arms on its way out -- so cancelling without the flag first
	 * leaves work queued against a device we are about to free. */
	WRITE_ONCE(gb->ir_polling, false);
	cancel_delayed_work_sync(&gb->ir_work);
	destroy_workqueue(gb->ir_wq);	/* drains anything that slipped through */
	gb->ir_wq = NULL;

	rc_unregister_device(gb->rc);	/* frees rc; no rc_free_device() after */
	gb->rc = NULL;

	/* Always, not only on failure. A run that reports zero events is how you
	 * tell "no bug" apart from "the poller never saw anything", and those
	 * look identical in a log that only prints problems. */
	dev_info(&gb->intf->dev, "IR: %lu events, %lu malformed\n",
		 gb->ir_events, gb->ir_bad_frames);
}
