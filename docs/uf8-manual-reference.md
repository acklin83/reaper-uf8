# UF8 User Guide — Reference for Rea-Sixty

Distilled from `docs/docs/SSL UF8 User Guide_Rev11.pdf` (Rev 11, Oct 2025, 190 pp., updated for SSL 360 v2.0). Only the parts relevant to building a native REAPER↔UF8 path without SSL 360° / SSL plugins.

## What SSL ships (and why we replace it)

Stock UF8 needs **two stacks** running:

1. **SSL 360°** — desktop app + V-MIDI driver. Owns the UF8 over a vendor-USB pipe (`bInterfaceClass=0xFF`). Surfaces 12 virtual MIDI ports (`SSL V-MIDI Port 1..12`) to the OS.
2. **DAW** — opens the V-MIDI ports as MCU/HUI controllers.

Three layers per UF8, mapped 1:1 to V-MIDI port groups (manual p.25):

| DAW Layer | 1st UF8 | 2nd UF8 | 3rd UF8 | 4th UF8 |
| --- | --- | --- | --- | --- |
| 1 | Port 1 | Port 2 | Port 3 | Port 4 |
| 2 | Port 5 | Port 6 | Port 7 | Port 8 |
| 3 | Port 9 | Port 10 | Port 11 | Port 12 |

Three protocols are in play (p.25):

- **HUI** — Pro Tools.
- **MCP/MCU** — Logic, Cubase/Nuendo, Live, Studio One, **REAPER**, Bitwig, DP, FL, Mixbus, etc. No track colour in the protocol.
- **Plug-in Mixer** — SSL-proprietary "Native SSL 360° control". Carries track colour, plug-in parameters, GR/VU, etc. **Requires a 360°-enabled VST3 plugin (Channel Strip 2 / 4K B / 4K E / 4K G / 360 Link / Bus Compressor) on every track that should appear.** Confirmed via the manual's repeated "VST3 only" / "compatible VST3 DAWs" caveats.

**Rea-Sixty's job:** replace stack #1 entirely. Open the UF8 vendor interface ourselves, drive the same Plug-in-Mixer protocol from REAPER's API (track colour, name, fader, pan, sends, automation, GR/VU), so that no SSL plugin is needed on any track.

## Hardware layout (p.14-17)

8 strips, each: V-Pot (pushable encoder) + colour TFT + SOLO/CUT/SEL + 100 mm motorised touch fader.

Per-unit globals:
- **LAYER 1-3** — switches DAW/profile.
- **QUICK 1-3** — user keys (in Plug-in Mixer Mode locked to: `1=Channel Strip`, `2=Bus Comp`, `3=I/O meter toggle`).
- **360° key** — opens/minimises SSL 360°.
- **8 SOFT KEYS** above each strip + bank selector (V-POT, 1-5).
- **PAN, FINE/SHIFT, FLIP, PLUGIN, CHANNEL, PAGE< >, SEND/PLUGIN 1-8.**
- **SELECTION MODE: NORM / REC / AUTO** — gates SEL key behaviour. AUTO is Pro-Tools-only.
- **BANK < >** — banks tracks in multiples of 8 × (number of UF8s).
- **Large CHANNEL encoder** (notched, pushable) with four modes: Standard (bank ±1), NAV (transport scrub), NUDGE, FOCUS (mouse-wheel emulation). **Press-and-hold = enter/exit Cursor-Transport mode** (see below).
- **Cursor + Mode (circle) keys** — default: zoom/scroll. After `hold CHANNEL`: `↓=Stop ↑=Play ←=Rew →=FF ◯=Rec`.
- **AUTOMATION keys** — Read / Write / Touch / Latch / Trim / Off. (Off has no effect in REAPER.)

Connector panel (p.17):
- **USB-C** to host (the only thing we care about).
- **USB-A "THRU"** for chaining additional UF8s or dongles.
- **DC** 12 V.
- **FS1/FS2** 1/4" foot-switches (normally-closed, momentary).

## Brightness & sleep (p.21)

