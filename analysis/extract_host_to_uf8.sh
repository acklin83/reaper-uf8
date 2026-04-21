#!/usr/bin/env bash
# Extract host→UF8 OUT payloads from a pcapng, with the common
# heartbeat patterns removed. First arg = pcapng; second (optional) =
# baseline pcapng whose payloads are additionally filtered out.
#
# UF8 device address in these Windows captures is 3.61.

set -eu

payloads() {
    tshark -r "$1" \
        -Y 'usb.dst matches "^3\\.61\\." && usb.capdata' \
        -T fields -e frame.time_relative -e usb.capdata 2>/dev/null \
    | awk '
        # Strip common heartbeats so the delta is readable.
        #   64-byte ff 66 21 09/0a … (paired heartbeat)
        #   13-byte ff 66 09 15/16 00*8 (plugin-mixer heartbeat)
        $2 ~ /^ff662109/ { next }
        $2 ~ /^ff66210a/ { next }
        $2 ~ /^ff66091[56]/ { next }
        $2 ~ /^ff1b01/     { next }   # 5-byte 150 ms-ish background ping
        $2 == "" { next }
        { print }
    '
}

if [ $# -eq 1 ]; then
    payloads "$1"
else
    # Build baseline fingerprint set then diff.
    baseline="$2"
    capture="$1"
    payloads "$baseline" | awk '{print $2}' | sort -u > /tmp/uf8_baseline_fps.txt
    payloads "$capture" | awk -v fps=/tmp/uf8_baseline_fps.txt '
        BEGIN { while ((getline l < fps) > 0) seen[l] = 1 }
        !($2 in seen) { print }
    '
fi
