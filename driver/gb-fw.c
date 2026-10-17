// SPDX-License-Identifier: GPL-2.0
/* FX2LP firmware upload for the GameBridge's loader identity (03f3:009b).
 *
 * A straight port of tools/gbfw.c, which has worked since day one. The FX2LP has
 * no flash: its EEPROM carries a "C0" record supplying VID/PID/DID and nothing
 * else, so the 8051 boots held in reset with empty RAM. The host writes an image
 * over endpoint 0 with the built-in 0xA0 vendor request, releases the core, and
 * the firmware renumerates the device as 03f3:009c.
 *
 * The upload is VOLATILE by design. Internal RAM only, EEPROM untouched -- a
 * replug returns the device to 009b. That is deliberate: writing the EEPROM
 * would make it permanent and risks bricking a discontinued device from a
 * defunct company that cannot be replaced. Never do it.
 */
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/usb.h>

#include "gamebridge.h"

/* One 0xA0 vendor write. usb_control_msg_send() copies the payload into a
 * DMA-able bounce buffer itself, which matters here: fw->data comes from the
 * firmware loader and is not guaranteed suitable for DMA. Doing this by hand
 * with usb_control_msg() and passing fw->data directly is a real bug on some
 * hosts, and it is the kind that works on the machine you tested it on. */
static int fx2_write(struct usb_device *udev, u16 addr,
		     const void *data, u16 len)
{
	return usb_control_msg_send(udev, 0,
				    FX2_REQ_FIRMWARE_LOAD,
				    USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				    addr, 0, data, len, 2000, GFP_KERNEL);
}

static int fx2_cpu_reset(struct usb_device *udev, bool hold)
{
	u8 v = hold ? 0x01 : 0x00;	/* bit 0 = 8051RES */

	return fx2_write(udev, FX2_REG_CPUCS, &v, 1);
}

int gb_upload_firmware(struct usb_interface *intf)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	const struct firmware *fw;
	size_t off;
	int ret;

	/* request_firmware(), deliberately, not request_firmware_nowait(). A
	 * silent background failure is exactly the wrong ergonomics for a device
	 * that would otherwise sit at 009b forever looking like a driver bug. */
	ret = request_firmware(&fw, GB_FIRMWARE_NAME, &intf->dev);
	if (ret) {
		dev_err(&intf->dev, "firmware \"%s\" not found.\n", GB_FIRMWARE_NAME);
		dev_err(&intf->dev, "this device needs an 8051 image extracted from Adaptec's own\n");
		dev_err(&intf->dev, "driver, which cannot be redistributed. To build it:\n");
		dev_err(&intf->dev, "  tools/fetch-firmware.sh\n");
		dev_err(&intf->dev, "  python3 tools/extract-fw.py firmware/avcgbfl.sys \\\n");
		dev_err(&intf->dev, "      /lib/firmware/%s\n", GB_FIRMWARE_NAME);
		return ret;
	}

	/* Sanity-check the image rather than trusting the filename. 7624 bytes is
	 * what tools/extract-fw.py produces today, but hardcoding that would break
	 * the moment the extractor improves -- so check the two things that must be
	 * true of any FX2 image: it fits in internal RAM, and it starts with the
	 * LJMP that is the 8051 reset vector. */
	if (!fw->size || fw->size > FX2_RAM_SIZE) {
		dev_err(&intf->dev, "firmware is %zu bytes; must be 1..%d\n",
			fw->size, FX2_RAM_SIZE);
		ret = -EINVAL;
		goto out;
	}
	if (fw->data[0] != 0x02) {
		dev_err(&intf->dev,
			"firmware does not begin with an LJMP (first byte 0x%02x) -- "
			"not an 8051 image\n", fw->data[0]);
		ret = -EINVAL;
		goto out;
	}

	dev_info(&intf->dev, "uploading %zu bytes of firmware\n", fw->size);

	ret = fx2_cpu_reset(udev, true);
	if (ret) {
		dev_err(&intf->dev, "cannot halt the 8051: %d\n", ret);
		goto out;
	}

	for (off = 0; off < fw->size; off += FX2_LOAD_CHUNK) {
		size_t len = min_t(size_t, FX2_LOAD_CHUNK, fw->size - off);

		ret = fx2_write(udev, (u16)off, fw->data + off, (u16)len);
		if (ret) {
			dev_err(&intf->dev, "upload failed at offset %zu: %d\n",
				off, ret);
			/* Leave the core in reset. A half-loaded 8051 running is
			 * worse than one that never started: it would enumerate as
			 * something, and that something would be undefined. */
			goto out;
		}
	}

	ret = fx2_cpu_reset(udev, false);
	if (ret) {
		dev_err(&intf->dev, "cannot release the 8051: %d\n", ret);
		goto out;
	}

	/* The firmware sets RENUM and re-enumerates itself as 03f3:009c within a
	 * second or so. disconnect() then fires for this loader instance and
	 * probe() fires again for the capture one. No state is carried across --
	 * deliberately, because there is nothing worth carrying and a global would
	 * have to be freed on a path that can race a second device. */
	dev_info(&intf->dev, "firmware running; expect re-enumeration as %04x:%04x\n",
		 GB_VENDOR_ADAPTEC, GB_PRODUCT_CAPTURE);
out:
	release_firmware(fw);
	return ret;
}
