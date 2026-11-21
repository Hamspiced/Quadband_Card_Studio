#!/usr/bin/env python3
"""Validate desktop versions and exact checked-out release provenance."""
import argparse
import ast
import json
from pathlib import Path
import plistlib
import re
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
VERSION = r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
TAG = re.compile(rf'v({VERSION})(?:-([0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*))?')
LEGACY = {'v1.01': '1.0.1', 'v1.02': '1.0.2'}


def tag_version(tag):
    if tag in LEGACY:
        return LEGACY[tag], False
    match = TAG.fullmatch(tag)
    if not match:
        raise ValueError('Use a canonical vX.Y.Z[-prerelease] tag or legacy v1.01/v1.02.')
    prerelease = match.group(2)
    if prerelease and any(part.isdigit() and len(part) > 1 and part[0] == '0'
                          for part in prerelease.split('.')):
        raise ValueError('Numeric prerelease identifiers must not have leading zeroes.')
    return match.group(1), bool(prerelease)


def application_version(root=ROOT):
    version = tomllib.loads((root/'desktop/pyproject.toml').read_text())['project']['version']
    if not re.fullmatch(VERSION, version):
        raise ValueError('Desktop version must be a numeric semantic version.')
    tree = ast.parse((root/'desktop/src/quadband/__init__.py').read_text())
    versions = [ast.literal_eval(node.value) for node in tree.body
                if isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name) and target.id == '__version__'
                        for target in node.targets)]
    with (root/'Resources/Info.plist').open('rb') as stream:
        bundle = plistlib.load(stream)['CFBundleShortVersionString']
    if versions != [version] or bundle != version:
        raise ValueError('pyproject.toml, Python __version__ and Info.plist versions must agree.')
    return version


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def release_metadata(root=ROOT, release_tag='', commit=None):
    version = application_version(root)
    current = git(root, 'rev-parse', 'HEAD')
    if not re.fullmatch(r'[0-9a-f]{40}', current):
        raise ValueError('Expected a full SHA-1 checked-out commit.')
    if commit is not None and (not re.fullmatch(r'[0-9a-f]{40}', commit) or commit != current):
        raise ValueError('Requested commit must be the full current checked-out commit.')
    prerelease = False
    if release_tag:
        expected, prerelease = tag_version(release_tag)
        if expected != version:
            raise ValueError(f'Tag {release_tag} does not match application version {version}.')
        tagged = git(root, 'rev-parse', '--verify', f'refs/tags/{release_tag}^{{commit}}')
        if tagged != current:
            raise ValueError('Release tag must resolve to the current checked-out commit.')
    return {
        'tag': release_tag, 'version': version, 'commit': current, 'prerelease': prerelease,
        'macos': f'Quadband-Card-Studio-{version}-macOS-universal.zip',
        'windows': f'Quadband-Card-Studio-{version}-Windows-x64.zip',
        'source': f'Quadband-Card-Studio-{version}-source.zip',
        'firmware': f'Quadband-CoreS3-{release_tag or "development"}.zip',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release-tag', default='')
    parser.add_argument('--commit')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--field', choices=['version', 'commit', 'tag', 'macos', 'windows', 'source', 'firmware'])
    parser.add_argument('--github-output', type=Path)
    args = parser.parse_args()
    try:
        if (args.field == 'version' and not args.release_tag and args.commit is None
                and not args.json and not args.github_output):
            print(application_version())
            return
        result = release_metadata(release_tag=args.release_tag, commit=args.commit)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
    if args.github_output:
        with args.github_output.open('a', encoding='utf-8') as stream:
            for key, value in result.items():
                stream.write(f'{key}={str(value).lower() if isinstance(value, bool) else value}\n')
    print(json.dumps(result, sort_keys=True) if args.json else result[args.field or 'version'])


if __name__ == '__main__':
    main()
