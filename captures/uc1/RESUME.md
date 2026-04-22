# UC1 Capture Session — Resume Point

**Paused:** 2026-04-22
**Last commit:** `a201e09` on `main`, pushed
**Last capture:** `uc1_09_display_params.pcapng`
**Next capture:** `uc1_10_track_select.pcapng`

## Environment at pause

- Windows host: `StoerPC` at `192.168.177.197`, user `claude` / `claudepass`
- UC1 connected on `\\.\USBPcap3`, device address was **28** (resets on any replug / Windows reboot — if address differs next time, re-run the VID check: `tshark -r <capture> -Y "usb.idVendor == 0x31e9" -T fields -e usb.device_address | sort /unique`)
- UF8 physically disconnected — keep it that way
- SSL 360° (SSL360Core + SSL360Gui) and REAPER running
- Last REAPER state: one track, both Bus Comp 2 and Channel Strip 2 loaded on it, UC1 mirroring
- Windows work directory: `C:\Users\claude\uc1_capture\`

## Captures done (01–09)

| # | File | Status |
|---|------|--------|
| 01 | `uc1_01_init_clean.pcapng` | 27944 pkts |
| 02 | `uc1_02_idle_baseline.pcapng` | 11288 pkts — use as `--baseline` for every diff |
| 03 | `uc1_03_plugin_presence.pcapng` | 34298 pkts — load/unload transitions |
| 04 | `uc1_04_knob_threshold_sweep.pcapng` | 17702 pkts |
| 05 | `uc1_05_knob_ratio_steps.pcapng` | 16940 pkts, one direction only |
| 06 | `uc1_06_knob_attack_release.pcapng` | 22540 pkts |
| 07 | `uc1_07_knob_makeup_mix.pcapng` | 29184 pkts (Makeup + Mix + SC HPF) |
| 08 | `uc1_08_buttons_all.pcapng` | 22758 pkts — 13 buttons, full set |
| 09 | `uc1_09_display_params.pcapng` | 23130 pkts — format only, no per-param order |

## Resume — next capture is uc1_10 (track-select retarget)

### REAPER setup required (user will do before saying "go")
- 4 tracks in the session
- Bus Comp 2 loaded on tracks 1 **and** 3
- Tracks 2 and 4 empty (no SSL plugins)
- UC1 locked to track 1 to start (click track 1 header so it's focused)

### Capture action (20 s)
tshark 20 s on `\\.\USBPcap3` → `uc1_10_track_select.pcapng`. During capture user clicks in REAPER:
- T ≈ 0 s: focus track 1 (start state, already selected)
- T ≈ 3 s: click track 2 header
- T ≈ 7 s: click track 3 header
- T ≈ 11 s: click track 4 header
- T ≈ 15 s: click track 1 header again
- remainder: tail / idle

Goal: isolate the "retarget UC1" frame SSL 360° emits on focused-track change, plus the "no plugin on this track" state frames (tracks 2 and 4).

### Capture command (runs from Mac side)
```
sshpass -p claudepass ssh -n -o StrictHostKeyChecking=no claude@192.168.177.197 \
  'cd /d C:\Users\claude\uc1_capture && "C:\Program Files\Wireshark\tshark.exe" \
   -i \\.\USBPcap3 -a duration:20 -w uc1_10_track_select.pcapng'
```
Then `scp` the pcapng back, write sibling `.md`, update `docs/protocol-notes-uc1.md` capture index, `git add -f` the pcapng, commit, push.

## After uc1_10 — remaining plan (11–14)

- `uc1_11_gr_static.pcapng` — sustained tone, aggressive comp, GR pinned high, 10 s
- `uc1_12_gr_dynamic.pcapng` — drum/bass loop through same comp, 10 s (GR stream rate)
- `uc1_13_vu_meters.pcapng` — test tones −20/−10/−6/0 dBFS with compressor bypassed
- `uc1_14_multiple_sc.pcapng` — external sidechain on, cycle SC sources

11/12 are the headline captures for decoding GR — also our chance to re-verify the cap17 routing claim (see `.claude/memory/uc1-gr-routing.md` → re-verification note).

## Pending analysis work (post-capture, context permitting)

- Run `analysis/parse_usbpcap_uc1.py` against each capture with `uc1_02` as baseline
- Start filling in `docs/protocol-notes-uc1.md` → Frame format / Commands sections with concrete bytes
- Extract init sequence from `uc1_01` into `extension/src/init_sequence_uc1.inc` (not yet written)
