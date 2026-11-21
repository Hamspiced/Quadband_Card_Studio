"""Compile the actual portable HF services and bridge with host sanitizers."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="hf-local-host-") as temporary:
    executable = str(Path(temporary) / "hf-local-test")
    command = shlex.split(os.environ.get("CXX", "c++")) + [
        "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
        str(root / "tests/hf_local_test.cpp"),
        str(root / "main/standalone/hf.cpp"),
        str(root / "main/standalone/ndef.cpp"),
        str(root / "main/bridge.cpp"), "-o", executable,
    ]
    subprocess.run(command, check=True)
    subprocess.run([executable], check=True)
