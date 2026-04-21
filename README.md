# Rea-Sixty

*REAPER × SSL UF8 — native track colors on the scribble strips, no SSL 360° plugin required on every track.*

> Repository name is `reaper-uf8` for historical reasons; the project's public name is **Rea-Sixty** (REAPER + 360).

Primary goal: **REAPER track colors on the UF8 scribble strips in the DAW layer** — something SSL 360° cannot do at all (their color path requires an SSL plugin on every track, and only in Plugin-Mixer mode). Secondary goal over time: replicate enough of SSL 360° that users can uninstall it entirely. See [`docs/architecture-decision.md`](docs/architecture-decision.md) for the standalone-surface plan.

**Status:** pre-alpha reverse-engineering. The UF8 USB protocol is documented (`docs/protocol-notes.md`). A C++ REAPER extension skeleton builds on macOS and links cleanly against libusb. Full phase plan is in [`ROADMAP.md`](ROADMAP.md).

## Why this exists

- SSL's UF8 scribble strips can display track colors, but **only** in SSL 360°'s Plugin-Mixer layer, **only** when an SSL plugin is loaded on every track. Unworkable for 100+ track sessions.
- SSL 360° holds the UF8 vendor-USB interface with an exclusive claim — there is no coexistence hack. Either we replace SSL 360°, or users live with the limitation.

## Approach

1. Capture SSL 360° ↔ UF8 USB traffic (USBPcap on Windows; macOS 15 no longer exposes USB interfaces to Wireshark).
2. Decode the vendor-specific wire protocol (done for color / scribble text / button / meter / heartbeat / layer frames — see `docs/protocol-notes.md`).
3. Ship a REAPER extension that opens the UF8 via libusb and replicates SSL 360°'s host-side responsibilities: init sequence, colors, soft-key mapping, layer management — registered directly as a REAPER `csurf_inst` (no CSI, no virtual MIDI).

## Repo layout

```
docs/                 Living protocol notes, capture workflow, architecture, legal
captures/             Selected .pcap reference captures (most gitignored)
analysis/             Python scripts for parsing captures (pyshark)
extension/            The C++ extension
  src/                Library: Protocol, Palette, UF8Device, ColorSync
  tools/              Standalone CLI tests (libusb only, no REAPER)
  tests/              Pure-logic unit tests (frame bytes, checksum, palette)
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
- `build/reaper_uf8.dylib` — the REAPER extension (incomplete — csurf migration in progress)
- `build/test_protocol` — unit test runner (passes)
- `build/uf8_color_test` — CLI tool that sends one color command (useful for isolated protocol verification)

Full install instructions: [`docs/install-macos.md`](docs/install-macos.md).

## Contributing

Early-stage. If you own a UF8/UC1 and want to help with captures — especially around layer-switching, PM-mode display zones, or UC1 protocol — open an issue. See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the capture-and-decode workflow.

## Legal & Safety

### Trademarks
Not affiliated with, endorsed by, or sponsored by Solid State Logic Ltd. "SSL", "Solid State Logic", "SSL 360°", "UF8", and "UC1" are trademarks of Solid State Logic and are used here solely to identify the hardware and software this project interoperates with (nominative fair use).

### Interoperability basis
Developed via independent, passive observation of the USB wire protocol between legally purchased SSL UF8 hardware and legally licensed SSL 360° software, for the sole purpose of achieving interoperability with REAPER. No SSL code, firmware, binaries, or proprietary creative content is decompiled, reproduced, or redistributed.

Legal footing: EU Software Directive [2009/24/EC](https://eur-lex.europa.eu/eli/dir/2009/24/oj) Art. 6 (interoperability exception); §69e UrhG (Germany); 17 USC §1201(f) (US interoperability exception). Rationale recorded in [`docs/interop-rationale.md`](docs/interop-rationale.md).

### No warranty, use at your own risk
This software is provided "as is" with no warranty of any kind (see [`LICENSE`](LICENSE)). It sends vendor-USB frames to the UF8 that are **not** part of SSL's documented public API. Hardware behaviour under unforeseen frames is not guaranteed and has not been exhaustively tested.

**Running third-party firmware-level communication with SSL hardware may void your hardware warranty with Solid State Logic.** If warranty preservation matters to you, do not run this extension.

### License
MIT — see [`LICENSE`](LICENSE).
