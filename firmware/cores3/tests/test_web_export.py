"""Synthetic, device-free web export regression tests."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("export_web", Path(__file__).parents[1]/"tools/export_web.py")
web = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(web)
COMMIT = "a"*40


def image(application=False):
    body = bytearray(128)
    if application:
        body[0:4] = b"\x32\x54\xcd\xab"
        body[16:21] = b"0.2.1"
        body[48:62] = b"quadband_cores3"
    header = bytearray(24)
    header[:4] = b"\xe9\x01\x02\x4f"
    header[12] = 9
    header[23] = 1
    data = header + struct.pack("<II", 0x3f000000, len(body)) + body
    checksum = 0xef
    for byte in body:
        checksum ^= byte
    data += b"\0"*((len(data)//16+1)*16-len(data)-1) + bytes([checksum])
    return bytes(data) + hashlib.sha256(data).digest()


def table():
    data = b""
    for kind, subtype, offset, size, label in ((1,2,0x9000,0x6000,b"nvs"),
            (1,1,0xf000,0x1000,b"phy_init"), (0,0,0x10000,0x177000,b"factory")):
        data += struct.pack("<HBBII16sI", 0x50aa, kind, subtype, offset, size, label, 0)
    return data + b"\xeb\xeb" + b"\xff"*14 + hashlib.md5(data).digest() + b"\xff"*(3072-128)


class ExportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)/"firmware"
        self.root.mkdir()
        lock = {"idfRevision": "b"*40, "dependencies": {}}
        for name, data in {"CMakeLists.txt":b"source", "sdkconfig.defaults":b"source", "build.py":b"source",
                           "main/main.cpp":b"source", "dependencies.lock":json.dumps(lock).encode(),
                           "firmware-manifest.json":b'{"version":"0.2.1"}'}.items():
            target = self.root/name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        args = {"flash_settings":{"flash_mode":"dio","flash_size":"16MB","flash_freq":"80m"},
                "write_flash_args":["--flash_mode","dio","--flash_size","16MB","--flash_freq","80m"],
                "flash_files":{hex(p[3]):p[1] for p in web.PARTS},
                "extra_esptool_args":{"chip":"esp32s3","stub":True}}
        for key, i in (("bootloader",0),("partition-table",1),("app",2)):
            args[key] = {"offset":hex(web.PARTS[i][3]),"file":web.PARTS[i][1],"encrypted":"false"}
        artifacts = {"bootloader/bootloader.bin":image(), "quadband_cores3.bin":image(True),
                     "partition_table/partition-table.bin":table(), "quadband_cores3.elf":b"ELF",
                     "flash_args":b"args", "flasher_args.json":json.dumps(args).encode()}
        for name, data in artifacts.items():
            target = self.root/".build/idf"/name
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes(data)
        self.provenance = dict(lock, idfStatus="", hardwareValidated=False,
            source={name:web.digest((self.root/name).read_bytes()) for name in
                    ("CMakeLists.txt","sdkconfig.defaults","build.py","dependencies.lock","main/main.cpp")},
            artifacts={name:{"bytes":len(data),"sha256":web.digest(data)} for name,data in artifacts.items()})
        self.save()

    def save(self):
        (self.root/".build/artifacts.json").write_text(json.dumps(self.provenance))

    def export(self, status="", **kwargs):
        with patch.object(web.subprocess, "check_output", side_effect=[COMMIT, status]):
            return web.export(Path(self.temp.name)/"payload", "v1.0.2", COMMIT, "1.0.2",
                              self.root, **kwargs)

    def test_valid_payload_and_checksums(self):
        result = self.export()
        self.assertFalse(result["sourceDirty"])
        self.assertEqual([p["offset"] for p in result["parts"]], [0,32768,65536])
        output = Path(self.temp.name)/"payload"
        for line in (output/"SHA256SUMS.txt").read_text().splitlines():
            sha, name = line.split("  ")
            self.assertEqual(web.digest((output/name).read_bytes()),sha)

    def test_dirty_refused_and_explicitly_marked(self):
        with self.assertRaisesRegex(ValueError,"Dirty"):
            self.export(status=" M tracked")
        self.assertTrue(self.export(status=" M tracked",allow_dirty=True)["sourceDirty"])

    def test_sdk_dirty_refused(self):
        self.provenance["idfStatus"] = " M sdk"
        self.save()
        with self.assertRaisesRegex(ValueError,"Dirty"):
            self.export()

    def test_changed_source(self):
        (self.root/"main/main.cpp").write_text("changed")
        with self.assertRaisesRegex(ValueError,"Source changed"):
            self.export()

    def test_incomplete_source(self):
        self.provenance["source"].pop("main/main.cpp")
        self.save()
        with self.assertRaisesRegex(ValueError,"Incomplete source"):
            self.export()

    def test_changed_artifact(self):
        (self.root/".build/idf/quadband_cores3.bin").write_bytes(b"broken")
        with self.assertRaisesRegex(ValueError,"Artifact changed"):
            self.export()

    def test_paths(self):
        for name in ("../escape","/absolute","..\\escape"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                web.safe(self.root,name)

    def test_wrong_commit(self):
        with patch.object(web.subprocess,"check_output",return_value="c"*40):
            with self.assertRaisesRegex(ValueError,"commit differs"):
                web.export(Path(self.temp.name)/"out","v1.0.2",COMMIT,"1.0.2",self.root)

    def test_image_corruption_and_version(self):
        for offset in (0,2,3,12,23,40,-1):
            data = bytearray(image(True))
            data[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                web.image(bytes(data),"0.2.1")
        with self.assertRaisesRegex(ValueError,"version mismatch"):
            web.image(image(True),"9.9.9")

    def test_partition_corruption(self):
        for offset in (0,4,64,96,112,150):
            data = bytearray(table())
            data[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                web.partition(data)

    def test_layout_rejected_even_when_hash_matches(self):
        p = self.root/".build/idf/flasher_args.json"
        args=json.loads(p.read_text())
        args["flash_files"]["0x9000"]="nvs.bin"
        p.write_text(json.dumps(args))
        self.provenance["artifacts"]["flasher_args.json"] = {"bytes":p.stat().st_size,"sha256":web.digest(p.read_bytes())}
        self.save()
        with self.assertRaisesRegex(ValueError,"Unexpected flash files"):
            self.export()


if __name__ == "__main__":
    unittest.main()
