#!/usr/bin/env python3
"""Decode UF8 top-soft-key LED cells + palette from cap41.

Filters OUT frames carrying FF 38 04 / FF 39 04 (LED colour-pair). Groups
by cell, tracks every distinct (a, b) byte-pair seen at each cell, and
prints a chronological table — first appearance of each (cell, colour)
combo — so the palette sequence can be eyeballed against the user's
click sequence.
"""

import sys
import subprocess
from collections import defaultdict, OrderedDict

SSL_VID = 0x31e9
UF8_PID = 0x0021

# Already-decoded cells from prior captures — anything in here is NOT a
# top-soft-key candidate. Listed so the decoder can clearly flag novel
# cells.
KNOWN_CELLS = {
    0x24: "selRenderTrigger",
    0x39: "Layer1", 0x3A: "Layer2",
    0x37: "Send1", 0x36: "Send2", 0x35: "Send3", 0x34: "Send4",
    0x33: "Send5", 0x32: "Send6", 0x31: "Send7", 0x30: "Send8",
    0x2F: "Plugin",
    0x2B: "Flip",
    0x26: "Read", 0x25: "Write", 0x23: "Latch", 0x22: "Touch",
    0x5F: "VPotBank",
    0x5E: "Soft1", 0x5D: "Soft2", 0x5C: "Soft3", 0x5B: "Soft4", 0x5A: "Soft5",
    0x59: "PAN", 0x58: "Fine", 0x57: "NormClear",
    0x56: "RecALL", 0x55: "AutoZero",
    0x54: "Nav", 0x53: "Nudge", 0x52: "Focus",
    0x4F: "BankLeft", 0x4E: "BankRight",
    0x4D: "ZoomUp", 0x4C: "ZoomLeft", 0x4B: "ZoomCentre",
    0x4A: "ZoomRight", 0x49: "ZoomDown",
}
# Per-strip SOLO/CUT/SEL = 0x00..0x17 (cell formula: 0x17 - 3*strip - led_offset)
for strip in range(8):
    base = 0x17 - 3 * strip
    KNOWN_CELLS[base]     = f"strip{strip}_SEL"
    KNOWN_CELLS[base - 1] = f"strip{strip}_CUT"
    KNOWN_CELLS[base - 2] = f"strip{strip}_SOLO"


def parse_ff_frames(payload):
    """Yield (cmd, cell, a, b) for every FF 38 04 / FF 39 04 frame in payload.
    Frame: FF <cmd> 04 <cell> 00 <a> <b> <cksum>  — 8 bytes total."""
    i = 0
    while i + 8 <= len(payload):
        if payload[i] == 0xFF and payload[i+1] in (0x38, 0x39) and payload[i+2] == 0x04:
            yield (payload[i+1], payload[i+3], payload[i+5], payload[i+6])
            i += 8
        else:
            i += 1


def main(path):
    # USBPcap captures don't always populate usb.idVendor (descriptor not
    # injected). Filter by destination address instead — UF8 = host -> bus.dev.
    # The bus.device varies per session; we discover it by finding any OUT
    # payload that starts with FF 66 (UF8 vendor magic).
    probe_cmd = [
        "tshark", "-r", path,
        "-Y", "usb.endpoint_address.direction == 0",
        "-T", "fields", "-e", "usb.dst", "-e", "usb.capdata",
    ]
    probe = subprocess.run(probe_cmd, capture_output=True, text=True, check=True)
    uf8_dst = None
    for line in probe.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or not parts[1]:
            continue
        if parts[1].lower().startswith("ff66"):
            uf8_dst = parts[0]
            break
    if not uf8_dst:
        print("ERROR: could not find UF8 device in capture (no FF 66 frames)", file=sys.stderr)
        sys.exit(1)
    print(f"# UF8 device address detected: {uf8_dst}", file=sys.stderr)

    df = f"usb.dst == \"{uf8_dst}\""
    cmd = [
        "tshark", "-r", path, "-Y", df, "-T", "fields",
        "-e", "frame.time_relative",
        "-e", "usb.endpoint_address",
        "-e", "usb.capdata",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)

    by_cell = defaultdict(list)  # cell -> [(t_rel, a, b)]
    timeline = []                # [(t_rel, cell, a, b)]

    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 3 or not parts[2]:
            continue
        try:
            t = float(parts[0])
        except ValueError:
            continue
        try:
            payload = bytes.fromhex(parts[2].replace(":", ""))
        except ValueError:
            continue
        for cmd, cell, a, b in parse_ff_frames(payload):
            by_cell[cell].append((t, cmd, a, b))
            timeline.append((t, cell, cmd, a, b))

    print("=" * 80)
    print("Per-cell summary (cells sorted ascending; novel cells flagged ★)")
    print("=" * 80)
    for cell in sorted(by_cell.keys()):
        events = by_cell[cell]
        ab38 = OrderedDict()
        ab39 = OrderedDict()
        for _t, cmd, a, b in events:
            d = ab38 if cmd == 0x38 else ab39
            d[(a, b)] = d.get((a, b), 0) + 1
        known = KNOWN_CELLS.get(cell, "")
        novel = "" if known else " ★"
        print(f"\ncell 0x{cell:02X}{novel}  {known}  events={len(events)}")
        print("  FF38 (bright/dim):")
        for (a, b), n in ab38.items():
            print(f"    a=0x{a:02X} b=0x{b:02X}    seen {n}x")
        print("  FF39 (companion):")
        for (a, b), n in ab39.items():
            print(f"    a=0x{a:02X} b=0x{b:02X}    seen {n}x")

    print()
    print("=" * 80)
    print("Chronological timeline of NOVEL cells (paired FF38+FF39 per event)")
    print("=" * 80)
    # Group by approximate timestamp + cell so the FF38/FF39 pair shows together.
    pairs = []   # list of (t, cell, ff38_ab, ff39_ab)
    pending = {}  # (cell) -> partial pair
    for t, cell, cmd, a, b in timeline:
        if cell in KNOWN_CELLS:
            continue
        key = cell
        if cmd == 0x38:
            pending[key] = {"t": t, "ff38": (a, b)}
        elif cmd == 0x39 and key in pending:
            pending[key]["ff39"] = (a, b)
            pairs.append((pending[key]["t"], cell,
                          pending[key]["ff38"], pending[key]["ff39"]))
            del pending[key]

    last_pair = None
    for t, cell, ff38, ff39 in pairs:
        on = (ff39 == (0x00, 0xF0))
        kind = "ON " if on else "off"
        line = (f"  {t:7.2f}s  cell=0x{cell:02X}  {kind} "
                f"FF38={ff38[0]:02X} {ff38[1]:02X}  "
                f"FF39={ff39[0]:02X} {ff39[1]:02X}")
        print(line)


if __name__ == "__main__":
    main(sys.argv[1])
