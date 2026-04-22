# uc1_08_buttons_all

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 28

## Session state

Track with SSL Native Bus Compressor 2 **and** SSL Native Channel Strip 2 loaded. UC1 mirroring both. (Both plugins had been removed in `uc1_03` and were reloaded before this capture.)

## Action

20 s capture. User pressed each UC1 button once in this order, ~1 s between presses:

| # | Button | Section |
|---|--------|---------|
| 1 | Bell HF | Channel Strip — EQ |
| 2 | Type (E) | Channel Strip — EQ character |
| 3 | EQ IN | Channel Strip — EQ enable |
| 4 | Bell LF | Channel Strip — EQ |
| 5 | Fast Attack | Channel Strip — Comp |
| 6 | Peak | Channel Strip — Comp |
| 7 | Dyn In | Channel Strip — dynamics enable |
| 8 | Expand | Channel Strip — Gate |
| 9 | Fast Attack (Gate) | Channel Strip — Gate |
| 10 | Polarity | Channel Strip — Input |
| 11 | S/C Listen | Bus Comp — Sidechain |
| 12 | Solo Clear | Global |
| 13 | Bus Comp IN | Bus Comp — enable |

Most UC1 controls are pots, not buttons — this list is the complete button set on the device.

## Summary

- 22758 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_08_buttons_all.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Expected: 13 novel-payload bursts at roughly 1 s intervals (plus one LED-feedback frame per press coming back from SSL 360° → UC1 for buttons that have a state-LED). Mapping burst N to row N of the table above gives the button-ID table for UC1.
