"""Release-only tests; all publication calls use a local fake API."""
import hashlib
import importlib.util
import json
import plistlib
import re
import shutil
import sys
import tempfile
import unittest
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
assets = importlib.import_module('release_assets')
metadata = importlib.import_module('release_metadata')


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_zip(path, files):
    with zipfile.ZipFile(path, 'w') as archive:
        for name, data in files.items():
            archive.writestr(name, data)


def manifest(files):
    return ''.join(hashlib.sha256(data).hexdigest()+'  '+name+'\n'
                   for name, data in sorted(files.items())).encode()


class VersionTests(unittest.TestCase):
    def test_legacy_and_canonical_tags(self):
        for tag, expected in [('v1.01', ('1.0.1', False)), ('v1.02', ('1.0.2', False)),
                              ('v1.0.2', ('1.0.2', False)), ('v2.3.4-rc.1', ('2.3.4', True)),
                              ('v0.0.0-beta-test', ('0.0.0', True))]:
            self.assertEqual(metadata.tag_version(tag), expected)

    def test_unsafe_and_noncanonical_tags(self):
        for tag in ['', 'main', '1.0.2', 'v1.03', 'v01.0.2', 'v1.0.02', 'v1.0.2+build',
                    'v1.0.2-rc.01', 'v1.0.2\n', 'v1.0.2;touch bad', '$(touch bad)', '--help',
                    'v1.0.2/../../main', 'v1.0.2-rc--1']:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                metadata.tag_version(tag)

    def test_repository_version_contract(self):
        self.assertRegex(metadata.application_version(), '^'+metadata.VERSION+'$')

    def test_mismatched_python_or_plist_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ('desktop/pyproject.toml', 'desktop/src/quadband/__init__.py', 'Resources/Info.plist'):
                target = root/name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT/name, target)
            for name in ('desktop/src/quadband/__init__.py', 'Resources/Info.plist'):
                original = (root/name).read_text()
                (root/name).write_text(original.replace(metadata.application_version(), '999.999.999'))
                with self.assertRaisesRegex(ValueError, 'must agree'):
                    metadata.application_version(root)
                (root/name).write_text(original)

    def test_exact_tag_commit_required(self):
        self.enterContext(patch.object(metadata, 'application_version', return_value='1.0.2'))
        commit = 'a'*40
        with patch.object(metadata, 'git', side_effect=[commit, commit]) as git:
            result = metadata.release_metadata(release_tag='v1.02', commit=commit)
        self.assertEqual(result['commit'], commit)
        self.assertEqual(git.call_args.args[1:], ('rev-parse', '--verify', 'refs/tags/v1.02^{commit}'))
        with patch.object(metadata, 'git', side_effect=[commit, 'b'*40]):
            with self.assertRaisesRegex(ValueError, 'current checked-out'):
                metadata.release_metadata(release_tag='v1.02')
        with patch.object(metadata, 'git', return_value=commit):
            for wrong in ('b'*40, 'aaaaaaa', '--all'):
                with self.assertRaises(ValueError):
                    metadata.release_metadata(commit=wrong)
            with self.assertRaisesRegex(ValueError, 'does not match'):
                metadata.release_metadata(release_tag='v1.01')

    def test_metadata_json_cli_without_mutation(self):
        import subprocess
        result = subprocess.check_output([sys.executable, str(ROOT/'scripts/release_metadata.py'), '--json'],
                                         text=True)
        value = json.loads(result)
        self.assertEqual(value['version'], metadata.application_version())
        self.assertRegex(value['commit'], r'^[0-9a-f]{40}$')
        self.assertEqual(value['windows'], f'Quadband-Card-Studio-{metadata.application_version()}-Windows-x64.zip')

    def test_version_only_mode_does_not_require_git(self):
        import io
        with patch.object(sys, 'argv', ['release_metadata.py', '--field', 'version']), \
                patch.object(metadata, 'git', side_effect=AssertionError('No Git in extracted source')), \
                patch('sys.stdout', new_callable=io.StringIO) as output:
            metadata.main()
        self.assertEqual(output.getvalue().strip(), metadata.application_version())


