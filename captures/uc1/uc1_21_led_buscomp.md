# uc1_21_led_buscomp

Date: 2026-04-23
Windows host: StoerPC
tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: 35

## Session state

**SSL Native Bus Compressor 2** loaded on the focused track (replacing CS 2 from `uc1_20`). UC1 mirroring. Bus Comp IN was **On** at capture start (user noted afterwards: "bus comp war IN, also verkehrt herum") — the Bus Comp IN press at the end of the sequence therefore shows "Bus Comp Out" (toggling the plugin off).

## Action

75 s capture. Same 17-button sequence as prior tests, with Solo and Solo Clear swapped to ensure Solo was active before Solo Clear so the Solo-Clear LED would light visibly:

1 HF Bell · 2 EQ Type · 3 EQ In · 4 LF Bell · 5 Fast Att (Comp) · 6 Peak · 7 Dyn In · 8 Expand · 9 Fast Att (Gate) · 10 Polarity · 11 S/C Listen · 12 **Solo** · 13 **Solo Clear** · 14 Cut · 15 Fine · 16 Channel IN · 17 Bus Comp IN

## Summary

- 84 276 packets total on USBPcap3
- 16 press events (HF Bell `0x08` silent, Bus Comp 2 has no EQ — matches the plugin-gated emission rule)
- 82 unique `FF 13 04` payloads seen — this command is where the real per-button LED feedback lives (not `FF 5C`)

## Direct evidence — Bus Comp IN = 0x0C

Last press at t=32.95 s fired id `0x0C`, zone 0x05 display showed `"Bus Comp        Out"`. Confirms the positional guess from `uc1_08`.

## LED feedback decode: `FF 13 04 <bank> <cell> 01 <state>`

Observation: each button press is followed within ~10 ms by exactly one `FF 13 04` frame with a unique `(bank, cell)` pair. The state byte is `0xFF` when the button's LED turns on and `0x00` when it turns off.

### Bank/Cell → Button LED map

| Button | ID | LED cell (`FF 13 04 02 XX 01 YY`) |
|--------|----|--------------------------------------|
| EQ Type | `0x09` | cell `0x51` |
| EQ In | `0x0A` | cell `0x50` |
| LF Bell | `0x0B` | cell `0x23` |
| Fast Attack (Comp) | `0x14` | cell `0x38` |
| Peak | `0x15` | cell `0x39` |
| Dyn In | `0x16` | cell `0x5B` |
| Expand | `0x17` | cell `0x92` |
| Fast Attack (Gate) | `0x18` | cell `0x93` |
| Polarity | `0x19` | cell `0x98` |
| S/C Listen | `0x1A` | cell `0x99` |
| Solo | `0x1C` | cell `0x97` |
| Cut | `0x1D` | cell `0x96` |
| Fine | `0x1F` | cell `0x95` |
| Channel IN | `0x1E` | cell `0x94` |
| Bus Comp IN | `0x0C` | cell `0x01` (bank `0x02`) |
| Solo Clear | `0x1B` | bank `0x01` cell `0x9A`, values cycle 01 → 03 → 00 (active-cleared-off indicator) |
| HF Bell | `0x08` | **not captured** (Bus Comp 2 had no EQ context, HF Bell silent) |

Bank byte is always `0x02` for Channel-Strip-area LEDs (top EQ/Dyn/Channel buttons). `0x01` seen only for Solo Clear transitions. Bus Comp IN also uses bank `0x02` (different from the `0x01` bank for Solo Clear).

### Plugin-bypass burst

Pressing Channel IN (to bypass the Channel Strip plugin) fires the direct `02 94 01 00` LED-off on the button, **then** a 47-frame burst of other LED cells going off — all the plugin's section LEDs go dark together. Same mechanism fires on Bus Comp IN toggling: additional cells update with `ff`/`00` values reflecting the new "plugin active" or "plugin bypassed" state of the whole section.

Observed additional cells addressed in the burst (when toggling Channel IN off):

```
02 83 00 33    02 78 00 33    02 6d 00 33    02 63 00 33    02 57 00 33
02 4a 00 33    02 3f 00 33    02 34 00 33    02 1d 00 33    02 29 00 33
02 3a 01 ff    02 3b 01 ff    02 3c 01 ff    02 3d 01 ff    02 3e 01 ff
02 45 01 ff    02 50 01 ff    02 66 01 ff    02 71 01 ff    02 72-7c 01 ff
02 87 01 ff    02 95 00 33    02 8a 00 33    02 c5 00 33
02 0d 01 ff    02 0e-14 01 ff    02 15 01 19
```

Some of those have state byte `0x33` — this is the **dim** brightness level used when the plugin is bypassed. Per user feedback: LEDs stay visible (at reduced brightness), they don't go fully dark. So `0x33` = dim, `0xFF` = bright-on, `0x00` = fully off. Bypass pushes every cell to `0x33`; re-engage pushes each cell back to its prior state.

### `FF 5C` revisited

Only one `FF 5C` frame appeared in the whole capture: `FF 5C 02 00 0A` (bits 1 + 3). This command is **not** the per-button LED feedback — more likely a small global status register (Bus-Comp-active, Solo-active, etc.). Per-button LED pushing should use the `FF 13 04 <bank> <cell> 01 <state>` mechanism.

## Implementation notes for Rea-Sixty

- Per-button LED push: `FF 13 04 <bank> <cell> 01 <state>` — bank `0x02` for most, table above for cells.
- Plugin-bypass: in addition to toggling the main button cell (e.g. `0x94` for Channel IN), also push the `state=0x33` / `0x00` updates for the section's feature LEDs so the hardware visually darkens when the plugin is bypassed. The 47-frame burst is the full reference; Rea-Sixty can cache the section's active-state cells and replay them.
- `FF 5C` can be mirrored for whatever small global state it represents (TBD — not critical for initial release).
