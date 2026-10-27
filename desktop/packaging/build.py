#!/usr/bin/env python3
"""Build a native portable directory package. SPDX-License-Identifier: GPL-3.0-only"""
import argparse
import importlib.metadata as metadata
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
DESKTOP = ROOT/'desktop'


def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def notices(destination):
    folder = destination/'licenses'
    folder.mkdir(exist_ok=True)
    report = []
    for dist in metadata.distributions():
        name = dist.metadata.get('Name', 'unknown')
        report.append({'name': name, 'version': dist.version, 'license': dist.metadata.get('License-Expression') or dist.metadata.get('License'),
                       'project_urls': dist.metadata.get_all('Project-URL') or [], 'homepage': dist.metadata.get('Home-page')})
        for file in dist.files or []:
            if any(token in file.name.lower() for token in ['license', 'copying', 'notice', 'copyright']) and file.suffix.lower() in ('', '.txt', '.md', '.rst', '.html'):
                source = Path(dist.locate_file(file))
                if source.is_file() and source.stat().st_size < 10_000_000:
                    # Retain relative paths; never execute a dependency's notice files.
                    relative = Path(*[p for p in file.parts if p not in ('..', '.')])
                    target = folder/name/relative
                    if target.suffix.lower() == '.md':
                        target = target.with_suffix('.txt')
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source, target)
    (destination/'dependencies.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (destination/'BUILD-INFO.json').write_text(json.dumps({'platform': platform.platform(), 'machine': platform.machine(),
            'python': sys.version, 'app_version': '1.0.0'}, indent=2), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--allow-macos-smoke', action='store_true', help='Developer packaging check only; native Mac app remains the supported Mac build.')
    args = parser.parse_args()
    if sys.platform not in ('win32', 'linux') and not args.allow_macos_smoke:
        parser.error('Build on Windows or Linux. PyInstaller does not cross-compile desktop binaries.')
    if sys.platform == 'win32':
        # Qt writes the .ico from the supplied original image; no extra imaging dependency.
        from PySide6.QtGui import QImage
        if not QImage(str(DESKTOP/'src/quadband/assets/MidwestGadgets.png')).scaled(256, 256).save(str(DESKTOP/'packaging/Quadband.ico')):
            raise RuntimeError('Could not create Windows icon')
    run(sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--distpath', DESKTOP/'dist',
        '--workpath', ROOT/'.build/pyinstaller', DESKTOP/'packaging/QuadbandCardStudio.spec')
    destination = DESKTOP/'dist/QuadbandCardStudio'
    for name in ['LICENSE', 'BRANDING.txt', 'THIRD_PARTY.txt']:
        shutil.copyfile(ROOT/name, destination/name)
    shutil.copyfile(DESKTOP/'README.txt', destination/'START-HERE.txt')
    notices(destination)
    run(sys.executable, ROOT/'scripts/source-archive.py', '--output', destination/'Quadband-Card-Studio-source.zip')
    if sys.platform == 'linux':
        for name in ['70-quadband.rules', 'quadband-card-studio.desktop']:
            shutil.copyfile(DESKTOP/'packaging'/name, destination/name)
    label = 'Windows' if sys.platform == 'win32' else 'Linux' if sys.platform == 'linux' else 'macOS-PORT-SMOKE-ONLY'
    extension = 'zip' if sys.platform == 'win32' else 'gztar'
    output = shutil.make_archive(str(DESKTOP/'dist'/f'Quadband-Card-Studio-1.0.0-{label}-{platform.machine()}'),
                                extension, root_dir=destination.parent, base_dir=destination.name)
    print(output)


if __name__ == '__main__':
    main()
