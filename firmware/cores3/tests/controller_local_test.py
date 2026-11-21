"""Compile portable coordinator plus actual services/bridge with host sanitizers."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="controller-local-host-") as temporary:
    executable = str(Path(temporary) / "controller-local-test")
    sources = ["tests/controller_local_test.cpp", "main/standalone/controller.cpp",
               "main/standalone/hf.cpp", "main/standalone/ndef.cpp",
               "main/standalone/uhf.cpp", "main/standalone/lf.cpp", "main/bridge.cpp"]
    subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
        "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-g", *[str(root / source) for source in sources],
        "-o", executable,
    ], check=True)
    subprocess.run([executable], check=True)
