#!/usr/bin/env python3
"""Create a local arm64 Debian package; never install, deploy or publish it."""
import argparse
import gzip
import hashlib
import importlib.util
import io
import json
from pathlib import Path, PurePosixPath
import struct
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DEVICE = Path(__file__).resolve().parent


def arm64_static(data):
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("Package requires a 64-bit little-endian Linux ELF, not a simulator binary.")
    kind, machine = struct.unpack_from("<HH", data, 16)
    entry, offset = struct.unpack_from("<QQ", data, 24)
    stride, count = struct.unpack_from("<HH", data, 54)
    if kind != 2 or machine != 183 or not entry or stride != 56 or not 0 < count <= 64:
        raise ValueError("Executable must be a static aarch64 Linux build.")
    executable = False
    for index in range(count):
        start = offset + index * stride
        if start + stride > len(data):
            raise ValueError("Truncated ELF program headers.")
        role, flags = struct.unpack_from("<II", data, start)
        file_offset, _, _, size = struct.unpack_from("<QQQQ", data, start + 8)
        if role in (2, 3):
            raise ValueError("Dynamic/interpreted ELF is not a self-contained device package.")
        if file_offset + size > len(data):
            raise ValueError("ELF segment exceeds executable.")
        executable |= role == 1 and bool(flags & 1)
    if not executable:
        raise ValueError("No executable load segment.")