Single global setting in SSL 360° → Control Setup → Controller Settings:

- **Control Surfaces Brightness** — 5 levels, applies to LCDs *and* button backlights.
- **Sleep timeout** — 1-99 min (or unticked = disabled). Any button/control wakes it. Capture work has already located both packets — see `captures/cap25_led_brightness*` and `docs/protocol-notes.md`.

There is also a per-Plug-in-Mixer toggle: **"Plug-in Mixer UF8/UF1 SEL Keys"** — when on, SEL key colour follows DAW track colour (compatible DAWs only). This is the exact behaviour we are reproducing natively.

## REAPER profile (p.114-122)

REAPER talks **MCU** to UF8. Stock-MCU caveats from SSL itself:

- V-Pots: **only Pan works**. Track / EQ / Send / Plugin / Instrument modes are listed but inert without a 3rd-party MCU script (CSI, ReLearn, CSurf_Klinke_MCU, DrivenByMoss).
- SEL keys: **double-press to single-select**; single press is additive.
- AUTO selection mode: disabled.
- AUTOMATION/OFF: disabled (REAPER has no MCU "Off" automation state).
- SHIFT: maps to MCU Shift but REAPER's behaviour is undocumented.
- Foot-switches → "Play Footswitch" / "Record Footswitch" DAW commands.

Default Quick keys: `1=Solo Clear, 2=F1, 3=F2`. User Banks 2 & 4 ship empty.

LCD layout in REAPER MCU mode (p.115):
- Top zone: soft-key label.
- UpLCD: 6-char track name.
- FaderdB: track level.
- LowLCD: blank in stock REAPER MCU (3rd-party scripts may write).
- 12-segment meter + clip on the right.
- V-Pot readout bar at the bottom.

This is the layout we currently hit through the MCU bridge. The native path needs to fill the same fields plus the ones MCU can't deliver: track colour, plug-in name, plug-in parameter readouts, GR/VU bars at higher resolution.

## Plug-in Mixer Mode (p.170-181) — the protocol we want

Configuration (Control Setup):
- `Auto scroll Plug-in Mixer when banking UF8`
- `Selected parameter follows UC1/Plug-in Mixer`
- `Fader banking follows UC1/Plug-in Mixer Channel Strip selection`
- `Fader banking follows UC1/Plug-in Mixer Bus Compressor selection`
- `Fader touch-sense selects channel`
- `PLUG-IN MIXER UF8/UF1 SEL KEYS` (colour follow)

DAW Control surface in Plug-in Mixer Mode (p.176, **VST3 hosts only** unless noted):

| Parameter | Compatible hosts |
| --- | --- |
| Track Volume | all VST3 |
| Pan | all VST3 |
| Send Levels 1-8 | all VST3 except FL Studio |
| Sends on/off 1-8 | Cubase/Nuendo only |
| **Synchronised Track Colour** | all VST3 + Pro Tools |
| Solo | all VST3 |
| Solo Safe/Defeat | Cubase/Nuendo only |
| Solo Clear | all VST3 |
| Mute | all VST3 |
| Selected Track | all VST3 + Pro Tools |

Listed VST3 hosts: Cubase/Nuendo, Live, Studio One, **REAPER**, LUNA, Bitwig, DP, FL Studio, Mixbus 11+. So REAPER in Plug-in Mixer Mode gets all of the above except Sends-on/off and Solo-Safe.

LCD layout, Channel Strip Mode (p.174):
- Top zone: soft-key label (selects the channel-strip param assigned to V-Pots).
- Plug-in Mixer position number.
- **Channel Strip Type** (CS 2 / 4K B / 4K E / 4K G / 360-Link plugin name).
- **DAW Track Colour** (only in Plug-in Mixer Mode — this is the field).
- TrkNam.
- O/PdB fader readout (plug-in fader, or DAW fader if `PLUG-IN/DAW` toggle is off).
- Input/Output metering bar (toggle via Quick 3).
- Dynamics (gate + comp GR) metering bar.
- Currently selected param name + value.
- V-Pot readout bar.

