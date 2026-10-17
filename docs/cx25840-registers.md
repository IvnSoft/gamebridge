# CX25840 register index

Extracted from the Conexant CX25840/1/2/3 production data sheet
(document 102284B, 2005-08-03, 289 pages), which carries the full register
map. Mainline `cx25840-core.c` writes many of these but names almost none of
them.

Only the address and name are reproduced here. The bit-level descriptions
stay in the datasheet. Fetch it with `tools/fetch-datasheet.sh`.

| Address | Register |
|---|---|
| `0x000` | Host Register 1 |
| `0x001` | Host Register 2 |
| `0x000` | VIP Vendor ID |
| `0x002` | VIP Device ID |
| `0x004` | VIP Subsystem ID |
| `0x006` | VIP Subsystem Vendor ID |
| `0x008` | VIP Command |
| `0x00a` | VIP Status |
| `0x00e` | VIP Clock Status |
| `0x100` | Device ID Low Byte |
| `0x101` | Da |
| `0x102` | Miscellaneous Chip Control |
| `0x103` | Video Input Control |
| `0x104` | AFE Control 1 |
| `0x105` | AFE Control 2 |
| `0x106` | AFE Control 3 |
| `0x107` | AFE Control 4 |
| `0x108` | Video PLL Integer |
| `0x109` | Video PLL Divider |
| `0x10a` | Aux PLL Integer |
| `0x10b` | Aux PLL Divider |
| `0x10c` | Video PLL Fractional 1 |
| `0x10d` | Video PLL Fractional 2 |
| `0x10e` | Video PLL Fractional 3 |
| `0x10f` | Video PLL Fractional 4 |
| `0x110` | Aux PLL Fractional 1 |
| `0x111` | Aux PLL Fractional 2 |
| `0x112` | Aux PLL Fractional 3 |
| `0x113` | Aux PLL Fractional 4 |
| `0x114` | Pin Control 1 |
| `0x115` | Pin Control 2 |
| `0x116` | Pin Control 3 |
| `0x117` | Pin Control 4 |
| `0x118` | Pin Control 5 |
| `0x119` | Pin Control 6 |
| `0x11c` | Pin Configuration 1 |
| `0x11d` | Pin Configuration 2 |
| `0x11e` | Pin Configuration 3 |
| `0x11f` | Pin Configuration 4 |
| `0x120` | Pin Configuration 5 |
| `0x121` | Pin Configuration 6 |
| `0x122` | Pin Configuration 7 |
| `0x123` | Pin Configuration 8 |
| `0x124` | Pin Configuration 9 |
| `0x125` | Pin Configuration 10 |
| `0x126` | Pin Configuration 11 |
| `0x127` | Pin Configuration 12 |
| `0x128` | Da Description |
| `0x129` | Video Count Mid |
| `0x12a` | Video Count High |
| `0x12b` | Audio Lock |
| `0x12c` | Audio Count Low |
| `0x12d` | Da |
| `0x12e` | Audio Lock 2 |
| `0x12f` | Audio Lock 3 |
| `0x130` | Power Control 1 |
| `0x131` | Power Control 2 |
| `0x134` | AFE Diagnostic Control 1 |
| `0x135` | AFE Diagnostic Control 2 |
| `0x136` | AFE Diagnostic Control 3 |
| `0x13c` | AFE Diagnostic Control 5 |
| `0x13d` | Da |
| `0x13e` | AFE Diagnostic Control 7 |
| `0x13f` | AFE Diagnostic Control 8 |
| `0x140` | PLL Diagnostic Control 1 |
| `0x141` | PLL Diagnostic Control 2 |
| `0x159` | DLL1 Diagnostic Control 2 |
| `0x15a` | DLL1 Diagnostic Control 3 |
| `0x15b` | DLL1 Diagnostic Control 4 |
| `0x15c` | DLL2 Diagnostic Control 1 |
| `0x15d` | DLL2 Diagnostic Control 2 Da |
| `0x15e` | DLL2 Diagnostic Control 3 |
| `0x15f` | DLL2 Diagnostic Control 4 |
| `0x200` | IR Control 1 |
| `0x201` | IR Control 2 |
| `0x204` | IR TX Clock Divider Low |
| `0x205` | IR TX Clock Divider High |
| `0x208` | IR RX Clock Divider Low |
| `0x209` | IR RX Clock Divider High |
| `0x20c` | IR TX Carrier Duty Cycle |
| `0x210` | IR Status |
| `0x214` | IR Interrupt Enable |
| `0x218` | IR Low-Pass Filter Low |
| `0x219` | IR Low-Pass Filter High |
| `0x23c` | IR FIFO Low |
| `0x23d` | IR FIFO High |
| `0x23e` | IR FIFO Level |
| `0x400` | Video Mode Control 1 |
| `0x401` | Video Mode Control 2 |
| `0x402` | Video Mode Control 3 |
| `0x403` | Video Mode Control 4 |
| `0x404` | Video Out Control 1 |
| `0x405` | Video Out Control 2 |
| `0x406` | Video Out Control 3 |
| `0x407` | Video Out Control 4 |
| `0x408` | Ancillary IDID-0 |
| `0x409` | Ancillary IDID-1 |
| `0x40a` | Ancillary IDID-0/1 |
| `0x40b` | Ancillary Audio Output Control |
| `0x40c` | Copy Protection Status |
| `0x40d` | General Status 1 |
| `0x40e` | General Status 2 |
| `0x410` | Interrupt Status 1 |
| `0x411` | Interrupt Status 2 |
| `0x412` | Interrupt Mask 1 |
| `0x413` | Interrupt Mask 2 |
| `0x414` | Brightness |
| `0x415` | Contrast |
| `0x416` | Luma Control |
| `0x418` | Horizontal Scaling Low |
| `0x419` | Horizontal Scaling Mid |
| `0x41a` | Horizontal Scaling High |
| `0x41b` | Horizontal Scaling Control |
| `0x41c` | Vertical Scaling Low |
| `0x41d` | Vertical Scaling High |
| `0x41e` | Vertical Scaling Control |
| `0x41f` | Vertical Line Control |
| `0x420` | Saturation U |
| `0x421` | Saturation V |
| `0x422` | Hue |
| `0x423` | Chroma Control |
| `0x424` | VBI Line Control 1 |
| `0x425` | VBI Line Control 2 |
| `0x426` | VBI Line Control 3 |
| `0x427` | VBI Line Control 4 |
| `0x428` | VBI Line Control 5 |
| `0x429` | VBI Line Control 6 |
| `0x42a` | VBI Line Control 7 |
| `0x42b` | VBI Line Control 8 |
| `0x42c` | VBI Line Control 9 |
| `0x42d` | VBI Line Control 10 |
| `0x42e` | VBI Line Control 11 |
| `0x42f` | Da |
| `0x430` | VBI Line Control 13 |
| `0x431` | VBI Line Control 14 |
| `0x432` | VBI Line Control 15 |
| `0x433` | VBI Line Control 16 |
| `0x434` | VBI Line Control 17 |
| `0x438` | VBI Frame Code Search Mode |
| `0x439` | VBI Alternate Frame Code Type |
| `0x43a` | VBI Alternate 1 Frame Code |
| `0x43b` | VBI Alternate 2 Frame Code |
| `0x43c` | VBI Miscellaneous Config 1 |
| `0x43d` | TTX Packet Address 1 |
| `0x43e` | TTX Packet Address 2 |
| `0x43f` | TTX Packet Address 3 |
| `0x440` | VBI 1 and 2 SDID |
| `0x441` | VBI 3 SDID |
| `0x442` | VBI FIFO Reset |
| `0x443` | VBI Hamming |
| `0x444` | Closed Caption Status |
| `0x445` | Closed Caption Data |
| `0x446` | GEMSTAR 1x Status |
| `0x447` | GEMSTAR 1x Data |
| `0x448` | GEMSTAR 2x Status |
| `0x449` | GEMSTAR 2x Data |
| `0x44a` | WSS Status |
| `0x44b` | WSS Data |
| `0x44c` | VBI Custom 1 Horizontal Delay |
| `0x44d` | VBI Custom 1 Bit Increment |
| `0x44e` | VBI Custom 1 Slice Distance |
| `0x44f` | VBI Custom 1 Clock Run-in Window |
| `0x450` | VBI Custom 1 Frame Code Low |
| `0x451` | VBI Custom 1 Frame Code Mid |
| `0x452` | VBI Custom 1 Frame Code High |
| `0x453` | VBI Custom 1 Frame Code Length |
| `0x454` | VBI Custom 1 Clock Run-in Period |
| `0x455` | VBI Custom 1 Clock Run-in Margin and Length |
| `0x456` | VBI Custom 1 Payload Length |
| `0x457` | VBI Custom 1 Miscellaneous |
| `0x458` | VBI Custom 2 Horizontal Delay |
| `0x459` | VBI Custom 2 Bit Increment |
| `0x45a` | VBI Custom 2 Slice Distance |
| `0x45b` | VBI Custom 2 Clock Run-In Window Da |
| `0x45c` | VBI Custom 2 Frame Code Low |
| `0x45d` | VBI Custom 2 Frame Code Mid |
| `0x45e` | VBI Custom 2 Frame Code High |
| `0x45f` | VBI Custom 2 Frame Code Length |
| `0x460` | VBI Custom 2 Clock Run-in Period |
| `0x461` | VBI Custom 2 Clock Run-in Margin and Length Da |
| `0x462` | VBI Custom 2 Payload Length |
| `0x463` | VBI Custom 2 Miscellaneous |
| `0x464` | VBI Custom 3 Horizontal Delay |
| `0x465` | VBI Custom 3 Bit Increment |
| `0x466` | VBI Custom 3 Slice Distance |
| `0x467` | VBI Custom 3 Clock Run-in Window Da |
| `0x468` | VBI Custom 3 Frame Code Low |
| `0x469` | VBI Custom 3 Frame Code Mid |
| `0x46a` | VBI Custom 3 Frame Code High |
| `0x46b` | VBI Custom 3 Frame Code Length |
| `0x46c` | VBI Custom 3 Clock Run-in Period |
| `0x46d` | VBI Custom 3 Clock Run-in Margin and Length Da |
| `0x46e` | VBI Custom 3 Payload Length |
| `0x46f` | VBI Custom 3 Miscellaneous |
| `0x470` | Horizontal Blanking Delay Low |
| `0x471` | Horizontal Blanking Delay High |
| `0x472` | Horizontal Active High |
| `0x473` | Burst Gate Delay |
| `0x474` | Vertical Blanking Delay Low |
| `0x475` | Vertical Blanking Delay High |
| `0x476` | Vertical Active High Da |
| `0x477` | Vertical Blanking Delay 656 |
| `0x478` | SRC Decimation Ratio Low |
| `0x479` | SRC Decimation Ratio High |
| `0x47a` | Comb Filter Bandwidth Select |
| `0x47b` | Comb Filter Enable |
| `0x47c` | Subcarrier Step Size Low |
| `0x47d` | Subcarrier Step Size Mid |
| `0x47e` | Subcarrier Step Size High Da |
| `0x47f` | VBI Offset |
| `0x480` | Field Count Low |
| `0x481` | Field Count High |
| `0x484` | Temporal Decimation |
| `0x485` | Miscellaneous Timing Control |
| `0x487` | Video Detect Configuration |
| `0x488` | VGA Gain Control |
| `0x489` | AGC Gain Control Low |
| `0x48a` | AGC Gain Control High |
| `0x48b` | Digital Front-End Control |
| `0x48c` | VGA Sync Control |
| `0x48d` | VGA Track Range |
| `0x48e` | VGA Acquire Range |
| `0x490` | DFE Control |
| `0x491` | Backporch Loop Gain |
| `0x492` | DFT Threshold |
| `0x493` | Backporch Percent |
| `0x494` | Da |
| `0x495` | PLL Offset High |
| `0x496` | PLL Indirect Loop Gain |
| `0x497` | PLL Direct Loop Gain |
| `0x498` | Horizontal Tracking Loop Indirect Gain—for -24 devices |
| `0x499` | Horizontal Tracking Loop Direct Gain Da |
| `0x49a` | Horizontal Timing Loop Control (Pre-24 = RESERVED) |
| `0x49c` | Luma Comb Error Limit Max |
| `0x49d` | Luma Comb Threshold |
| `0x49e` | Chroma Comb Error Limit Max |
| `0x49f` | Comb Phase Limit |
| `0x4a0` | White Crush Increment |
| `0x4a1` | White Crush Decrement |
| `0x4a2` | White Crush Comparison Point |
| `0x4a4` | Soft Reset Mask 1 |
| `0x4a5` | Soft Reset Mask 2 |
| `0x4b4` | Version ID |
| `0x4b8` | Miscellaneous Diagnostic Control (RESERVED in -24) |
| `0x4ba` | Da |
| `0x800` | Download Address Low Byte |
| `0x801` | Download Address High Byte |
| `0x802` | Download Data Control |
| `0x803` | Download Control |
| `0x804` | Mode Detect Status 0 |
| `0x805` | Mode Detect Status 1 |
| `0x808` | Audio Configuration |
| `0x809` | Preferred Decode Mode |
| `0x80a` | Embedded Microcontroller Video Format |
| `0x80b` | Audio Format Control 0 |
| `0x810` | Soft Reset |
| `0x812` | Audio Interrupt Control |
| `0x813` | Audio Interrupt Status |
| `0x814` | Analog AGC Hysteresis1 |
| `0x815` | Analog AGC Hysteresis2 |
| `0x816` | Analog AGC Control Da |
| `0x817` | AFE 12DB ENABLE |
| `0x81b` | IF SRC Mode |
| `0x8cc` | Phase Fix Control |
| `0x8cd` | Dematrix Bypass |
| `0x8ce` | Dematrix Select Control |
| `0x8cf` | Digital Audio Input Shifter |
| `0x8d0` | Path1 Audio Select Control |
| `0x8d1` | Path1 AVC Control1 |
| `0x8d2` | Path1 AVC Control2 |
| `0x8d3` | Mute Control |
| `0x8d4` | Path1 Volume |
| `0x8d5` | Path1 Balance |
| `0x8d6` | Path1 AVC Threshold Low |
| `0x8d7` | Path1 AVC Threshold High |
| `0x8d8` | Path1 EQ Band Select |
| `0x8d9` | Path1 EQ Bass Adjust |
| `0x8da` | Path1 EQ Midrange Adjust |
| `0x8db` | Path1 EQ Treble Adjust |
| `0x8dc` | Path1 Soft Clip Control1 |
| `0x8dd` | Path1 Soft Clip Control2 |
| `0x8de` | Path1 Soft Clip Threshold Low |
| `0x8df` | Path1 Soft Clip Threshold High |
| `0x8e0` | Path2 Audio Select Control |
| `0x8e1` | Path2 AVC Control1 |
| `0x8e2` | Path2 AVC Control2 |
| `0x8e3` | Path2 Balance |
| `0x8e4` | Path2 Volume |
| `0x8e5` | Path2 Balance |
| `0x8e6` | Path2 AVC Threshold Low |
| `0x8e7` | Path2 AVC Threshold High |
| `0x8e8` | Path2 EQ Band Select |
| `0x8e9` | Path2 EQ Bass Adjust |
| `0x8ea` | Path2 EQ Midrange Adjust |
| `0x8eb` | Path2 EQ Treble Adjust |
| `0x8ec` | Path2 Soft Clip Control1 |
| `0x8ed` | Path2 Soft Clip Control2 |
| `0x8ee` | Path2 Soft Clip Threshold Low |
| `0x8ef` | Path2 Soft Clip Threshold High |
| `0x8f1` | Sample Rate Converter Status1 |
| `0x8f2` | Sample Rate Converter Status2 |
| `0x8f3` | Sample Rate Converter Status3 |
| `0x8f8` | SRC1 Phase Increment Low |
| `0x8f9` | SRC1 Phase Increment Mid |
| `0x8fa` | SRC1 Phase Increment High Da |
| `0x8fc` | SRC2 Phase Increment Low |
| `0x8fd` | SRC2 Phase Increment Mid |
| `0x8fe` | SRC2 Phase Increment High |
| `0x900` | SRC3 Phase Increment Low |
| `0x901` | SRC3 Phase Increment Mid |
| `0x902` | SRC3 Phase Increment High |
| `0x903` | SRC3 FIFO Read Threshold |
| `0x904` | SRC4 Phase Increment Low |
| `0x905` | SRC4 Phase Increment Mid |
| `0x906` | SRC4 Phase Increment High |
| `0x907` | SRC4 FIFO Read Threshold |
| `0x908` | SRC5 Phase Increment Low |
| `0x909` | SRC5 Phase Increment Mid |
| `0x90a` | SRC5 Phase Increment High |
| `0x90b` | SRC5 FIFO Read Threshold |
| `0x90c` | SRC6 Phase Increment Low |
| `0x90d` | SRC6 Phase Increment Mid |
| `0x90e` | SRC6 Phase Increment HIgh |
| `0x90f` | SRC6 FIFO Read Threshold |
| `0x910` | Output SRC Source Select |
| `0x911` | Baseband Bypass Control |
| `0x912` | Input SRC Source Select1 |
| `0x913` | Input SRC Source Select2 |
| `0x914` | Serial Audio Input Control1 |
| `0x915` | Serial Audio Input Control2 |
| `0x918` | Serial Audio Output Control1 |
| `0x919` | Serial Audio Output Control2 |
| `0x91c` | AC97 Shutdown |
| `0x91d` | AC97 Wake Up |
| `0x91e` | AC97 Reset |
| `0x91f` | AC97 Upsample Bypass |
| `0x9c0` | RDS I Low |
| `0x9c1` | RDS I High |
| `0x9c2` | RDS Q Low |
| `0x9c3` | RDS Q High |
| `0x980` | ACL Control 0 |
| `0x984` | ACL Control 1 |
| `0x988` | ACL Control 2 |
| `0x98c` | ACL Control 3 |
| `0x990` | ACL Command |
| `0x994` | ACL GPIO Output Data |
| `0x998` | ACL GPIO Input Data |
| `0x99c` | ACL Status Slot Interface |
| `0x9a0` | ACL Warm Reset |
| `0x9a4` | ACL Tag |
