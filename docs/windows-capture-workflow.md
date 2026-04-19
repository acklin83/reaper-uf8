# USB Capture on Windows (USBPcap)

macOS 15 on Apple Silicon doesn't expose XHC USB interfaces to tshark/Wireshark anymore. Windows with USBPcap is the standard reverse-engineering path. Analysis still happens on macOS — the .pcapng file transfers cleanly.

## One-time setup (Windows machine)

### 1. Install Wireshark + USBPcap
Download: https://www.wireshark.org/download.html → **Wireshark for Windows (64-bit)**.

During the installer:
- ✅ **Install Npcap** (comes bundled) — default settings
- ✅ **Install USBPcap** (bundled component) — **this is the one we need**
- Reboot after install (USBPcap is a kernel filter driver, needs restart)

After reboot, open Wireshark. You should see interfaces like:
```
USBPcap1
USBPcap2
USBPcap3
```
One per root hub. UF8 will be on whichever hub its USB port belongs to.

### 2. Install SSL 360° (Windows build)
Download: https://www.solidstatelogic.com/support-page/uf8-downloads → SSL 360° for Windows.
Sign in, authorize UF8 for this machine (SSL 360° ties hardware to a machine ID; you may need to deauthorize on the Mac first or use multi-machine license).

### 3. Install a DAW + SSL plugin (so we can trigger color changes)
Options, easiest first:
- **REAPER** (64-bit Windows, free trial, cross-platform .rpp files from Mac work directly)
- Any DAW that hosts VST3 will do — colors come from REAPER's track-color, mirrored to UF8 via the SSL plugin

Put **one SSL Channel Strip** on one track so SSL 360° flips into Plugin Mixer mode and starts pushing colors.

### 4. Connect the UF8
Move the USB cable from the Mac to the Windows box. Confirm in Wireshark that one of the USBPcap interfaces shows traffic when you wiggle a UF8 knob.

## Capture Procedure

Same experiments as in `capture-workflow.md`, but on Windows:

### Find the right USBPcap interface
In Wireshark, click each `USBPcap*` interface — the one that shows burst traffic when you press a UF8 button is the one we want. Remember its number.

### Baseline (idle)
```
File → Capture → USBPcap2 (or whichever) → Start
```
Let it record 15 seconds with SSL 360° running but REAPER idle, no track-color changes. Save as:
```
captures/win-01-baseline-idle.pcapng
```

### Color-change captures
Open the same REAPER project used for testing. With recording running:
1. Wait 2s (embedded idle reference)
2. Change **one track's color** in REAPER to **pure red #FF0000**
3. Wait 2s
4. Stop

Save: `captures/win-02-color-red.pcapng`

Repeat for:
- `win-03-color-green.pcapng` — `#00FF00`
- `win-04-color-blue.pcapng` — `#0000FF`
- `win-05-color-white.pcapng` — `#FFFFFF`
- `win-06-color-black.pcapng` — `#000000`
- `win-07-color-midtone.pcapng` — `#7F4020` (tests 8-bit RGB vs palette)

### Bank-switch
```
captures/win-08-bank-switch.pcapng
```
Record 15s. During recording, press UF8 BANK → and BANK ← several times. Log the sequence in a sibling `.md`.

### For each capture — write a sibling `.md`

Example `captures/win-02-color-red.md`:
```markdown
# win-02-color-red

Date: 2026-04-19
Windows host: [hostname]
Wireshark: 4.x.x / USBPcap: 1.x
SSL 360°: [version]
REAPER: [version]

Session: 8 tracks, Track 3 had an SSL Channel Strip loaded.
Action: at ~T=3s, changed Track 3 color from default (grey) to #FF0000 (pure red).
```

## Filter noise in Wireshark

USBPcap captures **everything** on the root hub (keyboards, mice, storage). Display filter:
```
usb.idVendor == 0x31e9 and usb.idProduct == 0x0021
```
This pins us to the UF8 only.

## Transfer to Mac
- Syncthing (already configured between Frank's machines per Memory)
- Or: SMB share / SCP / USB stick
- Drop in `~/Documents/dev/reaper-uf8/captures/`

## Analyze on Mac
```bash
cd ~/Documents/dev/reaper-uf8
source .venv/bin/activate
python3 analysis/parse_usbpcap.py captures/win-02-color-red.pcapng --baseline captures/win-01-baseline-idle.pcapng
```
The baseline filter removes heartbeats/housekeeping, leaving only packets that are novel to the color-red event. Those are our color-command candidates.

## Tips

- USBPcap on newer Windows 11 may need **Test-Signing Mode** disabled if driver signature fails → follow USBPcap docs, but usually the bundled version in Wireshark installer "just works"
- If Wireshark doesn't show USBPcap interfaces after install: open services.msc, look for "USBPcap" — ensure it's running
- Larger captures (>100MB) = you captured too much noise. Tighten the 15s window, unplug unnecessary USB devices during capture
