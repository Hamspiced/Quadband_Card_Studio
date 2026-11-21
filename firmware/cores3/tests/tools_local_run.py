#!/usr/bin/env python3
"""Run sanitized portable tool UI tests, optionally exporting native-font PNG renders."""
import argparse
import binascii
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import zlib


def png_from_ppm(source):
    header, size, maximum, pixels = source.read_bytes().split(b"\n", 3)
    assert header == b"P6" and size == b"320 240" and maximum == b"255"
    assert len(pixels) == 320 * 240 * 3

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data))

    rows = b"".join(b"\0" + pixels[y * 960:(y + 1) * 960] for y in range(240))
    source.with_suffix(".png").write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 240, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--render-dir", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="tools-local-") as temporary:
        binary = Path(temporary) / "tools_local"
        subprocess.run(
            shlex.split(os.environ.get("CXX", "c++"))
            + [
                "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                str(root / "tests/tools_local.cpp"),
                str(root / "main/ui/tools.cpp"),
                str(root / "main/ui/input.cpp"),
                "-o", str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)] + ([str(args.render_dir.resolve())] if args.render_dir else []), check=True)
    if args.render_dir:
        for source in args.render_dir.glob("*.ppm"):
            png_from_ppm(source)


if __name__ == "__main__":
    main()
