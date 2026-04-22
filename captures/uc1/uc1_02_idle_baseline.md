# uc1_02_idle_baseline

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running (SSL360Core + SSL360Gui), version not recorded at capture time
REAPER: running, version not recorded at capture time
UF8: physically disconnected (same constraint as `uc1_01`)

## Session state

Same session as `uc1_01`, captured immediately after. REAPER with one track, SSL Native Bus Compressor 2 on that track, UC1 targeted at that track. Device address: **28** (post-replug from `uc1_01`).

## Action

Nothing. No UI clicks, no knob touches, no REAPER interaction. Clean 10 s window.

## Summary

- 11362 packets total on USBPcap3
- 11288 packets addressed to `usb.device_address == 28`
- Rate: ~1130 pkt/s sustained during idle
- Same endpoint set as `uc1_01`: control `0x00`/`0x80`, bulk `0x02`/`0x81`

## Use as baseline

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_NN_<something>.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Every later capture (03–14) should be diffed against this file to strip the heartbeat noise and leave only payloads novel to that capture's event.
