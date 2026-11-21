#!/usr/bin/env python3
"""Build and run portable local UHF tests in a temporary directory."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="uhf-local-") as temporary:
        binary = Path(temporary) / "uhf_local_test"
        subprocess.run(
            compiler
            + [
                "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                str(root / "tests/uhf_local_test.cpp"),
                str(root / "main/standalone/uhf.cpp"),
                str(root / "main/bridge.cpp"),
                "-o", str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
