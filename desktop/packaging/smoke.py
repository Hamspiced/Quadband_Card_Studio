#!/usr/bin/env python3
"""Run the frozen app without touching readers. SPDX-License-Identifier: GPL-3.0-only"""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
exe = root/'dist/QuadbandCardStudio'/('QuadbandCardStudio.exe' if sys.platform == 'win32' else 'QuadbandCardStudio')
environment = dict(os.environ, QT_QPA_PLATFORM='offscreen')
subprocess.run([str(exe), '--smoke-test'], env=environment, check=True, timeout=30)
print('Frozen desktop startup/shutdown passed without reader access.')
