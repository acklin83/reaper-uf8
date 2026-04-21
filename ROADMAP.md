# Roadmap

Goal: an open-source replacement for **SSL 360°** that drives the **SSL UF8** (and eventually **UC1**) controllers — without the SSL-plugin-on-every-track requirement, with DAW-layer scribble-strip colors, and cross-platform (macOS first, Windows and Linux follow).

The project started 2026-04-19 as a "just the colors" REAPER extension. After decoding the vendor-USB protocol it became clear that:
- The UF8 needs a wakeup/init sequence from the host to render anything, so a simple color-push side-car isn't possible
- SSL 360° claims the vendor-USB interface exclusively — can't coexist
- Therefore the only technically honest solution is to re-implement SSL 360°'s host-side responsibilities

## Phase 1 — UF8 standalone, full replacement

**Goal:** The user can quit SSL 360° for UF8 and lose no functionality. Bonus: DAW-layer scribble-strip colors, which SSL 360° doesn't offer at all.

**Scope clarification (2026-04-21):** We replace CSI entirely. The extension is the full host — no MCU-MIDI bridge, no reliance on any REAPER control-surface plug-in. All UF8 input (buttons, fader, fader-touch, V-Pot) is routed via the REAPER C API; all UF8 output (scribble, colors, LEDs, fader motor, meters) is driven by the extension from REAPER state on a timer. This keeps PM-mode — the only mode with color bars — fully functional without any SSL- or MCU-stack dependency.

Deliverables:
- `reaper_uf8` REAPER extension that claims UF8 over libusb
- Init-sequence replay at open() so the UF8 actually wakes up
- Color push on REAPER track-color change + bank shift
- Native button router: every UF8 button (per-strip SOLO/CUT/SEL/V-Pot-Push, top soft-keys, Bank, Layer, Send/Plugin, Page, Flip, Automation, Transport, 360, Channel-Encoder, Norm/Rec/Auto, PAN, Fine) mapped to a REAPER API call or named extension action
- Native fader + fader-touch + V-Pot rotation routing via REAPER API (no virtual MIDI)
- LED feedback driven from REAPER state (solo/mute/select/arm) on the extension's 30 Hz timer — UF8 LEDs reflect what REAPER thinks
- Keyboard-macro engine: press UF8 soft-key → run a REAPER named action (equivalent of SSL 360°'s "Shift Opt N1" mappings)
- Layer management: DAW / Send / Pan / Plugin / EQ / Instrument selected via left-side Layer buttons
- Meter display: REAPER peak meters drive UF8 meter bands via the native vendor-USB meter command (once decoded)
- Minimal config via JSON/INI: mappings, colors, layer bindings

Next capture/decode work: LED command formats (solo/mute/select/arm), meter command format.

**Milestone complete when:** User starts macOS, SSL 360° is never launched, no MCU control-surface plug-in is configured in REAPER, UF8 drives REAPER fully via the native extension, scribble strips show REAPER track colors.

## Phase 2 — Config UI

**Goal:** Mappings editable without touching code.

Deliverables:
- A WebView / Electron / native SwiftUI config UI equivalent to SSL 360°'s mapping screens: soft-keys, quick-keys, send/plugin rows, automation buttons, pan/shift, channel encoder, foot switches
- Color-picker per button
- Live preview on UF8
- Import/export JSON configs so users can share setups

**Milestone complete when:** A non-developer UF8 user can remap soft-keys via GUI, save, reload, and see the change on the hardware.

## Phase 3 — UC1 support

**Goal:** SSL 360° can be uninstalled — nothing depends on it anymore.

Deliverables:
- UC1 vendor-USB protocol reverse-engineering (separate capture + decode pass)
- UC1 driver integrating with the Phase 1 architecture
- Integration with SSL plugins (4K B / 4K E / 4K G / Channel Strip / Bus Compressor) so UC1 still drives the plugin GUIs — needs reverse-engineering of the SSL-plugin ↔ SSL-360° IPC
- If IPC is unreachable: UC1 in a "generic channel-strip mode" driving any REAPER-selected plugin via automation of its VST parameters

**Milestone complete when:** SSL 360° is uninstalled, UF8 + UC1 work fully, SSL plugins respond to UC1.

## Phase 4+ — Community

**Goal:** Project graduates from "Frank's studio tool" to "the open-source alternative for SSL controllers".

Candidate work:
- Windows port (libusb there, but USB backend differs in edge cases)
- Linux port (hidraw + libusb)
- DAW support beyond REAPER: anything MCU-compatible (Cubase, Studio One, Pro Tools via HUI, Logic, Bitwig)
- Firmware update path (since SSL will still ship firmware blobs; we may need to let SSL 360° do firmware updates, our tool handles everything else)
- Support for more SSL controllers (Big SiX, BiG SiX, SiX, other Nucleus variants)

## Non-goals (at least for now)

- Replacing SSL's plugin *GUIs* — the plugins keep their own UIs, we just drive parameters
- Full binary compatibility with SSL 360° config files — we ship our own simpler JSON format with a one-time import from SSL 360° XML
- Reverse-engineering any firmware — we only talk to the controllers through their existing USB protocols

## Working rhythm

- Each phase starts with a capture/decode session (Windows box, USBPcap)
- Decoded protocol lands in `docs/protocol-notes.md` (never let a capture go un-documented)
- Code in small layers with unit tests where the logic is pure (checksum, palette, frame parse)
- Extension/daemon rebuilds verified on Mac Studio and MacBook before anything lands on main