class PackagingTests(unittest.TestCase):
    def test_zero_package_job_installs_bluetooth_test_runtime(self):
        source = (ROOT/'.github/workflows/cardputerzero.yml').read_text()
        package_job = source.split('  test-and-package:', 1)[1]
        install = 'python -m pip install "bleak==3.0.2"'
        self.assertIn(install, package_job)
        self.assertLess(package_job.index(install), package_job.index('Cross-build and package without deployment'))

    def test_binary_upload_accepts_json_and_binary_download_accepts_octets(self):
        class Response:
            def __enter__(self):
                return self
            def __exit__(self, *_):
                return False
            def read(self):
                return b'{"id":1}'
        client = assets.GitHub('example/project')
        for method, url, data, accept in (
            ('POST', 'https://uploads.github.com/repos/example/project/releases/1/assets?name=test.zip',
             b'fixture', 'application/vnd.github+json'),
            ('GET', 'https://api.github.com/repos/example/project/releases/assets/1',
             None, 'application/octet-stream'),
        ):
            with self.subTest(method=method), patch.object(client.opener,'open',return_value=Response()) as request:
                self.assertEqual(client.request(url,method=method,data=data,binary=True), b'{"id":1}')
                self.assertEqual(request.call_args.args[0].get_header('Accept'), accept)
                if data is not None:
                    self.assertEqual(request.call_args.args[0].get_header('Content-type'), 'application/octet-stream')

    def test_mac_architecture_verification_puts_the_binary_before_arch_names(self):
        source = (ROOT/'scripts/build-app.sh').read_text()
        self.assertIn('lipo "$APP/Contents/MacOS/MagStripeStudio" -verify_arch "$ARCH"', source)

    def test_local_windows_cross_packager_uses_checked_out_version(self):
        source = (ROOT/'desktop/packaging/build-windows-local.py').read_text()
        self.assertIn("return module.application_version(ROOT)", source)
        self.assertIn("app_version = application_version()", source)
        self.assertIn("'app_version': app_version", source)
        self.assertIn("f'Quadband-Card-Studio-{app_version}-Windows-x64'", source)
        self.assertNotIn("Quadband Card Studio 1.0.2", source)

    def test_portable_names_and_build_info_are_version_driven(self):
        module = load(ROOT/'desktop/packaging/build.py', 'desktop_packaging')
        self.assertEqual(module.package_name('2.3.4', 'win32', 'AMD64'),
                         'Quadband-Card-Studio-2.3.4-Windows-x64')
        self.assertEqual(module.package_name('2.3.4', 'win32', 'x86_64'),
                         'Quadband-Card-Studio-2.3.4-Windows-x64')
        self.assertEqual(module.package_name('2.3.4', 'linux', 'x86_64'),
                         'Quadband-Card-Studio-2.3.4-Linux-x86_64')
        with self.assertRaises(ValueError):
            module.package_name('2.3.4', 'win32', 'ARM64')
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary)
            with patch.object(module.metadata, 'distributions', return_value=[]), \
                    patch.object(module, 'application_version', return_value='2.3.4'):
                module.notices(destination)
            self.assertEqual(json.loads((destination/'BUILD-INFO.json').read_text())['app_version'], '2.3.4')

    def test_source_archive_contains_release_tests_and_preserves_credits(self):
        module = load(ROOT/'scripts/source-archive.py', 'public_source')
        files = {path.relative_to(ROOT).as_posix() for path in module.public_files()}
        self.assertIn('tests_release/test_release.py', files)
        self.assertIn('scripts/release_metadata.py', files)
        self.assertFalse(any(Path(name).suffix.lower() == '.md' for name in files))
        self.assertFalse(any('.build' in Path(name).parts for name in files))
        notices = (ROOT/'THIRD_PARTY.txt').read_text()
        self.assertIn('Tototo31', notices)
        self.assertIn('Ashsh', notices)


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.artifacts = self.root/'artifacts'
        self.output = self.root/'publish'
        self.info = {'tag': 'v1.02', 'version': '1.0.2', 'commit': 'a'*40, 'prerelease': False,
                     'macos': 'Quadband-Card-Studio-1.0.2-macOS-universal.zip',
                     'windows': 'Quadband-Card-Studio-1.0.2-Windows-x64.zip',
                     'source': 'Quadband-Card-Studio-1.0.2-source.zip',
                     'firmware': 'Quadband-CoreS3-v1.02.zip'}
        for name in assets.ARTIFACTS:
            (self.artifacts/name).mkdir(parents=True)
        mac = {'Quadband Card Studio.app/Contents/MacOS/MagStripeStudio': b'universal-executable',
               'Quadband Card Studio.app/Contents/Info.plist':
                   plistlib.dumps({'CFBundleShortVersionString': '1.0.2'})}
        write_zip(self.artifacts/'Quadband-macOS'/self.info['macos'], mac)
        write_zip(self.artifacts/'Quadband-Windows'/self.info['windows'],
                  {'QuadbandCardStudio/QuadbandCardStudio.exe': b'executable',
                   'QuadbandCardStudio/_internal/python312.dll': b'runtime'})
        source = {'LICENSE': b'fixture source license'}
        source['SOURCE-MANIFEST.sha256'] = manifest(source)
        write_zip(self.artifacts/'Quadband-source-and-python'/self.info['source'],
                  {'Quadband-Card-Studio/'+name: data for name, data in source.items()})
        for suffix in ('-py3-none-any.whl', '.tar.gz'):
            (self.artifacts/'Quadband-source-and-python'/('quadband_card_studio-1.0.2'+suffix)).write_bytes(b'python')
        self.catalog = {'schemaVersion': 1, 'releaseTag': 'v1.02', 'applicationVersion': '1.0.2',
                        'sourceCommit': 'a'*40, 'sourceDirty': False, 'parts': [],
                        'board': 'M5Stack CoreS3', 'chipFamily': 'ESP32-S3', 'flashSize': 16777216,
                        'flashMode': 'dio', 'flashFrequency': '80m', 'firmwareVersion': '0.2.1'}
        payload = self.artifacts/'Quadband-CoreS3-web-payload'
        firmware = {'Source.zip': b'source', 'BUILD-INFO.json': b'{}', 'THIRD_PARTY.txt': b'credits'}
        for role, name, offset, data, packaged in [
                ('bootloader', 'bootloader.bin', 0, b'boot', 'bootloader/bootloader.bin'),
                ('partition-table', 'partition-table.bin', 32768, b'p'*3072, 'partition_table/partition-table.bin'),
                ('application', 'quadband_cores3.bin', 65536, b'app', 'quadband_cores3.bin')]:
            target = payload/'firmware'/name
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(data)
            (self.artifacts/'Quadband-CoreS3-firmware'/name).write_bytes(data)
            firmware[packaged] = data
            self.catalog['parts'].append({'role': role, 'path': 'firmware/'+name, 'offset': offset,
                                          'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        firmware['SHA256SUMS.txt'] = manifest(firmware)
        self.firmware_zip = self.artifacts/'Quadband-CoreS3-firmware'/self.info['firmware']
        write_zip(self.firmware_zip, {'Quadband-CoreS3/'+name: data for name, data in firmware.items()})
        self.firmware_zip.with_suffix('.zip.sha256').write_text(
            assets.digest(self.firmware_zip)+'  '+self.firmware_zip.name+'\n')
        self.save_catalog()

    def save_catalog(self):
        payload = self.artifacts/'Quadband-CoreS3-web-payload'
        (payload/'catalog.json').write_text(json.dumps(self.catalog))
        (payload/'BUILD-INFO.json').write_text('{}')
        files = {path.relative_to(payload).as_posix(): path.read_bytes()
                 for path in payload.rglob('*') if path.is_file() and path.name != 'SHA256SUMS.txt'}
        (payload/'SHA256SUMS.txt').write_bytes(manifest(files))

    def test_all_assets_and_checksums_collected(self):
        files = assets.collect(self.artifacts, self.output, self.info)
        self.assertEqual(len(files), 10)
        self.assertEqual(len(files[-1].read_text().splitlines()), 9)
        self.assertIn('Quadband-CoreS3-v1.02-bootloader.bin', [path.name for path in files])
        self.assertFalse(any(path.name == 'bootloader.bin' for path in files))

    def test_missing_required_build_or_wheel_blocks_collection(self):
        for name in assets.ARTIFACTS:
            original = self.artifacts/name
            moved = self.artifacts/(name+'.missing')
            original.rename(moved)
            with self.subTest(name=name), self.assertRaises(ValueError):
                assets.collect(self.artifacts, self.output, self.info)
            moved.rename(original)
        (self.artifacts/'Quadband-source-and-python/quadband_card_studio-1.0.2-py3-none-any.whl').unlink()
        with self.assertRaises(ValueError):
            assets.collect(self.artifacts, self.output, self.info)
        self.assertFalse(self.output.exists())

    def test_source_manifest_tampering_is_rejected(self):
        source_zip = self.artifacts/'Quadband-source-and-python'/self.info['source']
        files = assets.zip_files(source_zip)
        files['Quadband-Card-Studio/LICENSE'] = b'changed since manifest'
        write_zip(source_zip, files)
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            assets.collect(self.artifacts, self.output, self.info)

    def test_unknown_catalog_keys_and_bad_board_identity_are_rejected(self):
        self.catalog['unexpected'] = 'not in frozen schema'
        self.save_catalog()
        with self.assertRaises(ValueError):
            assets.collect(self.artifacts, self.output, self.info)
        del self.catalog['unexpected']
        for key, value in [('chipFamily', 'ESP32'), ('board', 'unconfirmed board'),
                           ('flashSize', 8388608), ('flashMode', 'qio'), ('schemaVersion', True)]:
            original = self.catalog[key]
            self.catalog[key] = value
            self.save_catalog()
            with self.subTest(key=key), self.assertRaises(ValueError):
                assets.collect(self.artifacts, self.output, self.info)
            self.catalog[key] = original

    def test_web_payload_checksum_must_cover_every_file(self):
        sums = self.artifacts/'Quadband-CoreS3-web-payload/SHA256SUMS.txt'
        sums.write_text('')
        with self.assertRaisesRegex(ValueError, 'every packaged file'):
            assets.collect(self.artifacts, self.output, self.info)

    def test_dirty_wrong_commit_wrong_tag_or_wrong_version_fails_closed(self):
        for key, value in [('sourceDirty', True), ('sourceCommit', 'b'*40),
                           ('releaseTag', 'v1.0.2'), ('applicationVersion', '9.9.9')]:
            original = self.catalog[key]
            self.catalog[key] = value
            self.save_catalog()
            with self.subTest(key=key), self.assertRaises(ValueError):
                assets.collect(self.artifacts, self.output, self.info)
            self.catalog[key] = original
        self.assertFalse(self.output.exists())

    def test_malformed_parts_or_corrupt_binary_fail(self):
        part = self.catalog['parts'][0]
        for key, value in [('offset', 0x9000), ('size', 100), ('sha256', '0'*64), ('role', 'nvs')]:
            original = part[key]
            part[key] = value
            self.save_catalog()
            with self.subTest(key=key), self.assertRaises(ValueError):
                assets.collect(self.artifacts, self.output, self.info)
            part[key] = original
        self.save_catalog()
        (self.artifacts/'Quadband-CoreS3-firmware/bootloader.bin').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'differ'):
            assets.collect(self.artifacts, self.output, self.info)

    def test_bare_windows_executable_is_rejected(self):
        write_zip(self.artifacts/'Quadband-Windows'/self.info['windows'],
                  {'QuadbandCardStudio/QuadbandCardStudio.exe': b'bare'})
        with self.assertRaisesRegex(ValueError, 'runtime'):
            assets.collect(self.artifacts, self.output, self.info)

    def test_invalid_firmware_zip_checksum_is_rejected(self):
        self.firmware_zip.with_suffix('.zip.sha256').write_text('0'*64+'  '+self.firmware_zip.name)
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            assets.collect(self.artifacts, self.output, self.info)

    def test_unsafe_archive_paths_are_rejected(self):
        archive = self.root/'unsafe.zip'
        for name in ('../escape', '/absolute', 'folder\\escape'):
            write_zip(archive, {name: b'data'})
            with self.assertRaises(ValueError):
                assets.zip_files(archive)


class FakeGitHub:
    base = 'https://api.github.com/repos/example/project'

    def __init__(self, existing=()):
        self.existing = [dict(item) for item in existing]
        self.mutations = []

    def release(self, metadata):
        return {'id': 1, 'upload_url': 'https://uploads.github.com/repos/example/project/releases/1/assets{?name,label}'}

    def assets(self, release):
        return self.existing

    def request(self, url, method='GET', data=None, binary=False):
        self.mutations.append((method, url))
        if method == 'DELETE':
            identity = int(url.rsplit('/', 1)[-1])
            self.existing[:] = [item for item in self.existing if item['id'] != identity]
            return b''
        query = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
        item = {'id': 100+len(self.existing), 'name': query['name'][0], 'label': query['label'][0],
                'digest': 'sha256:'+hashlib.sha256(data).hexdigest(), 'size': len(data)}
        self.existing.append(item)
        return json.dumps(item).encode()


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name)/'managed.zip'
        self.path.write_bytes(b'payload')
        self.item = {'id': 1, 'name': self.path.name, 'label': assets.LABEL,
                     'digest': 'sha256:'+assets.digest(self.path), 'size': self.path.stat().st_size}

    def test_initial_upload_and_identical_rerun(self):
        client = FakeGitHub()
        self.assertEqual(assets.publish(client, {}, [self.path]), 1)
        self.assertEqual(assets.publish(client, {}, [self.path]), 0)
        self.assertEqual([method for method, _ in client.mutations], ['POST'])

    def test_only_changed_owned_asset_replaced(self):
        self.item['digest'] = 'sha256:'+'0'*64
        unrelated = {'id': 2, 'name': 'hand-written-notes.txt', 'label': ''}
        client = FakeGitHub([self.item, unrelated])
        self.assertEqual(assets.publish(client, {}, [self.path]), 1)
        self.assertIn(unrelated, client.existing)
        self.assertEqual([method for method, _ in client.mutations], ['DELETE', 'POST'])

    def test_unmanaged_collision_prevents_all_mutations(self):
        self.item['label'] = ''
        client = FakeGitHub([self.item])
        new = self.path.with_name('new.zip')
        new.write_bytes(b'new')
        with self.assertRaisesRegex(ValueError, 'non-workflow'):
            assets.publish(client, {}, [new, self.path])
        self.assertEqual(client.mutations, [])

    def test_absent_api_digest_uses_download_without_uploading(self):
        del self.item['digest']
        client = FakeGitHub([self.item])
        with patch.object(client, 'request', return_value=self.path.read_bytes()) as request:
            self.assertEqual(assets.publish(client, {}, [self.path]), 0)
        self.assertEqual(request.call_count, 2)
        for call in request.call_args_list:
            self.assertTrue(call.kwargs['binary'])
            self.assertNotIn('method', call.kwargs)

    def test_upload_hash_mismatch_is_not_success(self):
        client = FakeGitHub()
        original = client.request

        def corrupt_response(*args, **kwargs):
            result = json.loads(original(*args, **kwargs))
            result['digest'] = 'sha256:'+'0'*64
            return json.dumps(result).encode()

        with patch.object(client, 'request', side_effect=corrupt_response):
            with self.assertRaisesRegex(ValueError, 'checksum verification'):
                assets.publish(client, {}, [self.path])

    def test_missing_draft_wrong_tag_and_invalid_release_status_rejected(self):
        client = assets.GitHub('example/project')
        good = {'draft': False, 'tag_name': 'v1.02', 'published_at': '2026-11-02', 'prerelease': False}
        for key, value in [('draft', True), ('tag_name', 'v1.01'), ('published_at', None), ('prerelease', None)]:
            with patch.object(client, 'request', return_value=dict(good, **{key: value})):
                with self.subTest(key=key), self.assertRaises(ValueError):
                    client.release({'tag': 'v1.02', 'prerelease': False})

    def test_canonical_tag_may_be_prerelease_but_suffixed_tag_may_not_be_stable(self):
        client = assets.GitHub('example/project')
        release = {'draft': False, 'tag_name': 'v1.0.3', 'published_at': '2026-11-02', 'prerelease': True}
        with patch.object(client, 'request', return_value=release):
            self.assertTrue(client.release({'tag': 'v1.0.3', 'prerelease': False})['prerelease'])
        release.update(tag_name='v1.0.3-rc.1', prerelease=False)
        with patch.object(client, 'request', return_value=release):
            with self.assertRaises(ValueError):
                client.release({'tag': 'v1.0.3-rc.1', 'prerelease': True})
    def test_release_status_outputs_actual_prerelease_without_publication(self):
        output = Path(self.temporary.name)/'github-output'
        arguments = ['release_assets.py', '--release-tag=v1.0.3', '--commit', 'a'*40,
                     '--check-release', '--github-output', str(output)]
        with patch.object(sys, 'argv', arguments), \
                patch.object(assets, 'release_metadata', return_value={'tag': 'v1.0.3', 'prerelease': False}), \
                patch.dict('os.environ', {'GITHUB_REPOSITORY': 'example/project'}), \
                patch.object(assets.GitHub, 'release', return_value={'prerelease': True}), \
                patch.object(assets, 'publish', side_effect=AssertionError('No publication in prepare')):
            assets.main()
        self.assertEqual(output.read_text(), 'prerelease=true\n')

    def test_existing_asset_lookup_is_paginated(self):
        client = assets.GitHub('example/project')
        with patch.object(client, 'request', side_effect=[[{'id': n} for n in range(100)], [{'id': 100}]]) as request:
            self.assertEqual(len(client.assets({'id': 42})), 101)
        self.assertIn('page=2', request.call_args.args[0])

    def test_redirect_drops_authorization_outside_api_host(self):
        request = urllib.request.Request('https://api.github.com/example',
                                         headers={'Authorization': 'test-marker'})
        redirected = assets.SafeRedirect().redirect_request(
            request, None, 302, 'Found', {}, 'https://objects.githubusercontent.com/file')
        self.assertFalse(redirected.has_header('Authorization'))
        with self.assertRaises(ValueError):
            assets.SafeRedirect().redirect_request(request, None, 302, 'Found', {}, 'http://example.com/file')

    def test_api_refuses_non_github_or_unencrypted_hosts(self):
        client = assets.GitHub('example/project')
        for url in ('http://api.github.com/test', 'https://example.com/test', 'https://api.github.com.evil.test/test'):
            with self.assertRaises(ValueError):
                client.request(url)


