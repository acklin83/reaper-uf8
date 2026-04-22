# uc1_10_track_select

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: **34** (changed from 28 seen in earlier captures — UC1 was replugged during the pause between sessions; parse with the address found in this specific file, not hard-coded)

## Session state

4 tracks in REAPER. SSL Native Bus Compressor 2 loaded on Track 1 and Track 3. Tracks 2 and 4 empty (no SSL plugins). UC1 focused on Track 1 at capture start.

## Action

20 s capture. User clicked track headers in REAPER to change focus:

| ~T   | Focused track | Expected UC1 state |
|------|---------------|--------------------|
| 0 s   | 1 (has Bus Comp 2) | Bus Comp section live |
| ~3 s  | 2 (empty)          | Bus Comp section dark / inert |
| ~7 s  | 3 (has Bus Comp 2) | Bus Comp section live, new values |
| ~11 s | 4 (empty)          | Bus Comp section dark |
| ~15 s | 1 (has Bus Comp 2) | Bus Comp section live |
| tail  | idle               | heartbeat only |

## Summary

- 22378 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_10_track_select.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Goal: isolate two classes of frames in the host → UC1 direction:
1. **Retarget** frame sent on every track-focus change (should appear 4× at the ~3 s / ~7 s / ~11 s / ~15 s marks).
2. **No-plugin** state frame(s) for Tracks 2 and 4 — compare against `uc1_03` plugin-removal bursts; same frame family expected.

Combined with `uc1_03`, this pins down the mechanism Rea-Sixty will need to mirror: `FocusedTrack` watches `SetTrackSelected`, then either emits a retarget frame if the plugin is present or the no-plugin frame if absent.
