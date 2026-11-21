#!/usr/bin/env python3
"""Preflight native/firmware assets, then update only workflow-owned release assets."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import urllib.parse
import urllib.request
import zipfile

from release_metadata import VERSION, release_metadata

LABEL = 'quadband-release-v1'
ARTIFACTS = ('Quadband-macOS', 'Quadband-Windows', 'Quadband-source-and-python',
             'Quadband-CoreS3-firmware', 'Quadband-CoreS3-web-payload')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def unique(folder, name):
    matches = list(folder.rglob(name))
    if len(matches) != 1 or not matches[0].is_file() or matches[0].is_symlink():
        raise ValueError(f'Expected exactly one regular {name} in {folder}.')
    if matches[0].stat().st_size == 0:
        raise ValueError(f'Empty asset: {name}')
    return matches[0]


def zip_files(path):
    with zipfile.ZipFile(path) as archive:
        if archive.testzip():
            raise ValueError(f'Corrupt archive: {path.name}')
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError(f'Duplicate archive paths: {path.name}')
        for name in names:
            if '\\' in name or PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts:
                raise ValueError(f'Unsafe archive path: {name}')
        return {name: archive.read(name) for name in names if not name.endswith('/')}

def verify_manifest(files, prefix, manifest):
    covered = set()
    for line in files[prefix+manifest].decode('utf-8').splitlines():
        checksum, separator, name = line.partition('  ')
        if (separator != '  ' or not re.fullmatch(r'[0-9a-f]{64}', checksum)
                or name in covered or prefix+name not in files
                or hashlib.sha256(files[prefix+name]).hexdigest() != checksum):
            raise ValueError(f'Archive checksum mismatch: {name}')
        covered.add(name)
    expected = {name.removeprefix(prefix) for name in files if name.startswith(prefix)}
    if covered != expected-{manifest}:
        raise ValueError('Archive checksum manifest must cover every packaged file.')


def collect(artifacts, output, metadata):
    """Fail before publication if any expected output or provenance is missing."""
    if not metadata['tag']:
        raise ValueError('Publication requires a release tag.')
    for name in ARTIFACTS:
        folder = artifacts/name
        if folder.is_symlink() or not folder.is_dir() or any(path.is_symlink() for path in folder.rglob('*')):
            raise ValueError(f'Missing artifact or unexpected symlink: {name}')
    picked = {}
    for key, artifact in [('macos', 'Quadband-macOS'), ('windows', 'Quadband-Windows'),
                          ('source', 'Quadband-source-and-python'), ('firmware', 'Quadband-CoreS3-firmware')]:
        picked[metadata[key]] = unique(artifacts/artifact, metadata[key])
    mac = zip_files(picked[metadata['macos']])
    app = 'Quadband Card Studio.app/Contents/'
    if not mac.get(app+'MacOS/MagStripeStudio'):
        raise ValueError('Mac ZIP must contain the app executable, not a bare binary.')
    if plistlib.loads(mac[app+'Info.plist'])['CFBundleShortVersionString'] != metadata['version']:
        raise ValueError('Mac app bundle version mismatch.')
    portable = zip_files(picked[metadata['windows']])
    if not portable.get('QuadbandCardStudio/QuadbandCardStudio.exe') or not any(
            name.startswith('QuadbandCardStudio/_internal/') for name in portable):
        raise ValueError('Windows ZIP must contain the executable and its runtime.')
    source = zip_files(picked[metadata['source']])
    if not source.get('Quadband-Card-Studio/SOURCE-MANIFEST.sha256'):
        raise ValueError('Source ZIP must contain its source manifest.')
    verify_manifest(source, 'Quadband-Card-Studio/', 'SOURCE-MANIFEST.sha256')
    python_name = 'quadband_card_studio-'+metadata['version']
    for name in (python_name+'-py3-none-any.whl', python_name+'.tar.gz'):
        picked[name] = unique(artifacts/'Quadband-source-and-python', name)
    firmware = zip_files(picked[metadata['firmware']])
    for name in ('Source.zip', 'BUILD-INFO.json', 'SHA256SUMS.txt', 'THIRD_PARTY.txt'):
        if not firmware.get('Quadband-CoreS3/'+name):
            raise ValueError(f'Firmware ZIP lacks {name}.')
    verify_manifest(firmware, 'Quadband-CoreS3/', 'SHA256SUMS.txt')
    payload = artifacts/'Quadband-CoreS3-web-payload'
    catalog = json.loads(unique(payload, 'catalog.json').read_text())
    keys = {'schemaVersion', 'board', 'chipFamily', 'flashSize', 'flashMode', 'flashFrequency',
            'applicationVersion', 'firmwareVersion', 'releaseTag', 'sourceCommit', 'sourceDirty', 'parts'}
    if (not isinstance(catalog, dict) or set(catalog) != keys or type(catalog.get('schemaVersion')) is not int
            or catalog.get('schemaVersion') != 1 or catalog.get('board') != 'M5Stack CoreS3'
            or catalog.get('chipFamily') != 'ESP32-S3' or type(catalog.get('flashSize')) is not int
            or catalog.get('flashSize') != 16777216 or catalog.get('flashMode') != 'dio'
            or catalog.get('flashFrequency') != '80m'
            or not isinstance(catalog.get('firmwareVersion'), str)
            or not re.fullmatch(VERSION, catalog['firmwareVersion'])
            or catalog.get('releaseTag') != metadata['tag']
            or catalog.get('applicationVersion') != metadata['version']
            or catalog.get('sourceCommit') != metadata['commit'] or catalog.get('sourceDirty') is not False):
        raise ValueError('Web payload provenance must match this clean release.')
    expected = [
        ('bootloader', 'firmware/bootloader.bin', 0, 32768, 'bootloader/bootloader.bin'),
        ('partition-table', 'firmware/partition-table.bin', 32768, 3072, 'partition_table/partition-table.bin'),
        ('application', 'firmware/quadband_cores3.bin', 65536, 1536000, 'quadband_cores3.bin'),
    ]
    if not isinstance(catalog['parts'], list) or len(catalog['parts']) != 3:
        raise ValueError('Expected exactly three split firmware parts.')
    for part, (role, relative, offset, maximum, packaged) in zip(catalog['parts'], expected):
        path = unique(payload, Path(relative).name)
        if path != payload/relative:
            raise ValueError(f'Unexpected payload path: {path}')
        data = path.read_bytes()
        if (not isinstance(part, dict) or set(part) != {'role', 'path', 'offset', 'size', 'sha256'}
                or type(part.get('offset')) is not int or type(part.get('size')) is not int
                or part.get('role') != role or part.get('path') != relative or part.get('offset') != offset
                or part.get('size') != len(data) or not 0 < len(data) <= maximum
                or (role == 'partition-table' and len(data) != 3072)
                or part.get('sha256') != hashlib.sha256(data).hexdigest()):
            raise ValueError(f'Invalid split firmware catalog part: {role}')
        if firmware.get('Quadband-CoreS3/'+packaged) != data:
            raise ValueError(f'Firmware ZIP/web payload differ: {role}')
        split = unique(artifacts/'Quadband-CoreS3-firmware', Path(relative).name)
        if split.read_bytes() != data:
            raise ValueError(f'Split firmware/web payload differ: {role}')
        picked[f'Quadband-CoreS3-{metadata["tag"]}-{Path(relative).name}'] = split
    payload_files = {path.relative_to(payload).as_posix(): path.read_bytes()
                     for path in payload.rglob('*') if path.is_file()}
    if set(payload_files) != {'catalog.json', 'BUILD-INFO.json', 'SHA256SUMS.txt',
                               *(entry[1] for entry in expected)}:
        raise ValueError('Unexpected or missing web payload files.')
    verify_manifest(payload_files, '', 'SHA256SUMS.txt')
    sidecar = unique(artifacts/'Quadband-CoreS3-firmware', metadata['firmware']+'.sha256')
    if sidecar.read_text().strip() != digest(picked[metadata['firmware']])+'  '+metadata['firmware']:
        raise ValueError('Firmware ZIP checksum mismatch.')
    output.mkdir(parents=True, exist_ok=True)
    files = []
    for name, source_path in sorted(picked.items()):
        destination = output/name
        if destination.exists():
            raise ValueError(f'Use an empty staging directory: {destination}')
        shutil.copyfile(source_path, destination)
        files.append(destination)
    checksums = output/f'SHA256SUMS-{metadata["tag"]}.txt'
    checksums.write_text(''.join(f'{digest(path)}  {path.name}\n' for path in files), encoding='utf-8')
    return files+[checksums]


class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        if urllib.parse.urlparse(newurl).scheme != 'https':
            raise ValueError('Refusing non-HTTPS API redirect.')
        redirected = super().redirect_request(request, fp, code, message, headers, newurl)
        if urllib.parse.urlparse(newurl).hostname != urllib.parse.urlparse(request.full_url).hostname:
            redirected.remove_header('Authorization')
        return redirected


class GitHub:
    def __init__(self, repository, token=None):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
            raise ValueError('Expected owner/repository.')
        self.base = 'https://api.github.com/repos/'+repository
        self.token = token
        self.opener = urllib.request.build_opener(SafeRedirect)

    def request(self, url, method='GET', data=None, binary=False):
        parsed = urllib.parse.urlparse(url)
        if parsed.scheme != 'https' or parsed.hostname not in ('api.github.com', 'uploads.github.com'):
            raise ValueError('Unexpected GitHub API host.')
        headers = {'Accept': 'application/octet-stream' if binary else 'application/vnd.github+json',
                   'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'quadband-release'}
        if self.token:
            headers['Authorization'] = 'Bearer '+self.token
        if data is not None:
            headers['Content-Type'] = 'application/octet-stream' if binary else 'application/json'
        request = urllib.request.Request(url, method=method, data=data, headers=headers)
        with self.opener.open(request, timeout=120) as response:
            result = response.read()
        return result if binary or not result else json.loads(result)

    def release(self, metadata):
        tag = metadata['tag']
        release = self.request(self.base+'/releases/tags/'+urllib.parse.quote(tag, safe=''))
        if (release.get('tag_name') != tag or release.get('draft') is not False
                or not release.get('published_at')
                or type(release.get('prerelease')) is not bool
                or (metadata['prerelease'] and not release['prerelease'])):
            raise ValueError('Release must already be published; prerelease tags cannot be marked stable.')
        return release

    def assets(self, release):
        found = []
        page = 1
        while True:
            batch = self.request(f'{self.base}/releases/{release["id"]}/assets?per_page=100&page={page}')
            found.extend(batch)
            if len(batch) < 100:
                return found
            page += 1


def publish(client, metadata, files):
    def remote_digest(asset):
        checksum = asset.get('digest')
        if not checksum:
            data = client.request(f'{client.base}/releases/assets/{asset["id"]}', binary=True)
            checksum = 'sha256:'+hashlib.sha256(data).hexdigest()
        return checksum
    release = client.release(metadata)
    existing = client.assets(release)
    actions = []
    for path in files:
        matches = [asset for asset in existing if asset['name'] == path.name]
        if len(matches) > 1:
            raise ValueError(f'Duplicate remote asset: {path.name}')
        asset = matches[0] if matches else None
        if asset and asset.get('label') != LABEL:
            raise ValueError(f'Refusing to alter a non-workflow asset: {path.name}')
        checksum = digest(path)
        if asset:
            if remote_digest(asset) == 'sha256:'+checksum and asset['size'] == path.stat().st_size:
                continue
        actions.append((path, asset))
    # Preflight every ownership conflict before the first mutation.
    for path, asset in actions:
        if asset:
            client.request(f'{client.base}/releases/assets/{asset["id"]}', method='DELETE')
        query = urllib.parse.urlencode({'name': path.name, 'label': LABEL})
        uploaded = client.request(release['upload_url'].split('{')[0]+'?'+query,
                                  method='POST', data=path.read_bytes(), binary=True)
        uploaded = json.loads(uploaded)
        if (uploaded.get('name') != path.name or uploaded.get('label') != LABEL
                or uploaded.get('size') != path.stat().st_size
                or remote_digest(uploaded) != 'sha256:'+digest(path)):
            raise ValueError(f'Uploaded asset failed checksum verification: {path.name}')
    final = client.assets(release)
    for path in files:
        matches = [asset for asset in final if asset['name'] == path.name and asset.get('label') == LABEL]
        if (len(matches) != 1 or matches[0].get('size') != path.stat().st_size
                or remote_digest(matches[0]) != 'sha256:'+digest(path)):
            raise ValueError(f'Remote asset postflight failed: {path.name}')
    return len(actions)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-tag', required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--artifacts', type=Path)
    parser.add_argument('--output', type=Path, default=Path('.build/release-assets'))
    parser.add_argument('--check-release', action='store_true')
    parser.add_argument('--github-output', type=Path)
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    metadata = release_metadata(release_tag=args.release_tag, commit=args.commit)
    if args.check_release:
        release = GitHub(os.environ['GITHUB_REPOSITORY'], os.environ.get('GITHUB_TOKEN')).release(metadata)
        if args.github_output:
            with args.github_output.open('a', encoding='utf-8') as stream:
                stream.write(f'prerelease={str(release["prerelease"]).lower()}\n')
        print('Existing published release verified.')
        return
    if args.artifacts is None:
        parser.error('--artifacts is required unless checking release metadata.')
    files = collect(args.artifacts, args.output, metadata)
    print(f'Preflight passed: {len(files)} managed files.')
    if args.publish:
        token = os.environ.get('GITHUB_TOKEN')
        if not token:
            parser.error('--publish requires job-scoped GITHUB_TOKEN.')
        changed = publish(GitHub(os.environ['GITHUB_REPOSITORY'], token), metadata, files)
        print(f'Publication verified: {changed} uploaded, {len(files)-changed} unchanged.')


if __name__ == '__main__':
    main()
