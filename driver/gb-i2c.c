// SPDX-License-Identifier: GPL-2.0
/* The GameBridge's I2C bus, exposed as a Linux i2c_adapter.
 *
 * This is the piece that makes the whole approach pay. The FX2 firmware has an
 * I2C passthrough; wrapping it as an i2c_adapter means the in-kernel cx25840
 * subdev driver -- standards, input muxing, brightness/contrast/saturation/hue,
 * the sync slicer, the PLL, maintained since 2005 -- binds at 0x44 with no work
 * from us. Three devices answer on this bus: the CX25840 at 0x44, the FX2's own
 * boot EEPROM at 0x50, and the XC2028 tuner at 0x61.
 *
 * The firmware offers two commands, and they are bus primitives rather than
 * register operations:
 *
 *   08 <addr7> <len> <payload...>              write <len> bytes to <addr7>
 *   09 <wcount> <rcount> <addr7> <data...>     write, repeated START, read
 *
 * Neither knows anything about registers. captures/cx25840-init.txt reads like a
 * register table only because 16-bit big-endian register addressing is the
 * CX25840's convention, not the firmware's.
 *
 * WHAT SHAPES THIS FILE IS A MEASUREMENT, NOT THE PROTOCOL.
 *
 * Command 0x09 wedges the 8051 for some transaction shapes and not others. The
 * cause is a two-instruction firmware bug: the routine that starts a read
 * refuses when its state variable is non-zero and signals that by clearing the
 * carry flag, and its caller never tests the carry, then busy-waits forever for
 * a transfer that was never started.
 *
 * Measured, 2026-08-24:
 *
 *     wcount=0 rcount=0   fine, and RESYNCS a wedged state machine
 *     wcount=1 rcount=1   wedges immediately, on 0x44 and 0x50 alike
 *     wcount=2 rcount=1   100 of 100 correct, with no resync at all
 *     wcount=2 rcount=2   wedges  (measured 2026-08-26, the last gap)
 *     wcount=2 rcount=4   wedges
 *
 * The matrix is now complete, and the rule is RCOUNT MUST BE 1.
 *
 * So this adapter refuses the shapes that wedge rather than issuing them and
 * recovering afterwards. A wedge stops the command endpoint answering for
 * everything, not just I2C, so provoking one deliberately would be a poor trade
 * for a driver that has a video stream to keep alive.
 *
 * The one shape that matters works perfectly: wcount=2, rcount=1 is exactly
 * what cx25840_read() issues, and cx25840_probe() reads 0x101 then 0x100 one
 * byte at a time.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>

#include "gamebridge.h"

/* Allow the wcount=2/rcount=2 read shape. It WEDGES -- measured 2026-08-26, see
 * the use site. Kept behind a parameter, off, so the experiment is reproducible
 * rather than a claim in a comment. The XC2028 is the only thing on this bus
 * that wants this shape, and it cannot have it. */
/* Answer the XC2028's register reads from a table instead of the bus.
 *
 * THIS LIES TO A DRIVER. It is here as a diagnostic and it must never become a
 * default. What justifies it: no read from 0x61 works through this bridge at any
 * shape (see the step-1 result recorded in git), and xc2028's check_firmware()
 * treats the version read-back as fatal -- goto fail -- so the part can never
 * reach XC2028_ACTIVE however well the firmware upload went. And it went
 * perfectly: 8706 bytes of base image plus SCODE, all acknowledged.
 *
 * The read-back is a VERIFICATION step, not a functional one. Everything the
 * tuner needs in order to tune has already been written to it. So the question
 * this answers, and it cannot be answered any other way, is: does the XC2028
 * actually work through this bridge, or is the read the only thing in the way?
 *
 * With ctrl.read_not_reliable set, xc2028 jumps past BOTH the firmware-version
 * comparison and the hwmodel check, so the values here are never inspected --
 * they only have to arrive. They are plausible rather than zero so the driver's
 * own debug line reads sensibly.
 *
 * It touches no bus at all, which is the point: a real attempt wedges the
 * command endpoint, and this cannot. */
