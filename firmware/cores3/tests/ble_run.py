#!/usr/bin/env python3
"""Exercise the production NUS client with a deterministic radio/scheduler mock."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / ".build" / "ble-host"
build.mkdir(parents=True, exist_ok=True)
command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
           "-I" + str(root / "tests/nus_stubs"),
           str(root / "tests/nus_mock.cpp"), "-o", str(build / "nus_tests")]
subprocess.run(command, check=True)
subprocess.run([str(build / "nus_tests")], check=True)

peripheral = command[:-3] + [str(root / "tests/peripheral_mock.cpp"),
    str(root / "main/bridge.cpp"), str(root / "main/workbench.cpp"), "-o", str(build / "peripheral_tests")]
subprocess.run(peripheral, check=True)
subprocess.run([str(build / "peripheral_tests")], check=True)
