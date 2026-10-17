# What Adaptec's own INF says: input routing, the tuner, and the IR

`firmware/avcgbdr.in_` is the Windows capture driver's INF, compressed. It is 294
lines of printable text.

Extract it with:

```sh
python3 - <<'PY'
import re
d = open('firmware/avcgbdr.in_','rb').read()
print(b'\n'.join(re.findall(rb'[\x20-\x7e]{4,}', d)).decode('ascii','replace'))
PY
```

The file is fetched by `tools/fetch-firmware.sh` and the findings live here.

---

## 1. The input crossbar

The INF carries the whole routing table, with the pin-type enum in a comment
right above it:

```
;     0 - SVIDEO        ;Pin 0 - Tuner In
;     1 - Tuner         HKR,"DriverData\XBarPin0","PinType",  0x01
;     2 - Composite     HKR,"DriverData\XBarPin0","InputMux", 0x02,0x05
;     3 - audio tuner in
;     4 - audio line in ;Pin 1 - Composite in
                        HKR,"DriverData\XBarPin1","PinType",  0x02
                        HKR,"DriverData\XBarPin1","InputMux", 0x01

                        ;Pin 2 - S-Video in
                        HKR,"DriverData\XBarPin2","PinType",  0x00
                        HKR,"DriverData\XBarPin2","InputMux", 0x03,0x04
```

`InputMux` is the CX25840 input mux number. Translated into the constants in
`media/drv-intf/cx25840.h`:

| Input | InputMux | CX25840 value | Related audio pin |
|---|---|---|---|
| **Composite** | `1` | `CX25840_COMPOSITE1` = 1 | Pin 4, audio line in |
| **Tuner** | `2` (+`5`) | `CX25840_COMPOSITE2` = 2 | Pin 3, tuner audio |
| **S-Video** | `3` luma, `4` chroma | `CX25840_SVIDEO_LUMA3 \| CX25840_SVIDEO_CHROMA4` = **0x430** | Pin 4, audio line in |


`gb-v4l2.c` already defaults `vid_input` to `CX25840_COMPOSITE1`.

**S-Video would not have been guessable.** Luma on mux 3 with chroma on mux 4 is
`0x430`, and that is *not* one of the predefined combinations in the header -
those are `CX25840_SVIDEO1` = 0x510, `SVIDEO2` = 0x620, `SVIDEO3` = 0x730. 

**The audio route is a function of the input**: `RelatedPinIndex` ties Composite
and S-Video to *audio line in* (pin 4) and the Tuner to *tuner audio* (pin 3).

## 2. Tuner parameters

```
TunerType        0x20     (0x14 present but commented out)
TunerI2CAddress  0xC0     8-bit -> 7-bit 0x60
Tuner2IFAddress  0x86     8-bit -> 7-bit 0x43
I2SInputFormat   0x01
;Tuner Type - TALF_H800Y  Change to 0x15 for TALF-W800Y
```

The commented-out `0x14`/`0x15` pair and the TALF part numbers are Philips/NXP
tuner-can types, i.e. the *other* board variant. The active value is `0x20`,
which is the line that matters for the AVC-1410/1510 in hand. Note the INF
exposes separate `TvAudio` and `Tuner` filters, so Windows treated tuner audio
as its own path.

## 3. There is no IR receiver mentioned in the driver

| Evidence | Result |
|---|---|
| `Remote`, `Infrared`, `HID`, `Consumer`, `Button`, `Key` in the INF | **0 hits each** |
| IR-ish strings in `avcgbdr.sys` (125 KB driver) | **none** |
| USB configuration descriptor | **`bNumInterfaces = 1`** - one vendor-specific interface, three endpoints, no HID |
| Extra `.sys` in the driver package | none; only capture, loader, and the CX25840 audio ROMs |

Also relevant: mainline **does** ship `cx25840-ir.c`, but every entry point in it
is gated behind `is_cx23885()` / `is_cx23888()`. This device reports **0x8404, a
plain CX2584x**, so that IR block belongs to the later integrated bridge parts
and is not on our decoder.

### The receiver exists

**There is an IR window on the front of the unit and a 28-button Adaptec remote
in the box.** Both work. The driver reads them today; see
[the IR receiver](ir-receiver.md) and `driver/gb-ir.c`.

EP 0x81 IN was the only remaining candidate, and that is where the receiver answers -
though by **polled command `0x85`**, not by the asynchronous delivery I imagined
before. `0x85` had been sitting in my own captures since day one, mislabelled as a counter.

