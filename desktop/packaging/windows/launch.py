"""Bundled Windows entry point. SPDX-License-Identifier: GPL-3.0-only"""
import ctypes
import os
from pathlib import Path
import sys
import traceback


def main():
    root = Path(__file__).resolve().parents[1]
    # Restrict DLL/Qt plugin discovery to the private, replaceable runtime.
    qt = root / 'runtime/Lib/site-packages/PySide6'
    os.environ['QT_PLUGIN_PATH'] = str(qt / 'plugins')
    os.environ['QT_QPA_PLATFORM_PLUGIN_PATH'] = str(qt / 'plugins/platforms')
    handles = [os.add_dll_directory(str(p)) for p in (root / 'runtime', qt) if p.is_dir()]
    try:
        if '--smoke-test' in sys.argv:
            os.environ['QT_QPA_PLATFORM'] = 'offscreen'
            # Exercise Windows-specific imports without enumerating or opening devices.
            import hid  # noqa: F401
            import smartcard.scard  # noqa: F401
            import serial.tools.list_ports_windows  # noqa: F401
            import bleak.backends.winrt.client  # noqa: F401
            import bleak.backends.winrt.scanner  # noqa: F401
        from quadband.app import main as run
        return run()
    finally:
        for handle in handles:
            handle.close()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception:
        detail = traceback.format_exc()
        base = Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'Midwest Gadgets/Quadband Card Studio'
        try:
            base.mkdir(parents=True, exist_ok=True)
            log = base / 'startup-error.txt'
            log.write_text(detail, encoding='utf-8')
            message = f'Quadband could not start. Details were saved to:\n{log}'
        except OSError:
            message = 'Quadband could not start. Extract the entire application ZIP and try again.'
        if '--smoke-test' not in sys.argv:
            ctypes.windll.user32.MessageBoxW(None, message, 'Quadband Card Studio', 0x10)
        raise SystemExit(1)
