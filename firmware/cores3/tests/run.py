#!/usr/bin/env python3
"""Compile the actual portable C++ policy against the shared golden contract."""
import argparse
import json
import os
import re
import uuid
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--protocol", type=Path, default=root.parents[1] / "protocol")
args = parser.parse_args()
manifest = json.loads((args.protocol / "cores3-bridge-v1.json").read_text())
fixtures = json.loads((args.protocol / "fixtures" / "cores3-v1.json").read_text())
assert "CONFIG_SPIRAM_IGNORE_NOTFOUND=y" in (root / "sdkconfig.defaults").read_text()
assert "CONFIG_SPIRAM_MODE_QUAD=y" in (root / "sdkconfig.defaults").read_text()
assert manifest["headerSize"] == 26 and manifest["maxPayload"] == 2048
build = root / ".build" / "host"
build.mkdir(parents=True, exist_ok=True)
frames = ["qb::Bytes{" + ",".join(str(v) for v in bytes.fromhex(f["frameHex"])) + "}" for f in fixtures["frames"]]
generated = "#include <vector>\nstatic const std::vector<qb::Bytes> golden_frames{" + ",".join(frames) + "};\n"
generated += "static void authoritative_locks() {\n"
for case in fixtures.get("ntagLockCases", []):
    layout = fixtures["geometries"][case["family"]]
    initial = bytearray(16)
    initial[10:12] = bytes.fromhex(case["staticLockHex"])
    initial[12:16] = bytes([0xe1, 0x10, {45: 18, 135: 62, 231: 111}[layout["unitCount"]], 0])
    dyn = bytes.fromhex(case["dynamicLockPageHex"])
    generated += "{ uint8_t initial[16]{" + ",".join(map(str, initial)) + "};"
    generated += "uint8_t dyn[4]{" + ",".join(map(str, dyn)) + "}; uint8_t cfg[8]{0,0,0,255};\n"
    for page in range(4, layout["lastUserPage"] + 1):
        expected = "false" if page in case["lockedUserPages"] else "true"
        generated += f"assert(qb::ntag_write_allowed({layout['unitCount']},{page},initial,dyn,cfg,false)=={expected});\n"
    generated += "}\n"
generated += "}\n"
(build / "golden.hpp").write_text(generated)
ble = (root / "main" / "ble.cpp").read_text()
for name, value in manifest["uuids"].items():
    match = re.search(rf"{name}_uuid\s*=\s*BLE_UUID128_INIT\((.*?)\)", ble, re.S)
    assert match and bytes(int(v, 16) for v in re.findall(r"0x([0-9a-f]+)", match[1])) == uuid.UUID(value).bytes[::-1]
for family, sectors in fixtures["geometries"].items():
    if family.startswith("classic"):
        count = 0
        for sector in sectors:
            assert sector["trailer"] == sector["firstBlock"] + sector["blockCount"] - 1
            count += sector["blockCount"] - 1 - (sector["sector"] == 0)
        assert count == (47 if family == "classic1k" else 215)
for profile in fixtures["profiles"]:
    g = profile["geometry"]
    units = [b["number"] for b in profile["blocks"]]
    if profile["family"] == "classic4k":
        expected = [n for n in range(1, 256) if n % (4 if n < 128 else 16) != (3 if n < 128 else 15)]
    else:
        expected = list(range(4, fixtures["geometries"][profile["family"]]["lastUserPage"] + 1))
    assert units == expected and len(set(units)) == len(units)
    assert all(len(bytes.fromhex(b["hex"])) == g["unitSize"] for b in profile["blocks"])
for legacy in fixtures.get("legacyProfiles", []):
    assert legacy["format"] == "quadband.classic1k.data" and legacy["version"] == 1
    assert [b["number"] for b in legacy["blocks"]] == [n for n in range(1, 64) if n % 4 != 3]
    assert all(len(bytes.fromhex(b["hex"])) == 16 for b in legacy["blocks"])
subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I" + str(root / "main"), "-I" + str(build),
                str(root / "main" / "bridge.cpp"), str(root / "tests" / "host.cpp"),
                "-o", str(build / "host_tests")], check=True)
subprocess.run([str(build / "host_tests")], check=True)
subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                "-I" + str(root / "main"), str(root / "main" / "bridge.cpp"),
                str(root / "main" / "workbench.cpp"), str(root / "tests" / "modes.cpp"),
                "-o", str(build / "mode_tests")], check=True)
subprocess.run([str(build / "mode_tests")], check=True)
print(f"Validated {len(frames)} golden frames, {len(fixtures['profiles'])} complete profiles, "
      f"{len(fixtures.get('ntagLockCases', []))} authoritative lock cases, legacy profiles and manifest UUIDs.")
