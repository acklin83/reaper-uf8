# cap50_plugin_top

**Date:** 2026-04-30
**Action:** User pressed Plugin button (toggling) and the top-row
parameter buttons above each strip display. Goal: confirm Plugin LED
behaviour and re-verify per-strip top-soft-keys.

## Findings

- **Plugin button cell `0x2F`** confirmed. Toggle pattern:
  - Bright: `FF 38 04 0x2F 00 FF FF` + `FF 39 04 0x2F 00 00 F0`
  - Dim:    `FF 38 04 0x2F 00 11 F1` + `FF 39 04 0x2F 00 11 F1`
  - **No legacy `FF 3B 03` frame** — Plugin button is 2-state via
    pure colour-pair, distinct from Page L/R + Send/Plugin row.
- Per-strip top-soft-keys (cells `0x18..0x1F`) — already known via
  cap41; this capture re-verifies them as 3-state with the same
  encoding (off / dim / bright) plus an optional legacy frame.

## Plugin "outlier" memory note correction

`uf8-3state-led-encoding.md` previously called Plugin a "2-state
outlier". cap50 confirms that — but for a different reason than first
assumed. Plugin doesn't NEED the legacy frame because it's not a
3-state LED, it's just bright/dim. The off-state visible in cap44
earlier was firmware default, not a separate state in the protocol.
