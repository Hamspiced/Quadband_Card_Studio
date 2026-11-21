#!/usr/bin/env python3
"""Create a public, source-only archive from an explicit allowlist. GPL-3.0-only."""
import argparse
import hashlib
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOP_FILES = ['Package.swift', 'README.rst', 'LICENSE', 'BRANDING.txt', 'THIRD_PARTY.txt',
             'CONTRIBUTING.txt', 'SECURITY.txt', '.gitignore']
DIRECTORIES = ['Sources', 'Tests', 'scripts', 'Resources', 'desktop', 'docs', '.github', 'firmware',
               'protocol', 'tests_release', 'webflasher', 'devices']
EXCLUDED = {'.build', '.git', 'dist', 'build', 'managed_components', '.pio', '__pycache__',
            '.pytest_cache', '.ruff_cache', '.venv', 'node_modules', 'playwright-report',
            'test-results', '.cache', '.vite', '.test-payload', '.test-site'}


def public_files(root=ROOT):
    files = [root / f for f in TOP_FILES if (root / f).is_file()]
    for folder in DIRECTORIES:
        pending = [root / folder] if (root / folder).is_dir() else []
        while pending:
            for path in pending.pop().iterdir():
                relative = path.relative_to(root)
                if (path.name in EXCLUDED or path.name.endswith('.egg-info')
                        or path.name == '.env' or path.name.startswith('.env.')):
                    continue
                if path.is_symlink():
                    raise RuntimeError(f'Unexpected symlink in source tree: {relative}')
                if path.is_dir():
                    pending.append(path)
                    continue
                if not path.is_file() or path.name == '.DS_Store' or path.suffix.lower() in ('.md', '.pyc', '.qcard', '.qlf', '.csv'):
                    continue
                if relative.as_posix() == 'Resources/acr1552u_rf_config.py':
                    continue  # Local reference: redistribution permission not established.
                if relative.parts[0] == 'firmware' and path.name in ('sdkconfig', 'sdkconfig.old', 'compile_commands.json'):
                    continue
                files.append(path)
    return sorted(set(files))


def archive(destination, root=ROOT):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    files = public_files(root)
    manifest = []
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as out:
        for path in files:
            relative = path.relative_to(root).as_posix()
            data = path.read_bytes()
            info = zipfile.ZipInfo('Quadband-Card-Studio/'+relative, (2026, 10, 27, 0, 0, 0))
            info.external_attr = (0o100755 if path.suffix == '.sh' else 0o100644) << 16
            out.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
            manifest.append(hashlib.sha256(data).hexdigest()+'  '+relative)
        out.writestr('Quadband-Card-Studio/SOURCE-MANIFEST.sha256', '\n'.join(manifest)+'\n')
    print(f'{len(files)} source/assets files: {destination}')
    return destination


if __name__ == '__main__':
    from release_metadata import application_version
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    archive(args.output or ROOT/'dist'/f'Quadband-Card-Studio-{application_version()}-source.zip')
