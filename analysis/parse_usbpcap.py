#!/usr/bin/env python3
"""
Parse a pcapng USB capture and extract UF8 traffic (VID 0x31e9 / PID 0x0021).

Usage:
    python3 parse_usbpcap.py <capture.pcapng> [--baseline <baseline.pcapng>]

Without --baseline: prints every UF8 bulk/interrupt packet with timestamp,
endpoint direction, and payload hex.

With --baseline: filters out payloads that ALSO appear in the baseline
(heartbeats etc.), so you only see the interesting stuff.
"""

import argparse
import sys
from collections import Counter

try:
    import pyshark
except ImportError:
    sys.stderr.write(
        "pyshark not installed. Run: pip install pyshark\n"
        "Also requires tshark on PATH (installed with Wireshark).\n"
    )
    sys.exit(1)


SSL_VID = 0x31e9
UF8_PID = 0x0021


def extract_uf8_payloads(pcap_path, display_filter_extra=""):
    """Yield (timestamp, endpoint, direction, payload_bytes) for each UF8 packet."""
    # USBPcap/macOS USB dissector exposes fields usb.idVendor, usb.idProduct
    # usb.transfer_type: 0=isochronous, 1=interrupt, 2=control, 3=bulk
    # usb.endpoint_address.direction: 0=OUT, 1=IN
    df = f"usb.idVendor == 0x{SSL_VID:04x} and usb.idProduct == 0x{UF8_PID:04x}"
    if display_filter_extra:
        df = f"({df}) and ({display_filter_extra})"

    cap = pyshark.FileCapture(pcap_path, display_filter=df, keep_packets=False)
    try:
        for pkt in cap:
            try:
                usb = pkt.usb
                ts = float(pkt.sniff_timestamp)
                ep = int(getattr(usb, "endpoint_address", "0x00"), 0)
                direction = "IN" if (ep & 0x80) else "OUT"
                ep_num = ep & 0x0F
                payload_hex = getattr(usb, "capdata", None) or getattr(pkt, "data", None)
                payload = bytes.fromhex(str(payload_hex).replace(":", "")) if payload_hex else b""
                yield (ts, ep_num, direction, payload)
            except AttributeError:
                continue
    finally:
        cap.close()


def fingerprint(payload):
    """Short hex signature for dedup."""
    return payload.hex()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", help="pcapng file to analyze")
    ap.add_argument("--baseline", help="baseline pcapng — filter out packets seen here too")
    ap.add_argument("--min-len", type=int, default=1, help="ignore payloads shorter than this")
    args = ap.parse_args()

    baseline_fps = set()
    if args.baseline:
        print(f"# building baseline fingerprint set from {args.baseline}", file=sys.stderr)
        for _, _, _, payload in extract_uf8_payloads(args.baseline):
            baseline_fps.add(fingerprint(payload))
        print(f"# baseline: {len(baseline_fps)} unique payloads", file=sys.stderr)

    counts = Counter()
    t0 = None
    for ts, ep, direction, payload in extract_uf8_payloads(args.capture):
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
