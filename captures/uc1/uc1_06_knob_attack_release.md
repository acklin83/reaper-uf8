# uc1_06_knob_attack_release

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 28

## Session state

Same session as `uc1_04`/`uc1_05`. SSL Native Bus Compressor 2 on the focused track, UC1 mirroring.

## Action

20 s capture:
- Attack knob full CCW → full CW (~5 s)
- ~2 s pause
- Release knob full CCW → full CW (~5 s)
- ~3 s pause / tail

## Summary

- 22540 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_06_knob_attack_release.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Two dense novel-payload windows separated by an idle gap. Compare event-frame IDs against `uc1_04` (Threshold) to extract Attack + Release knob IDs — same event family expected.
