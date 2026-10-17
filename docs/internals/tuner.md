# The XC2028 tuner

**Loaded and ACTIVE, never tuned.** The part accepts its
firmware over this bridge and the driver reports it ready. Whether it produces
valid tuning is untested.

Everything here is behind `tuner=1`, which is **off by default**.

---

### Where the reset line is

Read out of the 8051. Command `0x87` at `0x0109`:

```
0109:  CLR A / MOV IOA,A            port A low
010c:  delay 100
0112:  MOV IOD,#0x87 / MOV OED,#0x9f
011f..0148:  MOV IOA,#0x08 / MOV IOA,#0   x3, with delays   port A bit 3 strobe
014f:  R5=0 R7=0  ->  clear port D bit 0
0155:  R5=0 R7=7  ->  clear port D bit 7   ]
015c:  delay 10                            ]  a RESET PULSE
0163:  R5=1 R7=7  ->  set   port D bit 7   ]
016a:  ANL IOD,#0xf7 / ORL IOD,#0x10       LED back to steady
0170:  reply 0x08 (ACK)
```

Port D **bit 1** is the CX25840's reset (command `0x83`). Bit 7 is the only other
reset-shaped pulse in the image, and this board has exactly one other resettable
part. **That bit 7 is the tuner's reset is an inference, not a measurement** -
it is the most likely thing on this page to be wrong.

The parameterised GPIO command `0x0e` cannot substitute. Its handler at `0x0379`
bounds the bit number to **3..6** (`SUBB A,#0x03 / JC`, then
`SETB C / SUBB A,#0x06 / JNC`), and under `OED 0x9f` only bits 3 and 4 of those
are outputs. `0x0e` is an LED command. `0x87` is the only route to bit 7.

## The firmware loads

On the debug kernel under KASAN, `tuner=1`:

```
Loading firmware for type=BASE (1), id 0.      8706 bytes
  ... 2.5 s ...
Load init1 firmware, if exists                 done
Loading firmware for type=(0), id b700.        done
Loading SCODE for MONO SCODE HAS_IF_4320       done
```

8706 bytes into the part, thirteen at a time, plus the SCODE. Zero splats.

### One thing had to be supplied

The XC2028 requests its firmware from exactly one place - `xc2028_set_config()` -
and `xc2028_attach()` reaches it only `if (cfg->ctrl)`. **tuner-core attaches
XC2028 with `cfg.ctrl = NULL`.** So through tuner-core alone the part never
leaves `XC2028_NO_FIRMWARE`, and the symptom is silence:

```
xc2028 1-0061: check_firmware called
   (nothing further -- check_device_status() returns -EAGAIN and it bails
    before logging anything at all)
```

An `xc2028_ctrl` goes in through the `s_config` subdev op, and that is what makes
the load happen. It needs `struct xc2028_ctrl` from a **private** header
(`drivers/media/tuners/xc2028.h`) that kernel-devel does not ship -
`driver/xc2028.h` is a trimmed verbatim copy with its provenance and coupling
risk stated at the top.

## What actually blocks it: this bridge cannot read the tuner

After the SCODE, xc2028 reads back its version - `wcount=2, rcount=2`. Two shapes
were tried against `0x61` and **both fail**:

| shape | against `0x44` | against `0x61` |
|---|---|---|
| `wcount=2, rcount=1` | **100 of 100 correct** | **-EIO, then the endpoint wedges** |
| `wcount=2, rcount=2` | wedges | -EIO, endpoint wedges |

**The "100 of 100" figure is a property of the shape AND THE SLAVE.** Every one
of those transactions went to the decoder.

The failure is `-EIO`, not `-ENXIO`, so it is not a plain NACK: the firmware
answers, but with fewer bytes than asked for, and the command endpoint is wedged
afterwards.

### A mechanism that fits

Writes to the tuner go through command `0x08`, which is a **separate firmware
implementation** - polled, I2C interrupt disabled, never touching the read state
machine - and 8706 bytes went through it without complaint. Reads go through
`0x09`, whose read path carries the known two-instruction bug
(see [the I2C path](README.md#the-i2c-path-and-why-reads-are-fragile)): it refuses
when its state variable is non-zero, signals that by clearing carry, and its caller
never tests carry, then busy-waits forever.

## The diagnostic lie, and what it proved

`i2c_fake_tuner_read=1` answers the XC2028's register reads from a table and
**touches no bus**, so it cannot wedge anything. `ctrl.read_not_reliable` is set
alongside, which makes xc2028 jump past both the firmware-version comparison and
the hwmodel check - so the faked values are never inspected.

```
Loading firmware for type=BASE (1), id 0.
i2c i2c-1: FAKING XC2028 reads: reg 0004 -> 1227 (bus untouched)
xc2028 1-0061: Device is Xceive 3028 version 1.2, firmware version 2.7
  ... second check_firmware, NO reload ...
xc2028 1-0061: Device is Xceive 3028 version 1.2, firmware version 2.7
```

This is a trick to use the driver and the code says so. It is justified only because
the read-back is a **verification** step - everything the tuner needs in order to
tune had already been written to it successfully.

**It will not become a default**, and neither will `tuner=1`, until a picture has
been seen. (probably my Atari 65XE)

## What is left

**Does it tune?** Reaching ACTIVE means the driver believes the part is ready, not
that RF becomes video. That test needs `v4l2-ctl` and an RF source, so it belongs
on the workstation with my Atari's RF output (channel 3 should be 61.25 MHz).

```sh
sudo insmod driver/gamebridge.ko tuner=1 i2c_fake_tuner_read=1
v4l2-ctl -d /dev/videoN --list-inputs        # a third input, "Tuner"
v4l2-ctl -d /dev/videoN --set-input 2
v4l2-ctl -d /dev/videoN --set-freq 61.25
v4l2-ctl -d /dev/videoN --get-tuner
```

## Parameters

| | |
|---|---|
| `tuner=1` | attach the XC2028. Off by default - never seen an RF signal. |
| `i2c_fake_tuner_read=1` | the diagnostic lie above. Off by default. |
| `i2c_rcount2=1` | allow `wcount=2/rcount=2`. **Measured to wedge.** Kept only so the experiment is reproducible. |
| `i2c_tuner_split=1` | synthesised continue-read. Fails at step 1; the tuner cannot be read at all. |
