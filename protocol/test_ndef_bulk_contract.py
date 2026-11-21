"""Shared bulk NDEF policy, geometry and canonical formatting contract."""
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent


class BulkContractTests(unittest.TestCase):
    def setUp(self):
        self.fixture = json.loads((ROOT/"fixtures/ndef-bulk-v1.json").read_text())
        self.manifest = json.loads((ROOT/"cores3-bridge-v1.json").read_text())

    def test_distinct_capability_gated_operations(self):
        operations = self.manifest["operations"]
        self.assertEqual(operations["classicFormat1k"], 22)
        self.assertEqual(operations["ntagInitialize"], 23)
        self.assertEqual(len(operations), len(set(operations.values())))
        formats = self.fixture["formatOperations"]
        self.assertEqual(formats["classic1k"]["capability"], "classic1k_format")
        for family in ("ntag213", "ntag215", "ntag216"):
            self.assertEqual(formats[family]["capability"], "ntag_initialize")
            self.assertEqual(formats[family]["operation"], operations["ntagInitialize"])

    def test_complete_snapshot_sizes_exclude_secret_ntag_pages(self):
        formats = self.fixture["formatOperations"]
        self.assertEqual(formats["classic1k"]["snapshotBytes"], 64 * 16)
        for family, pages, cc in (("ntag213", 45, "e1101200"),
                                  ("ntag215", 135, "e1103e00"),
                                  ("ntag216", 231, "e1106d00")):
            spec = formats[family]
            self.assertEqual(spec["snapshotBytes"], (pages - 2) * 4)
            self.assertEqual(spec["cc"], cc)
            self.assertLessEqual(11 + 1 + spec["snapshotBytes"], self.manifest["maxPayload"])
            self.assertLessEqual(bytes.fromhex(cc)[2] * 8, (pages - 9) * 4)

    def test_factory_trailer_layout_keeps_valid_access_redundancy(self):
        for key in ("dataTrailer", "directoryTrailer"):
            value = bytes.fromhex(self.fixture["classicFormat"][key])
            self.assertEqual(len(value), 16)
            c1, c2, c3 = value[7] >> 4, value[8] & 15, value[8] >> 4
            self.assertEqual((value[6] & 15) ^ c1, 15)
            self.assertEqual((value[6] >> 4) ^ c2, 15)
            self.assertEqual((value[7] & 15) ^ c3, 15)
        self.assertEqual(self.fixture["classicFormat"]["emptyNdef"], "0300fe")
        self.assertEqual(self.fixture["classicFormat"]["madInfo"], 1)

    def test_bulk_policy_preserves_target_review_and_removal(self):
        self.assertEqual(self.fixture["bulkLimit"], 3)
        self.assertEqual(self.fixture["policy"], {
            "initialRemovalRequired": True, "perTargetReviewRequired": True,
            "overwriteDefault": False, "formatDefault": False,
            "readerErrorIsRemoval": False, "automaticWriteRetry": False,
        })
        self.assertEqual(self.fixture["textBoundaries"], [
            {"asciiCharacters": 247, "messageBytes": 254, "tlvHeaderBytes": 2},
            {"asciiCharacters": 248, "messageBytes": 255, "tlvHeaderBytes": 4},
        ])


if __name__ == "__main__":
    unittest.main()
