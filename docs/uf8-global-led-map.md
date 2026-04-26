# UF8 Global Button LED Map

Decoded 2026-04-26 from `cap35_uf8_global_buttons.pcapng`. Frame
family: `FF 38 04 <cell> 00 <a> <b> CKSUM` + `FF 39 04 <cell> 00 <a>
<b> CKSUM` (pair-write per LED, same as per-strip SOLO/MUTE/SEL).

Per-strip LEDs use cells `0x00..0x17`. Global button LEDs use cells
**`0x18..0x60`** (same bank, same frame family — different cell
range).

## Confirmed cell + colour map

| Button | Button-ID | LED cell | ON-colour `<a><b>` |
|---|---|---|---|
| Layer 1 | `0x40` | `0x39` | white (`FF FF`) |
| Layer 2 | `0x41` | `0x3A` | white |
| Layer 3 | `0x42` | TBD | (inactive in test session) |
| 360 (Settings) | `0x46` | TBD | white (always lit) |
| Send/Plugin 1 | `0x48` | `0x37` | white |
| Send/Plugin 2 | `0x49` | `0x36` | white |
| Send/Plugin 3 | `0x4A` | `0x35` | white |
| Send/Plugin 4 | `0x4B` | `0x34` | white |
| Send/Plugin 5 | `0x4C` | `0x33` | white |
| Send/Plugin 6 | `0x4D` | `0x32` | white |
| Send/Plugin 7 | `0x4E` | `0x31` | white |
| Send/Plugin 8 | `0x4F` | `0x30` | white |
| Plugin | `0x50` | `0x2F` | white |
| Channel | `0x51` | TBD | no LED changes captured (modal/inactive in test) |
| Page ← | `0x52` | `0x5D` (toggles with `0x5E`) | white |
| Page → | `0x53` | `0x5C` | white |
| Flip | `0x54` | `0x2B` | white (BLINKS when active) |
| Automation Off | `0x58` | TBD | (not pressed in capture) |
| Read | `0x59` | `0x26` | **green** (`F0 F0`) |
| Write | `0x5A` | `0x25` | **red** (`0F F0`) |
| Trim | `0x5B` | `0x24` | **orange** (`3F F0`) |
| Latch | `0x5C` | `0x23` | **red** (`0F F0`) |
| Touch | `0x5D` | `0x22` | **yellow** (`EF F0`) |
| V-Pot Bank | `0x68` | `0x5F` | white |
| Soft 1 | `0x69` | `0x5E` | white |
| Soft 2 | `0x6A` | `0x5D` | white |
| Soft 3 | `0x6B` | `0x5C` | white |
| Soft 4 | `0x6C` | `0x5B` | white |
| Soft 5 | `0x6D` | `0x5A` | white |
| PAN | `0x6E` | `0x59` | white |
| Fine / Shift | `0x6F` | `0x58` | white |
| Norm / Clear | `0x70` | `0x57` | white |
| Rec / ALL | `0x71` | `0x56` | **red** (`0F F0`) |
| Auto / Zero | `0x72` | `0x55` | white |
| Nav | `0x73` | `0x54` | white |
| Nudge | `0x74` | `0x53` | white |
| Focus | `0x75` | `0x52` | white |
| Bank ← | `0x78` | `0x4F` | white |
| Bank → | `0x79` | `0x4E` | white |
| Zoom ↑ | `0x7A` | `0x4D` | **green** |
| Zoom ← | `0x7B` | `0x4C` | white |
| Zoom centre ⊙ | `0x7C` | `0x4B` | **red** |
| Zoom → | `0x7D` | `0x4A` | white |
| Zoom ↓ | `0x7E` | `0x49` | **yellow** |

## Off-state

Same as per-strip LEDs: dim value where `FF 38 == FF 39 = 11 F1`
(white-off) or `12 F0`/`11 F0` for orange/yellow off.

## Rec-Arm per strip

User confirmed: Rec-Arm uses the **same SEL LED** as the SEL button,
just rendered in **red** (probably colour pair `0F F0` like Write/
Latch). Cell formula stays the same as SEL (`0x17 - 3*(strip-1) - 2`).
No separate LED ID.

## Colour palette observed

| Colour | `<a> <b>` (FF38) | FF39 base |
|---|---|---|
| white | `FF FF` | `00 F0` |
| green | `F0 F0` | `00 F0` |
| red | `0F F0` | `00 F0` |
| orange | `3F F0` | `00 F0` |
| yellow | `EF F0` | `00 F0` |
| off (dim white) | `11 F1` | `11 F1` |