static bool i2c_fake_tuner_read;
module_param(i2c_fake_tuner_read, bool, 0644);
MODULE_PARM_DESC(i2c_fake_tuner_read,
		 "DIAGNOSTIC LIE: answer XC2028 register reads from a table rather "
		 "than the bus, because this bridge cannot read that slave. Lets the "
		 "tuner reach ACTIVE so tuning itself can be tested. Default off.");

/* Synthesise a 2-byte read as an addressed 1-byte read followed by a bare read.
 * Off by default until it is shown to return the right bytes -- it is safe (both
 * sub-shapes are known-good) but possibly WRONG, which is the harder problem. */
static bool i2c_tuner_split;
module_param(i2c_tuner_split, bool, 0644);
MODULE_PARM_DESC(i2c_tuner_split,
		 "synthesise 2-byte reads as addressed-read + bare-read, for the "
		 "XC2028. Safe shapes, correctness unproven. Default off.");

static bool i2c_rcount2;
module_param(i2c_rcount2, bool, 0644);
MODULE_PARM_DESC(i2c_rcount2,
		 "allow 2-byte I2C reads (wcount=2/rcount=2). MEASURED TO WEDGE the "
		 "command endpoint, 2026-08-26; kept only so the experiment can be "
		 "re-run. Leave it off.");

/* One command 0x09. Returns 0, or negative errno. */
static int gb_i2c_cmd09(struct gb_dev *gb, u16 addr,
			const u8 *wbuf, int wlen, u8 *rbuf, int rlen)
{
	u8 cmd[GB_CMD_MAX], reply[GB_CMD_MAX];
	int n = 0, ret;

	if (wlen < 0 || rlen < 0 || wlen + 4 > GB_CMD_MAX || rlen + 1 > GB_CMD_MAX)
		return -EINVAL;

	cmd[n++] = GB_CMD_I2C_XFER;
	cmd[n++] = (u8)wlen;
	cmd[n++] = (u8)rlen;
	cmd[n++] = (u8)addr;		/* 7-bit; the firmware doubles it itself */
	if (wlen)
		memcpy(cmd + n, wbuf, wlen);
	n += wlen;

	ret = gb_cmd(gb, cmd, n, reply, rlen + 1);
	if (ret < 0)
		return ret;
	if (ret < 1)
		return -EIO;

	/* Reply byte 0 is the bus result: 0x08 ACK, 0x07 NACK. */
	if (reply[0] == GB_I2C_NACK)
		return -ENXIO;
	if (reply[0] != GB_I2C_ACK)
		return -EIO;
	if (ret < rlen + 1)
		return -EIO;
	if (rlen && rbuf)
		memcpy(rbuf, reply + 1, rlen);
	return 0;
}

/* Resync the read state machine.
 *
 * A zero-count transaction is safe on any address and drives the state machine
 * to a terminal state, which is the whole of what the historical 112-address
 * sweep was doing -- it was never "write-only transactions", because a zero
 * write count makes the firmware skip the write phase and fall through to the
 * read path. One transaction is enough; the sweep cost 1-2 s and this costs
 * about a millisecond.
 *
 * Nothing here should ever need it, because the shapes that wedge are refused.
 * It exists for a bus left wedged by something else -- a killed userspace tool,
 * for instance -- so the module can recover rather than requiring a replug. */
static void gb_i2c_resync(struct gb_dev *gb)
{
	gb_i2c_cmd09(gb, 0x08, NULL, 0, NULL, 0);
}

/* Plain write, command 0x08. This is a SEPARATE firmware implementation from
 * command 0x09: polled, with the I2C interrupt disabled, and it never touches
 * the read state machine. That is why the historical "does a write recover a
 * wedged read?" experiments all came back negative -- they could not have done
 * anything else. */
