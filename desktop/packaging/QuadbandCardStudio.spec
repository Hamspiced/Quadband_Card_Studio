# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import sys
from PyInstaller.utils.hooks import collect_data_files, collect_submodules, copy_metadata

root = Path(SPECPATH).parents[1]
datas = collect_data_files('quadband')
for package in ['PySide6-Essentials', 'shiboken6', 'bleak', 'pyserial', 'hidapi', 'pyscard']:
    datas += copy_metadata(package)
hidden = collect_submodules('smartcard') + collect_submodules('serial.tools')
hidden += collect_submodules('bleak.backends.' + ('winrt' if sys.platform == 'win32' else 'bluezdb' if sys.platform.startswith('linux') else 'corebluetooth'))
a = Analysis([str(Path(SPECPATH) / 'entry.py')], pathex=[str(root / 'desktop' / 'src')],
             datas=datas, hiddenimports=hidden, excludes=['tkinter', 'PySide6.QtQml', 'PySide6.QtQuick', 'PySide6.QtWebEngineCore'])
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name='QuadbandCardStudio',
          debug=False, strip=False, upx=False, console=False,
          icon=str(Path(SPECPATH) / 'Quadband.ico') if sys.platform == 'win32' else None)
coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name='QuadbandCardStudio')
