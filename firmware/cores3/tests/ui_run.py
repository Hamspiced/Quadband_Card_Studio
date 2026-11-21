#!/usr/bin/env python3
"""Compile the production UI controller, test it, and record its drawing surface."""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--png", action="store_true", help="Rasterize production command recordings with optional PySide6")
parser.add_argument("--font-file", type=Path, help="Legacy option; Studio Sans pixels are embedded in the recordings")
args = parser.parse_args()
build = root / ".build" / "ui"
build.mkdir(parents=True, exist_ok=True)
command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
           str(root / "main" / "ui" / "controller.cpp"), str(root / "main" / "ui" / "tools.cpp"),
           str(root / "main" / "ui" / "input.cpp"), str(root / "tests" / "ui_host.cpp"),
           "-o", str(build / "ui_tests")]
subprocess.run(command, check=True)
subprocess.run([str(build / "ui_tests"), str(build)], check=True)
buffered = [*command[:-2], str(root / "main/ui/ui.cpp"), str(root / "tests/repaint_host.cpp"),
            "-I" + str(root / "tests/ui_stubs"), "-o", str(build / "repaint_tests")]
buffered.remove(str(root / "tests/ui_host.cpp"))
subprocess.run(buffered, check=True)
subprocess.run([str(build / "repaint_tests")], check=True)
if args.png:
    import xml.etree.ElementTree as ET
    from PySide6.QtCore import QRect
    from PySide6.QtGui import QColor, QGuiApplication, QImage, QPainter
    app = QGuiApplication([])
    for path in sorted(build.glob("*.svg")):
        document = ET.parse(path).getroot()
        image = QImage(320, 240, QImage.Format_RGB32)
        image.fill(0)
        painter = QPainter(image)
        # Qt's SVG Tiny backend omits clipPath support. Replay the recorded operations
        # with actual software clipping instead of silently dropping dirty-region clips.
        ns = "{http://www.w3.org/2000/svg}"
        clips = {node.attrib["id"]: node[0] for node in document.findall(f".//{ns}clipPath")}
        for group in document.findall(f"{ns}g"):
            reference = group.attrib["clip-path"][5:-1]
            clip = clips[reference]
            painter.setClipRect(QRect(*(int(clip.attrib[k]) for k in ("x", "y", "width", "height"))))
            for operation in group:
                assert operation.tag == f"{ns}rect"
                painter.fillRect(QRect(*(int(operation.attrib[k]) for k in ("x", "y", "width", "height"))),
                                 QColor(operation.attrib["fill"]))
        painter.end()
        light = "-light" in path.stem
        accent = "#007c72" if light else "#00dfbd"
        canvas = "#f0f4f8" if light else "#060c14"
        surface = "#ffffff" if light else "#111f30"
        if path.stem.startswith("home-"):
            assert image.pixelColor(20, 170).name() == accent
            assert image.pixelColor(8, 166).name() == canvas
            assert image.pixelColor(176, 170).name() == surface
            assert image.pixelColor(0, 100).name() == canvas
            assert image.pixelColor(65, 60).name() != canvas
        if path.stem.startswith("connected-"):
            assert image.pixelColor(20, 195).name() == ("#ffe4e9" if light else "#321822")
        if path.stem.startswith("pairing-priority-"):
            assert image.pixelColor(32, 184).name() == surface
            assert image.pixelColor(180, 184).name() == accent
        assert image.save(str(path.with_suffix(".png"))), path
    print("Raster glyphs: shared Studio Sans antialiased coverage; no host-font substitution")
print(f"Production command evidence: {build}")
