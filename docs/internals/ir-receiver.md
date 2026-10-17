# The infrared receiver

The unit has an IR window on the front and shipped with a 28-button Adaptec
remote. Both work, and the driver reads them as a standard Linux rc-core device.

**This is the only feature in the driver with no vendor implementation behind
it.** Everything else here was recovered by watching Adaptec's own software do
the thing and then doing the same thing. Adaptec's driver has no IR support
anywhere in it (see [the INF findings](vendor-inf-findings.md)) - whatever
read this remote under Windows lived in the GameBridge application, which I
didn't try.

---

## What command `0x85` is

`0x85` had been in the bring-up prologue since the first week, commented as
*"reads a counter … looks like a status poll"*. It is the IR receiver.

It answers **five bytes**:

| byte | meaning |
|---|---|
| 0 | `0x08` a frame is latched and bytes 1–4 are it; `0x07` nothing new, **bytes 1–4 are stale**. Reading clears the latch. |
| 1 | address low - `0x82` on this remote |
| 2 | address high - `0xf1` on this remote |
| 3 | command, with **bit 7 set while the key is held** |
| 4 | `~command`, i.e. `(byte3 & 0x7f) ^ byte4 == 0xff` always |

Byte 0 is not an IR field. It is `GB_I2C_ACK` / `GB_I2C_NACK`, already in
`gamebridge.h` - the firmware's generic "this reply carries data" flag, which
every command uses.

`0x82` and `0xf1` are **not** complements (`0x82 ^ 0xff` is `0x7d`), so this is
**extended NEC**. The scancode rc-core expects is
`addr_lo << 16 | addr_hi << 8 | cmd`, matching how `ir-nec-decoder.c` composes
`RC_PROTO_NECX` - so every button on this remote is `0x82f1XX`.

**Bit 7 of byte 3 is the repeat flag.** The device tells us a key is held rather
than making us guess it from arrival times, which is why `gb-ir.c` calls
`rc_repeat()` and not a second `rc_keydown()`. Held keys arrive every ~108 ms,
the NEC repeat interval; the driver polls at 50 ms so it cannot miss one.

### The firmware agrees, and adds two things

All of the above was read off the wire. Disassembling the 8051 image afterwards
confirms it independently, and answers two questions:

```
1d70:  MOV A,0x09 / CJNE A,#0x82,0x1d80    ] address must be 0x82 ...
1d75:  MOV A,0x0a / CJNE A,#0xf1,0x1d80    ] ... and 0xf1, or the frame is dropped
1d7a:  ORL IOD,#0x08                         LED bit 3 ON
1d7d:  MOV 0x08,#0x01                        latch: a frame is available

03cc:  MOV A,0x08 / XRL A,#0x01 / JNZ 0x0401   no frame -> the NACK path
03d2:  MOV 0x08,A                              clear the latch
03d4:  MOV DPTR,#0xE7C0 / MOV A,#0x08 / MOVX   reply byte 0 = 0x08 = ACK
03da:  ...reads internal RAM 0x09 onward       the frame bytes
```

That is the latch-and-clear protocol i guessed from watching bytes, but in
the firmware's own instructions.

**The remote's address is hardcoded.** A frame is latched only if the address is
`0x82 0xf1`. **A different NEC remote is silently ignored**, however valid its
framing - this is a firmware constraint and no driver change can lift it. Anyone
reaching for a universal remote should know that before starting.

**`ORL IOD,#0x08` is the LED.** The receive path lights port D bit 3 on a valid
frame, which is why the front LED shows orange while buttons are pressed.
The same bit is one of the two the blink routine drives.

## The keymap

Established by pressing all 28 buttons in reading order and reading the codes
back - **twice, on two machines, through two entirely different code paths**:

| | |
|---|---|
| `tools/gbcap irwatch` | libusb, workstation, xHCI, kernel 6.16 |
| `driver/gb-ir.c` | in-kernel, test machine, EHCI, kernel 6.19+debug |

Both runs returned the same 28 codes in the same order, byte for byte.

```
START 33   ▲ 36   ↩ 34   ▶/❚❚ 30
◀     38   OK 03  ▶ 39   ■    23
◀◀    31   ▼ 37   ▶▶ 15  ●    35
1     26   2  19   3  10  CH/PG+ 01
4     27   5  20   6  11  CH/PG− 05
7     28   8  21   9  12  VOL+   04
CC    16   0  22   🔇 13  VOL−   02
```

