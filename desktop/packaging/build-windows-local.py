#!/usr/bin/env python3
"""Assemble a Windows x64 app locally on macOS/Linux. SPDX-License-Identifier: GPL-3.0-only

Uses verified official Windows runtime/wheels, never uploads or commits the project.
Requires host Python 3.12+, packaging, PySide6-Essentials and Zig 0.17.0.
"""
import argparse
from collections import deque
from email import message_from_bytes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import urllib.parse
import urllib.request
import zipfile

from packaging.markers import default_environment
from packaging.requirements import Requirement
from packaging.tags import compatible_tags, cpython_tags
from packaging.utils import canonicalize_name, parse_wheel_filename
from packaging.version import Version

ROOT = Path(__file__).resolve().parents[2]
CACHE = ROOT / '.build/windows-downloads'
PYTHON = '3.13.17'
PYTHON_SHA = '9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38'
ENV = dict(default_environment(), implementation_name='cpython', implementation_version=PYTHON,
           os_name='nt', platform_machine='AMD64', platform_python_implementation='CPython',
           platform_release='11', platform_system='Windows', platform_version='10.0.22000',
           python_full_version=PYTHON, python_version='3.13', sys_platform='win32', extra='')
TAGS = list(cpython_tags((3, 13), ['cp313'], ['win_amd64'])) + list(compatible_tags((3, 13), 'cp313', ['win_amd64']))
RANK = {tag: index for index, tag in enumerate(TAGS)}
ROOTS = ['PySide6-Essentials==6.12.0', 'pyserial==3.5', 'hidapi==0.15.0', 'bleak==3.0.2', 'pyscard==2.3.1']


