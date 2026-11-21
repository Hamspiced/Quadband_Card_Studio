#!/usr/bin/env python3
"""Fetch locked dependencies and build without accessing a device."""
import json
import hashlib
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent
lock = json.loads((root / "dependencies.lock").read_text())
if not os.environ.get("IDF_PATH"):
    raise SystemExit("Source ESP-IDF 5.5.3 export.sh before running this helper.")
idf = Path(os.environ["IDF_PATH"])
idf_revision = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
if idf_revision != lock["idfRevision"]:
    raise SystemExit(f"Expected ESP-IDF {lock['idf']} revision {lock['idfRevision']}, got {idf_revision}")
for name, entry in lock["dependencies"].items():
    destination = root / ".build" / "deps" / name
    if not destination.exists():
        subprocess.run(["git", "clone", "--no-checkout", entry["url"], str(destination)], check=True)
    subprocess.run(["git", "-C", str(destination), "fetch", "--depth", "1", "origin", entry["revision"]], check=True)
    subprocess.run(["git", "-C", str(destination), "checkout", "--detach", entry["revision"]], check=True)
env = dict(os.environ, IDF_COMPONENT_MANAGER="0", IDF_TARGET="esp32s3")
config = root / ".build" / "sdkconfig"
config.parent.mkdir(parents=True, exist_ok=True)
config.write_bytes((root / "sdkconfig.defaults").read_bytes())
subprocess.run(["idf.py", "-C", str(root), "-B", str(root / ".build" / "idf"),
                "-D", "SDKCONFIG=" + str(config),
                "reconfigure"], env=env, check=True)
subprocess.run(["ninja", "-C", str(root / ".build" / "idf"), "-j", "4"], env=env, check=True)
build = root / ".build" / "idf"
files = ["quadband_cores3.bin", "quadband_cores3.elf", "bootloader/bootloader.bin",
         "partition_table/partition-table.bin", "flash_args", "flasher_args.json"]
artifacts = {name: {"bytes": (build / name).stat().st_size,
                    "sha256": hashlib.sha256((build / name).read_bytes()).hexdigest()} for name in files}
source_files = sorted([*root.glob("main/**/*"), root / "CMakeLists.txt", root / "sdkconfig.defaults",
                       root / "dependencies.lock", root / "build.py"])
source = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files if p.is_file()}
provenance = {"idfRevision": idf_revision,
              "idfStatus": subprocess.check_output(["git", "-C", str(idf), "status", "--short"], text=True),
              "dependencies": lock["dependencies"], "source": source, "artifacts": artifacts,
              "hardwareValidated": False}
(root / ".build" / "artifacts.json").write_text(json.dumps(provenance, indent=2) + "\n")