**The blue buttons are a bank.** The remote prints exactly five buttons on a blue
background - OK, CH/PG ±, VOL ±. Their codes are `0x03`, `0x01`, `0x05`, `0x04`,
`0x02`: precisely the complete `0x01`–`0x05` bank.

**The digit columns run consecutively in BCD.** Column *3/6/9/MUTE* is
`0x10,0x11,0x12,0x13`. Column *2/5/8/0* is `0x19,0x20,0x21,0x22` - which is +7 in
hex but **+1 read as decimal**. Column *1/4/7* is `0x26,0x27,0x28`. So these are
BCD scan positions and the runs follow the remote's physical columns (usually 
how a matrix keypad is wired).

That also disposes of the apparent gaps. `0x06`–`0x09`, `0x14`, `0x24`, `0x29`,
`0x32` are not missing data - they are BCD-invalid or unpopulated positions. The
28 codes are the whole remote.

**The one uncertain entry is START**, mapped to `KEY_MEDIA`. Every other button
has an obvious Linux keycode; START launched Adaptec's application, which I did
not try (never used it. Always used virtualdub). Override it with `ir-keytable`
- no recompile needed, which is most of the reason this ships as an rc-core map.

## How the driver reads it

There is **no unsolicited path**. EP `0x81` IN answers commands and does nothing
else, so the only way to see a keypress is to ask. Adaptec's application asked
about nine times a second; `gb-ir.c` asks every 50 ms (`ir_poll_ms`, floor 10).

```
rc0: Adaptec GameBridge AVC-1410 IR, RC_DRIVER_SCANCODE, RC_PROTO_BIT_NECX
     keymap rc-gamebridge, plus a lirc scancode node
```

Three (perhaps noteworthy?) choices :

**NECX only.** The receiver decodes in hardware and hands over a composed frame,
so there is no raw path on which other protocols could be offered.

**Its own workqueue,** not `system_wq`. Every poll blocks on USB, and `gb_cmd()`
will wait 1 s to send and 500 ms for a reply if the 8051 stops answering.
Occupying a shared system worker to do that 20 times a second is a bit antisocial.
It is `alloc_ordered_workqueue()`, so a poll can never overlap its successor.

**The keymap registers from module init, not from probe.** `rc_map_register()`
adds to a global list; doing it per device would add a duplicate entry for every
GameBridge attached. This is why `gb-core.c` uses explicit `module_init`/
`module_exit` rather than `module_usb_driver()`.

Teardown clears `ir_polling` **before** `cancel_delayed_work_sync()`, because the
work re-arms itself - cancelling without the flag first leaves work queued
against a device about to be freed. And `gb_ir_unregister()` runs *first* in
`gb_disconnect()`: it is the only thing in this driver that issues commands on
its own schedule, so anything torn down ahead of it is torn down underneath a
live `gb_cmd()`.

## The risk this introduced, and how it was tested

The IR poller is **this driver's first asynchronous command source**. Every
command before it was issued by the thread that also owned the state transition
it belonged to.

That matters because `gb_stream_up()` switches the altsetting several lines
before it updates `gb->cur_alt`, and `gb_stream_down()` does the reverse - and EP
`0x01`/`0x81` change between **bulk and interrupt** across that boundary.
Nothing could reach inside those windows before, but now something asks
the device a question 20 times a second regardless of what else is happening.

The USB core turns out to cover the transfer-type half of it - `usb_interrupt_msg()`
is literally `usb_bulk_msg()`, which re-types the pipe from the real endpoint
descriptor - so a mismatch is not a crash.

The concurrency was tested on a debug kernel with KASAN and lockdep, using a soak
shaped to **maximise altsetting transitions** rather than stream time: audio
holding the refcounted stream up, video started and stopped every few seconds
underneath it, IR polling throughout.

## Reproducing any of this

```sh
sudo ./tools/gbcap irwatch 60          # userspace, no driver needed; press buttons
ir-keytable                            # should list rc0, keymap rc-gamebridge
sudo ir-keytable -t                    # live scancodes and keycodes
```

If the driver is loaded and nothing arrives, check the poll is happening at all
before concluding anything about the receiver - the exchange is visible on the
wire and takes one command to confirm:

```sh
sudo modprobe usbmon
sudo timeout 3 cat /sys/kernel/debug/usb/usbmon/<bus>u | grep ':1 '
#  S Bo:3:006:1  1 = 85              command out
#  C Bi:3:006:1  0 5 = 0700039b 00   five bytes back, byte0 07 = no key
```

