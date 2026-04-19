# reaper-uf8

An open-source replacement for Solid State Logic's **SSL 360°** software, targeting the **UF8** (and later **UC1**) control surfaces.

Primary goal: **REAPER track colors on the UF8 scribble strips in the DAW layer** — something SSL 360° cannot do at all (their color path requires an SSL plugin on every track, and only in Plugin-Mixer mode). Secondary goal over time: replicate enough of SSL 360° that users can uninstall it entirely.

**Status:** early reverse-engineering. The UF8 USB protocol is documented (`docs/protocol-notes.md`). A C++ REAPER extension skeleton builds on macOS and links cleanly against libusb. Full phase plan is in [`ROADMAP.md`](ROADMAP.md).

## Why this exists

- SSL's UF8 scribble strips can display track colors, but **only** in SSL 360°'s Plugin-Mixer layer, **only** when an SSL plugin is loaded on every track. Unworkable for 100+ track sessions.
- Meanwhile many of us already drive UF8 via [CSI](https://github.com/GeoffAWaddington/CSI) over MCU MIDI — faders, V-pots, meters, transport all work. The *only* thing missing is the vendor-USB path that carries colors.
- SSL 360° holds the vendor-USB interface with an exclusive claim, so there's no coexistence hack — we either replace SSL 360° or live with the limitation.

## Approach

1. Capture SSL 360° ↔ UF8 USB traffic (USBPcap on Windows, since macOS Sequoia no longer exposes USB interfaces to Wireshark).
2. Decode the vendor-specific protocol (done for color / button / meter / heartbeat frames — see `docs/protocol-notes.md`).
3. Write a REAPER extension (or standalone daemon) in C++ that opens the UF8 via libusb and replicates SSL 360°'s host-side responsibilities: init sequence, colors, soft-key mapping, layer management.

## Repo layout

```
docs/                 Living protocol notes, capture workflow, platform docs
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
- `build/reaper_uf8.dylib` — the REAPER extension (incomplete — init sequence TODO)
- `build/test_protocol` — unit test runner (passes)
- `build/uf8_color_test` — CLI tool that sends one color command (useful for isolated protocol verification)

Full install instructions: [`docs/install-macos.md`](docs/install-macos.md).

## Contributing

Early-stage. If you own an UF8/UC1 and want to help with captures, especially around the init sequence or UC1 protocol, open an issue.

License: MIT. This project is an independent reverse-engineering effort — not affiliated with or endorsed by Solid State Logic.
