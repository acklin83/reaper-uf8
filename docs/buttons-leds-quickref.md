# UF8 + UC1 — Button & LED Quick Reference

Flat ID tables. For frame formats, palette logic, capture history, etc. see `protocol-notes.md` (UF8) and `protocol-notes-uc1.md` (UC1).

All multi-byte values are hex. Per-strip indices are `N = 0..7` left → right.

---

## UF8

VID `0x31E9` / PID `0x0021`. EP `0x02` OUT, `0x81` IN. Frames start `FF`, end `<chk = sum(cmd+len+data) % 256>`.

### Inbound — button events

Frame: `FF 22 03 <button_id> 00 <state> <chk>` (state `0x01` press, `0x00` release).

#### Per-strip (formula + first/last)

| Button | Formula | N=0 | N=7 |
|---|---|---|---|
| V-Pot push | `0x08 + N` | `0x08` | `0x0F` |
| Top soft-key (above scribble) | `0x18 + N` | `0x18` | `0x1F` |
| SOLO | `0x20 + 3·N` | `0x20` | `0x35` |
| CUT (Mute) | `0x21 + 3·N` | `0x21` | `0x36` |
| SEL (Select) | `0x22 + 3·N` | `0x22` | `0x37` |

⚠ Top soft-key range `0x18..0x1F` overlaps MCU SELECT — in PM mode it's the per-strip parameter-page key. Don't blindly forward as MCU SELECT.

#### Globals

| ID | Name | ID | Name |
|---|---|---|---|
| `0x40` | Layer 1 | `0x41` | Layer 2 |
| `0x42` | Layer 3 | `0x43` | Quick 1 (PM: Channel Strip) |
| `0x44` | Quick 2 (PM: Bus Comp) | `0x45` | Quick 3 (PM: I/O meter) |
| `0x46` | 360 (Settings) | | |
| `0x48` | Send/Plugin 1 | `0x49` | Send/Plugin 2 |
| `0x4A` | Send/Plugin 3 | `0x4B` | Send/Plugin 4 |
| `0x4C` | Send/Plugin 5 | `0x4D` | Send/Plugin 6 |
| `0x4E` | Send/Plugin 7 | `0x4F` | Send/Plugin 8 |
| `0x50` | Plugin | `0x51` | Channel |
| `0x52` | Page ← | `0x53` | Page → |
| `0x54` | Flip | `0x58` | Automation Off |
| `0x59` | Read | `0x5A` | Write |
| `0x5B` | Trim | `0x5C` | Latch |
| `0x5D` | Touch | `0x68` | V-POT bank (top-soft right) |
| `0x69` | Soft 1 | `0x6A` | Soft 2 |
| `0x6B` | Soft 3 | `0x6C` | Soft 4 |
| `0x6D` | Soft 5 | `0x6E` | PAN |
| `0x6F` | Fine / Shift | `0x70` | Norm / Clear |
| `0x71` | Rec / ALL | `0x72` | Auto / Zero |
| `0x73` | Nav | `0x74` | Nudge |
| `0x75` | Focus | `0x76` | Channel encoder push |
| `0x78` | Bank ← | `0x79` | Bank → |
| `0x7A` | Zoom ↑ | `0x7B` | Zoom ← |
| `0x7C` | Zoom centre (◯) | `0x7D` | Zoom → |
| `0x7E` | Zoom ↓ | | |

### Outbound — LED / colour

| Function | Frame | Notes |
|---|---|---|
| Per-strip TFT colour | `FF 66 09 18 <8 palette indices> <chk>` | One frame, 8 strips. Palette indices 0x00..0x0F (see palette table in `protocol-notes.md`). |
| Per-strip SEL/MUTE/SOLO LED | `FF 3B 03 <id> 00 <state> <chk>` | `id = strip*3 + type`, type: 0=SEL, 1=MUTE, 2=SOLO → `0x00..0x17`. State `0x01`/`0x00`. REC-arm probably `0x18+N`, unverified. |
| 12-segment track meter | `FF 38 04 …` / `FF 39 04 …` | 8 B frames; per-strip meter level. Detailed encoding in protocol-notes. |
| Layer / page | `FF 1B 01 <XX> <chk>` | Also acts as PM-mode keepalive (current code sends every 150 ms). |
| Heartbeat (host→UF8) | `FF 66 09 15 …` / `FF 66 09 16 …` (13 B) and `FF 66 21 09/0A …` (64 B) | Cycling pair, required to keep PM mode alive. |

