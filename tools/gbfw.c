// SPDX-License-Identifier: GPL-2.0
/*
 * gbfw - Cypress EZ-USB FX2LP firmware loader and descriptor dumper
 *        for the Adaptec GameBridge AVC-1410/1510.
 *
 * The GameBridge ships an FX2LP with a "C0" EEPROM: the EEPROM supplies only the
 * VID/PID, while the 8051 core is held in reset with no firmware. The device
 * therefore enumerates as 03f3:009b ("AVC-1410/1510 Loader" per avcgbfl.inf) and
 * presents the FX2's *built-in default* descriptors, which describe nothing about
 * the real capture hardware.
 *
 * Uploading the Adaptec 8051 image into the chip's 16 KB internal RAM and
 * releasing the core makes the device re-enumerate as 03f3:009c with its real
 * descriptors. That is what this tool does.
 *
 * Protocol (EZ-USB TRM ch. 3, "EZ-USB Vendor Requests"):
 *   bmRequestType 0x40, bRequest 0xA0 ("firmware load"), wValue = target address.
 *   CPUCS is a special register at 0xE600; bit 0 is 8051RES (1 = hold in reset).
 *
 * Build:  make
 * Usage:  sudo ./gbfw info
 *         sudo ./gbfw load ../firmware/makoaudC.rom
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID_ADAPTEC   0x03f3
#define PID_LOADER    0x009b   /* AVC-1410/1510, 8051 in reset          */
#define PID_CAPTURE   0x009c   /* AVC-1410/1510, firmware running       */
#define VID_CYPRESS   0x04b4
#define PID_FX2LP     0x8613   /* bare FX2LP, EEPROM missing            */

#define FX2_REQ_FIRMWARE_LOAD 0xA0
#define FX2_REG_CPUCS         0xE600
#define FX2_RAM_SIZE          0x4000  /* 16 KB internal RAM on FX2LP    */
#define CHUNK                 1024
#define TIMEOUT_MS            2000

static const char *xfer_name(int t)
{
	switch (t & LIBUSB_TRANSFER_TYPE_MASK) {
	case LIBUSB_TRANSFER_TYPE_CONTROL:     return "control";
	case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "isoc";
	case LIBUSB_TRANSFER_TYPE_BULK:        return "bulk";
	case LIBUSB_TRANSFER_TYPE_INTERRUPT:   return "interrupt";
	default:                               return "?";
	}
}

/* Walk every configuration/interface/altsetting and print the endpoint map. */
static void dump_device(libusb_device *dev, struct libusb_device_descriptor *dd)
{
	printf("USB %04x:%04x  bcdDevice %04x  class %d/%d/%d  numConfigs %d\n",
	       dd->idVendor, dd->idProduct, dd->bcdDevice,
	       dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol,
	       dd->bNumConfigurations);

	for (int c = 0; c < dd->bNumConfigurations; c++) {
		struct libusb_config_descriptor *cfg;
		if (libusb_get_config_descriptor(dev, c, &cfg) != 0)
			continue;
		printf("  config %d: %d interface(s), maxpower %d mA, attrs 0x%02x\n",
		       cfg->bConfigurationValue, cfg->bNumInterfaces,
		       cfg->MaxPower * 2, cfg->bmAttributes);

		for (int i = 0; i < cfg->bNumInterfaces; i++) {
			const struct libusb_interface *itf = &cfg->interface[i];
			for (int a = 0; a < itf->num_altsetting; a++) {
				const struct libusb_interface_descriptor *id =
					&itf->altsetting[a];
				printf("    iface %d alt %d: class %d/%d/%d, %d endpoint(s)\n",
				       id->bInterfaceNumber, id->bAlternateSetting,
				       id->bInterfaceClass, id->bInterfaceSubClass,
				       id->bInterfaceProtocol, id->bNumEndpoints);

				for (int e = 0; e < id->bNumEndpoints; e++) {
					const struct libusb_endpoint_descriptor *ep =
						&id->endpoint[e];
					int mps  = ep->wMaxPacketSize & 0x7ff;
					int mult = ((ep->wMaxPacketSize >> 11) & 3) + 1;
					printf("      EP 0x%02x %-3s %-9s %4d bytes"
					       " x%d  interval %d",
					       ep->bEndpointAddress,
					       (ep->bEndpointAddress & 0x80) ? "IN" : "OUT",
					       xfer_name(ep->bmAttributes),
					       mps, mult, ep->bInterval);
					if (mult > 1)
						printf("  => %d B/microframe", mps * mult);
					printf("\n");
				}
			}
		}
		libusb_free_config_descriptor(cfg);
	}
}

static int cmd_info(libusb_context *ctx)
{
	libusb_device **list;
	ssize_t n = libusb_get_device_list(ctx, &list);
	int found = 0;

	for (ssize_t i = 0; i < n; i++) {
		struct libusb_device_descriptor dd;
		if (libusb_get_device_descriptor(list[i], &dd) != 0)
			continue;

		int match = (dd.idVendor == VID_ADAPTEC &&
		             (dd.idProduct == PID_LOADER || dd.idProduct == PID_CAPTURE)) ||
		            (dd.idVendor == VID_CYPRESS && dd.idProduct == PID_FX2LP);
		if (!match)
			continue;

		found++;
		printf("bus %d addr %d -- %s\n",
		       libusb_get_bus_number(list[i]),
		       libusb_get_device_address(list[i]),
		       dd.idProduct == PID_LOADER  ? "LOADER state (8051 in reset)" :
		       dd.idProduct == PID_CAPTURE ? "CAPTURE state (firmware running)" :
		                                     "bare FX2LP (no EEPROM)");
		dump_device(list[i], &dd);
		printf("\n");
	}

	if (!found)
		fprintf(stderr, "no GameBridge / FX2LP device found\n");
	libusb_free_device_list(list, 1);
	return found ? 0 : 1;
}

