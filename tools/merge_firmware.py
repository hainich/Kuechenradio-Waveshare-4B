#!/usr/bin/env python3
"""Create and verify a combined ESP32-S3 flash image from ESP-IDF build."""
import hashlib
import json
import sys
from pathlib import Path

def merge(manifest, output):
    args = json.loads(manifest.read_text(encoding="utf-8"))
    entries = args["flash_files"]
    segments = sorted((int(a, 0), (manifest.parent / p).read_bytes(), p)
                      for a, p in entries.items())
    if len(segments) < 3 or segments[0][0] != 0:
        raise ValueError("Missing bootloader or partition/application entries")
    for i, (offset, data, name) in enumerate(segments):
        if offset < 0 or offset % 4096 or not data:
            raise ValueError(f"Bad flash segment: {name}")
        if i and offset < segments[i-1][0] + len(segments[i-1][1]):
            raise ValueError(f"Overlapping flash segment: {name}")
    end = max(offset + len(data) for offset, data, _ in segments)
    if end > 16 * 1024 * 1024:
        raise ValueError("Image exceeds Waveshare 16MB flash")
    if segments[0][1][0] != 0xE9:
        raise ValueError("Invalid ESP32-S3 bootloader magic")
    image = bytearray(b"\xff" * end)
    for offset, data, _ in segments:
        image[offset:offset + len(data)] = data
    output.write_bytes(image)
    written = output.read_bytes()
    for offset, data, name in segments:
        if written[offset:offset + len(data)] != data:
            raise ValueError(f"Verification failed: {name}")
        print(f"0x{offset:06X} {name} ({len(data)} bytes)")
    print(f"Merged image: {len(written)} bytes; SHA256 {hashlib.sha256(written).hexdigest()}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("Usage: merge_firmware.py build/flasher_args.json build/merged.bin")
    merge(Path(sys.argv[1]), Path(sys.argv[2]))
