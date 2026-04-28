# cap44 — UF8 Send/Plugin row 3-state (OFF / DIM / BRIGHT)

**Date:** 2026-04-28
**Interface:** `\\.\USBPcap1`
**Devices in stream:** UF8 (VID 31e9 PID 0021), UC1 (VID 31e9 PID 0023)
**Size:** ~3.85 MB

## Why

`uf8-global-led-map.md` confirms cells `0x37..0x30` (Send/Plugin 1..8, reverse-mapped) and `0x2F` (Plugin button) but only with their **ON colour** (white `FF FF`) plus the generic `11 F1` dim baseline. The handoff established a 3-state model (OFF / DIM / BRIGHT) for the per-strip top-soft-keys; we want the same precision for the global Send/Plugin row.

User report (2026-04-28): firmware drives these LEDs differently per Layer:
- **Layer 1** → Send/Plugin row is **OFF** entirely
- **Layer 2** → Send/Plugin row is **DIM** for non-active and **BRIGHT** for the currently-active selection

That gives us a clean source for all three states inside one capture: Layer-1 segment = OFF bytes, Layer-2 idle = DIM bytes, sequential Layer-2 presses = BRIGHT bytes for each cell.

Buttons explicitly **excluded** (firmware never lights them in any tested SSL360 config, per user 2026-04-28):
- Channel (`0x51`)
- Layer 3 (`0x42`)
- 360 (`0x46`)
- Auto Off (`0x58`)

→ Drop these from any 3-state TBD list. The map entries can stay as references but they're effectively dead in current SSL firmware.

## What the user did (action timeline)

1. **Select Layer 1.** Send/Plugin 1-8 all dark (OFF). Held ~2 s.
2. **Select Layer 2.** Send/Plugin 1-8 all DIM. Held ~2 s.
3. On Layer 2: **press Send/Plugin 1**, briefly hold (cell `0x37` → BRIGHT). Release.
4. Repeat for Send/Plugin **2, 3, 4, 5, 6, 7, 8** (cells `0x36, 0x35, 0x34, 0x33, 0x32, 0x31, 0x30`).
5. Plugin-button (`0x50` / cell `0x2F`) — pressed. **User reports: Plugin button is on/off only, NO dim state.** So decode this cell as 2-state (off / on `FF FF`), not 3-state.

## What to look for in analysis

- **OFF byte for cells `0x37..0x30`**: any FF38/FF39 frame addressed to these cells during the Layer-1 hold — should be a value distinct from `11 F1` (DIM) and `FF FF` (BRIGHT, presumed).
- **DIM byte**: re-confirm `11 F1` (or whatever) at Layer-2 idle.
- **BRIGHT byte**: for each press window, the active cell flips to BRIGHT — confirm whether it's `FF FF` or a separate brighter encoding.
- **Mutual-exclusion**: does pressing Send 2 send "0x37=DIM, 0x36=BRIGHT" (radio), or just "0x36=BRIGHT" with implicit retain on others? Layer-2 model probably radio (single active selection).
- **Plugin button** (`0x2F`): user-reported on/off only. Cap should still confirm there's no DIM frame for it.

## Decoder hints

- Diff against the Layer-1 segment as the OFF baseline.
- Eight discrete press events ≈ eight burst windows. Same slicing approach as cap42.
- This capture also incidentally has Layer-1↔Layer-2 transition frames — useful for confirming the Layer-button (`0x40`/`0x41`) own LED state-change encoding.

## Next steps after decode

- Extend `Protocol.cpp` `buildUf8GlobalLed(btn, state)` from boolean `on` to a tri-state enum (Off / Dim / Bright). Already done for top-soft-keys per handoff; same pattern for the global row.
- Wire Settings UI later: per-Layer auto-state for Send/Plugin row, plus user-configurable behaviour for Plugin button.
