# UF8 Protocol Notes

This is the living spec of the SSL UF8 USB protocol, reverse-engineered by capturing SSL 360° ↔ UF8 traffic with USBPcap on Windows.

## Device
- VID `0x31E9` / PID `0x0021`, USB 2.0 Full Speed, vendor-specific class `0xFF/0xFF/0xFF`
- **Endpoints:**
  - `0x02` OUT bulk — host → UF8 (commands, colors, meters)
  - `0x81` IN bulk — UF8 → host (heartbeat, button events)
- Sibling device on same workstation: UC1 (PID `0x0023`) — same protocol, separate device
- HID controller for UF8 knobs/buttons: separate device PID `0x0022` (runs alongside; MCU/HUI path)

## Frame format — both directions

```
FF <payload bytes> <checksum>
```

- **Magic:** leading `FF`
- **Checksum:** `sum(payload bytes) mod 256`, appended as last byte
- Incoming (UF8 → host) packets are prefixed by a 2-byte session header `31 60` before the `FF` frame starts

Verified against every unique payload captured (idle + plugin-mixer + color-change + bank-switch + button-press).

## Commands — host → UF8 (EP 0x02 OUT)

### `FF 66 09 18 <8 palette-indices> CKSUM` — color data (14 B)
**The one we came for.** Sets the color of all 8 currently-visible scribble strips.

Example: Track 1 red (after initial state change):
```
FF 66 09 18  02 06 0C 09 0A 08 07 0C  C9
        ^^^^^^^^^^^^^^^^^^^^^^^^^^^  ^^
        strip 1…8 palette indices    cksum
```
Sent on initial sync, on track-color change, and on every bank switch (the host re-resolves which 8 REAPER tracks are visible and re-sends).

### `FF 66 09 15 00×8 84` / `FF 66 09 16 00×8 85` — heartbeat pair (13 B)
Sent continuously (~50 Hz) in Plugin-Mixer mode, zero payload. Plausibly status-poll / keepalive. Our extension can send these periodically to match 360°'s pattern.

### `FF 66 21 09 00×59 90` / `FF 66 21 0A 00×59 91` — heartbeat pair (64 B)
Always present (idle + plugin-mixer). Zero payload. Likely display-framebuffer poll or plugin-mixer-specific status. Our extension probably doesn't need these for color-only.

### `FF 1B 01 XX CKSUM` — layer / page selector (5 B)
`XX` cycles `00 → 01 → 02 → 03`. Plausibly "which UF8 display layer is active" (Channel / Plugin Mixer / Send / …).

### `FF 38 04 …` and `FF 39 04 …` — meter updates (8 B)
High volume (hundreds per second). First payload byte indexes a strip/side, remaining bytes are level-data. **Irrelevant for colors** — REAPER pushes meters natively over MCU/HUI; our extension can ignore these.

## Events — UF8 → host (EP 0x81 IN)

### `31 60` (2 B) — idle heartbeat
Sent ~500 Hz when nothing happens.

### `31 60 FF 22 03 <button_id> 00 <state> CKSUM` (9 B payload, 36-byte frame) — button event
Format:
- `22 03` — button-event command type
- `button_id` — see table below
- `state` — `0x01` pressed, `0x00` released

Checksum verified: `22+03+78+00+01 = 0x9E` ✓

#### Full PM-mode button ID map (captured 2026-04-21 on a physical UF8)

Per-strip buttons use strip index `N` = 0..7 (left → right).

| Group | Button | ID formula | Strip 0 | Strip 7 |
|-------|--------|------------|---------|---------|
| Per-strip | V-Pot Push | `0x08 + N` | `0x08` | `0x0F` |
| Per-strip | Top Soft-key (above scribble) | `0x18 + N` | `0x18` | `0x1F` |
| Per-strip | SOLO | `0x20 + 3N` | `0x20` | `0x35` |
| Per-strip | CUT (Mute) | `0x21 + 3N` | `0x21` | `0x36` |
| Per-strip | SEL (Select) | `0x22 + 3N` | `0x22` | `0x37` |

Global buttons (fixed IDs):

| Button | ID | Button | ID |
|--------|----|--------|----|
| Layer 1 | `0x40` | Layer 2 | `0x41` |
| Layer 3 | `0x42` | 360 (Settings) | `0x46` |
| Send/Plugin 1 | `0x48` | Send/Plugin 2 | `0x49` |
| Send/Plugin 3 | `0x4A` | Send/Plugin 4 | `0x4B` |
| Send/Plugin 5 | `0x4C` | Send/Plugin 6 | `0x4D` |
| Send/Plugin 7 | `0x4E` | Send/Plugin 8 | `0x4F` |
| Plugin | `0x50` | Channel | `0x51` |
| Page ← | `0x52` | Page → | `0x53` |
| Flip | `0x54` | Automation Off | `0x58` |
| Read | `0x59` | Write | `0x5A` |
| Trim | `0x5B` | Latch | `0x5C` |
| Touch | `0x5D` | V-POT (top soft right) | `0x68` |
| Soft-key 1 (top right) | `0x69` | 2 | `0x6A` |
| 3 | `0x6B` | 4 | `0x6C` |
| 5 | `0x6D` | PAN | `0x6E` |
| Fine / Shift | `0x6F` | Norm / Clear | `0x70` |
| Rec / ALL | `0x71` | Auto / Zero | `0x72` |
| Nav | `0x73` | Nudge | `0x74` |
| Focus | `0x75` | Channel Encoder Push | `0x76` |
| Bank ← | `0x78` | Bank → | `0x79` |
| Zoom ↑ | `0x7A` | Zoom ← | `0x7B` |
| Zoom Center | `0x7C` | Zoom → | `0x7D` |
| Zoom ↓ | `0x7E` | | |

