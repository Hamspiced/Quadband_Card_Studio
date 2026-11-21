#!/usr/bin/env python3
"""Compile the complete brand mark into a bounded RGBA4444 flash asset."""
from pathlib import Path
from PySide6.QtCore import Qt
from PySide6.QtGui import QImage

root = Path(__file__).resolve().parents[3]
source = root / "Resources" / "MidwestGadgets.png"
target = root / "firmware" / "cores3" / "main" / "ui" / "logo.hpp"
image = QImage(str(source))
if image.isNull():
    raise SystemExit(f"Cannot load {source}")
image = image.scaled(112, 112, Qt.KeepAspectRatio, Qt.SmoothTransformation)
pixels = []
for y in range(image.height()):
    for x in range(image.width()):
        c = image.pixelColor(x, y)
        pixels.append((c.red() >> 4) << 12 | (c.green() >> 4) << 8 |
                      (c.blue() >> 4) << 4 | (c.alpha() >> 4))
lines = ["#pragma once", "#include <cstdint>", "", "namespace qb::ui {",
         f"inline constexpr int logo_width = {image.width()}, logo_height = {image.height()};",
         "inline constexpr uint16_t logo_rgba[] = {"]
lines += ["    " + ", ".join(f"0x{p:04x}" for p in pixels[i:i + 16]) + ","
          for i in range(0, len(pixels), 16)]
lines += ["};", "}", ""]
target.write_text("\n".join(lines))
print(f"{target}: {len(pixels) * 2} flash bytes; no framebuffer allocation")
