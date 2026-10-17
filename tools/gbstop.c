// SPDX-License-Identifier: GPL-2.0
#define _POSIX_C_SOURCE 200809L
/* gbstop -- send the stop command (0x37) to an idle GameBridge from userspace.
 *
 *   gbstop /dev/bus/usb/BBB/DDD
 *
 * A diagnostic for the bug where after one stream cycle the front LED flashes
 * forever although the driver's 0x37 reached EP1OUT. Sending a second 0x37
 * with nothing streaming separates "the 8051 ignored the first one" from "0x37
 * does not govern the LED".
 *
 * The gamebridge driver must be UNBOUND from the interface first (usbfs cannot
 * claim an interface a kernel driver holds), and the interface must be at alt
 * 0, where EP1 OUT is bulk. Talks usbfs directly, so it needs no libusb.
 *
 * Sends exactly one byte, 0x37, and nothing else -- deliberately not a general
 * command tool. The device is irreplaceable; anything broader belongs in a tool
 * reviewed for that purpose.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

int main(int argc, char **argv)
{
	unsigned char stop = 0x37;
	unsigned int ifc = 0;
	struct usbdevfs_bulktransfer bt = {
		.ep = 0x01, .len = 1, .timeout = 1000, .data = &stop,
	};
	int fd, r;

	if (argc != 2) {
		fprintf(stderr, "usage: gbstop /dev/bus/usb/BBB/DDD\n");
		return 2;
	}
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 2;
	}
	if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc) < 0) {
		fprintf(stderr, "claim interface 0: %s (is gamebridge still bound?)\n",
			strerror(errno));
		return 2;
	}
	r = ioctl(fd, USBDEVFS_BULK, &bt);
	if (r < 0)
		printf("0x37: %s\n", strerror(errno));
	else
		printf("0x37: %d byte accepted by EP1 OUT\n", r);
	ioctl(fd, USBDEVFS_RELEASEINTERFACE, &ifc);
	close(fd);
	return r == 1 ? 0 : 1;
}