def metadata(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return json.load(response)


def download(url, sha):
    if urllib.parse.urlparse(url).hostname not in {'www.python.org', 'files.pythonhosted.org'}:
        raise ValueError(f'Unexpected download host: {url}')
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / Path(urllib.parse.urlparse(url).path).name
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != sha:
        temporary = path.with_suffix(path.suffix + '.partial')
        print(f'Downloading {path.name}', flush=True)
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != sha:
            temporary.unlink()
            raise ValueError(f'Checksum mismatch: {path.name}')
        temporary.replace(path)
    return path


def wheel(req):
    lock = ROOT / 'desktop/packaging/windows/wheels.lock.json'
    if lock.exists():
        selected = next((item for item in json.loads(lock.read_text())
                         if canonicalize_name(item['name']) == canonicalize_name(req.name)), None)
        if selected is None or Version(selected['version']) not in req.specifier:
            raise RuntimeError(f'The Windows wheel lock does not satisfy {req}')
        if not any(tag in RANK for tag in parse_wheel_filename(selected['filename'])[3]):
            raise RuntimeError(f'Incompatible locked wheel: {selected["filename"]}')
        return download(selected['url'], selected['sha256']), {
            'url': selected['url'], 'digests': {'sha256': selected['sha256']}}
    info = metadata(f'https://pypi.org/pypi/{req.name}/json')
    for version in sorted((Version(v) for v in info['releases'] if not Version(v).is_prerelease), reverse=True):
        if version not in req.specifier:
            continue
        choices = []
        for file in info['releases'][str(version)]:
            if not file['filename'].endswith('.whl') or file.get('yanked'):
                continue
            if file.get('requires_python') and Version(PYTHON) not in Requirement('python' + file['requires_python']).specifier:
                continue
            tags = parse_wheel_filename(file['filename'])[3]
            compatible = [RANK[tag] for tag in tags if tag in RANK]
            if compatible:
                choices.append((min(compatible), file))
        if choices:
            file = min(choices, key=lambda item: item[0])[1]
            return download(file['url'], file['digests']['sha256']), file
    raise RuntimeError(f'No compatible Windows x64 wheel for {req}')


def member_path(name):
    path = Path(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
        raise ValueError(f'Unsafe archive path: {name}')
    return path


def install_wheel(archive, destination):
    with zipfile.ZipFile(archive) as z:
        for item in z.infolist():
            path = member_path(item.filename)
            if not path.parts or item.is_dir():
                continue
            if path.parts[0].endswith('.data'):
                if path.parts[1] in ('purelib', 'platlib'):
                    path = Path(*path.parts[2:])
                elif path.parts[1] in ('scripts', 'headers'):
                    continue  # Development scripts/headers are not used by this app.
                else:
                    raise RuntimeError(f'Unsupported wheel layout: {path}')
            target = destination / path
            content = z.read(item)
            if target.exists() and target.read_bytes() != content:
                raise RuntimeError(f'Wheel file collision: {path}')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)


def dependencies(destination):
    queue = deque(Requirement(r) for r in ROOTS)
    resolved = {}
    processed_extras = {}
    report = []
    while queue:
        req = queue.popleft()
        name = canonicalize_name(req.name)
        if name in resolved:
            version, meta = resolved[name]
            if Version(version) not in req.specifier:
                raise RuntimeError(f'Conflicting dependency requirement: {req}, selected {version}')
        else:
            archive, file = wheel(req)
            with zipfile.ZipFile(archive) as z:
                matches = [n for n in z.namelist() if n.endswith('.dist-info/METADATA')]
                if len(matches) != 1:
                    raise RuntimeError(f'Invalid wheel metadata: {archive}')
                meta = message_from_bytes(z.read(matches[0]))
            version = meta['Version']
            resolved[name] = version, meta
            install_wheel(archive, destination)
            report.append({'name': meta['Name'], 'version': version, 'filename': archive.name,
                           'url': file['url'], 'sha256': file['digests']['sha256'],
                           'license': meta.get('License-Expression') or meta.get('License'),
                           'project_urls': meta.get_all('Project-URL') or []})
            print(f'Installed Windows wheel: {name} {version}', flush=True)
        extras = {''} | req.extras
        fresh = extras - processed_extras.get(name, set())
        processed_extras.setdefault(name, set()).update(extras)
        for value in meta.get_all('Requires-Dist') or []:
            child = Requirement(value)
            if fresh and (child.marker is None or any(child.marker.evaluate(dict(ENV, extra=e)) for e in fresh)):
                queue.append(child)
    return sorted(report, key=lambda item: item['name'].lower())


def trim_qt(qt):
    """Ship the Qt Widgets runtime and used plugins, not development tools/QML."""
    modules = {'QtCore', 'QtGui', 'QtWidgets'}
    libraries = {'qt6core.dll', 'qt6gui.dll', 'qt6widgets.dll', 'pyside6.abi3.dll',
                 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_codecvt_ids.dll',
                 'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll', 'opengl32sw.dll'}
    plugins = {'platforms/qwindows.dll', 'platforms/qoffscreen.dll', 'styles/qmodernwindowsstyle.dll',
               'styles/qwindowsvistastyle.dll', 'imageformats/qico.dll', 'imageformats/qjpeg.dll',
               'imageformats/qgif.dll'}
    for path in list(qt.iterdir()):
        if path.is_dir():
            if path.name not in {'support', 'plugins', 'translations'}:
                shutil.rmtree(path)
        elif ((path.suffix.lower() == '.dll' and path.name.lower() not in libraries)
              or path.suffix.lower() == '.exe'
              or (path.suffix in {'.pyd', '.pyi'} and path.stem not in modules)):
            path.unlink()
    for path in (qt / 'plugins').rglob('*'):
        if path.is_file() and path.relative_to(qt / 'plugins').as_posix() not in plugins:
            path.unlink()


def pe_machine(path):
    data = path.read_bytes()
    if data[:2] != b'MZ':
        raise ValueError(f'Not a Windows binary: {path}')
    offset = struct.unpack_from('<I', data, 0x3C)[0]
    if data[offset:offset + 4] != b'PE\0\0':
        raise ValueError(f'Invalid PE header: {path}')
    return struct.unpack_from('<H', data, offset + 4)[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--zig', required=True, type=Path, help='Path to Zig 0.17.0 executable')
    args = parser.parse_args()
    zig = args.zig.resolve()
    version = subprocess.check_output([str(zig), 'version'], text=True).strip()
    if version != '0.17.0':
        raise RuntimeError(f'Expected Zig 0.17.0, found {version}')
    build = ROOT / '.build/windows-embedded'
    build.mkdir(parents=True, exist_ok=True)
    output = ROOT / 'desktop/dist'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=build) as temporary:
        folder = Path(temporary) / 'QuadbandCardStudio'
        runtime = folder / 'runtime'
        runtime.mkdir(parents=True)
        url = f'https://www.python.org/ftp/python/{PYTHON}/python-{PYTHON}-embed-amd64.zip'
        python_archive = download(url, PYTHON_SHA)
        with zipfile.ZipFile(python_archive) as z:
            for item in z.infolist():
                path = member_path(item.filename)
                target = runtime / path
                target.parent.mkdir(parents=True, exist_ok=True)
                if not item.is_dir():
                    target.write_bytes(z.read(item))
        packages = runtime / 'Lib/site-packages'
        packages.mkdir(parents=True)
        report = dependencies(packages)
        trim_qt(packages / 'PySide6')
        (runtime / 'python313._pth').write_text('python313.zip\n.\nLib/site-packages\n../app\nimport site\n', encoding='utf-8')
        app = folder / 'app'
        shutil.copytree(ROOT / 'desktop/src/quadband', app / 'quadband',
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        shutil.copyfile(ROOT / 'desktop/packaging/windows/launch.py', app / 'launch.py')
        from PySide6.QtGui import QImage
        icon = Path(temporary) / 'Quadband.ico'
        if not QImage(str(app / 'quadband/assets/MidwestGadgets.png')).scaled(256, 256).save(str(icon)):
            raise RuntimeError('Failed to create Windows icon')
        rc = Path(temporary) / 'launcher.rc'
        rc.write_text('1 ICON "' + icon.as_posix() + '"\n', encoding='utf-8')
        resource = Path(temporary) / 'launcher.res'
        environment = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / '.build/zig-cache'))
        subprocess.run([str(zig), 'rc', '/fo', str(resource), str(rc)], check=True, env=environment)
        exe = folder / 'QuadbandCardStudio.exe'
        subprocess.run([str(zig), 'cc', '-target', 'x86_64-windows-gnu', '-municode', '-Wl,--subsystem,windows', '-Os', '-static',
                        str(ROOT / 'desktop/packaging/windows/launcher.c'), str(resource), '-o', str(exe)],
                       check=True, env=environment)
        for name in ('LICENSE', 'BRANDING.txt', 'THIRD_PARTY.txt'):
            shutil.copyfile(ROOT / name, folder / name)
        shutil.copyfile(ROOT / 'desktop/README.txt', folder / 'READER-SETUP.txt')
        (folder / 'START-HERE.txt').write_text(
            'Quadband Card Studio 1.0.1 by Midwest Gadgets\n\n'
            'Windows 11 x64 portable application.\n'
            '1. Extract ALL files from this ZIP to a local folder.\n'
            '2. Open QuadbandCardStudio.exe. No Python installation is required.\n'
            '3. Keep runtime and app beside the executable.\n\n'
            'Reader setup: READER-SETUP.txt. Close other reader apps before connecting.\n'
            'T5577 password writes are disabled. No physical cards were changed during the build.\n\n'
            'This package was assembled locally on macOS/Linux and has not yet been run on Windows.\n'
            'For an offline startup check, run QuadbandCardStudio.exe --smoke-test.\n'
            'If startup fails, see %LOCALAPPDATA%\\Midwest Gadgets\\Quadband Card Studio\\startup-error.txt.\n'
            'The app is unsigned. It does not install drivers or change system settings.\n\n'
            'Application source: Quadband-Card-Studio-source.zip (GPLv3).\n'
            'Third-party runtime source provision before public binary release: THIRD_PARTY.txt and docs/RELEASING.txt in source.\n',
            encoding='utf-8')
        (folder / 'dependencies.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        (folder / 'BUILD-INFO.json').write_text(json.dumps({'app_version': '1.0.1', 'target': 'Windows x64',
            'runtime': {'python': PYTHON, 'url': url, 'sha256': PYTHON_SHA}, 'launcher_compiler': 'Zig 0.17.0',
            'method': 'local cross-compiled launcher and official Windows runtime/wheels',
            'windows_execution_verified': False, 'hardware_verified': False}, indent=2), encoding='utf-8')
        # Include every application source from this checkout, including these build scripts.
        spec = importlib.util.spec_from_file_location('source_archive', ROOT / 'scripts/source-archive.py')
        archiver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(archiver)
        archiver.archive(folder / 'Quadband-Card-Studio-source.zip')
        binaries = [p for p in folder.rglob('*') if p.suffix.lower() in {'.dll', '.exe', '.pyd'}]
        for path in binaries:
            if pe_machine(path) != 0x8664:
                raise RuntimeError(f'Non-x64 binary: {path}')
        required = ['PySide6/plugins/platforms/qwindows.dll', 'PySide6/plugins/platforms/qoffscreen.dll',
                    'PySide6/QtWidgets.pyd', 'smartcard/scard', 'serial', 'bleak/backends/winrt', 'winrt']
        for name in required:
            if not (packages / name).exists():
                raise RuntimeError(f'Missing Windows runtime component: {name}')
        manifest = '\n'.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.relative_to(folder).as_posix()
                             for p in sorted(folder.rglob('*')) if p.is_file()) + '\n'
        (folder / 'SHA256SUMS.txt').write_text(manifest, encoding='utf-8')
        archive = shutil.make_archive(str(output / 'Quadband-Card-Studio-1.0.1-Windows-x64'), 'zip',
                                      root_dir=folder.parent, base_dir=folder.name)
        checksum = hashlib.sha256(Path(archive).read_bytes()).hexdigest()
        Path(archive + '.sha256').write_text(checksum + '  ' + Path(archive).name + '\n', encoding='utf-8')
        print(f'Validated {len(binaries)} Windows x64 binaries. Native Windows execution remains unverified.')
        print(archive)


if __name__ == '__main__':
    main()
