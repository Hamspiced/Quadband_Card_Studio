#!/usr/bin/env python3
"""Test actual local runtime/coordinator against a fake portable DevicePort."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--main", type=Path, default=root / "main",
                    help="Production main directory to compile (default: sibling main).")
args = parser.parse_args()
main = args.main.resolve()
with tempfile.TemporaryDirectory(prefix="quadband-runtime-") as directory:
    exe = Path(directory) / "runtime_local_test"
    sources = [main / "bridge.cpp", main / "local_runtime.cpp"]
    sources += [main / "standalone" / name for name in
                ("controller.cpp", "hf.cpp", "ndef.cpp", "uhf.cpp", "lf.cpp")]
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
        "-pthread", "-I" + str(root / "tests" / "host_stubs"), "-I" + str(main),
        *map(str, sources), str(root / "tests" / "runtime_local_test.cpp"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
