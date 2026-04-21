# LED decode session — 2026-04-21

USB captures from the SSL 360° PM-mode LED reverse-engineering attempt.

## What we tried
- Windows box (192.168.177.197), SSL 360° running, UF8 in PM (Layer 2), REAPER with 8 SSL Channel Strip tracks.
- Captures on all three USBPcap interfaces while triggering SOLO/CUT/SEL state changes through UF8 physical buttons, REAPER mixer clicks, and plugin-GUI clicks.

## Findings
- **UF8 LEDs on SOLO/CUT/SEL light firmware-locally** when the physical button is pressed — SSL 360° does not need to issue a command for that.
- **No novel vendor-USB traffic** appears on the UF8 device (addr 3.61 on this bus) when the DAW track or plugin toggles SOLO/CUT. Heartbeats + a `FF 38/39 04 17 …` meter pair are the only non-baseline frames, and those correlate with audio level, not LED state.
- **UF8's HID interface (PID 0x0022)** is IN-only — no OUT endpoint to receive LED/MCU commands. Its own USB-class descriptor is HID (0x03), not USB-MIDI. Device appears as 3.60 in these captures.
- REAPER with Mackie Control Universal surface pointed at SSL V-MIDI Port 1 **does** light UF8 LEDs on state change, but the path is opaque from USBPcap's perspective — SSL 360° must be bridging through a kernel-level path USBPcap does not capture.

## Conclusion
Without SSL 360° running (which would re-claim UF8 exclusively), there is currently no decoded path to drive UF8 LEDs from host state. LED feedback remains firmware-local (press-to-toggle).

Future capture targets to try if we re-open this:
- Enable USBPcap's "capture control transfers" flag (setup tokens may hide MCU data in standard control packets)
- Compare DAW layer (MCU mode) capture with PM layer capture — the DAW layer definitely receives LED commands over USB
- Sniff SSL 360°'s IPC to the Channel Strip VST3 plugin (not USB but IPC pipes) for a plugin-state → surface bridge
