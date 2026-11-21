#!/usr/bin/env python3
"""Build and execute portable bounded input tests with sanitizers."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="quadband-input-") as directory:
    exe = Path(directory) / "input_local"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
        "-I" + str(root / "main"), str(root / "main" / "ui" / "input.cpp"),
        str(root / "tests" / "input_local.cpp"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
