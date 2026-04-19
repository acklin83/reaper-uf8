# reaper-uf8

REAPER extension that sends REAPER track colors to SSL UF8 scribble strips natively — no per-track SSL plugin required.

Status: **reverse-engineering phase**. USB protocol not yet decoded. See `CLAUDE.md` for strategy and `docs/protocol-notes.md` for current findings.

## Requirements
**Capture (Windows):** Wireshark for Windows (includes USBPcap bundle) + SSL 360° Windows + any DAW + UF8. See `docs/windows-capture-workflow.md`. macOS 15 no longer exposes USB capture to Wireshark on Apple Silicon — we capture on Windows and analyze on macOS.

**Analysis (macOS):** Python 3 + pyshark (`.venv` already set up in this repo).

## Why
SSL UF8 only displays track colors in 360° Plugin Mixer Mode, which needs an SSL plugin on every track. For sessions with 100+ tracks this is unusable. This extension bypasses that by talking directly to the UF8 over USB using the same vendor-specific protocol SSL 360° uses.