static int gb_i2c_write(struct gb_dev *gb, u16 addr, const u8 *buf, int len)
{
	u8 cmd[GB_CMD_MAX];
	int ret;

	if (len < 0 || len > GB_I2C_MAX_LEN)
		return -EINVAL;

	cmd[0] = GB_CMD_I2C_WRITE;
	cmd[1] = (u8)addr;
	cmd[2] = (u8)len;
	if (len)
		memcpy(cmd + 3, buf, len);

	ret = gb_cmd(gb, cmd, 3 + len, NULL, 0);
	return ret < 0 ? ret : 0;
}

/* A combined read of more than one byte, synthesised from single-byte reads.
 *
 * cx25840_read4() issues wcount=2, rcount=4, and rcount>1 wedges. But a 32-bit
 * CX25840 register is four consecutive byte registers, so four single-byte
 * reads at incrementing addresses return the same value -- verified on hardware
 * against 0x0100..0x0103, sustainably and with no resync.
 *
 * This is NOT the dangerous split that breaks a transfer into pieces and
 * inserts a STOP where the slave expected a repeated START. Each synthesised
 * read here is a complete, legal transaction with its own address phase.
 * Nothing is inserted mid-transfer; there is simply more bus traffic.
 *
 * It IS a board quirk, and it is only sound for a slave with explicit register
 * addressing and no auto-increment semantics of its own. That is why it is
 * restricted to the decoder by address rather than applied generally: the
 * EEPROM at 0x50 takes a one-byte address and does auto-increment, so splitting
 * a read there would silently return the wrong bytes.
 */
static int gb_i2c_read_split(struct gb_dev *gb, u16 addr,
			     const u8 *wbuf, u8 *rbuf, int rlen)
{
	u16 reg = ((u16)wbuf[0] << 8) | wbuf[1];
	int i, ret;

	for (i = 0; i < rlen; i++) {
		u8 a[2] = { (u8)((reg + i) >> 8), (u8)((reg + i) & 0xff) };

		ret = gb_i2c_cmd09(gb, addr, a, 2, rbuf + i, 1);
		if (ret)
			return ret;
	}
	return 0;
}


/* A 2-byte read synthesised as one addressed read plus one BARE read.
 *
 * The XC2028 wants wcount=2/rcount=2 and this firmware cannot do it -- rcount
 * must be 1, measured. gb_i2c_read_split() cannot help either: it works for the
 * decoder only because a CX25840 32-bit register is four consecutive BYTE
 * registers, so re-addressing reg+1 genuinely fetches the next byte. An XC2028
 * register is one 16-bit value at one address, so re-addressing would fetch the
 * same value again, not its second half.
 *
 * So instead of re-addressing, continue: address the register and take one byte
 * (wcount=2/rcount=1, the 100-of-100 cell), then issue a bare read
 * (wcount=0/rcount=1) and hope the slave hands over the next byte of the value
 * it is already streaming.
 *
 * BOTH sub-shapes avoid the two known killers -- neither uses wcount=1, neither
 * uses rcount>1 -- which is the whole reason this is worth trying at all after
 * wcount=2/rcount=2 took the endpoint down.
 *
 * WHETHER IT IS CORRECT IS THE OPEN QUESTION, not whether it is safe. A bare
 * read is a fresh START; whether the XC2028's I2C engine continues the previous
 * value or restarts it is not documented anywhere we have. If it restarts, both
 * bytes come back identical and xc2028 will reject the version rather than
 * mis-trust it -- a wrong answer that announces itself. */
