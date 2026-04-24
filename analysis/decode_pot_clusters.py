#!/usr/bin/env python3
"""
Cluster FF 13 04 LED-cell events in a UC1 capture by time gap.

Given a pot-sweep capture where the user turned pots sequentially with
~1-2s pauses between them, this clusters the cell activations into
per-pot groups and prints each cluster's cell range + unique states.

Usage:
    python3 decode_pot_clusters.py <capture.pcapng> --address <uc1_addr> [--gap <sec>]
"""
import argparse
import subprocess
import sys
from collections import defaultdict


def extract_led_events(pcap_path, addr):
    cmd = [
        "tshark", "-r", pcap_path,
        "-Y", f"usb.device_address == {addr} and usb.endpoint_address == 0x02",
        "-T", "fields",
        "-e", "frame.time_relative",
        "-e", "usb.capdata",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        ts_str, hex_ = parts
        if not hex_.startswith("ff1304"):
            continue
        try:
            ts = float(ts_str)
        except ValueError:
            continue
        if len(hex_) < 16:
            continue
        bank = hex_[6:8]
        cell = hex_[8:10]
        role = hex_[10:12]
        state = hex_[12:14]
        yield (ts, bank, cell, role, state)


def cluster(events, gap):
    clusters = []
    current = []
    last_ts = None
    for ev in events:
        ts = ev[0]
        if last_ts is not None and ts - last_ts > gap:
            if current:
                clusters.append(current)
            current = []
        current.append(ev)
        last_ts = ts
    if current:
        clusters.append(current)
    return clusters


def summarise(cluster):
    start = cluster[0][0]
    end = cluster[-1][0]
    cells_b01 = defaultdict(set)
    cells_b02 = defaultdict(set)
    for _, bank, cell, _, state in cluster:
        if bank == "01":
            cells_b01[cell].add(state)
        elif bank == "02":
            cells_b02[cell].add(state)
    return start, end, cells_b01, cells_b02


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--address", type=int, required=True)
    ap.add_argument("--gap", type=float, default=1.0, help="seconds of silence = new pot")
    args = ap.parse_args()

    events = list(extract_led_events(args.capture, args.address))
    if not events:
        print("# no FF 13 04 events found")
        return
    clusters = cluster(events, args.gap)
    print(f"# {len(clusters)} clusters (gap > {args.gap}s)")
    for i, c in enumerate(clusters, 1):
        start, end, b01, b02 = summarise(c)
        all_cells = sorted(set(b01) | set(b02), key=lambda x: int(x, 16))
        range_str = f"0x{all_cells[0]}..0x{all_cells[-1]}" if all_cells else "-"
        print(f"\n## Cluster {i}: t={start:.2f}s..{end:.2f}s  cells={range_str}  n={len(c)}")
        if b01:
            print(f"  bank=0x01 cells: {', '.join(f'0x{c}' for c in sorted(b01, key=lambda x: int(x, 16)))}")
            states_b01 = set()
            for s in b01.values():
                states_b01 |= s
            print(f"  bank=0x01 states: {sorted(states_b01)}")
        if b02:
            print(f"  bank=0x02 cells: {', '.join(f'0x{c}' for c in sorted(b02, key=lambda x: int(x, 16)))}")
            states_b02 = set()
            for s in b02.values():
                states_b02 |= s
            print(f"  bank=0x02 states: {sorted(states_b02)}")


if __name__ == "__main__":
    main()
