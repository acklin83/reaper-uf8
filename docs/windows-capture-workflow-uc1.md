# USB Capture on Windows (USBPcap) — UC1

> **UF8 abstecken bevor irgendwas passiert.** See `protocol-notes-uc1.md` → *Capture constraints* (cap17 GR-routing problem): when UF8 and UC1 are both attached, SSL 360° steers GR / Bus Comp traffic to whichever controller owns the dedicated GR display, and UC1 wins that contest. To see the complete UC1 frame family the UC1 has to be the only SSL device on the bus.

macOS 15 on Apple Silicon doesn't expose XHC USB interfaces to tshark/Wireshark anymore. Windows with USBPcap is the standard reverse-engineering path. Analysis still happens on macOS — the .pcapng file transfers cleanly.

This file is the UC1 fork of `windows-capture-workflow.md`. Setup is identical; only the device and the capture list differ.

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
One per root hub. UC1 will be on whichever hub its USB port belongs to.

### 2. Install SSL 360° (Windows build)
Download: https://www.solidstatelogic.com/support-page/uf8-downloads → SSL 360° for Windows.
Sign in, authorize UC1 for this machine (SSL 360° ties hardware to a machine ID; you may need to deauthorize on the Mac first or use multi-machine license).

### 3. Install a DAW + SSL plugins (so we can trigger UC1 frames)
Options, easiest first:
- **REAPER** (64-bit Windows, free trial, cross-platform .rpp files from Mac work directly)
- Any DAW that hosts VST3 will do — UC1 drives SSL Native Bus Compressor 2 (and Channel Strip 2 when in Channel Strip mode)

Put **one SSL Native Bus Compressor 2** on a track so SSL 360° populates UC1's knob mirror. For the Channel Strip captures add an **SSL Native Channel Strip 2** to the focused track.

### 4. Connect the UC1 (and ONLY the UC1)
Move the USB cable from the Mac to the Windows box. **Physically unplug the UF8 first** — not just powered off, fully disconnected. Confirm in Wireshark that one of the USBPcap interfaces shows traffic when you wiggle a UC1 knob. Cross-check device list in Windows Device Manager: only `Solid State Logic UC1` should be present, no `UF8`.

## Capture Procedure

### Find the right USBPcap interface
In Wireshark, click each `USBPcap*` interface — the one that shows burst traffic when you press a UC1 button is the one we want. Remember its number.

### Capture every session as follows
1. Start Wireshark on the chosen USBPcap interface
2. Perform the action described in `captures/uc1/README.md` for this capture
3. Stop Wireshark, save as `captures/uc1/<name>.pcapng` with the exact filename listed
4. Write a sibling `<name>.md` next to the pcapng describing host state (REAPER version, SSL 360° version, plugin version, what exactly was done and at which second)

The planned capture list — and the REAPER-side action each one expects — lives in `captures/uc1/README.md`. That file is the source of truth; follow it in order.

### For each capture — write a sibling `.md`

Example `captures/uc1/uc1_04_knob_threshold_sweep.md`:
```markdown
# uc1_04_knob_threshold_sweep

Date: 2026-04-23
Windows host: [hostname]
Wireshark: 4.x.x / USBPcap: 1.x
SSL 360°: [version]
REAPER: [version]
SSL Native Bus Comp 2: [version]

Session: single track, SSL Native Bus Compressor 2 loaded, UC1 locked to that track.
Action: at ~T=2s, slowly rotated Threshold knob from full CCW to full CW over ~5s, then back.
```

## Filter noise in Wireshark

USBPcap captures **everything** on the root hub (keyboards, mice, storage). Display filter:
```
usb.idVendor == 0x31e9 and usb.idProduct == 0x0023
```
This pins the view to the UC1 only.

## Transfer to Mac
- Syncthing (already configured between Frank's machines per Memory)
- Or: SMB share / SCP / USB stick
- Drop in `~/Documents/dev/reaper-uf8/captures/uc1/`

## Analyze on Mac
```bash
cd ~/Documents/dev/reaper-uf8
source .venv/bin/activate
python3 analysis/parse_usbpcap_uc1.py captures/uc1/uc1_04_knob_threshold_sweep.pcapng --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```
The baseline filter removes heartbeats/housekeeping, leaving only packets novel to the threshold-sweep event. Those are our knob-event candidates.

## Tips

- USBPcap on newer Windows 11 may need **Test-Signing Mode** disabled if driver signature fails → follow USBPcap docs, but usually the bundled version in Wireshark installer "just works"
- If Wireshark doesn't show USBPcap interfaces after install: open services.msc, look for "USBPcap" — ensure it's running
- Larger captures (>100MB) = you captured too much noise. Tighten the window, unplug unnecessary USB devices during capture
- If UF8 ever lights up during a UC1 capture session: stop, unplug it, start over. The routing behavior documented in cap17 silently splits the frame family and wastes the capture
