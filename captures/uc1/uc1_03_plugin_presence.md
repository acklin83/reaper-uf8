# uc1_03_plugin_presence

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 28

## Session state

One track in REAPER. UC1 targeted at that track. Start state: **no SSL plugins** on the track — both UC1 sections (Bus Comp center + Channel Strip sections) inert / dark.

## Action

30 s capture. Approximate timeline (user-reported):

| ~T  | Action |
|-----|--------|
| 0 s  | Capture started. Track empty, UC1 idle / inert. |
| ~3 s | SSL Native Bus Compressor 2 loaded on the track. Expected: UC1 center section wakes up. |
| ~8 s | SSL Native Channel Strip 2 loaded on the track. Expected: UC1 EQ + Gate/Comp sections wake up. |
| ~15 s | Bus Comp 2 removed from the track. Expected: UC1 center section goes inert. |
| ~22 s | Channel Strip 2 removed from the track. Expected: UC1 EQ + Gate/Comp sections go inert. |
| 30 s | Capture ends. |

"Approximate" = to the nearest 1–2 s. Exact frame counts per transition are what the diff against `uc1_02_idle_baseline.pcapng` will surface.

## Summary

- 34298 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_03_plugin_presence.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

The four transitions should each show as a distinct burst in the novel-payload output, roughly clustered around the timestamps above. Goal: identify the frames SSL 360° sends to light/dark each UC1 section.
