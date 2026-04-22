# uc1_01_init_clean

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running (SSL360Core + SSL360Gui), version not recorded at capture time
REAPER: running, version not recorded at capture time
UF8: **physically disconnected** — confirmed via `Get-PnpDevice -PresentOnly` showing only `USB\VID_31E9&PID_0023` present before capture

## Session state

REAPER open with one track. SSL Native Bus Compressor 2 loaded on that track. UC1 targeted at that track.

## Action

1. tshark capture started on `\\.\USBPcap3`, duration 25 s
2. User physically unplugged the UC1 USB cable at the device
3. ~3 s wait
4. User re-inserted the cable
5. SSL 360° re-enumerated UC1 (observed as device address jumping from 27 → 28)
6. Capture continued running ~20 s past re-enum to cover the full init/wakeup stream before settling into idle

## Summary

- 28066 packets total on USBPcap3 for the whole 25 s window
- 27944 packets addressed to `usb.device_address == 28` (the re-enumerated UC1)
- Endpoints seen:
  - `0x00` / `0x80` (control IN/OUT, transfer_type 0x02) — enumeration + configuration
  - `0x02` / `0x81` (bulk OUT/IN, transfer_type 0x03) — vendor protocol

Traffic tails off into a steady heartbeat toward the end of the capture — the last ~5 s double as a bonus idle sample, though the dedicated baseline is `uc1_02_idle_baseline.pcapng`.

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py captures/uc1/uc1_01_init_clean.pcapng
```

Init-sequence frame extraction feeds `extension/src/init_sequence_uc1.inc` (not yet written).
