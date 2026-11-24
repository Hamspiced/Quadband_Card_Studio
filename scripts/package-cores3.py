"""Package an already-built CoreS3 application without accessing a device."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def notices(source, destination):
    if not source.is_dir():
        return
    for directory, folders, names in os.walk(source, followlinks=False):
        folders[:] = [name for name in folders if name not in {'.git', '.build', 'build', '__pycache__'}]
        for name in names:
            path = Path(directory)/name
            if (path.is_symlink() or not name.lower().startswith(('license', 'copying', 'notice', 'copyright'))
                    or path.suffix.lower() not in ('', '.txt', '.md', '.rst', '.html')
                    or path.stat().st_size > 2_000_000):
                continue
            target = destination/path.relative_to(source)
            if target.suffix.lower() == '.md':
                target = target.with_suffix('.txt')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)


def package(output):
    firmware = ROOT/'firmware/cores3'
    build = firmware/'.build'
    provenance = json.loads((build/'artifacts.json').read_text())
    version = json.loads((firmware/'firmware-manifest.json').read_text())['version']
    for name, digest in provenance['source'].items():
        if hashlib.sha256((firmware/name).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'Firmware source changed since build: {name}')
    for name, item in provenance['artifacts'].items():
        data = (build/'idf'/name).read_bytes()
        if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
            raise RuntimeError(f'Firmware artifact differs from build provenance: {name}')
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='cores3-package-') as temporary:
        folder = Path(temporary)/'Quadband-CoreS3'
        folder.mkdir()
        (folder/'START-HERE.txt').write_text(
            'Quadband CoreS3 development firmware\n\n'
            'This package has been compiled and simulated, not hardware-verified.\n'
            'Nothing in the build or packaging process flashes a device.\n'
            'Use only the confirmed M5Stack CoreS3 ESP32-S3 target.\n\n'
            f'Firmware {version} boots to Home. Choose Connected for desktop readers,\n'
            'or Standalone for local Grove HF/NFC, UHF and Ultra LF tools.\n'
            'Writes require explicit target review and read-back verification.\n'
            'Local results/source captures are RAM-only; SD/file services are unavailable.\n'
            'Pairing, USB authorization and bond reset are explicit Settings actions.\n\n'
            'Settings > Ultra USB / Bluetooth selects one Chameleon transport.\n'
            'Bluetooth requires explicit scan/peer selection and local passkey entry.\n'
            'Desktop bridge pairing and Ultra pairing are separate roles; forgetting\n'
            'the desktop bond preserves the selected Ultra bond. No automatic transport\n'
            'fallback or command replay is performed. Unverified cleanup blocks reuse.\n\n'
            'The application BIN is not a merged image and must NOT be flashed at offset zero.\n'
            'Inspect flash_args and flasher_args.json for the exact split-file offsets.\n'
            'USB host output starts OFF: disconnect the programming PC and use appropriate\n'
            'independent power before authorizing host mode locally.\n\n'
            'Source.zip contains the complete application/firmware/protocol tree. Extract it\n'
            'for correctly rooted documentation links and reproducible build instructions.\n'
            'BUILD-INFO.json records source/artifact hashes and the pinned SDK/dependencies.\n'
            'SHA256SUMS.txt covers the packaged files. Preserve licenses and notices.\n',
            encoding='utf-8')
        for name in provenance['artifacts']:
            if name.endswith('.elf'):
                continue
            target = folder/name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(build/'idf'/name, target)
        for source, name in [(firmware/'README.rst', 'README.rst'), (firmware/'dependencies.lock', 'dependencies.lock'),
                             (firmware/'main/ui/LICENSE-DejaVu.txt', 'LICENSE-StudioSans.txt'),
                             (build/'artifacts.json', 'BUILD-INFO.json'), (ROOT/'LICENSE', 'LICENSE'),
                             (ROOT/'BRANDING.txt', 'BRANDING.txt'), (ROOT/'THIRD_PARTY.txt', 'THIRD_PARTY.txt'),
                             (ROOT/'docs/CORES3.rst', 'CORES3-SETUP.rst')]:
            shutil.copyfile(source, folder/name)
        for dependency in provenance['dependencies']:
            notices(build/'deps'/dependency, folder/'licenses'/dependency)
        sdk = Path(os.environ['IDF_PATH']) if os.environ.get('IDF_PATH') else None
        if sdk:
            revision = provenance['idfRevision']
            actual = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
            if actual != revision:
                raise RuntimeError('Activated SDK does not match firmware provenance.')
            notices(sdk, folder/'licenses/ESP-IDF')
        else:
            raise RuntimeError('Activate the matching ESP-IDF SDK to gather its required notices.')
        spec = importlib.util.spec_from_file_location('source_archive', ROOT/'scripts/source-archive.py')
        archiver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(archiver)
        archiver.archive(folder/'Source.zip')
        files = sorted(p for p in folder.rglob('*') if p.is_file())
        if any(p.suffix.lower() == '.md' for p in files):
            raise RuntimeError('Unexpected Markdown file in firmware package.')
        checksums = ''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.relative_to(folder).as_posix()+'\n'
                            for p in files)
        (folder/'SHA256SUMS.txt').write_text(checksums)
        staging = output.with_suffix(output.suffix+'.new')
        with zipfile.ZipFile(staging, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(folder.rglob('*')):
                if path.is_file():
                    archive.write(path, 'Quadband-CoreS3/'+path.relative_to(folder).as_posix())
        staging.replace(output)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix+'.sha256').write_text(digest+'  '+output.name+'\n')
    print(f'Packaged verified firmware, source and notices: {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT/'dist/Quadband-CoreS3-development.zip')
    package(parser.parse_args().output)
