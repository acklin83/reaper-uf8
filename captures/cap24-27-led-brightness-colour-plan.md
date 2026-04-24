# cap24-27 — UF8 LED brightness + colour + SEL palette

Planned 2026-04-24.

Settings UI (`docs/plan-settings-ui.md`) needs:
- Global **LED brightness** command (slider 0..100 in SSL 360°)
- Global **scribble-strip brightness** command (separate slider in 360°)
- **SEL / button LED colour** — palette picker per-class (if 360° exposes)
- **SEL follows track colour** toggle

All sniffed while SSL 360° drives the UF8. UC1 physically disconnected for clean attribution.

## Capture plan

| # | File | Action in SSL 360° |
|---|------|--------------------|
| 24 | `cap24_idle_baseline_v2.pcapng` | 10 s idle with current SSL 360° version. Fresh baseline — `cap02` is stale enough that the app may push new frame families at startup. REAPER open, one track, UF8 in PM mode, nobody touching anything. |
| 25 | `cap25_led_brightness.pcapng` | Open SSL 360° device-settings panel for UF8. Drag the **LED brightness** slider from min → max in ~5 even steps, pause ~2 s at each. Goal: identify the single-frame brightness command + its value range. |
| 26 | `cap26_scribble_brightness.pcapng` | Same panel — drag the **scribble-strip brightness** slider min → max, 5 steps × 2 s. Goal: separate command (or same with different target byte) for the LCD backlight. |
| 27 | `cap27_sel_follows_colour.pcapng` | Toggle "SEL follows track colour" on → off → on in SSL 360°, 2 s between. Then colour a track (SEL on it) → change REAPER track colour → observe what SSL pushes. Goal: decode whether SEL uses the existing `FF 66 09 18` palette stream or a separate per-class colour command. |

### Optional (if 360° exposes)

| 27b | `cap27b_button_led_colour.pcapng` | If SSL 360° has a "button LED colour" picker (red / green / amber for Rec/Solo/Mute), cycle through the available colours with 2 s pauses. Goal: decode per-class LED colour command. |

## Pre-capture checklist

- Windows host up, SSH reachable (`sshpass -p claudepass ssh claude@192.168.177.197`).
- UF8 on its USBPcap interface (verify via short probe first — address drifts per boot).
- **UC1 physically disconnected** (avoid bank/addr confusion).
- SSL 360° running, UF8 selected as target.
- REAPER open with ≥1 coloured track so the colour palette has something non-neutral to push.

## After each capture

- `scp` to `captures/` on mac, write sibling `.md` describing trigger + findings.
- Run `python3 analysis/parse_usbpcap.py <file>.pcapng --baseline captures/cap24_idle_baseline_v2.pcapng`.
- Log novel-payload frames in `docs/protocol-notes.md` under a new **LED brightness + colour** section.

## Why now

Phase 2 Settings UI needs these frames to ship the brightness sliders and SEL-colour toggle. Without the decode, those UI controls are dead.

## Frame-family hypothesis (to verify in analysis)

- Global brightness → new `FF xx` command or extension of the existing heartbeat header; likely 1–2 payload bytes.
- Scribble brightness → probably same command family with a target byte differentiating LED vs LCD backlight.
- SEL colour → could be either an extra payload byte on the existing `FF 66 09 18` palette stream, or a dedicated frame writing to a different target zone.

Write the hypotheses into the analysis report and confirm / disconfirm from the capture diffs.