Note: the UF8 firmware re-uses some MCU-standard IDs for non-MCU functions. The top soft-key range `0x18..0x1F` is MCU-SELECT in DAW/MCU mode but is the per-strip parameter-page key in PM mode — do not forward it blindly as MCU SELECT.

## Color palette (16 indices, 0x00-0x0F hypothesis)

| Index | Color | Source |
|-------|-------|--------|
| `0x02` | Red (pure) | isolated capture #FF0000 |
| `0x03` | Green (pure) | isolated capture #00FF00 |
| `0x04` | Blue (pure) | isolated capture #0000FF |
| `0x06` | Magenta | screenshot Track 2 |
| `0x07` | Yellow | screenshot Track 7 |
| `0x08` | Orange | screenshot Track 6 |
| `0x09` | Purple (dark) | screenshot Track 4 |
| `0x0A` | Green (alt, darker) | screenshot Track 5 |
| `0x0B` | Orange (bright) | isolated capture #FF8000 |
| `0x0C` | Violet | screenshot Tracks 3 & 8 |
| `0x0E` | White / light gray | Track 1 #FFFFFF |

**Not yet mapped:** `0x00`, `0x01`, `0x05`, `0x0D`, `0x0F` — likely black/gray-shades, missing primaries. Extension can either ship an incomplete palette + fall back to nearest-match via Euclidean distance in RGB space, or we do one more focused capture session.

**Quantization behavior:** SSL 360° quantizes REAPER's 24-bit RGB to the nearest palette index. Our extension must replicate this mapping.

## Bank-switch behavior (confirmed)

Captured BANK→ BANK→ BANK← BANK← sequence. Observed:
1. UF8 sends button event (`FF 22 03 0x78 00 01 …` then `… 00 00 …`)
2. Host reacts within ~50 ms with a new `FF 66 09 18 <new 8 indices> CKSUM`

The UF8 does NOT hold a bank pointer. Every visible-window change = new 8-index array from the host. For our REAPER extension:
- Hook REAPER's mixer/TCP scroll-position change
- Determine currently-visible 8 tracks
- Resolve their REAPER colors → palette indices
- Send `FF 66 09 18 <8> <cksum>` over EP 0x02 OUT

## Minimum command set for a color-only extension