static int gb_i2c_read_continue(struct gb_dev *gb, u16 addr,
				const u8 *wbuf, u8 *rbuf)
{
	int ret;

	/* Report each step separately. The two sub-shapes have very different
	 * pedigrees and conflating their failures wasted a replug:
	 *
	 *   step 1  wcount=2/rcount=1 -- the "100 of 100" cell, but that was
	 *           measured against the DECODER at 0x44 and has never been
	 *           issued to the tuner at 0x61
	 *   step 2  wcount=0/rcount=1 -- a bare read, which gb_i2c_xfer() has
	 *           always described as untested on this device
	 *
	 * If step 1 fails, reads from 0x61 are impossible at any shape and the
	 * tuner is unreachable through this bridge. If step 1 succeeds and step 2
	 * fails, only the continuation is the problem. Those are entirely
	 * different conclusions. */
	ret = gb_i2c_cmd09(gb, addr, wbuf, 2, rbuf, 1);
	dev_info(&gb->i2c_adap.dev,
		 "continue-read 0x%02x step 1 (wcount=2/rcount=1): rc=%d%s\n",
		 addr, ret, ret ? "" : " -- byte0 read OK");
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	ret = gb_i2c_cmd09(gb, addr, NULL, 0, rbuf + 1, 1);
	dev_info(&gb->i2c_adap.dev,
		 "continue-read 0x%02x step 2 (wcount=0/rcount=1): rc=%d\n",
		 addr, ret);
	return ret;
}

