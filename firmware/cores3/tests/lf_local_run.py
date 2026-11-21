#!/usr/bin/env python3
"""Sanitized portable LF tests and differential desktop encoder fixtures."""
import os
import importlib
from pathlib import Path
import random
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
repo = root.parents[1]
sys.path.insert(0, str(repo / "desktop" / "src"))
LFCard = importlib.import_module("quadband.chameleon").LFCard
blocks = importlib.import_module("quadband.t5577").blocks

with tempfile.TemporaryDirectory(prefix="quadband-lf-") as directory:
    exe = Path(directory) / "lf_local"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
        "-I" + str(root / "main"), str(root / "main" / "bridge.cpp"),
        str(root / "main" / "standalone" / "lf.cpp"), str(root / "tests" / "lf_local.cpp"),
        "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
    rng = random.Random(3016)
    formats = [(0, "EM410x", 5), (1, "EM410x Electra", 13), (3, "Viking", 4),
               (4, "ioProx", 16), (5, "PAC / Stanley", 8), (6, "Jablotron", 5)]
    existing = {0: bytes.fromhex("0102030405"), 1: bytes(range(13)),
                3: bytes([1, 2, 3, 4]), 4: bytes(range(16)),
                5: b"12345678", 6: bytes(range(5))}
    count = 0
    for value, name, length in formats:
        for case in range(36):
            if name == "PAC / Stanley":
                data = bytes([32 if case == 0 else 126] * length) if case < 2 else bytes(rng.randrange(32, 127) for _ in range(length))
                text = data.decode("ascii")
            else:
                data = bytes([0 if case == 0 else 255] * length) if case < 2 else bytes(rng.randrange(256) for _ in range(length))
                text = data.hex()
            if case == 35:
                data = existing[value]
                text = data.decode("ascii") if name == "PAC / Stanley" else data.hex()
            output = subprocess.check_output([str(exe), "encode", str(value), text], text=True)
            actual = [tuple(map(int, line.split(":"))) for line in output.splitlines()]
            assert actual == blocks(LFCard(name, data)), (name, data.hex(), actual)
            assert actual[-1][0] == 0 and not actual[-1][1] & 0x10
            assert all(0 <= block <= 6 for block, _ in actual)
            count += 1
    print(f"Matched {count} desktop T5577 goldens across all six writable formats.")
