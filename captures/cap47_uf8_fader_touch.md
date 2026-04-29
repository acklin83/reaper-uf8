# cap47 — UF8 fader touch + position decode

**Date:** 2026-04-29
**Bus:** USBPcap3, UF8 at addr 3.9 (this boot)
**Duration:** 25 s
**Trigger conditions:** Reaper-UF8 extension running (consuming events), no SSL360 installed.

## What the user did

| Window     | Action                                                          |
|------------|-----------------------------------------------------------------|
| 3-5 s      | (intended: brief tap on Fader 1 — not seen in capture)          |
| 10-13 s    | Fader 1 touched and held (~1.3 s)                               |
| 13-16 s    | Fader 1 touched again, slowly slid down through ~3 s            |
| 18-20 s    | Fader 8 quickly tapped                                          |

The Sek-3 brief tap likely fell before the user reacted to the start prompt, or was too short for the touch sensor's debounce to register. Three of four actions visible.

## Captured events (UF8 IN, EP 0x81, src 3.9.1)

| t (s)   | Frame                          | Decoded                          |
|---------|--------------------------------|----------------------------------|
| 10.6460 | `31 60 FF 20 02 00 01 23`       | Fader 1 (strip 0) TOUCH-ON       |
| 11.9657 | `31 60 FF 20 02 00 00 22`       | Fader 1 TOUCH-OFF                |
| 13.0055 | `31 60 FF 20 02 00 01 23`       | Fader 1 TOUCH-ON                 |
| 13.795+ | `31 60 FF 21 03 00 <lo> <hi> ck` | Fader 1 position stream (134 frames, ~10 ms cadence) |
| 16.0050 | `31 60 FF 20 02 00 00 22`       | Fader 1 TOUCH-OFF                |
| 17.8766 | `31 60 FF 20 02 07 01 2A`       | Fader 8 (strip 7) TOUCH-ON       |
| 19.3463 | `31 60 FF 20 02 07 00 29`       | Fader 8 TOUCH-OFF                |

Position stream during slow-drag: monotonic LE16 descent
`0x5FBC → 0x5FA3 → 0x5F6C → 0x5F37 → 0x5F08 → 0x5EBB → 0x5E5E → 0x5DFB → …`

10× of those frames had a `FF 33 02 00 <flag>` segment appended, `<flag>` toggling 00/01. Hypothesis: secondary sensor or detent flag. Not pinned down.

## Decoded protocol additions

Both new opcodes documented in `docs/protocol-notes.md` under "Events — UF8 → host":
- `FF 20 02 <strip> <state>` — fader touch
- `FF 21 03 <strip> <lsb> <msb>` — fader position (16-bit LE)
- Composite `FF 21 03 … FF 33 02 00 <flag>` — position + secondary tag (TBD)

## What's still open

- The mystery `FF 33 02 00 <flag>` tag — what triggers it, what does the flag mean.
- Position min/max — only saw ~0x37xx..0x62xx during a partial throw. A full bottom-to-top sweep would establish the range and confirm linearity.
- No `FF 22 03` button frames in this capture, only fader-related — confirms the existing button decoding is independent.