def tar(files):
    stream = io.BytesIO()
    with gzip.GzipFile(fileobj=stream, mode="wb", mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            directories = {str(parent) for name in files for parent in PurePosixPath(name).parents
                           if str(parent) != "."}
            for name in sorted(directories, key=lambda value: (value.count("/"), value)):
                info = tarfile.TarInfo(name + "/")
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                info.mtime = 0
                archive.addfile(info)
            for name, (data, mode) in sorted(files.items()):
                info = tarfile.TarInfo(name)
                info.mode = mode
                info.size = len(data)
                info.mtime = 0
                archive.addfile(info, io.BytesIO(data))
    return stream.getvalue()


def deb(members):
    out = bytearray(b"!<arch>\n")
    for name, content in members:
        header = f"{name + '/':<16}{0:<12}{0:<6}{0:<6}{'100644':<8}{len(content):<10}`\n".encode()
        if len(header) != 60:
            raise ValueError("Invalid archive header.")
        out += header + content
        if len(content) % 2:
            out += b"\n"
    return bytes(out)


def package(build, output, zig_root=None):
    build = Path(build).resolve()
    provenance = json.loads((build / "BUILD-INFO.json").read_text())
    binary = (build / "quadband-cardputerzero").read_bytes()
    arm64_static(binary)
    if provenance["target"] not in ("linux-arm64", "linux"):
        raise ValueError("Host preview cannot be packaged as a device application.")
    if hashlib.sha256(binary).hexdigest() != provenance["binary"]["sha256"] or len(binary) != provenance["binary"]["bytes"]:
        raise ValueError("Binary differs from build provenance.")
    expected = {str(path.relative_to(ROOT)) for path in DEVICE.rglob("*") if path.is_file() and
                not any(part in {".build", "build", "__pycache__"} for part in path.relative_to(DEVICE).parts)}
    if not expected <= set(provenance["sources"]):
        raise ValueError("Device source tree changed since build; refresh build provenance.")
    shared = {f"desktop/src/quadband/{name}" for name in ("__init__.py", "bluetooth.py", "common.py")}
    if not shared <= set(provenance["sources"]):
        raise ValueError("Shared Bluetooth payload is absent from source provenance.")
    for name, digest in provenance["sources"].items():
        path = (ROOT / name).resolve()
        if not path.is_relative_to(ROOT) or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Source changed since build: {name}")
    files = {}
    doc = "usr/share/doc/quadband-cardputerzero/"
    files["usr/lib/quadband-cardputerzero/quadband-cardputerzero"] = (binary, 0o755)
    for source in (DEVICE / "helper").glob("*.py"):
        files["usr/lib/quadband-cardputerzero/helper/" + source.name] = (source.read_bytes(), 0o644)
    for source in shared:
        files["usr/lib/quadband-cardputerzero/helper/quadband/" + Path(source).name] = (
            (ROOT / source).read_bytes(), 0o644)
    files["usr/bin/quadband-cardputerzero"] = ((DEVICE / "packaging/quadband-cardputerzero").read_bytes(), 0o755)
    files["usr/lib/udev/rules.d/90-quadband-grove-power.rules"] = (
        (DEVICE / "packaging/90-quadband-grove-power.rules").read_bytes(), 0o644)
    files["usr/share/APPLaunch/applications/quadband-cardputerzero.desktop"] = (
        (DEVICE / "packaging/quadband-cardputerzero.desktop").read_bytes(), 0o644)
    files["usr/share/APPLaunch/share/images/quadband-cardputerzero.png"] = (
        (ROOT / "Resources/AppIcon.iconset/icon_128x128.png").read_bytes(), 0o644)
    for source, name in ((ROOT / "LICENSE", "copyright"), (ROOT / "THIRD_PARTY.txt", "THIRD_PARTY.txt"),
                         (ROOT / "BRANDING.txt", "BRANDING.txt"), (DEVICE / "README.rst", "README.rst"),
                         (build / "BUILD-INFO.json", "BUILD-INFO.json")):
        files[doc + name] = (source.read_bytes(), 0o644)
    lvgl = ROOT / ".build/cardputerzero-deps/lvgl"
    files[doc + "licenses/LVGL-LICENCE.txt"] = ((lvgl / "LICENCE.txt").read_bytes(), 0o644)
    files[doc + "licenses/LVGL-COPYRIGHTS.txt"] = ((lvgl / "COPYRIGHTS.md").read_bytes(), 0o644)
    notices = json.loads((DEVICE / "dependencies.lock").read_text())["fontNotices"]
    for name, notice in notices.items():
        data = (DEVICE / "licenses" / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != notice["sha256"]:
            raise ValueError("Font notice differs from dependency lock: " + name)
        files[doc + "licenses/" + name] = (data, 0o644)
    if provenance["target"] == "linux-arm64":
        if not zig_root:
            raise ValueError("Pass --zig-root to include static runtime redistribution notices.")
        compiler = Path(zig_root).resolve()
        for path, name in (("LICENSE", "Zig.txt"), ("lib/libc/musl/COPYRIGHT", "musl.txt"),
                           ("lib/libcxx/LICENSE.TXT", "libcxx.txt"), ("lib/libcxxabi/LICENSE.TXT", "libcxxabi.txt"),
                           ("lib/libunwind/LICENSE.TXT", "libunwind.txt")):
            files[doc + "licenses/" + name] = ((compiler / path).read_bytes(), 0o644)
    else:
        raise ValueError("Native static runtime notice collection is not configured; use the documented Zig build.")
    spec = importlib.util.spec_from_file_location("source_archive", ROOT / "scripts/source-archive.py")
    source_archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(source_archive)
    with tempfile.TemporaryDirectory(prefix="quadband-zero-source-") as temporary:
        path = source_archive.archive(Path(temporary) / "Source.zip")
        files[doc + "Source.zip"] = (path.read_bytes(), 0o644)
    checksums = "".join(hashlib.sha256(data).hexdigest() + "  " + name + "\n"
                        for name, (data, _) in sorted(files.items()))
    files[doc + "SHA256SUMS.txt"] = (checksums.encode(), 0o644)
    control = ("Package: quadband-cardputerzero\nVersion: 0.4.1\nSection: utils\nPriority: optional\n"
               "Architecture: arm64\nMaintainer: Midwest Gadgets\n"
               "Depends: python3 (>= 3.11), bluez, python3-bleak (>= 0.22.3), python3-dbus-fast\n"
               "Description: Quadband Card Studio handheld card workbench\n"
               " Keyboard-first HF/NFC, UHF and Chameleon LF tools. Hardware validation pending.\n"
               "Homepage: https://github.com/Hamspiced/Quadband_Card_Studio\n")
    data = deb([("debian-binary", b"2.0\n"),
                ("control.tar.gz", tar({"control": (control.encode(), 0o644),
                                       "postinst": ((DEVICE / "packaging/postinst").read_bytes(), 0o755)})),
                ("data.tar.gz", tar(files))])
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    output.with_suffix(output.suffix + ".sha256").write_text(hashlib.sha256(data).hexdigest() + "  " + output.name + "\n")
    print(f"Packaged arm64 executable/source/notices: {output} (not installed)")
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".build/cardputerzero-arm64")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/quadband-cardputerzero_0.4.1_arm64.deb")
    parser.add_argument("--zig-root", type=Path, required=True)
    args = parser.parse_args()
    package(args.build_dir, args.output, args.zig_root)
