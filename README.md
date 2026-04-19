# reaper-uf8

REAPER extension that sends REAPER track colors to SSL UF8 scribble strips natively — no per-track SSL plugin required.

Status: **reverse-engineering phase**. USB protocol not yet decoded. See `CLAUDE.md` for strategy and `docs/protocol-notes.md` for current findings.

## Requirements (macOS)
- Wireshark (`brew install --cask wireshark`) — includes ChmodBPF for USB capture
- Python 3 + pyshark (`pip install pyshark`) — for `analysis/` scripts
- REAPER + SSL 360° + UF8 — for generating capture traffic

## Why
SSL UF8 only displays track colors in 360° Plugin Mixer Mode, which needs an SSL plugin on every track. For sessions with 100+ tracks this is unusable. This extension bypasses that by talking directly to the UF8 over USB using the same vendor-specific protocol SSL 360° uses.