LCD layout, Bus Compressor Mode (p.175): position, TrkNam, MAKE-UPdB, GR meter, param name, value, V-Pot bar.

Special key behaviours unique to Plug-in Mixer Mode:
- `PLUGIN` toggles fader+pan between the channel-strip plug-in and the DAW track.
- `CLEAR` (hold NORM) + active SOLO/CUT clears all channel-strip solos/cuts.
- `ZERO` (hold REC) + SEL defaults the parameter assigned to that fader.
- Fader touch can auto-select channel (config option).

## Hybrid Mode (p.182-183)

When a UF8 layer is set to Plug-in Mixer, you can borrow HUI/MCU commands from a designated DAW layer:

- Cursor keys → Transport (`↓Stop ↑Play ←Rew →FF ◯Rec`) regardless of CHANNEL-encoder hold.
- Channel encoder retains NAV / FOCUS modes.
- Automation keys passthrough from the HUI/MCU layer (Factory or User mode).

Per-DAW caveats:
- **Seamless: Cubase/Nuendo, Studio One, LUNA, Bitwig, REAPER.** Just SEL the track and trigger the automation mode.
- Pro Tools / Logic / Mixbus: bank-syncing issues — the MCU bank and the Plug-in-Mixer bank don't align; workarounds documented per DAW.
- Digital Performer: automation passthrough doesn't work at all.

For Rea-Sixty this means: REAPER is in the "seamless" list. No bank-sync trickery needed.

## LCD status messages (p.184-185)

Strings the firmware itself writes to the LCDs (so we can recognise/replace them):

- `UF8 Initialisation Complete` + `Awaiting Connection to SSL 360° Software` — boot-up before any host opens the vendor pipe.
- `Layer Set To None` — layer not configured.
- `Waiting For DAW` — Pro Tools profile, DAW not running. (Won't apply to us.)
- `SSL 360° Connection Lost. Attempting to Reconnect` — vendor pipe was opened then dropped.
- "Fader wave" + UF8 logo on Identify (HOME page click).

## Implications for the extension (cross-refs)

| Manual finding | Where in the codebase |
| --- | --- |
| Vendor-USB pipe is the only colour transport | `docs/architecture-decision.md`, `extension/src/UF8Device.cpp` |
| MCU stock REAPER = only Pan, no colour | `extension/src/main.cpp` MCU bridge — kept as transitional |
| Plug-in Mixer LCD fields (TrkNam, O/PdB, ChType, colour, GR, VU, V-Pot bar) | `extension/src/Protocol.cpp`, `docs/protocol-notes.md` |
| Brightness 5 levels, Sleep 1-99 min | `captures/cap25_led_brightness*`, `docs/protocol-notes.md` |
| `PLUG-IN MIXER UF8/UF1 SEL KEYS` colour-follow | `extension/src/ColorSync.cpp` (we always do this) |
| Hybrid-Mode "seamless" list includes REAPER | confirms our REAPER-first scope |
| 4 UF8s × 8 strips chain via THRU | bank/scroll model (`extension/src/UC1Surface.cpp` already handles 8/16/24/32) |
| Quick keys reserved in Plug-in Mixer Mode (1=CS, 2=BC, 3=Meter toggle) | `docs/bindings.md`, settings UI plan |
| CHANNEL hold = Cursor-Transport toggle | input dispatcher (TBD; not yet wired) |
| Fader touch-select option | future setting in the Settings UI plan |

## Conventions used in the manual

- "Compatible VST3 DAWs" everywhere = the host list above. REAPER is on it.
- Every per-DAW caveat about colour explicitly says it requires the SSL plugin on the track. There is no documented MCU/HUI path for colour. Reinforces the capture+reverse-engineer approach.
- "PTSL" = Pro Tools-specific bridge; not relevant to us.
- "UC1" appears throughout because UF8 and UC1 share the Plug-in Mixer state. Our UC1 work in `extension/src/UC1*` and `analysis/uc1_decode/` is the same protocol family.
