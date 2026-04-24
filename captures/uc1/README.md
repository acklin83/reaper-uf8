# UC1 Captures

Planned capture sessions for decoding the SSL UC1 USB protocol. Order matters: `uc1_01` and `uc1_02` are prerequisites for the diff-based analysis in every later session.

**Before every capture:** UF8 physically unplugged (not just powered down). See `docs/windows-capture-workflow-uc1.md` → first line, and `docs/protocol-notes-uc1.md` → *Capture constraints* for the cap17 GR-routing rationale.

**Filename rule:** exact names below. Each `.pcapng` gets a sibling `.md` (same stem) once recorded, documenting host state and the precise action+timing. Capture-index table in `docs/protocol-notes-uc1.md` is updated with the date + one-line summary as each capture lands.

## Capture plan

| # | File | REAPER-side action |
|---|------|--------------------|
| 01 | `uc1_01_init_clean.pcapng` | Start Wireshark → power-cycle/replug UC1 → SSL 360° re-enumerates it → stop after device is responsive (~5 s past last burst). REAPER itself idle, no project actions. Goal: full init/wakeup frame sequence. |
| 02 | `uc1_02_idle_baseline.pcapng` | REAPER open with one track, SSL Native Bus Comp 2 on it, UC1 targeted at that track, nobody touching anything. Record 10 s clean idle. Used as `--baseline` for every subsequent diff. |
| 03 | `uc1_03_plugin_presence.pcapng` | UC1 has no mode toggle — its layout is fixed (Bus Comp 2 center + Channel Strip 2 around it, both always live). Instead capture the plugin presence/absence transitions: start from the focused track having **neither** plugin; load Bus Comp 2 (wait 2s), load Channel Strip 2 (wait 2s), remove Bus Comp 2 (wait 2s), remove Channel Strip 2 (wait 2s). Goal: isolate the frames that light/dark each UC1 section. |
| 04 | `uc1_04_knob_threshold_sweep.pcapng` | Bus Comp mode. Slowly rotate Threshold knob full CCW → full CW → back over ~10 s. No other input. Goal: continuous-knob event encoding + rate. |
| 05 | `uc1_05_knob_ratio_steps.pcapng` | Bus Comp mode. Click through every Ratio step one at a time (1.5, 2, 4, 10, stepped), pause ~300 ms per step. Goal: stepped-knob vs continuous encoding delta. |
| 06 | `uc1_06_knob_attack_release.pcapng` | Bus Comp mode. Attack full sweep, pause, Release full sweep. Goal: confirm both knobs share the event family and reveal their IDs. |
| 07 | `uc1_07_knob_makeup_mix.pcapng` | Bus Comp mode. Makeup sweep, pause, Mix sweep, pause, Sidechain HPF sweep. Goal: complete the knob-ID table for Bus Comp layer. |
| 08 | `uc1_08_buttons_all.pcapng` | Press every UC1 button once in a recorded order (document the order in the sibling .md). Include Bypass, Ext-SC, any mode/utility buttons, encoder pushes. Goal: button-ID map. |
| 09 | `uc1_09_display_params.pcapng` | From REAPER, change each Bus Comp 2 parameter via the plugin GUI (not the UC1), one at a time with 500 ms pauses. UC1 displays should update. Goal: host → UC1 display-write frame format (ASCII vs encoded, zone/knob addressing). |
| 10 | `uc1_10_track_select.pcapng` | Session with 4 tracks, Bus Comp 2 on tracks 1 and 3 only. Click-select track 1 → 2 → 3 → 4 → 1 in REAPER, ~1 s between. Goal: the retarget frame SSL 360° pushes to UC1 when focused-track changes, plus the "no plugin on this track" state. |
| 11 | `uc1_11_gr_static.pcapng` | Route a sustained –6 dBFS tone into the track, pin Threshold very low and Ratio high so GR sits near max. Hold steady ~10 s. Audio playing but no user input. Goal: GR frame format at a stable value. |
| 12 | `uc1_12_gr_dynamic.pcapng` | Loop an aggressive drum/bass clip through the same compressor, 10 s. Goal: GR stream rate and per-sample encoding under real program material. Answers whether `FF 13 04 …` seen in cap17 is the whole picture. |
| 13 | `uc1_13_vu_meters.pcapng` | Play calibrated test tones at –20, –10, –6, 0 dBFS, ~2 s each with silence between. Compressor bypassed so VU reflects input/output cleanly. Goal: VU LED frame format + mapping of level → LED index. |
| 14 | `uc1_14_multiple_sc.pcapng` | External sidechain ON. Cycle between 2–3 different sidechain sources in REAPER (different tracks routed to the comp's SC input). Include one cycle of SC on → off → on. Goal: external-SC indicator frames + any source-identity frames. |

## Planned: LED brightness + colour (2026-04-24)

Settings UI needs the global-brightness frame and the per-class LED-colour frames. SSL 360° exposes both as sliders / pickers in its app UI — sniff while the user drags them.

**Device selection:** run one UC1-focused pass and one UF8-focused pass (other device physically disconnected for clean attribution). Tables below are the UC1 pass; mirror filenames as `uf8_NN_…` for the UF8 pass.

| # | File | Action in SSL 360° |
|---|------|--------------------|
| 28 | `uc1_28_idle_baseline_v2.pcapng` | 10 s idle with current SSL 360° version — fresh baseline in case the app version has drifted since `uc1_02`. |
| 29 | `uc1_29_led_brightness.pcapng` | Open 360°'s device-settings panel. Drag LED-brightness slider from min → max in ~5 even steps, pause ~2 s at each. Goal: identify the single-frame brightness command + its value range. |
| 30 | `uc1_30_led_colour_solo.pcapng` | Change the palette colour SSL assigns to the **Solo** indicator LED (if 360° exposes it). Pick 3–4 distinct colours, pause 2 s per change. Goal: decode colour command for fixed-purpose LEDs (non-strip). |
| 31 | `uc1_31_led_colour_scribble.pcapng` | UF8-only — skip for UC1 pass. Change the "strip accent colour" or "SEL follows track colour" toggle, toggle 3× with 2 s pauses. |
| 32 | `uc1_32_brightness_power_cycle.pcapng` | Power-cycle UC1 with brightness slider at 25 % (non-default). 360° should re-push the current setting post-enumeration — confirms the brightness frame is in the init flood / replay too. |

### Pre-capture checklist

- Windows host up, SSH reachable (`sshpass -p claudepass ssh claude@192.168.177.197`).
- UC1 on `\\.\USBPcap3`, UF8 disconnected.
- SSL 360° running, target device selected.
- Wireshark/USBPcap ready, one capture window per step (10 s each).

### After each capture

- `scp` to `captures/uc1/` on mac, write sibling `.md` (action + timing + SSL-360°-version).
- Run `python3 analysis/parse_usbpcap_uc1.py <file>.pcapng --baseline captures/uc1/uc1_28_idle_baseline_v2.pcapng`.
- Log novel-payload frames in `docs/protocol-notes-uc1.md` under a new "LED brightness + colour" section.

## Notes for the person at the keyboard

- Start Wireshark on the UC1's USBPcap interface **before** the action, stop **~2 s after**. Two-second pre/post buffers help the diff tool align.
- Save pcapng with the exact filename in this table — analysis scripts and `protocol-notes-uc1.md` reference these names directly.
- Sibling `.md` is non-optional. Six months from now the capture is worthless without it.
- If something goes sideways mid-capture (phone rings, wrong knob, UF8 accidentally plugged in): discard, rename the file with a `_bad` suffix, redo. Don't try to salvage.
