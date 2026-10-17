/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xc2028
 *
 * Copyright (c) 2007-2008 Mauro Carvalho Chehab <mchehab@kernel.org>
 *
 * An excerpt of the kernel's own drivers/media/tuners/xc2028.h. The copyright
 * line above is the upstream file's and is reproduced because this is a partial
 * verbatim copy of it, not original work.
 *
 * WHY THIS FILE EXISTS. That header is PRIVATE: it lives under drivers/, not
 * include/media/, so it is not in kernel-devel and an out-of-tree module cannot
 * include it. driver/Makefile already records the same problem for the media
 * sources generally -- drivers/media subdirectories in kernel-devel contains a Kconfig and
 * a Makefile and nothing else.
 *
 * WHY IT IS NEEDED AT ALL. The XC2028 only requests its firmware from
 * xc2028_set_config(), and xc2028_attach() calls that only `if (cfg->ctrl)`.
 * tuner-core attaches XC2028 with cfg.ctrl = NULL, so through tuner-core alone
 * the part never leaves XC2028_NO_FIRMWARE -- check_firmware() returns -EAGAIN
 * out of check_device_status() before it logs anything. Measured on a server,
 * 2026-08-26: "check_firmware called" and then silence, no request_firmware.
 * Supplying a ctrl through the s_config subdev op is the only way to start it.
 *
 * PROVENANCE. Copied verbatim from Linux v6.16,
 * drivers/media/tuners/xc2028.h, trimmed to the declarations we use. Same
 * licence as this driver. Refresh with:
 *
 *   curl -O https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/\
 *   plain/drivers/media/tuners/xc2028.h?h=vX.Y
 *
 * THE RISK, STATED. This is a struct layout shared with a module we do not
 * build. It has been unchanged since 2007, which is why copying is tolerable,
 * but it is a real coupling and it is the first thing to
 * check after a kernel upgrade breaks the tuner.
 */
#ifndef GB_XC2028_H
#define GB_XC2028_H

#define XC2028_DEFAULT_FIRMWARE "xc3028-v27.fw"

/* Demodulator IF, in kHz. 0 means "do not load an SCODE". */
#define XC3028_FE_DEFAULT	0

enum firmware_type {
	XC2028_AUTO = 0,	/* By default, auto-detects */
	XC2028_D2633,
	XC2028_D2620,
};

struct xc2028_ctrl {
	char			*fname;
	int			max_len;
	int			msleep;
	unsigned int		scode_table;
	unsigned int		mts   :1;
	unsigned int		input1:1;
	unsigned int		vhfbw7:1;
	unsigned int		uhfbw8:1;
	unsigned int		disable_power_mgmt:1;
	unsigned int		read_not_reliable:1;
	unsigned int		demod;
	enum firmware_type	type:2;
};

/* xc2028 commands for the bridge's tuner_callback. */
#define XC2028_TUNER_RESET	0
#define XC2028_RESET_CLK	1
#define XC2028_I2C_FLUSH	2

#endif /* GB_XC2028_H */
