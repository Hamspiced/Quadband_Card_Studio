#!/usr/bin/env python3
"""Export verified split images; never build, connect to, or flash a device."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

PARTS = (
    ("bootloader", "bootloader/bootloader.bin", "firmware/bootloader.bin", 0, 32768),
    ("partition-table", "partition_table/partition-table.bin", "firmware/partition-table.bin", 32768, 3072),
    ("application", "quadband_cores3.bin", "firmware/quadband_cores3.bin", 65536, 1536000),
)
SEMVER = r"[0-9]+\.[0-9]+\.[0-9]+"
TAG = rf"v(?:{SEMVER}(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?|1\.0[12])"


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe(root, name):
    require(isinstance(name, str) and name and "\\" not in name, "Invalid local path")
    p = Path(name)
    require(not p.is_absolute() and all(x not in ("..", ".") for x in p.parts), "Unsafe local path")
    target = root / p
    require(not any(x.is_symlink() for x in [target, *target.parents] if x != root.parent),
            "Symlink not permitted")
    require(target.resolve().is_relative_to(root.resolve()), "Path escapes firmware root")
    return target


def partition(data):
    require(len(data) == 3072, "Partition table must be 3072 bytes")
    expected = ((1, 2, 0x9000, 0x6000, b"nvs"),
                (1, 1, 0xF000, 0x1000, b"phy_init"),
                (0, 0, 0x10000, 0x177000, b"factory"))
    for i, (kind, subtype, offset, size, label) in enumerate(expected):
        entry = struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, label, 0)
        require(data[i*32:(i+1)*32] == entry, "Unsupported partition layout")
    require(data[96:112] == b"\xeb\xeb" + b"\xff"*14, "Partition MD5 record missing")
    require(data[112:128] == hashlib.md5(data[:96]).digest(), "Partition MD5 mismatch")
    require(data[128:] == b"\xff"*(3072-128), "Unexpected partition entries")


def image(data, version=None):
    require(len(data) >= 48 and data[0] == 0xE9 and 1 <= data[1] <= 16,
            "Invalid ESP image")
    require(data[2:4] == b"\x02\x4f" and data[12:14] == b"\x09\x00",
            "Image must target ESP32-S3 DIO 80MHz 16MB")
    require(data[23] == 1, "Image SHA256 trailer required")
    pos, checksum = 24, 0xEF
    for _ in range(data[1]):
        require(pos + 8 <= len(data), "Truncated image segment")
        length = struct.unpack_from("<I", data, pos+4)[0]
        pos += 8
        require(pos+length <= len(data), "Truncated image data")
        for byte in data[pos:pos+length]:
            checksum ^= byte
        pos += length
    end = (pos // 16 + 1)*16
    require(end+32 == len(data) and data[end-1] == checksum, "Image checksum/length mismatch")
    require(data[pos:end-1] == b"\x00"*(end-1-pos), "Unexpected image padding")
    require(hashlib.sha256(data[:end]).digest() == data[end:], "Image digest mismatch")
    if version is not None:
        require(data[32:36] == b"\x32\x54\xcd\xab", "Application descriptor missing")
        require(data[48:80].split(b"\0")[0].decode("ascii") == version,
                "Application descriptor firmware version mismatch")
        require(data[80:112].split(b"\0")[0] == b"quadband_cores3", "Wrong application")


def export(output, release_tag, commit, application_version, firmware_root, allow_dirty=False):
    root = Path(firmware_root).resolve()
    require(re.fullmatch(TAG, release_tag), "Invalid release tag")
    require(re.fullmatch(SEMVER, application_version), "Invalid application version")
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "Invalid source commit")
    tag_version = {"v1.01": "1.0.1", "v1.02": "1.0.2"}.get(
        release_tag, release_tag[1:].split("-")[0])
    require(tag_version == application_version, "Release tag/application version mismatch")
    actual = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    require(actual == commit, "Requested commit differs from checked-out source")
    status = subprocess.check_output(["git", "-C", str(root), "status", "--porcelain", "--untracked-files=normal"], text=True)
    provenance = json.loads((root/".build/artifacts.json").read_text())
    lock = json.loads((root/"dependencies.lock").read_text())
    require(provenance["idfRevision"] == lock["idfRevision"]
            and provenance["dependencies"] == lock["dependencies"], "Build dependency lock mismatch")
    dirty = bool(allow_dirty or status.strip() or provenance["idfStatus"].strip())
    require(allow_dirty or not dirty, "Dirty source/SDK: release export refused; local use requires --allow-dirty")
    source_files = {str(p.relative_to(root)) for p in root.glob("main/**/*") if p.is_file()}
    source_files |= {"CMakeLists.txt", "sdkconfig.defaults", "dependencies.lock", "build.py"}
    require(set(provenance["source"]) == source_files, "Incomplete source provenance")
    for name, sha in provenance["source"].items():
        require(digest(safe(root, name).read_bytes()) == sha, f"Source changed since build: {name}")
    build = root/".build/idf"
    required = {p[1] for p in PARTS} | {"flasher_args.json", "flash_args", "quadband_cores3.elf"}
    require(required <= set(provenance["artifacts"]), "Incomplete artifact provenance")
    for name, metadata in provenance["artifacts"].items():
        data = safe(build, name).read_bytes()
        require(len(data) == metadata["bytes"] and digest(data) == metadata["sha256"],
                f"Artifact changed since build: {name}")
    args = json.loads((build/"flasher_args.json").read_text())
    require(args["flash_settings"] == {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
            "Unexpected flash settings")
    require(args["write_flash_args"] == ["--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "80m"],
            "Unexpected flash arguments")
    require(args["flash_files"] == {hex(p[3]): p[1] for p in PARTS}, "Unexpected flash files")
    require(args["extra_esptool_args"]["chip"] == "esp32s3"
            and args["extra_esptool_args"]["stub"] is True, "Unexpected chip/stub")
    for key, index in (("bootloader", 0), ("partition-table", 1), ("app", 2)):
        p = PARTS[index]
        require(args[key] == {"offset": hex(p[3]), "file": p[1], "encrypted": "false"},
                "Unexpected encrypted image/offset")
    version = json.loads((root/"firmware-manifest.json").read_text())["version"]
    require(re.fullmatch(SEMVER, version), "Invalid firmware version")
    catalog = dict(schemaVersion=1, board="M5Stack CoreS3", chipFamily="ESP32-S3",
                   flashSize=16777216, flashMode="dio", flashFrequency="80m",
                   applicationVersion=application_version, firmwareVersion=version,
                   releaseTag=release_tag, sourceCommit=commit, sourceDirty=dirty, parts=[])
    payload = {}
    previous = 0
    for role, name, path, offset, maximum in PARTS:
        data = safe(build, name).read_bytes()
        end = (offset+len(data)+4095)//4096*4096
        require(0 < len(data) <= maximum and offset >= previous and end <= 16777216,
                "Image size/sector collision")
        require(end <= 0x9000 or offset >= 0x10000, "Protected NVS/PHY sector overlap")
        previous = end
        if role == "partition-table":
            partition(data)
        else:
            image(data, version if role == "application" else None)
        catalog["parts"].append(dict(role=role, path=path, offset=offset, size=len(data), sha256=digest(data)))
        payload[path] = data
    payload["catalog.json"] = (json.dumps(catalog, indent=2)+"\n").encode()
    payload["BUILD-INFO.json"] = (json.dumps(dict(provenance, sourceCommit=commit, sourceDirty=dirty,
                                                releaseTag=release_tag), indent=2)+"\n").encode()
    payload["SHA256SUMS.txt"] = "".join(f"{digest(data)}  {name}\n" for name, data in sorted(payload.items())).encode()
    output = Path(output)
    require(not output.exists() or (output.is_dir() and not any(output.iterdir())), "Output must be absent or empty")
    for name, data in payload.items():
        target = output/name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--release-tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--application-version", required=True)
    parser.add_argument("--firmware-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--allow-dirty", action="store_true")
    args = parser.parse_args()
    try:
        export(**vars(args))
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Export refused: {exc}\n")


if __name__ == "__main__":
    main()
