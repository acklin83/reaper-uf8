# USB Capture Workflow (macOS Apple Silicon)

## One-time setup
1. `brew install --cask wireshark` — installs app + ChmodBPF LaunchDaemon
2. After install, add yourself to the `access_bpf` group (installer does this; reboot or `sudo dseditgroup -o edit -a $USER -t user access_bpf`)
3. Verify: `ls -la /dev/bpf0` should be `rw-rw----` with group `access_bpf`

## Identify the right XHC interface
UF8 sits on a specific USB host controller. Find it:
```bash
ioreg -p IOUSB -l -w 0 | grep -B 40 "UF-001254" | grep -i "AppleT8132\|AppleEmbedded\|IOPCI" | head -3
```
Then list capturable XHC* pseudo-interfaces:
```bash
tshark -D 2>/dev/null | grep -i XHC
```
Pick the one matching the host controller UF8 is on (usually `XHC20` for internal bus, `XHC29` for Thunderbolt, exact name varies by macOS version).

## Record a baseline
UF8 idle, nothing happening. Establishes "always-on" traffic (heartbeats, LED polls, etc.) so we can filter it out later.
```bash
tshark -i XHC20 -w captures/01-baseline-idle.pcapng -a duration:15
```
Write a matching `captures/01-baseline-idle.md` with:
- Date
- SSL 360° state (running? Plugin Mixer? empty session?)
- REAPER state (not running / open with empty project)
- What buttons/knobs you touched (ideally none)

## Record a color-change event
With SSL 360° Plugin Mixer active + REAPER session with colored tracks:
```bash
tshark -i XHC20 -w captures/02-color-change-red.pcapng -a duration:10
```
While recording:
1. Wait 2s (idle baseline inside the capture)
2. Change ONE track's color in REAPER to a known RGB (e.g. pure red `#FF0000`)
3. Wait 2s
4. Stop
5. Note in `02-color-change-red.md`: which channel on the UF8, what RGB, at roughly what timestamp inside the capture

Repeat for:
- Pure green `#00FF00`
- Pure blue `#0000FF`
- White `#FFFFFF`
- Black `#000000`
- A mid-tone (e.g. `#7F4020`) — to see if encoding is 8-bit-per-channel RGB

## Record a bank-switch
Lets us identify which packets map channels → hardware strips.
```bash
tshark -i XHC20 -w captures/03-bank-switch.pcapng -a duration:15
```
While recording: press the UF8 bank-right / bank-left buttons several times. Note sequence in `.md`.

## Parse
```bash
python3 analysis/parse_usbpcap.py captures/02-color-change-red.pcapng
```
Diffs packets against the baseline, highlights payloads that appear only after the color change.

## Notes
- USB capture on macOS records **everything** on the XHC bus. Filter by VID 0x31e9 in Wireshark display filter: `usb.idVendor == 0x31e9`
- If `XHC20` doesn't show UF8 traffic, try other XHC* interfaces — macOS labels them inconsistently across versions
- If Wireshark shows XHC interfaces but capture is empty: ChmodBPF didn't apply. Reboot after install.
