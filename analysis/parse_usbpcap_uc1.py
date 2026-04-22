#!/usr/bin/env python3
"""
Parse a pcapng USB capture and extract UC1 traffic (VID 0x31e9 / PID 0x0023).

UC1 fork of parse_usbpcap.py. Uses tshark via subprocess (not pyshark) to avoid
pyshark's incompatibility with Python 3.12+ asyncio changes.

The UC1's bus/device address is assigned dynamically each time it enumerates.
This script filters by VID/PID first (stable across sessions) and then narrows
to whichever device_address showed up carrying SSL-VID traffic. If multiple
SSL devices are present, re-capture with UF8 unplugged — see
docs/windows-capture-workflow-uc1.md.

Usage:
    python3 parse_usbpcap_uc1.py <capture.pcapng> [--baseline <baseline.pcapng>]

Example:
    python3 parse_usbpcap_uc1.py captures/uc1/uc1_04_knob_threshold_sweep.pcapng \\
        --baseline captures/uc1/uc1_02_idle_baseline.pcapng

Without --baseline: prints every UC1 bulk/interrupt packet with timestamp,
endpoint direction, and payload hex.

With --baseline: filters out payloads that ALSO appear in the baseline
(heartbeats etc.), so you only see the interesting stuff.
"""

import argparse
import shutil
import subprocess
import sys
from collections import Counter

SSL_VID = 0x31e9
UC1_PID = 0x0023


def find_device_address(pcap_path):
    """Return the USB device address assigned to the UC1 in this capture.

    We look at packets whose SSL VID/PID descriptor fields match. On USBPcap
    those descriptor fields are only populated on the enumeration packets,
    so we take whichever address(es) we find there and pick the most common.
    If nothing matches (rare — means the capture missed the descriptor
    exchange), falls back to every device address in the file.
    """
    out = subprocess.run(
        [
            "tshark", "-r", pcap_path,
            "-Y", f"usb.idVendor == 0x{SSL_VID:04x} and usb.idProduct == 0x{UC1_PID:04x}",
            "-T", "fields", "-e", "usb.device_address",
        ],
        capture_output=True, text=True, check=True,
    )
    addrs = [a.strip() for a in out.stdout.splitlines() if a.strip()]
    if not addrs:
        return None
    # most common wins (enumeration packets often show both parent-hub addr=0
    # and the newly assigned device addr; device addr is the repeating one)
    counts = Counter(addrs)
    return counts.most_common(1)[0][0]


def extract_payloads(pcap_path, device_address):
    """Yield (timestamp, endpoint_num, direction, payload_bytes) for each
    bulk/interrupt packet to/from the given device_address."""
    # transfer_type: 0=iso, 1=interrupt, 2=control, 3=bulk — we want bulk.
    # Include usb.data_len so we can filter zero-length URBs cheaply later.
    cmd = [
        "tshark", "-r", pcap_path,
        "-Y", f"usb.device_address == {device_address} and usb.transfer_type == 0x03",
        "-T", "fields",
        "-e", "frame.time_relative",
        "-e", "usb.endpoint_address",
        "-e", "usb.capdata",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        ts_str, ep_str, payload_hex = parts[0], parts[1], parts[2]
        if not ts_str or not ep_str:
            continue
        try:
            ts = float(ts_str)
            ep = int(ep_str, 16) if ep_str.startswith("0x") else int(ep_str, 0)
        except ValueError:
            continue
        direction = "IN" if (ep & 0x80) else "OUT"
        ep_num = ep & 0x0F
        payload = bytes.fromhex(payload_hex.replace(":", "")) if payload_hex else b""
        yield (ts, ep_num, direction, payload)


def fingerprint(payload):
    return payload.hex()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="pcapng file to analyze")
    ap.add_argument("--baseline", help="baseline pcapng — filter out packets seen here too")
    ap.add_argument("--min-len", type=int, default=1, help="ignore payloads shorter than this")
    ap.add_argument("--address", help="override auto-detected UC1 device_address")
    args = ap.parse_args()

    if shutil.which("tshark") is None:
        sys.stderr.write("tshark not on PATH — install Wireshark and re-run.\n")
        sys.exit(1)

    addr = args.address or find_device_address(args.capture)
    if not addr:
        sys.stderr.write(f"# no UC1 VID/PID descriptors found in {args.capture}; pass --address explicitly\n")
        sys.exit(2)
    print(f"# UC1 device_address = {addr} (capture: {args.capture})", file=sys.stderr)

    baseline_fps = set()
    if args.baseline:
        b_addr = args.address or find_device_address(args.baseline) or addr
        print(f"# baseline device_address = {b_addr} ({args.baseline})", file=sys.stderr)
        for _, _, _, payload in extract_payloads(args.baseline, b_addr):
            baseline_fps.add(fingerprint(payload))
        print(f"# baseline: {len(baseline_fps)} unique payloads", file=sys.stderr)

    counts = Counter()
    t0 = None
    for ts, ep, direction, payload in extract_payloads(args.capture, addr):
        if len(payload) < args.min_len:
            continue
        fp = fingerprint(payload)
        if fp in baseline_fps:
            counts[("baseline", direction, ep)] += 1
            continue
        if t0 is None:
            t0 = ts
        rel = ts - t0
        print(f"{rel:8.4f}s  EP{ep} {direction:3s}  len={len(payload):3d}  {payload.hex()}")
        counts[("novel", direction, ep)] += 1

    print("\n# summary", file=sys.stderr)
    for (kind, direction, ep), n in sorted(counts.items()):
        print(f"#   {kind:8s} EP{ep} {direction:3s}: {n}", file=sys.stderr)


if __name__ == "__main__":
    main()
