# CLAUDE.md — reaper-uf8

## What This Is
A REAPER extension that sends REAPER track colors natively to the SSL UF8 Scribble Strips — without requiring an SSL plugin on every track.

SSL only displays track colors when the controller is in 360° Plugin Mixer Mode, which requires an SSL VST3 plugin (Channel Strip, 4K B, or 360° Link) on **every** track. For 100+ track sessions this is impractical. MCU/HUI mode has no color support at all because those protocols don't carry color parameters.

## Device Facts (UF8)
- **VID:** `0x31e9` (Solid State Logic)
- **PID:** `0x0021` (UF8)  — `0x0023` is UC1, `0x0022` is the HID controller
- **USB class:** `bInterfaceClass=0xFF` (vendor-specific), `SubClass=0xFF`, `Protocol=0xFF`
- **Endpoints:** 2 (likely 1 Bulk-IN + 1 Bulk-OUT, TBD from descriptor)
- **bcdUSB:** 2.0, **speed:** Full Speed (12 Mbit/s)
- **Exclusive owner (when SSL360Core runs):** `SSL360Core` — cannot co-open the interface

Implication: the 360° color commands are NOT MIDI-over-USB. They travel on a vendor-specific pipe that no MCU/HUI client can see. Reverse-engineering = USB capture while SSL360Core drives the device.

## Strategy
1. **Sniff:** capture USB traffic on the XHC interface while SSL360Core is actively setting colors in REAPER. Baseline (idle) vs. color-change deltas.
2. **Decode:** identify packet structure — channel index, color encoding (RGB? palette?), framing bytes.
3. **Emit:** write a REAPER extension that opens the UF8 (when SSL360Core is NOT running) and sends the same packets driven by `GetTrackColor()` + bank-switch hooks.

Stretch: test whether UF8 accepts color packets while ALSO in MCU/HUI mode (second USB endpoint, different altsetting?) — if yes, we could run alongside SSL360Core instead of replacing it.

## Repo Layout
```
captures/    .pcapng files from USB sniffing (with matching .md notes)
analysis/    Python scripts to parse captures and diff packet streams
docs/        protocol-notes.md — living spec built up from capture analysis
extension/   REAPER extension source (reaper_plugin.h based, C++)
```

## Capture Workflow (macOS Apple Silicon)
See `docs/capture-workflow.md`. TL;DR:
1. Identify XHC interface UF8 sits on: `ioreg -p IOUSB -l | grep -B1 UF-001254` → `locationID` → XHC bus
2. Install Wireshark (incl. ChmodBPF for BPF permissions)
3. `tshark -i XHC20 -w captures/baseline.pcapng` — UF8 idle for 10s
4. Trigger color-change in SSL 360° while recording, stop capture
5. Parse with `analysis/parse_usbpcap.py`

## Tone
- Minimal. Code speaks.
- Protocol findings go straight into `docs/protocol-notes.md` as we discover them.
- Every capture file has a matching `.md` sibling describing WHAT was captured and WHY (so 6 months later we still know).

## References
- Behringer X-Touch scribble strip SysEx (known format): `F0 00 00 66 14 72 [8 color bytes] F7` — 8-color palette, per-channel
- REAPER extension SDK: https://github.com/justinfrankel/reaper-sdk
- libusb (we will need it for the extension): https://libusb.info
- SSL 360° uses VST3 extensions + proprietary IPC — confirmed by SSL support that colors are plugin-mixer only
