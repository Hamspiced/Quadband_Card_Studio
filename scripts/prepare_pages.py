"""Validate a released CoreS3 payload before staging it into a built site."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

PARTS = (
    ("bootloader", "firmware/bootloader.bin", 0, 32768),
    ("partition-table", "firmware/partition-table.bin", 32768, 3072),
    ("application", "firmware/quadband_cores3.bin", 65536, 1536000),
)
FIELDS = {
    "schemaVersion", "board", "chipFamily", "flashSize", "flashMode", "flashFrequency",
    "applicationVersion", "firmwareVersion", "releaseTag", "sourceCommit", "sourceDirty", "parts",
}


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def validate_payload(payload, tag, commit, version):
    payload = Path(payload)
    require(payload.is_dir() and not payload.is_symlink(), "Payload must be a real directory.")
    paths = list(payload.rglob("*"))
    require(not any(path.is_symlink() for path in paths), "Payload symlinks are not allowed.")
    expected = {part[1] for part in PARTS} | {"catalog.json", "BUILD-INFO.json", "SHA256SUMS.txt"}
    files = {path.relative_to(payload).as_posix() for path in paths if path.is_file()}
    require(files == expected, "Payload contains missing or unexpected files.")
    require(all((payload/name).stat().st_size <= 16_777_216 for name in files), "Payload file too large.")
    catalog = json.loads((payload/"catalog.json").read_text(encoding="utf-8"))
    require(isinstance(catalog, dict) and set(catalog) == FIELDS, "Invalid catalog fields.")
    require(type(catalog["schemaVersion"]) is int and catalog["schemaVersion"] == 1, "Unsupported schema.")
    for key, value in {"board": "M5Stack CoreS3", "chipFamily": "ESP32-S3", "flashSize": 16_777_216,
                       "flashMode": "dio", "flashFrequency": "80m"}.items():
        require(type(catalog[key]) is type(value) and catalog[key] == value, f"Invalid {key}.")
    require(catalog["sourceDirty"] is False, "Never publish an uncommitted development payload.")
    require(re.fullmatch(r"[0-9a-f]{40}", commit) is not None, "Invalid source commit.")
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is not None, "Invalid application version.")
    require(re.fullmatch(r"v(?:[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?|1\.0[12])",
                         tag) is not None, "Invalid release tag.")
    require(catalog["releaseTag"] == tag and catalog["sourceCommit"] == commit and
            catalog["applicationVersion"] == version, "Release identity mismatch.")
    require(isinstance(catalog["firmwareVersion"], str) and
            re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", catalog["firmwareVersion"]) is not None,
            "Invalid firmware version.")
    parts = catalog["parts"]
    require(isinstance(parts, list) and len(parts) == len(PARTS), "Exactly three image parts are required.")
    end = 0
    for part, (role, name, offset, maximum) in zip(parts, PARTS):
        require(isinstance(part, dict) and set(part) == {"role", "path", "offset", "size", "sha256"},
                "Invalid image part fields.")
        require(part["role"] == role and part["path"] == name and type(part["offset"]) is int and
                part["offset"] == offset, "Unexpected image role, path or offset.")
        size = part["size"]
        require(type(size) is int and 0 < size <= maximum, "Invalid image size.")
        require(role != "partition-table" or size == 3072, "Invalid partition table size.")
        require(isinstance(part["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", part["sha256"]) is not None,
                "Invalid image digest.")
        rounded_end = (offset + size + 4095) // 4096 * 4096
        require(offset >= end and (rounded_end <= 0x9000 or offset >= 0x10000),
                "Image erasure overlaps another image or protected NVS/PHY sectors.")
        data = (payload/name).read_bytes()
        require(len(data) == size and hashlib.sha256(data).hexdigest() == part["sha256"],
                f"Image hash or length mismatch: {name}")
        end = rounded_end
    require(isinstance(json.loads((payload/"BUILD-INFO.json").read_text(encoding="utf-8")), dict),
            "Invalid build provenance.")
    checked = set()
    for line in (payload/"SHA256SUMS.txt").read_text(encoding="utf-8").splitlines():
        digest, separator, name = line.partition("  ")
        require(separator and name in expected - {"SHA256SUMS.txt"} and name not in checked and
                re.fullmatch(r"[0-9a-f]{64}", digest) is not None, "Invalid checksum manifest.")
        require(hashlib.sha256((payload/name).read_bytes()).hexdigest() == digest, f"Checksum mismatch: {name}")
        checked.add(name)
    require(checked == expected - {"SHA256SUMS.txt"}, "Incomplete checksum manifest.")
    return catalog, expected


def stage_payload(payload, site, tag, commit, version):
    catalog, files = validate_payload(payload, tag, commit, version)
    payload, site = Path(payload).resolve(), Path(site).resolve()
    require(site.is_dir() and (site/"index.html").is_file(), "Build the website before staging firmware.")
    require(payload != site and not payload.is_relative_to(site) and not site.is_relative_to(payload),
            "Site and payload must be separate directories.")
    for name in files:
        target = site/name
        require(not target.is_symlink(), "Site destination must not be a symlink.")
        require(not any(path.is_symlink() for path in target.parents if path != site.parent),
                "Site parent directories must not be symlinks.")
    for name in sorted(files):
        target = site/name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(payload/name, target)
    return catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", required=True, type=Path)
    parser.add_argument("--site", required=True, type=Path)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    catalog = stage_payload(args.payload, args.site, args.tag, args.commit, args.version)
    print(f"Staged verified {catalog['releaseTag']} / CoreS3 {catalog['firmwareVersion']} at {args.site}")


if __name__ == "__main__":
    main()
