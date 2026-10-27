#!/usr/bin/env python3
"""Create a public, source-only archive from an explicit allowlist. GPL-3.0-only."""
import argparse
import hashlib
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOP_FILES = ['Package.swift', 'README.rst', 'LICENSE', 'BRANDING.txt', 'THIRD_PARTY.txt',
             'CONTRIBUTING.txt', 'SECURITY.txt', '.gitignore']
DIRECTORIES = ['Sources', 'Tests', 'scripts', 'Resources', 'desktop', 'docs', '.github']
EXCLUDED = {'.build', 'dist', 'build', '__pycache__', '.pytest_cache', '.ruff_cache', '.venv', 'node_modules'}


def public_files(root=ROOT):
    files = [root / f for f in TOP_FILES if (root / f).is_file()]
    for folder in DIRECTORIES:
        for path in (root / folder).rglob('*'):
            relative = path.relative_to(root)
            if any(p in EXCLUDED or p.endswith('.egg-info') for p in relative.parts):
                continue
            if path.is_symlink():
                raise RuntimeError(f'Unexpected symlink in source tree: {relative}')
            if not path.is_file() or path.name == '.DS_Store' or path.suffix.lower() in ('.md', '.pyc', '.qcard', '.qlf', '.csv'):
                continue
            if relative.as_posix() == 'Resources/acr1552u_rf_config.py':
                continue  # Local reference: redistribution permission not established.
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
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT/'dist'/'Quadband-Card-Studio-1.0.2-source.zip')
    archive(parser.parse_args().output)