### Palette indices (16 entries; gaps still unmapped)

| Idx | Colour | Idx | Colour |
|---|---|---|---|
| `0x02` | Red | `0x09` | Purple (dark) |
| `0x03` | Green | `0x0A` | Green (alt darker) |
| `0x04` | Blue | `0x0B` | Orange (bright) |
| `0x05` | REAPER default blue (#009FD5) | `0x0C` | Violet |
| `0x06` | Magenta | `0x0E` | White / light grey |
| `0x07` | Yellow | | |
| `0x08` | Orange | | |

Unmapped: `0x00`, `0x01`, `0x0D`, `0x0F` — likely black/grey shades. `Palette.cpp` falls back to nearest-RGB Euclidean match.

---

## UC1

VID `0x31E9` / PID `0x0023`. EP `0x02` OUT, `0x81` IN. IN frames carry a `31 60` poll-token prefix on the wire; OUT frames are raw `FF …`.

### Inbound — button events

Frame: `FF 22 03 <button_id> 00 <state> <chk>`. Button IDs are **stable across plugin contexts** (4K E vs CS 2 vs Bus Comp 2).

| ID | Button | Notes |
|---|---|---|
| `0x08` | HF Bell | |
| `0x09` | EQ Type | Label varies per plugin (Black/Orange/Brown 4K E; In/Out CS 2) |
| `0x0A` | EQ In | |
| `0x0B` | LF Bell | |
| `0x0C` | Bus Comp IN | |
| `0x14` | Fast Attack (Comp) | |
| `0x15` | Peak | Display only with CS 2 |
| `0x16` | Dyn In | |
| `0x17` | Expand | |
| `0x18` | Fast Attack (Gate) | |
| `0x19` | Polarity | |
| `0x1A` | S/C Listen | |
| `0x1B` | Solo Clear | State cycles 01 → 03 → 00 |
| `0x1C` | Solo | |
| `0x1D` | Cut | |
| `0x1E` | Channel IN | |
| `0x1F` | Fine | |

### Inbound — knob rotation

Frame: `FF 24 02 <knob_id> <delta> <chk>`. `delta` = 6-bit signed (−32..+31), CW positive.

#### Dedicated Channel-Strip pots (always drive CS params)

| ID | CS knob | ID | CS knob |
|---|---|---|---|
| `0x00` | LPF freq | `0x07` | LMF Gain |
| `0x01` | HPF freq | `0x08` | LMF Frequency |
| `0x02` | HF Gain | `0x09` | LMF Q |
| `0x03` | HF Frequency | `0x0A` | LF Frequency |
| `0x04` | HMF Gain | `0x0B` | LF Gain |
| `0x05` | HMF Frequency | `0x17` | Gate Release |
| `0x06` | HMF Q | `0x18` | Gate Hold (CS 2 only) |
| `0x19` | Gate Threshold | `0x1A` | Gate Range |
| `0x1B` | Dyn Comp Release | `0x1C` | Dyn Comp Threshold |
| `0x1D` | Dyn Comp Ratio | | |

#### Top-centre V-Pots (`0x0C..0x16`) — soft-mapped per focused plugin

Bus Comp 2 mapping:

| ID | Knob (Bus Comp 2) |
|---|---|
| `0x0E` | Ratio |
| `0x0F` | Makeup |
| `0x10`? | Attack (unverified) |
| `0x11` | Release |
| `0x12` | Threshold |
| `0x14` | Mix |
| `0x16` | S/C HPF |

4K E mapping (different plugin → different param semantics, same IDs):
- `0x0C` = Input Trim
- `0x16` = Fader Level / Output Trim

### Outbound — LED feedback (per button)

Frame: `FF 13 04 <bank> <cell> 01 <state> <chk>`. State: `0x00` off, `0x33` dim (plugin bypassed but armed-state visible), `0xFF` on.

Bank `0x02` carries button LEDs; bank `0x01` carries VU meters and 7-seg digits.

| Button ID | Bank | Cell |
|---|---|---|
| `0x08` HF Bell | `0x02` | `0x89` |
| `0x09` EQ Type | `0x02` | `0x51` |
| `0x0A` EQ In | `0x02` | `0x50` |
| `0x0B` LF Bell | `0x02` | `0x23` |
| `0x0C` Bus Comp IN | `0x02` | `0x01` |
| `0x14` Fast Attack (Comp) | `0x02` | `0x38` |
| `0x15` Peak | `0x02` | `0x39` |
| `0x16` Dyn In | `0x02` | `0x5B` |
| `0x17` Expand | `0x02` | `0x92` |
| `0x18` Fast Attack (Gate) | `0x02` | `0x93` |
| `0x19` Polarity | `0x02` | `0x98` |
| `0x1A` S/C Listen | `0x02` | `0x99` |
| `0x1B` Solo Clear | `0x01` | `0x9A` |
| `0x1C` Solo | `0x02` | `0x97` |
| `0x1D` Cut | `0x02` | `0x96` |
| `0x1E` Channel IN | `0x02` | `0x94` |
| `0x1F` Fine | `0x02` | `0x95` |

Bypass-state convention: re-push every cell at `0x33`. Un-bypass: re-push at the previously active value.

### Outbound — meters / displays

| Function | Frame | Notes |
|---|---|---|
| GR meter | `FF 5B 02 <BE-16 dB×10> <chk>` | Range 0x0026 (3.8 dB) … 0x0098 (15.2 dB) seen. Monotonic. |
| VU meter | `FF 13 04 01 <level> 01 <in/out>` | Bank `0x01`, byte 4 selects in/out (`0x00`/`0x01`). |
| 7-seg digits (3-digit red, left of CHANNEL encoder) | `FF 13 04 01 <cell> 00 <state>` | Per-segment addressing, ~22 cells around 0x08..0x16. Decode pending. |
| Display text (zoned) | `FF 66 <len> <zone> <ascii…> <chk>` | See zone table below. |
| Keepalive | `FF 1B 01 <counter> <chk>` | 4-phase counter 0x00..0x03, ~1 Hz. **Required**, else UC1 times out. |
| Init | none beyond keepalive + `FF 5B 02 00 00 5D` (zero GR) | UC1 has no vendor-init flood. |

### Display zones (`FF 66`)

| Zone | Length | Purpose |
|---|---|---|
| `0x02` | 37 B | Channel-strip context line; track name at column 12 |
| `0x03` | 18–19 B | Channel-strip readout (knob just turned). Status button label + on/off text. |
| `0x04` | 43 B | General status / "No Plug-ins" line. Bus-Comp track name at column 14. |
| `0x05` | 22 B | Bus-Comp readout (knob just turned, time-multiplexed) |
| `0x0E` | 3–4 B | Plugin state tag (`Off`, `On`, `N/A`) |
| `0x10` | 4–5 B | Plugin name tag (`4K E`, etc.) |

### Global status (low priority)

`FF 5C 02 <16-bit mask> <chk>` — only 3 distinct values seen across all captures (`00 00`, `00 0A`, `00 32`). Probably a global bitmask (Bus-Comp-active, Solo-active). Per-button LED state goes via `FF 13 04`, not this.

---

## Cross-references

- Frame format details, checksum verification, capture provenance: `docs/protocol-notes.md` (UF8) / `docs/protocol-notes-uc1.md` (UC1).
- Init sequence (UF8 only — UC1 needs none): `extension/src/uc1_init_sequence.inc` (legacy filename; actually UF8 init bytes).
- Code that consumes these IDs: `extension/src/main.cpp` (UF8 dispatch), `extension/src/UC1Surface.cpp`, `extension/src/UC1Protocol.cpp`.