static int gb_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct gb_dev *gb = i2c_get_adapdata(adap);
	int ret;

	/* (a) a single write, including the zero-length address probe that
	 * i2cdetect and i2c_detect() use. */
	if (num == 1 && !(msgs[0].flags & I2C_M_RD)) {
		if (msgs[0].len == 0) {
			/* An address-only probe. Route it through command 0x09 with
			 * both counts zero: that is the shape the bus scan uses, it
			 * is known good, and it reports ACK/NACK cleanly. */
			ret = gb_i2c_cmd09(gb, msgs[0].addr, NULL, 0, NULL, 0);
			return ret ? ret : 1;
		}
		ret = gb_i2c_write(gb, msgs[0].addr, msgs[0].buf, msgs[0].len);
		return ret ? ret : 1;
	}

	/* (b) a single read with no preceding write. Never exercised on this
	 * device by anything -- every read in the captured Windows session has
	 * wcount >= 1 -- so it is allowed but announced, once, the first time
	 * something uses it. */
	if (num == 1 && (msgs[0].flags & I2C_M_RD)) {
		if (msgs[0].len != 1) {
			dev_warn_once(&adap->dev,
				      "refusing a %u-byte bare read: rcount>1 wedges the firmware\n",
				      msgs[0].len);
			return -EOPNOTSUPP;
		}
		dev_warn_once(&adap->dev,
			      "bare read (wcount=0) used for the first time; untested on this device\n");
		ret = gb_i2c_cmd09(gb, msgs[0].addr, NULL, 0, msgs[0].buf, 1);
		return ret ? ret : 1;
	}

	/* (c) write-then-read at one address. This is the case that matters: it is
	 * the shape cx25840_read() and cx25840_read4() take. */
	if (num == 2 &&
	    !(msgs[0].flags & I2C_M_RD) && (msgs[1].flags & I2C_M_RD) &&
	    msgs[0].addr == msgs[1].addr) {

		/* wcount=1 wedges the 8051 outright, on every address tried. Refuse
		 * it rather than issue it and clean up afterwards -- a wedge takes
		 * the whole command endpoint down, not just this transfer. Nothing
		 * on this bus needs it: the CX25840 uses 2-byte register addresses,
		 * and the EEPROM is not something the kernel driver reads. */
		if (msgs[0].len == 1) {
			dev_warn_once(&adap->dev,
				      "refusing a 1-byte address write: it wedges the firmware\n");
			return -EOPNOTSUPP;
		}
		if (msgs[0].len < 1)
			return -EOPNOTSUPP;

		if (msgs[1].len == 1) {
			ret = gb_i2c_cmd09(gb, msgs[0].addr, msgs[0].buf,
					   msgs[0].len, msgs[1].buf, 1);
			return ret ? ret : 2;
		}

		if (msgs[0].len == 2 && msgs[0].addr == GB_I2C_ADDR_DECODER) {
			ret = gb_i2c_read_split(gb, msgs[0].addr, msgs[0].buf,
						msgs[1].buf, msgs[1].len);
			return ret ? ret : 2;
		}

		/* wcount=2, rcount=2 -- MEASURED 2026-08-26, AND IT WEDGES.
		 *
		 * This was the one cell of the read matrix nobody had tested, and
		 * it is the exact shape the XC2028 needs. It fails with -EIO and
		 * takes the command endpoint down with it: the next command, the
		 * tuner reset, came back -ETIMEDOUT. gb_i2c_resync() did not fully
		 * recover it either. The path is kept, off, because the experiment
		 * should be reproducible and because the alternative is a comment
		 * claiming a result nobody can re-run.
		 *
		 * So the rule is simply RCOUNT MUST BE 1. What follows was the
		 * reasoning before the measurement, and it is left because it is
		 * why the experiment was worth running.
		 *
		 * The measured table is:
		 *
		 *     wcount=0 rcount=0   works, and resyncs a wedge
		 *     wcount=1 rcount=1   WEDGES, on 0x44 and 0x50 alike
		 *     wcount=2 rcount=1   100 of 100 correct
		 *     wcount=2 rcount=4   WEDGES
		 *
		 * So wcount=1 is established as fatal and rcount=4 is established
		 * as fatal, but rcount=2 sits in the untested gap and the blanket
		 * "rcount>1 wedges" below is an extrapolation from rcount=4, not a
		 * measurement. The XC2028 reads a 16-bit register in one
		 * transaction, so the decoder's split trick is unsound for it --
		 * two single-byte reads would return two different things, not the
		 * halves of one value.
		 *
		 * Opt-in, because being wrong here costs a wedged command endpoint
		 * rather than a failed transfer. Being wrong is exactly what
		 * happened, and the resync was not enough -- so do NOT turn this on
		 * expecting it to work. It is a documented dead end. */
		/* The diagnostic lie. First, because it must pre-empt every path
		 * that would touch the bus. */
		if (i2c_fake_tuner_read && msgs[0].len == 2 && msgs[1].len == 2 &&
		    msgs[0].addr == GB_I2C_ADDR_TUNER) {
			u16 reg = ((u16)msgs[0].buf[0] << 8) | msgs[0].buf[1];
			u16 val;

			switch (reg) {
			case 0x0004:	/* version: hw 1.2, firmware 2.7 */
				val = 0x1227;
				break;
			case 0x0008:	/* hwmodel, decimal 3028 */
				val = 3028;
				break;
			default:
				val = 0;
				break;
			}
			/* xc2028_get_reg() composes (ibuf[1]) | (ibuf[0] << 8). */
			msgs[1].buf[0] = val >> 8;
			msgs[1].buf[1] = val & 0xff;
			dev_info_once(&adap->dev,
				      "FAKING XC2028 reads: reg %04x -> %04x (bus untouched)\n",
				      reg, val);
			return 2;
		}

		/* The synthesised continue-read, for a 2-byte read this firmware
		 * cannot issue natively. Tried before the native shape because it
		 * uses only known-safe primitives. */
		if (i2c_tuner_split && msgs[0].len == 2 && msgs[1].len == 2 &&
		    msgs[0].addr != GB_I2C_ADDR_DECODER) {
			ret = gb_i2c_read_continue(gb, msgs[0].addr,
						   msgs[0].buf, msgs[1].buf);
			if (ret) {
				dev_warn(&adap->dev,
					 "continue-read from 0x%02x failed (%d); resyncing\n",
					 msgs[0].addr, ret);
				gb_i2c_resync(gb);
				return ret;
			}
			dev_info_once(&adap->dev,
				      "continue-read from 0x%02x returned %02x %02x\n",
				      msgs[0].addr, msgs[1].buf[0], msgs[1].buf[1]);
			return 2;
		}

		if (i2c_rcount2 && msgs[0].len == 2 && msgs[1].len == 2) {
			ret = gb_i2c_cmd09(gb, msgs[0].addr, msgs[0].buf, 2,
					   msgs[1].buf, 2);
			if (ret) {
				dev_warn(&adap->dev,
					 "wcount=2/rcount=2 read from 0x%02x failed (%d); "
					 "resyncing\n", msgs[0].addr, ret);
				gb_i2c_resync(gb);
				return ret;
			}
			dev_info_once(&adap->dev,
				      "wcount=2/rcount=2 read from 0x%02x SUCCEEDED -- "
				      "the untested cell of the read matrix works\n",
				      msgs[0].addr);
			return 2;
		}

		dev_warn_once(&adap->dev,
			      "refusing a %u-byte read from 0x%02x: rcount>1 wedges, and "
			      "splitting is only sound for the decoder\n",
			      msgs[1].len, msgs[0].addr);
		return -EOPNOTSUPP;
	}

	/* Anything else -- more than two messages, two reads, mismatched
	 * addresses, I2C_M_NOSTART, 10-bit addressing. Do NOT try to synthesise it
	 * from multiple transactions: inserting a STOP where the slave expected a
	 * repeated START silently produces wrong values, which is the worst failure
	 * mode available. */
	return -EOPNOTSUPP;
}