/* One 0xA0 vendor write. Data is copied by libusb before the transfer. */
static int fx2_write(libusb_device_handle *h, uint16_t addr, unsigned char *buf, int len)
{
	int r = libusb_control_transfer(h,
		LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
		FX2_REQ_FIRMWARE_LOAD, addr, 0, buf, len, TIMEOUT_MS);
	if (r != len) {
		fprintf(stderr, "write at 0x%04x failed: %s\n",
		        addr, r < 0 ? libusb_strerror(r) : "short transfer");
		return -1;
	}
	return 0;
}

static int fx2_cpu_reset(libusb_device_handle *h, int hold)
{
	unsigned char v = hold ? 1 : 0;
	return fx2_write(h, FX2_REG_CPUCS, &v, 1);
}

static int cmd_load(libusb_context *ctx, const char *path, int any_fx2)
{
	unsigned char *fw;
	long size;
	FILE *f = fopen(path, "rb");

	if (!f) { perror(path); return 1; }
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);

	if (size < 3 || size > FX2_RAM_SIZE) {
		fprintf(stderr, "%s: %ld bytes is not an FX2LP RAM image (3 to %d bytes)\n",
		        path, size, FX2_RAM_SIZE);
		fclose(f);
		return 1;
	}
	fw = malloc(size);
	if (!fw) {
		fprintf(stderr, "out of memory\n");
		fclose(f);
		return 1;
	}
	if (fread(fw, 1, size, f) != (size_t)size) {
		fprintf(stderr, "%s: short read\n", path);
		fclose(f); free(fw); return 1;
	}
	fclose(f);

	/* Raw 8051 images start with LJMP (0x02) at the reset vector. A file that
	 * begins with ':' is Intel HEX, which this tool deliberately does not parse. */
	if (fw[0] == ':') {
		fprintf(stderr, "%s looks like Intel HEX; this tool takes raw binary\n", path);
		free(fw); return 1;
	}
	printf("firmware %s: %ld bytes, reset vector %02x %02x %02x\n",
	       path, size, fw[0], fw[1], fw[2]);

	libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, VID_ADAPTEC, PID_LOADER);
	/* A bare FX2LP (04b4:8613) is a GameBridge whose EEPROM is missing -- or
	 * any Cypress dev board on the bus. Only when asked: programming an
	 * unrelated board because no GameBridge was plugged in is not a fallback
	 * (fifth review, 2026-10-17). */
	if (!h && any_fx2)
		h = libusb_open_device_with_vid_pid(ctx, VID_CYPRESS, PID_FX2LP);
	if (!h) {
		fprintf(stderr, "no GameBridge in loader state (03f3:009b)%s.\n"
		                "if it already shows 03f3:009c the firmware is loaded; replug to redo.\n",
		        any_fx2 ? " or bare FX2LP (04b4:8613)" : "");
		free(fw); return 1;
	}

	/* The kernel has no driver for this VID/PID, but detach defensively so a
	 * future in-tree driver does not make this tool fail confusingly. */
	libusb_set_auto_detach_kernel_driver(h, 1);
	if (libusb_claim_interface(h, 0) != 0)
		fprintf(stderr, "warning: could not claim interface 0, continuing\n");

	printf("halting 8051 (CPUCS 0x%04x <- 0x01)\n", FX2_REG_CPUCS);
	if (fx2_cpu_reset(h, 1)) goto fail;

	printf("uploading %ld bytes to internal RAM", size);
	fflush(stdout);
	for (long off = 0; off < size; off += CHUNK) {
		int len = (size - off < CHUNK) ? (int)(size - off) : CHUNK;
		if (fx2_write(h, (uint16_t)off, fw + off, len)) goto fail;
		printf(".");
		fflush(stdout);
	}
	printf(" done\n");

	printf("releasing 8051 (CPUCS 0x%04x <- 0x00)\n", FX2_REG_CPUCS);
	if (fx2_cpu_reset(h, 0)) goto fail;

	libusb_release_interface(h, 0);
	libusb_close(h);
	free(fw);

	printf("\nfirmware running. the device should now re-enumerate as 03f3:009c.\n");
	return 0;

fail:
	libusb_close(h);
	free(fw);
	return 1;
}

int main(int argc, char **argv)
{
	libusb_context *ctx = NULL;
	int rc;

	if (argc < 2) {
		fprintf(stderr, "usage: %s info\n       %s load <firmware.rom> [--any-fx2]\n",
		        argv[0], argv[0]);
		return 64;
	}
	if (libusb_init(&ctx) != 0) {
		fprintf(stderr, "libusb_init failed\n");
		return 1;
	}

	if (!strcmp(argv[1], "info"))
		rc = cmd_info(ctx);
	else if (!strcmp(argv[1], "load") && argc == 3)
		rc = cmd_load(ctx, argv[2], 0);
	else if (!strcmp(argv[1], "load") && argc == 4 && !strcmp(argv[3], "--any-fx2"))
		rc = cmd_load(ctx, argv[2], 1);
	else {
		fprintf(stderr, "unknown command\n");
		rc = 64;
	}

	libusb_exit(ctx);
	return rc;
}
