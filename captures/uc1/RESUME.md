# UC1 Capture Session — Resume Point

**Phase 2 status:** all 14 planned captures recorded. Next phase is **decode / analysis**, not more captures.
**Last commit:** `0e8f309` on `main`, pushed.

## Captures in hand (01–14)

| # | File | Pkts | Gist |
|---|------|-----:|------|
| 01 | `uc1_01_init_clean.pcapng` | 27944 | Init/wakeup on fresh enumeration |
| 02 | `uc1_02_idle_baseline.pcapng` | 11288 | 10 s idle — baseline for every diff |
| 03 | `uc1_03_plugin_presence.pcapng` | 34298 | Load/unload transitions |
| 04 | `uc1_04_knob_threshold_sweep.pcapng` | 17702 | Threshold CCW→CW→CCW |
| 05 | `uc1_05_knob_ratio_steps.pcapng` | 16940 | Ratio stepped, one direction |
| 06 | `uc1_06_knob_attack_release.pcapng` | 22540 | Attack + Release sweeps |
| 07 | `uc1_07_knob_makeup_mix.pcapng` | 29184 | Makeup + Mix + SC HPF sweeps |
| 08 | `uc1_08_buttons_all.pcapng` | 22758 | 13 buttons — full set (order documented in sibling) |
| 09 | `uc1_09_display_params.pcapng` | 23130 | GUI-side param changes (order not recorded) |
| 10 | `uc1_10_track_select.pcapng` | 22378 | Focus 1→2→3→4→1, BusComp on T1/T3 only |
| 11 | `uc1_11_gr_static.pcapng` | 11302 | GR steady ~12 dB |
| 12 | `uc1_12_gr_dynamic.pcapng` | 11370 | GR animated |
| 13 | `uc1_13_vu_meters.pcapng` | 22894 | Test tones −20/−10/−6/0 dBFS |
| 14 | `uc1_14_multiple_sc.pcapng` | 22547 | Ext-SC LED feedback |

## Environment notes for future capture re-runs

- Windows host: `StoerPC` at `192.168.177.197`, user `claude` / `claudepass`
- USBPcap interface for SSL devices: `\\.\USBPcap3`
- UC1 device address **varies per replug**; last seen 34. Always confirm from the pcap itself via `usb.idVendor == 0x31e9` filter rather than hard-coding.
- UF8 must stay physically disconnected during UC1 captures (capture hygiene — see `docs/windows-capture-workflow-uc1.md` and `uc1-gr-routing` memory note)
- Windows work directory: `C:\Users\claude\uc1_capture\`

## Pending decode work (next session focus)

1. Run `analysis/parse_usbpcap_uc1.py` against each 03–14 capture with `uc1_02` as baseline. Save output hex digests alongside for review.
2. Start filling `docs/protocol-notes-uc1.md` → *Frame format* and *Commands* sections with concrete bytes:
   - Init sequence extraction from `uc1_01` → `extension/src/init_sequence_uc1.inc` (not yet created)
   - Knob event-frame IDs from uc1_04–07 → table in protocol notes
   - Button event-frame IDs from uc1_08 (sequence is documented in its sibling .md)
   - GR frame family + byte→dB calibration from uc1_11 (~12 dB anchor) and uc1_12 (full range)
   - VU frame family + 4-point calibration from uc1_13
   - Plugin-presence + track-retarget frames from uc1_03 and uc1_10
3. Cross-check GR routing claim from cap17 against uc1_11/12 findings (see `uc1-gr-routing` memory — is the `FF 13 04` family really UC1-exclusive, or does UF8 receive GR via a different route?).
4. Once the event table is solid, start the UC1 counterpart to `extension/src/` — mirrors the UF8 pipeline but speaks to the UC1 section of the device.

## Housekeeping
- `probe1/2/3.pcapng` and `probe_resume.pcapng` on the Windows side are throwaway probes; can be deleted at any time.
- The 14 captures total ~17 MB in the repo (`git add -f` style, tracked despite `captures/` gitignore rule).
