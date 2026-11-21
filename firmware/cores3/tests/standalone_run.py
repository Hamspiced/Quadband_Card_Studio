#!/usr/bin/env python3
"""Exercise real standalone engines, touch input and runtime with mock devices."""
from pathlib import Path
import subprocess
import sys

tests = Path(__file__).resolve().parent
for script in (
    "hf_local_host_test.py",
    "uhf_local_test.py",
    "lf_local_run.py",
    "input_local_run.py",
    "tools_local_run.py",
    "controller_local_test.py",
    "runtime_local_test.py",
):
    print(f"Standalone validation: {script}", flush=True)
    subprocess.run([sys.executable, str(tests/script)], check=True)
print("Standalone reader, keyboard, review and runtime regressions passed.")
