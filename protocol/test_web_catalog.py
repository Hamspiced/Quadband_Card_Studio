"""Release web catalog and Pages staging checks without devices or credentials."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_pages", ROOT/"scripts/prepare_pages.py")
PAGES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PAGES)


class PagesPayloadTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.payload, self.site = root/"payload", root/"site"
        self.payload.mkdir()
        self.site.mkdir()
        (self.site/"index.html").write_text("site fixture", encoding="utf-8")
        self.catalog = {
            "schemaVersion": 1, "board": "M5Stack CoreS3", "chipFamily": "ESP32-S3",
            "flashSize": 16_777_216, "flashMode": "dio", "flashFrequency": "80m",
            "applicationVersion": "1.0.2", "firmwareVersion": "0.2.1",
            "releaseTag": "v1.0.2", "sourceCommit": "a"*40, "sourceDirty": False, "parts": [],
        }
        for role, name, offset, maximum in PAGES.PARTS:
            data = b"\xff" * (3072 if role == "partition-table" else 256)
            path = self.payload/name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(data)
            self.catalog["parts"].append({"role": role, "path": name, "offset": offset,
                                           "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        (self.payload/"BUILD-INFO.json").write_text("{}", encoding="utf-8")
        self.save()

    def save(self):
        (self.payload/"catalog.json").write_text(json.dumps(self.catalog), encoding="utf-8")
        names = {part[1] for part in PAGES.PARTS} | {"catalog.json", "BUILD-INFO.json"}
        (self.payload/"SHA256SUMS.txt").write_text("".join(
            hashlib.sha256((self.payload/name).read_bytes()).hexdigest()+"  "+name+"\n"
            for name in sorted(names)), encoding="utf-8")

    def validate(self):
        return PAGES.validate_payload(self.payload, "v1.0.2", "a"*40, "1.0.2")

    def test_valid_payload_and_staging(self):
        value = PAGES.stage_payload(self.payload, self.site, "v1.0.2", "a"*40, "1.0.2")
        self.assertEqual(value["firmwareVersion"], "0.2.1")
        self.assertEqual((self.site/"firmware/quadband_cores3.bin").read_bytes(), b"\xff"*256)
        self.assertEqual((self.site/"index.html").read_text(), "site fixture")

    def test_dirty_or_wrong_release_never_staged(self):
        for key, value in [("sourceDirty", True), ("sourceDirty", "false"), ("releaseTag", "v1.0.3"),
                           ("sourceCommit", "b"*40), ("applicationVersion", "1.0.3")]:
            with self.subTest(key=key, value=value):
                original = self.catalog[key]
                self.catalog[key] = value
                self.save()
                with self.assertRaises(ValueError):
                    PAGES.stage_payload(self.payload, self.site, "v1.0.2", "a"*40, "1.0.2")
                self.assertFalse((self.site/"catalog.json").exists())
                self.catalog[key] = original

    def test_wrong_offsets_paths_sizes_and_hashes_rejected(self):
        for key, value in [("offset", 0x9000), ("offset", False), ("path", "../app.bin"),
                           ("size", 0), ("size", True), ("sha256", "0"*64)]:
            with self.subTest(key=key):
                original = self.catalog["parts"][2][key]
                self.catalog["parts"][2][key] = value
                self.save()
                with self.assertRaises(ValueError):
                    self.validate()
                self.catalog["parts"][2][key] = original

    def test_missing_extra_or_duplicate_checksum_entries_rejected(self):
        path = self.payload/"SHA256SUMS.txt"
        original = path.read_text()
        for text in [original.splitlines()[0]+"\n", original+original.splitlines()[0]+"\n",
                     original+"0"*64+"  ../secret\n"]:
            path.write_text(text)
            with self.assertRaises(ValueError):
                self.validate()

    def test_extra_payload_and_symlink_rejected(self):
        extra = self.payload/"private-data.txt"
        extra.write_text("not deployable")
        with self.assertRaises(ValueError):
            self.validate()
        extra.unlink()
        extra.symlink_to(self.payload/"catalog.json")
        with self.assertRaises(ValueError):
            self.validate()

    def test_frozen_schema_offsets_and_supported_target(self):
        schema = json.loads((ROOT/"protocol/cores3-web-catalog-v1.json").read_text())
        self.assertEqual(schema["properties"]["flashSize"]["const"], 16_777_216)
        self.assertEqual(schema["properties"]["chipFamily"]["const"], "ESP32-S3")
        offsets = [part["properties"]["offset"]["const"]
                   for part in schema["properties"]["parts"]["prefixItems"]]
        self.assertEqual(offsets, [0, 0x8000, 0x10000])


if __name__ == "__main__":
    unittest.main()
