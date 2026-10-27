"""Inspect a Windows bundle without executing it. SPDX-License-Identifier: GPL-3.0-only"""
import argparse
import hashlib
import io
from pathlib import Path, PurePosixPath
import zipfile
import pefile

SYSTEM_DLLS = set('''advapi32.dll authz.dll bcrypt.dll comdlg32.dll crypt32.dll d3d11.dll
d3d12.dll d3d9.dll d3dcompiler_47.dll dnsapi.dll dwmapi.dll dwrite.dll dxgi.dll gdi32.dll
icuuc.dll imagehlp.dll imm32.dll iphlpapi.dll kernel32.dll mpr.dll mscms.dll ncrypt.dll
netapi32.dll ntdll.dll ole32.dll oleaut32.dll propsys.dll rpcrt4.dll secur32.dll setupapi.dll
shell32.dll shlwapi.dll uiautomationcore.dll user32.dll userenv.dll uxtheme.dll version.dll
winhttp.dll winmm.dll winscard.dll winspool.drv ws2_32.dll wtsapi32.dll'''.split())


def verify_manifest(z, prefix, manifest_name):
    expected = set()
    for line in z.read(prefix + manifest_name).decode().splitlines():
        digest, relative = line.split('  ', 1)
        path = PurePosixPath(relative)
        assert not path.is_absolute() and '..' not in path.parts, relative
        name = prefix + relative
        assert name not in expected, name
        expected.add(name)
        assert hashlib.sha256(z.read(name)).hexdigest() == digest, name
    actual = {name for name in z.namelist() if not name.endswith('/') and name != prefix + manifest_name}
    assert actual == expected, 'Manifest does not cover exactly the included files'


def verify(archive):
    prefix = 'QuadbandCardStudio/'
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        verify_manifest(z, prefix, 'SHA256SUMS.txt')
        binaries = [name for name in z.namelist() if name.lower().endswith(('.dll', '.exe', '.pyd'))]
        bundled = {Path(name).name.lower() for name in binaries}
        missing = set()
        for name in binaries:
            pe = pefile.PE(data=z.read(name), fast_load=True)
            assert pe.FILE_HEADER.Machine == 0x8664, name
            pe.parse_data_directories(directories=[1, 2, 13])
            for item in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []) + getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', []):
                dll = item.dll.decode().lower()
                if dll not in bundled | SYSTEM_DLLS and not dll.startswith(('api-ms-win-', 'ext-ms-win-')):
                    missing.add((name, dll))
            if name == prefix + 'QuadbandCardStudio.exe':
                assert pe.OPTIONAL_HEADER.Subsystem == 2, 'Launcher must use Windows GUI subsystem'
                assert {3, 14} <= {entry.id for entry in pe.DIRECTORY_ENTRY_RESOURCE.entries}, 'Icon missing'
        assert not missing, f'Missing non-system DLLs: {sorted(missing)}'
        assert z.read(prefix + 'runtime/python313._pth') == b'python313.zip\n.\nLib/site-packages\n../app\nimport site\n'
        with zipfile.ZipFile(io.BytesIO(z.read(prefix + 'Quadband-Card-Studio-source.zip'))) as source:
            verify_manifest(source, 'Quadband-Card-Studio/', 'SOURCE-MANIFEST.sha256')
            for name in z.namelist():
                if name.startswith(prefix + 'app/quadband/') and not name.endswith('/'):
                    original = 'Quadband-Card-Studio/desktop/src/' + name[len(prefix + 'app/'):]
                    assert z.read(name) == source.read(original), name
        print(f'PASS: checksums, included source, GUI launcher/icon, {len(binaries)} x64 binaries and DLL dependencies.')
        print('This is static validation, not a Windows execution or reader-hardware test.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('archive', type=Path)
    verify(parser.parse_args().archive)
