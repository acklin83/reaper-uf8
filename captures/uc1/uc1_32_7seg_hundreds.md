# uc1_32_7seg_hundreds

Date: TBD
Windows host: StoerPC
tshark: latest
USBPcap interface: `\\.\USBPcap3`
UF8: physically disconnected
UC1 device address: TBD

## Why

`uc1_27` only captured the 99→100 transition, giving us the "1"-shape cell pattern (`0x00, 0x03, 0x04, 0x05`) for the hundreds digit but no data for digits 0, 2..9. Current `buildSevenSeg` in [extension/src/UC1Protocol.cpp](../../extension/src/UC1Protocol.cpp) is guessing a "0" pattern and using the "1" pattern as a placeholder for 2..9. Need a full 0..9 sweep on the hundreds position.

## Setup

- REAPER project with **at least 999 tracks** — easiest: run Action 40285 ("Track: Insert new track") in a loop, OR generate a project programmatically. Track names don't matter.
- SSL plugins NOT required — the 7-seg shows REAPER track index regardless of plugin presence
- Audio off, no pot movement

## Action

Use the UC1 CHANNEL encoder to scroll through the following positions (just transit through, no need to dwell):

1. Start at track 1 (`001`)
2. Scroll fast to position 99 (`099`)
3. Step slowly across 99 → 100 → 101 (`099` → `100` → `101`)
4. Jump to 199 → 200 → 201
5. Jump to 299 → 300 → 301
6. ...continue through 399, 499, 599, 699, 799, 899, 999
7. Scroll back fast to 1 (capture the high→low transitions on the way down)

If the encoder feels too slow for the long jumps, use REAPER's Track Manager or click directly on a track in the TCP at the desired position (UC1 will follow REAPER's selection).

Capture ~60 s total — long enough to cover all 0..9 transitions on the hundreds digit at least once each.

## Decode targets

- For each digit 0..9 in the hundreds position, the set of bank 0x01 cells `0x00..0x07` lit (state 0x01)
- Compare against ones digit (cells `0x10..0x16`) and tens digit (cells `0x08..0x0E`) — if hundreds follows the same alphabetical a..g layout, we can drop the special-case code
- If hundreds uses a different cell→segment topology (per uc1_27's "1"=4-cells observation), build a `kHundredsSegments[10][n]` lookup table

## Output

Update `kHundredsCells` (or replace with full digit table) in [extension/src/UC1Protocol.cpp](../../extension/src/UC1Protocol.cpp) and remove the placeholder behavior for 2..9.
