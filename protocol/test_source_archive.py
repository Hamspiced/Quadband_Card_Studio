"""Ensure firmware and contract sources ship without private/build artifacts."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile


class SourceArchiveTests(unittest.TestCase):
    def test_firmware_and_protocol_allowlist_excludes_generated_data(self):
        root = Path(__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location('public_archive', root/'scripts/source-archive.py')
        archive = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(archive)
        include = ['README.rst', 'firmware/cores3/main/main.cpp', 'firmware/cores3/main/ui/controller.cpp',
                   'firmware/cores3/tests/ui_run.py', 'firmware/cores3/sdkconfig.defaults',
                   'firmware/cores3/dependencies.lock', 'protocol/cores3-bridge-v1.json',
                   'webflasher/index.html', 'webflasher/src/policy.js', 'webflasher/package-lock.json']
        exclude = ['firmware/cores3/sdkconfig', 'firmware/cores3/sdkconfig.old',
                   'firmware/cores3/compile_commands.json', 'firmware/cores3/build/firmware.bin',
                   'firmware/cores3/managed_components/vendor/source.c',
                   'firmware/cores3/.build/deps/vendor.c', 'firmware/cores3/.git/config',
                   'firmware/cores3/device.qcard', 'firmware/cores3/vendor.md',
                   'webflasher/node_modules/vendor/index.js', 'webflasher/dist/catalog.json',
                   'webflasher/.test-payload/firmware.bin', 'webflasher/.test-site/firmware.bin',
                   'webflasher/test-results/result.txt',
                   'webflasher/playwright-report/index.html', 'webflasher/.cache/private.txt',
                   'webflasher/.env', 'webflasher/.env.local']
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)/'source'
            for relative in include+exclude:
                target = source/relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('fixture\n')
            output = archive.archive(Path(temporary)/'public.zip', root=source)
            with zipfile.ZipFile(output) as result:
                names = {name.removeprefix('Quadband-Card-Studio/') for name in result.namelist()}
                self.assertTrue(set(include) <= names)
                self.assertTrue(set(exclude).isdisjoint(names))
                self.assertEqual(names, set(include)|{'SOURCE-MANIFEST.sha256'})


if __name__ == '__main__':
    unittest.main()