1. `FF 66 09 18 <8> <cksum>` — push colors (on init, color-change, bank-switch)
2. Optionally `FF 66 09 15/16 …` heartbeat — TBD whether UF8 requires it to stay awake
3. Read `FF 22 03 …` button events from EP 0x81 to detect bank presses (or let REAPER's MCU surface do it)

Everything else (meters, faders, text scribble-strip content) can be left to SSL's MCU-over-MIDI path or ignored.

## Init sequence — REQUIRED for any color to render

Empirical discovery (2026-04-19): sending `FF 66 09 18 <8 indices> CKSUM`
over EP 0x02 when SSL 360° is NOT running lands fine at the USB layer
(libusb reports rc=0, full byte count transferred) but the UF8 renders
nothing — display stays dark, no layer active.

This means SSL 360° pushes a **wakeup / mode-set sequence** shortly after
device enumeration that puts the UF8 into a state where scribble-strip
color data is actually rendered. Our extension must replay the same
sequence at `open()` time before any color command will be visible.

**TODO — next session:**
1. On Windows, start a USBPcap capture on USBPcap3 (or whichever bus the
   UF8 is on after a fresh boot).
2. Trigger a fresh enumeration: replug the UF8 while SSL 360° is running
   and watching.
3. Stop capture ~3 s after replug.
4. Look at the packets on EP 0x02 OUT between USB enumeration and the
   first meter/heartbeat — those are the init commands.
5. Transcribe into `src/UF8Device.cpp::open()` as a preamble.

Until we have that, the extension can still be built and loaded but won't
drive the hardware.

## Plug-in Mixer LCD zones — per-strip addressable commands

Each of the 8 scribble-strip LCDs in Plug-in Mixer / Channel Strip Mode
is split into multiple text and graphic zones (see UF8 User Guide
page 153). Commands decoded 2026-04-20 from cap14a–cap18:

| Zone | Command | Frame size | Notes |
|------|---------|-----------|-------|
| DAW Colour | `FF 66 09 18 <8 palette idx> CKSUM` | 14 B | Already implemented |
| Channel Strip Type | `FF 66 06 17 <strip> <4 ASCII> CKSUM` | 9 B | "CS 2", "4K B", "4K E" |
| Currently Selected Parameter | `FF 66 <N+2> 04 <strip> <N ASCII> CKSUM` | 5+N B | "BYPASS", "LF FREQ", "COMP RATIO" |
| Value Line | `FF 66 15 0E <strip> <19 ASCII> CKSUM` | 24 B | Combined label+value: "In Trim       0.0dB" |
| O/PdB Fader Readout | `FF 66 0A 0C <strip> <4 ASCII> 00 00 "dB" CKSUM` | 14 B | Fader dB — `64 42` = "dB" fixed |
| V-Pot Readout Bar | `FF 66 09 0D <8 bytes> CKSUM` | 13 B | Broadcast: 1 byte per strip |
| Fader / meter bar | `FF 66 11 0F <16 bytes> CKSUM` | 20 B | Broadcast: 2 bytes per strip (8 × 16-bit) |
| TrkNam (big) | `FF 66 <N+2> 0B <strip> <N ASCII> CKSUM` | 5+N B | Driven via MCU scribble-strip sysex pass-through |
| Fader motor | `FF 1E 03 <strip> <LSB> <MSB> CKSUM` | 7 B | Bidirectional (host→UF8 = set position, UF8→host = user touch) |
| Motor limp | `FF 1D 02 <strip> <01\|00> CKSUM` | 6 B | 01 = motor active, 00 = user controls |

**Still undecoded:**
- Top Zone soft-key labels (top row above each strip)
- Plug-in Mixer Position ("No" slot number top-left)
- Dynamics Metering (GR bars) — cap17 inconclusive, needs audio-playing retry
- Input/Output Metering — `FF 38 04` / `FF 39 04` present but byte layout TBD
- `FF 5B 02 00 00 5D` — always-present poll-like frame (likely status)
- `FF 13 04 <4 bytes>` — present during fader moves, not a meter (fewer values than expected)

## Palette corrections (2026-04-20)

REAPER's default "blue" (#009FD5) quantizes to palette index **0x05**,
not 0x04 as our original capture assumed. cap14a confirmed this: the
reference `FF 66 09 18` frame with T1-blue set index 0x05 on strip 0.
Current Palette.cpp is incomplete and needs a re-sweep. Until then,
our quantizer falls back to nearest-match among the 5 measured entries
and can misroute colors.

## Session log
| date | capture | finding |
|------|---------|---------|
| 2026-04-19 | probe_inj.pcap (5 s idle) | Device addr 10 on USBPcap3. Endpoints 0x02 OUT / 0x81 IN. Frame = `FF <payload> CKSUM`, checksum = sum mod 256. |
| 2026-04-19 | cap01_baseline_pluginmixer.pcap (10 s) | Plugin-Mixer adds `FF 66 09 15/16 00×8 CKSUM` heartbeat pair. |
| 2026-04-19 | cap02_track1_red.pcap | Color command identified: `FF 66 09 18 <8 indices> CKSUM`. Track 1 red = index 0x02. |
| 2026-04-19 | cap03_track1_green.pcap | Track 1 green (#00FF00) = 0x03 |
| 2026-04-19 | cap04_track1_blue.pcap | Track 1 blue (#0000FF) = 0x04 |
| 2026-04-19 | cap05_track1_sequence.pcap | Track 1 white=0x0E, sequence partial due to timing/quantization |
| 2026-04-19 | cap06_bankswitch.pcap | Bank change = new 8-index array. Button event format: `FF 22 03 <id> 00 <state> CKSUM`. BANK→ = 0x78, BANK← = 0x79. Meter cmds `FF 38/39 04 …` identified but ignorable. |
| 2026-04-20 | cap13_layer_switch.pcap | PM layer state-flood captured. Slot populate flag `FF 66 0A 00 03 …` identified as color-bar activation gate. |
| 2026-04-20 | cap14a_track1_blue_populated.pcap | REAPER "blue" = palette 0x05 (not 0x04); single color-change triggers ~10 state frames. |
| 2026-04-20 | cap15_pm_param_cycle.pcap | Parameter Label zone (`FF 66 <n> 04 <strip> <text>`) + Value Line zone (`FF 66 15 0E <strip> <19 chars>`) + V-Pot Readout Bar (`FF 66 09 0D`) + 16-byte bar (`FF 66 11 0F`). |
| 2026-04-20 | cap16_pm_fader_dB.pcap | O/PdB zone decoded: `FF 66 0A 0C <strip> <4 ASCII> 00 00 "dB" CKSUM`. |
| 2026-04-20 | cap17_pm_gain_reduction.pcap | Inconclusive — audio didn't play through comp. Only `FF 13 04` (not a meter). |
| 2026-04-20 | cap18_pm_cs_type.pcap | **Channel Strip Type zone**: `FF 66 06 17 <strip> <4 ASCII>` — "CS 2", "4K B", "4K E". |
| 2026-04-20 | cap19_pm_bank_position.pcap | No bank shift captured (only DYN page shifts); Position indicator still TBD. |
