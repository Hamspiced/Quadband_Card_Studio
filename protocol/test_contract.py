"""Hardware-free consistency checks for the shared bridge contract."""
import importlib.util
import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent


class ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ROOT/'cores3-bridge-v1.json').read_text())
        cls.fixtures = json.loads((ROOT/'fixtures/cores3-v1.json').read_text())

    def test_wire_headers_and_checksums(self):
        header = self.manifest['headerFormat']
        self.assertEqual(struct.calcsize(header), 26)
        for fixture in self.fixtures['frames']:
            with self.subTest(name=fixture['name']):
                raw = bytes.fromhex(fixture['frameHex'])
                magic, version, kind, session, request, channel, op, status, generation, length = struct.unpack(header, raw[:26])
                self.assertEqual((magic, version), (b'QB', 1))
                self.assertEqual((kind, session, request, channel, op, status, generation),
                                 (fixture['kind'], int(fixture['session']), fixture['requestId'],
                                  fixture['channel'], fixture['operation'], fixture['status'], fixture['generation']))
                self.assertEqual(raw[26:-4], bytes.fromhex(fixture['payloadHex']))
                self.assertEqual(length, len(raw)-30)
                self.assertEqual(zlib.crc32(raw[:-4]), int.from_bytes(raw[-4:], 'little'))

    def test_geometry_boundaries(self):
        small = self.fixtures['geometries']['classic1k']
        large = self.fixtures['geometries']['classic4k']
        self.assertEqual(len(small), 16)
        self.assertEqual(len(large), 40)
        self.assertEqual((large[31]['firstBlock'], large[31]['trailer']), (124, 127))
        self.assertEqual((large[32]['firstBlock'], large[32]['trailer']), (128, 143))
        self.assertEqual((large[39]['firstBlock'], large[39]['trailer']), (240, 255))
        self.assertEqual(large[32]['accessGroups'], [list(range(128, 133)), list(range(133, 138)), list(range(138, 143))])
        for rows, total, count in [(small, 64, 47), (large, 256, 215)]:
            trailers = {row['trailer'] for row in rows}
            self.assertEqual(len([b for b in range(1, total) if b not in trailers]), count)

    def test_profiles_match_family_data_units(self):
        for profile in self.fixtures['profiles']:
            family = profile['family']
            self.assertEqual(profile['format'], 'quadband.card.data')
            self.assertEqual(profile['version'], 2)
            if family == 'classic4k':
                trailers = {row['trailer'] for row in self.fixtures['geometries'][family]}
                expected, size, total = set(range(1, 256))-trailers, 16, 256
            else:
                geometry = self.fixtures['geometries'][family]
                expected = set(range(geometry['firstUserPage'], geometry['lastUserPage']+1))
                size, total = geometry['unitSize'], geometry['unitCount']
                self.assertNotIn(geometry['dynamicLockPage'], expected)
                self.assertNotIn(geometry['firstConfigPage'], expected)
                self.assertEqual(geometry['firstConfigPage']+4, total)
            self.assertEqual(profile['geometry'], {'unitSize': size, 'unitCount': total})
            self.assertEqual(len(profile['blocks']), len(expected))
            self.assertEqual({b['number'] for b in profile['blocks']}, expected)
            for unit in profile['blocks']:
                self.assertEqual(len(bytes.fromhex(unit['hex'])), size)
            self.assertTrue({'uid', 'key', 'password', 'pack'}.isdisjoint(profile))
        legacy = self.fixtures['legacyProfiles'][0]
        self.assertEqual((legacy['format'], legacy['version']), ('quadband.classic1k.data', 1))
        self.assertEqual(len(legacy['blocks']), 47)
        self.assertNotIn('family', legacy)

    def test_fixture_generation_is_reproducible(self):
        spec = importlib.util.spec_from_file_location('fixture_generator', ROOT/'generate-fixtures.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temporary:
            module.ROOT = Path(temporary)
            module.main()
            for name in ['cores3-bridge-v1.json', 'fixtures/cores3-v1.json']:
                self.assertEqual((ROOT/name).read_bytes(), (module.ROOT/name).read_bytes())

    def test_ntag_permission_vectors_match_manufacturer_mapping(self):
        for case in self.fixtures['ntagLockCases']:
            static = bytes.fromhex(case['staticLockHex'])
            dynamic = bytes.fromhex(case['dynamicLockPageHex'])
            geometry = self.fixtures['geometries'][case['family']]
            stride = 2 if case['family'] == 'ntag213' else 16
            actual = []
            for page in range(4, geometry['lastUserPage']+1):
                if page < 16:
                    locked = int.from_bytes(static, 'little') & (1 << page)
                else:
                    locked = int.from_bytes(dynamic[:2], 'little') & (1 << ((page-16)//stride))
                if locked:
                    actual.append(page)
            self.assertEqual(actual, case['lockedUserPages'], case['name'])
        for case in self.fixtures['ntagLockControlCases']:
            data = bytes.fromhex(case['lockControlHex'])
            address = (data[0] >> 4)*(1 << (data[2] & 15))+(data[0] & 15)
            self.assertEqual(address, case['lockByteAddress'])
            self.assertEqual(address, self.fixtures['geometries'][case['family']]['dynamicLockPage']*4)

    def test_mad2_fixture_preserves_directory_and_large_sector_geometry(self):
        fixture = self.fixtures['classic4kNdef']
        blocks = {unit['number']: bytes.fromhex(unit['hex']) for unit in fixture['blocks']}
        self.assertEqual(blocks[3][9], 0xC2)
        self.assertEqual(blocks[67][9], 0)
        self.assertTrue(set([64, 65, 66, 67]).isdisjoint(fixture['ndefBlocks']))
        self.assertIn(128, fixture['ndefBlocks'])
        self.assertIn(254, fixture['ndefBlocks'])
        self.assertNotIn(255, fixture['ndefBlocks'])
        memory = b''.join(blocks[b] for b in fixture['ndefBlocks'])
        self.assertEqual(memory[:2], b'\x03\xFF')
        size = int.from_bytes(memory[2:4], 'big')
        self.assertEqual(memory[4:4+size], bytes.fromhex(fixture['messageHex']))


if __name__ == '__main__':
    unittest.main()
