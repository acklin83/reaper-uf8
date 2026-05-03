# Rea-Sixty

*REAPER × SSL UF8 / UC1 — a native, open-source replacement for SSL 360° on REAPER.*

> Repository name is `reaper-uf8` for historical reasons; the project's public name is **Rea-Sixty** (REAPER + 360).

Rea-Sixty drives the SSL **UF8** and **UC1** controllers directly from a REAPER extension over their vendor-USB protocols — no SSL 360° required, no plugin-on-every-track restriction, no CSI, no virtual MCU MIDI. Track colors land on the UF8 scribble strips in the DAW layer (something SSL 360° does not offer at all), faders run at full 16-bit precision, and the SSL Bus Compressor's GR meter on the UC1 follows audio-driven gain reduction on the focused REAPER track.

## Status

Working extension, near feature-complete on macOS. Phase 1 (UF8 standalone replacement) and Phase 2 (UC1 integration with parameter mirror + GR pipeline) are in. Phase 2.5 (folder mode, selection sets, send/receive layers, generic FX learn), 2.6 (on-screen Plugin Mixer window) and 2.7 (in-app Settings screen) are landing — see [`ROADMAP.md`](ROADMAP.md).

## What it does today

- **Direct REAPER ↔ UF8 / UC1** via `csurf_inst` and libusb. No CSI, no virtual MIDI, no MCU.
- **DAW-layer track colors** on the UF8 scribble strips, polled from `GetTrackColor()` and pushed on bank shifts.
- **Full vendor-USB host responsibility**: init-sequence replay on open, scribble text + value zones, color bars, button/V-Pot/fader/fader-touch routing, LED feedback (solo / mute / select / arm / transport), meter bands, layer management, heartbeat.
- **UC1 parameter mirror** — physical knobs follow the focused track's SSL Bus Compressor / Channel Strip in real time, with values mirrored back to the UC1 displays.
- **UC1 GR display driven by the SSL plugin itself** via `TrackFX_GetNamedConfigParm("GainReduction_dB", …)` — the PreSonus VST3 GR extension that REAPER exposes for the SSL Bus Compressor / Channel Strip Comp + Gate. No JSFX probe, no sidechain tap.
- **Bindings system** — per-strip, transport, global, soft-keys per layer; modifier combos; 12 user-defined Soft-Key Banks (8 buttons each) plus the 6 SSL stock banks; Learn mode.
- **Generic FX learn** for any VST/JS/AU plugin parameter, GUID-keyed so FX-slot reorders don't break mappings.
- **Themed on-screen Plugin Mixer + Settings window** (vendored Dear ImGui inside a dockable SWELL window) that picks up the user's REAPER theme.

## Why this exists

SSL's UF8 scribble strips can display track colors, but **only** in SSL 360°'s Plugin-Mixer layer, **only** with an SSL plugin loaded on every track — unworkable for 100+ track sessions. SSL 360° also holds the UF8 vendor-USB interface with an exclusive claim, so coexistence isn't an option. Either replace SSL 360°, or live with the limitation. Rea-Sixty replaces it.

The protocols, capture workflow, and decisions are documented under `docs/` — start with `docs/protocol-notes.md`, `docs/protocol-notes-uc1.md`, and `docs/architecture-decision.md`.

## Repo layout

```
docs/                 Living protocol notes, capture workflows, architecture, plans, legal
captures/             Reference .pcap captures (most gitignored — large binary blobs)
analysis/             Python parsers / diffing tools for USB captures (pyshark)
extension/            The C++ REAPER extension
  src/                Protocol, Palette, UF8Device, UC1Device, ColorSync, Bindings,
                      MixerWindow, MixerLayout, ThemeBridge, SettingsScreen,
                      PluginMap, FocusedParam, HidDevice, …
  tools/              Standalone CLI probes (libusb-only, no REAPER): color test, palette
                      probe, HID probe
  tests/              Pure-logic unit tests (frame bytes, checksum, palette, CSI import)
  vendor/             reaimgui (vendored, docking branch)
  CMakeLists.txt      FetchContent for reaper-sdk + WDL, pkg-config for libusb
ROADMAP.md            Phase plan
```

## Build (macOS)

```bash
brew install libusb cmake pkg-config
cd extension
cmake -B build
cmake --build build -j
```

Outputs:
- `build/reaper_uf8.dylib` — the REAPER extension
- `build/test_protocol`, `build/test_uc1_protocol`, `build/test_csi_import` — unit-test runners
- `build/uf8_color_test`, `build/uf8_hid_probe`, `build/uf8_palette_probe` — standalone CLI probes

Full install instructions: [`docs/install-macos.md`](docs/install-macos.md). Windows and Linux ports are tracked in [`ROADMAP.md`](ROADMAP.md) (Phase 4).

## Contributing

If you own a UF8 or UC1 and want to help — capture work for layer-switch edge cases, the foot-switch USB events, or extra plugins for the Bus-Comp / Channel-Strip parameter map — open an issue. See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the capture-and-decode workflow.

## Legal & Safety

### Trademarks
Not affiliated with, endorsed by, or sponsored by Solid State Logic Ltd. "SSL", "Solid State Logic", "SSL 360°", "UF8", and "UC1" are trademarks of Solid State Logic and are used here solely to identify the hardware and software this project interoperates with (nominative fair use).

### Interoperability basis
Developed via independent, passive observation of the USB wire protocol between legally purchased SSL UF8 / UC1 hardware and legally licensed SSL 360° software, for the sole purpose of achieving interoperability with REAPER. No SSL code, firmware, binaries, or proprietary creative content is decompiled, reproduced, or redistributed.

Legal footing: EU Software Directive [2009/24/EC](https://eur-lex.europa.eu/eli/dir/2009/24/oj) Art. 6 (interoperability exception); §69e UrhG (Germany); 17 USC §1201(f) (US interoperability exception). Rationale recorded in [`docs/interop-rationale.md`](docs/interop-rationale.md).

### No warranty, use at your own risk
This software is provided "as is" with no warranty of any kind (see [`LICENSE`](LICENSE)). It sends vendor-USB frames to the UF8 and UC1 that are **not** part of SSL's documented public API. Hardware behaviour under unforeseen frames is not guaranteed and has not been exhaustively tested.

**Running third-party firmware-level communication with SSL hardware may void your hardware warranty with Solid State Logic.** If warranty preservation matters to you, do not run this extension.

### License
MIT — see [`LICENSE`](LICENSE).
