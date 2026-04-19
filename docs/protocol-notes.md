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
- `button_id` — observed `0x78` (BANK→), `0x79` (BANK←). Other buttons TBD.
- `state` — `0x01` pressed, `0x00` released

Checksum verified: `22+03+78+00+01 = 0x9E` ✓

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