class WorkflowTests(unittest.TestCase):
    def test_actions_are_immutable_and_release_inputs_not_shell_interpolated(self):
        for path in (ROOT/'.github/workflows/build.yml', ROOT/'.github/workflows/release.yml'):
            text = path.read_text()
            actions = re.findall(r'uses: (actions/[^\s]+)', text)
            self.assertTrue(actions)
            for action in actions:
                self.assertRegex(action, r'@[0-9a-f]{40}$')
            lines = text.splitlines()
            in_run = False
            indent = 0
            for line in lines:
                if 'run: |' in line:
                    in_run = True
                    indent = len(line)-len(line.lstrip())
                elif in_run and line.strip() and len(line)-len(line.lstrip()) <= indent:
                    in_run = False
                if in_run:
                    self.assertNotIn('${{', line)
            self.assertIn('persist-credentials: false', text)

    def test_release_gates_and_interface(self):
        text = (ROOT/'.github/workflows/release.yml').read_text()
        self.assertIn('types: [published]', text)
        self.assertIn('needs: [prepare, build]', text)
        self.assertEqual(text.count('contents: write'), 1)
        self.assertGreaterEqual(text.count('GITHUB_TOKEN:'), 2)
        self.assertIn('prerelease: ${{ steps.release-status.outputs.prerelease }}', text)
        self.assertIn('cancel-in-progress: false', text)
        self.assertIn('uses: ./.github/workflows/build.yml', text)
        self.assertIn('build-ref: ${{ needs.prepare.outputs.commit }}', text)
        self.assertIn('tag: ${{ steps.published.outputs.tag }}', text)
        build = (ROOT/'.github/workflows/build.yml').read_text()
        self.assertIn("if: inputs.release-tag != ''", build)
        self.assertNotIn('--allow-dirty', build)
        self.assertNotIn('contents: write', build)
        for artifact in assets.ARTIFACTS:
            self.assertIn('name: '+artifact, build)


if __name__ == '__main__':
    unittest.main()
