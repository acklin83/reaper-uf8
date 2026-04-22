# uc1_04_knob_threshold_sweep

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 28

## Session state

One track in REAPER. SSL Native Bus Compressor 2 loaded on that track. UC1 targeted at that track. Channel Strip 2 status: as left after `uc1_03` — not material to this capture; the Threshold knob sits in the Bus Comp center section.

## Action

15 s capture. User rotated the Threshold knob:
- full CCW → full CW over ~5 s
- brief hold at CW
- full CW → full CCW over ~5 s

No other input during the window.

## Summary

- 17702 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_04_knob_threshold_sweep.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Expected: a long stream of novel payloads in the IN direction (UC1 → host), dense during the two sweep phases and sparse around the hold. Decode targets:
- Knob ID byte for Threshold
- Value encoding (absolute vs delta)
- Frame rate during continuous rotation
