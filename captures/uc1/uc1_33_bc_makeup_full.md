# uc1_33_bc_makeup_full

Date: TBD
Windows host: StoerPC
tshark: latest
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: TBD

## Why

`dual_40_bc_pots` only captured 5 cells for BC Makeup (`0xEF..0xF3`). Other BC pots have 7-LED rings, so Makeup is suspected to also be 7 cells but the original sweep didn't reach min/max extremes. Need a full sweep.

## Setup

- REAPER project, single track, **SSL Bus Comp 2** loaded
- Audio off, all other pots untouched

## Action

Sweep the **BC Makeup** pot only. Full range, slowly: ~5 s min→max, hold at max for 1 s, slowly max→min, hold at min for 1 s. Repeat once. ~15 s total.

## Decode targets

- Full bank 0x01 cell list for BC Makeup (expect ~7 cells, a superset of the current `0xEF..0xF3`)
- Whether Makeup is single-dot Position or has brightness gradient — record bank 0x02 byte values

## Output

Update `kBcMakeupCells[]` in [extension/src/UC1Surface.cpp](../../extension/src/UC1Surface.cpp).
