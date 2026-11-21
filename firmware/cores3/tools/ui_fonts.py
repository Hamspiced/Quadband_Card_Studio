#!/usr/bin/env python3
"""Build Studio Sans coverage glyphs from the pinned M5GFX DejaVu assets."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = root / ".build/deps/M5GFX/src/lgfx/Fonts/Custom"
lines = ["#pragma once", "#include <cstdint>", "", "namespace qb::ui::typeface {",
         "// Studio Sans: resampled DejaVu Sans; see LICENSE-DejaVu.txt.",
         "struct Glyph { uint16_t offset; uint8_t advance; };"]
for name, original in [("small", "DejaVu24"), ("large", "DejaVu40")]:
    data = (source / (original + ".h")).read_text()
    bitmap = bytes(int(v, 16) for v in re.findall(
        r"0x([0-9a-fA-F]{2})", data.split("Bitmaps[] PROGMEM = {")[1].split("};")[0]))
    glyphs = [tuple(map(int, values)) for values in re.findall(
        r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", data)]
    assert len(glyphs) == 95
    top = min(g[5] for g in glyphs)
    height = (max(g[5] + g[2] for g in glyphs) - top + 1) // 2
    pixels, metrics = [], []
    for offset, width, gh, advance, left, yoff in glyphs:
        cell = (max(advance, left + width + 1) + 1) // 2
        metrics.append((len(pixels), cell))
        for y in range(height):
            for x in range(cell):
                coverage = 0
                for sy in range(2):
                    for sx in range(2):
                        gx, gy = x * 2 + sx - 1 - left, y * 2 + sy + top - yoff
                        if 0 <= gx < width and 0 <= gy < gh:
                            bit = gy * width + gx
                            coverage += bool(bitmap[offset + bit // 8] & (0x80 >> (bit % 8)))
                pixels.append((coverage * 255 + 2) // 4)
    assert len(pixels) < 65536
    lines += [f"inline constexpr int {name}_height = {height};",
              f"inline constexpr Glyph {name}_glyphs[] = {{"]
    lines += ["    " + ", ".join(f"{{{at},{width}}}" for at, width in metrics[i:i+8]) + ","
              for i in range(0, len(metrics), 8)]
    lines += ["};", f"inline constexpr uint8_t {name}_pixels[] = {{"]
    lines += ["    " + ",".join(map(str, pixels[i:i+32])) + "," for i in range(0, len(pixels), 32)]
    lines += ["};"]
lines += ["}", ""]
target = root / "main/ui/typeface.hpp"
target.write_text("\n".join(lines))
print(f"Generated Studio Sans coverage assets: {target}")
