// SPDX-License-Identifier: GPL-2.0
#define _POSIX_C_SOURCE 200809L
/* gbcxreg -- read CX25840 registers through the KERNEL's I2C adapter.
 *
 *   gbcxreg /dev/i2c-N REG [REG...]     (REG in hex, 16-bit)
 *
 * Why not tools/gbcap cxreg: that tool detaches the kernel driver to take the
 * bus, and in that state every read came back 0x8a, the device ID included.
 * This goes through the bound driver's adapter instead -- the same
 * path, and the same transfer, cx25840_read() uses: one combined transaction,
 * a 2-byte register address written, 1 byte read back. That shape is the one
 * the firmware handles indefinitely.
 *
 * It always reads the device ID (0x101:0x100, must be 0x8404) first and refuses
 * to print anything else if that is wrong.
 *
 * Read-only: no register is ever written. Needs i2c-dev loaded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define CX25840_ADDR 0x44

static int rd(int fd, unsigned reg, unsigned char *val)
{
	unsigned char a[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg m[2] = {
		{ .addr = CX25840_ADDR, .flags = 0,        .len = 2, .buf = a },
		{ .addr = CX25840_ADDR, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	struct i2c_rdwr_ioctl_data x = { .msgs = m, .nmsgs = 2 };

	return ioctl(fd, I2C_RDWR, &x) == 2 ? 0 : -1;
}

int main(int argc, char **argv)
{
	unsigned char lo, hi, v;
	int fd, i;

	if (argc < 2) {
		fprintf(stderr, "usage: gbcxreg /dev/i2c-N [REG...]\n");
		return 2;
	}
	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 2;
	}
	if (rd(fd, 0x100, &lo) || rd(fd, 0x101, &hi)) {
		fprintf(stderr, "device ID read failed: %s\n", strerror(errno));
		return 1;
	}
	printf("0x101:0x100 = 0x%02x%02x %s\n", hi, lo,
	       hi == 0x84 && lo == 0x04 ? "(CX2584x, reads are trustworthy)"
					: "-- NOT 0x8404, refusing to dump");
	if (hi != 0x84 || lo != 0x04)
		return 1;
	for (i = 2; i < argc; i++) {
		unsigned reg = strtoul(argv[i], NULL, 16);

		if (rd(fd, reg, &v)) {
			printf("0x%03x = read failed: %s\n", reg, strerror(errno));
			continue;
		}
		printf("0x%03x = 0x%02x\n", reg, v);
	}
	close(fd);
	return 0;
}