static u32 gb_i2c_func(struct i2c_adapter *adap)
{
	/* I2C_FUNC_I2C is not optional: cx25840_loadfw() checks for it and refuses
	 * outright without it. */
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm gb_i2c_algo = {
#ifdef GB_COMPAT_I2C_MASTER_XFER	/* set by Kbuild from the header; see there */
	.master_xfer = gb_i2c_xfer,
#else
	.xfer      = gb_i2c_xfer,
#endif
	.functionality = gb_i2c_func,
};

/* The i2c core enforces these before ever calling .xfer, so a subdev asking for
 * something impossible gets a clean -EOPNOTSUPP instead of a truncated transfer.
 *
 * The length limits are the arithmetic ceiling implied by the FX2's 64-byte EP1
 * buffers, and the Windows driver demonstrably reaches it during the CX25840
 * audio microcode download. They have not been walked up from our side, so if a
 * long write ever misbehaves this is the first thing to suspect. */
static const struct i2c_adapter_quirks gb_i2c_quirks = {
	.flags                = I2C_AQ_COMB_WRITE_THEN_READ,
	.max_num_msgs         = 2,
	.max_write_len        = GB_I2C_MAX_LEN,
	.max_read_len         = GB_I2C_MAX_LEN,
	.max_comb_1st_msg_len = GB_I2C_MAX_LEN,
	.max_comb_2nd_msg_len = GB_I2C_MAX_LEN,
};

int gb_i2c_register(struct gb_dev *gb)
{
	int ret;

	strscpy(gb->i2c_adap.name, "gamebridge", sizeof(gb->i2c_adap.name));
	gb->i2c_adap.owner      = THIS_MODULE;
	gb->i2c_adap.algo       = &gb_i2c_algo;
	gb->i2c_adap.quirks     = &gb_i2c_quirks;
	gb->i2c_adap.dev.parent = &gb->intf->dev;
	i2c_set_adapdata(&gb->i2c_adap, gb);

	/* algo_data is NOT decoration here, and it is not the same thing as
	 * adapdata. tuner-core's do_tuner_callback() passes
	 * `priv->i2c_props.adap->algo_data` as the `dev` argument to a bridge's
	 * tuner_callback on the ANALOG side (the digital side uses fe->dvb->priv
	 * instead). Leave it NULL and the XC2028's mandatory reset callback is
	 * handed a NULL device pointer. */
	gb->i2c_adap.algo_data = gb;

	/* Clear anything a previously killed userspace tool left wedged, so the
	 * module comes up on a working bus rather than inheriting someone else's
	 * mess. Costs one transaction. */
	gb_i2c_resync(gb);

	ret = i2c_add_adapter(&gb->i2c_adap);
	if (ret)
		return ret;
	gb->i2c_registered = true;
	return 0;
}

void gb_i2c_unregister(struct gb_dev *gb)
{
	if (!gb->i2c_registered)
		return;
	i2c_del_adapter(&gb->i2c_adap);
	gb->i2c_registered = false;
}
