import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import tarfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("zero_package", ROOT / "devices/cardputerzero/package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def elf():
    data = bytearray(256)
    data[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQQ", data, 16, 2, 183, 1, 0x400080, 64, 0)
    struct.pack_into("<HH", data, 54, 56, 1)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 128, 0x400080, 0x400080, 128, 128, 4096)
    return data


def members(data):
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("Not an ar archive")
    offset, found = 8, {}
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise ValueError("Bad ar header")
        size = int(header[48:58])
        start = offset + 60
        content = data[start:start + size]
        if len(content) != size:
            raise ValueError("Truncated ar member")
        found[header[:16].decode().strip().rstrip("/")] = content
        offset = start + size + size % 2
    return found


class Packaging(unittest.TestCase):
    def test_arm64_elf(self):
        package.arm64_static(elf())

    def test_reject_other_architecture_or_interpreter(self):
        for offset, value, format in ((18, 62, "<H"), (64, 3, "<I"), (64, 2, "<I"), (16, 3, "<H")):
            data = elf()
            struct.pack_into(format, data, offset, value)
            with self.assertRaises(ValueError):
                package.arm64_static(data)
        for data in (b"", b"MZ" + b"\0" * 300, bytes(elf()[:80])):
            with self.assertRaises(ValueError):
                package.arm64_static(data)

    def test_archive_reproducibility(self):
        files = {"usr/bin/test": (b"abc", 0o755), "usr/share/test.txt": (b"1234", 0o644)}
        first = package.tar(files)
        self.assertEqual(first, package.tar(files))
        result = members(package.deb([("debian-binary", b"2.0\n"), ("data.tar.gz", first)]))
        self.assertEqual(result["debian-binary"], b"2.0\n")
        with tarfile.open(fileobj=io.BytesIO(result["data.tar.gz"])) as archive:
            for name in ("usr", "usr/bin", "usr/share"):
                self.assertTrue(archive.getmember(name).isdir())
                self.assertEqual(archive.getmember(name).mode, 0o755)
            self.assertLess(archive.getnames().index("usr/bin"), archive.getnames().index("usr/bin/test"))
            self.assertEqual(archive.getmember("usr/bin/test").mode, 0o755)
            self.assertEqual(archive.extractfile("usr/bin/test").read(), b"abc")

    def test_source_allowlist(self):
        spec = importlib.util.spec_from_file_location("source_archive", ROOT / "scripts/source-archive.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        files = {str(path.relative_to(ROOT)) for path in module.public_files()}
        self.assertIn("devices/cardputerzero/src/runtime.cpp", files)
        self.assertIn("firmware/cores3/main/standalone/hf.cpp", files)
        self.assertFalse(any(".build/" in name or name.endswith(".md") for name in files))

    def test_real_development_package(self):
        path = ROOT / "dist/quadband-cardputerzero_0.4.0_arm64.deb"
        if not path.exists():
            self.skipTest("Build the arm64 package to exercise its contents")
        ar = members(path.read_bytes())
        self.assertEqual(set(ar), {"debian-binary", "control.tar.gz", "data.tar.gz"})
        with tarfile.open(fileobj=io.BytesIO(ar["control.tar.gz"])) as archive:
            control = archive.extractfile("control").read().decode()
            self.assertIn("Architecture: arm64\n", control)
            self.assertIn("python3-bleak (>= 0.22.3)", control)
            self.assertIn("python3-dbus-fast", control)
            self.assertEqual(set(archive.getnames()), {"control", "postinst"})
            postinst = archive.extractfile("postinst").read().decode()
            self.assertIn("attribute=/sys/class/leds/grove_5v_out/brightness", postinst)
            self.assertIn('chmod 0660 "$attribute"', postinst)
            for forbidden in ("sudo", "systemctl", "setuid", "grove_fun", "ext_5v", "pinctrl"):
                self.assertNotIn(forbidden, postinst)
        with tarfile.open(fileobj=io.BytesIO(ar["data.tar.gz"])) as archive:
            self.assertTrue(archive.getmember("usr/lib/quadband-cardputerzero").isdir())
            files = {item.name: archive.extractfile(item).read() for item in archive if item.isfile()}
            self.assertFalse(any(name.endswith(".md") or "systemd/" in name or "sudoers" in name for name in files))
            app = "usr/lib/quadband-cardputerzero/quadband-cardputerzero"
            for name in ("ble_helper.py", "ipc.py", "bluez_pair.py", "quadband/bluetooth.py",
                         "quadband/common.py", "quadband/__init__.py"):
                self.assertIn("usr/lib/quadband-cardputerzero/helper/" + name, files)
            package.arm64_static(files[app])
            rule = files["usr/lib/udev/rules.d/90-quadband-grove-power.rules"].decode()
            self.assertIn('KERNEL=="grove_5v_out"', rule)
            self.assertNotIn("grove_fun", rule)
            doc = "usr/share/doc/quadband-cardputerzero/"
            for line in files[doc + "SHA256SUMS.txt"].decode().splitlines():
                digest, name = line.split("  ", 1)
                self.assertEqual(hashlib.sha256(files[name]).hexdigest(), digest, name)
            with zipfile.ZipFile(io.BytesIO(files[doc + "Source.zip"])) as source:
                self.assertIn("Quadband-Card-Studio/devices/cardputerzero/src/runtime.cpp", source.namelist())
                for name in ("desktop/src/quadband/bluetooth.py", "desktop/src/quadband/common.py",
                             "devices/cardputerzero/helper/ble_helper.py", "devices/cardputerzero/helper/bluez_pair.py"):
                    archived = "Quadband-Card-Studio/" + name
                    self.assertIn(archived, source.namelist())
                    self.assertEqual(source.read(archived), (ROOT / name).read_bytes())
                self.assertFalse(any(name.endswith(".md") for name in source.namelist()))
            self.assertIn(b"Tototo31", files[doc + "THIRD_PARTY.txt"])
            for notice in ("musl.txt", "libcxx.txt", "libcxxabi.txt", "libunwind.txt", "LVGL-LICENCE.txt",
                           "Montserrat-OFL.txt", "FontAwesome-LICENSE.txt"):
                self.assertIn(doc + "licenses/" + notice, files)


if __name__ == "__main__":
    unittest.main()
