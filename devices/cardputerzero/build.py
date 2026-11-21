#!/usr/bin/env python3
"""Build/simulate the CardputerZero application; never deploy or access readers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DEVICE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", action="store_true", help="Build Linux framebuffer executable")
    parser.add_argument("--zig", type=Path, help="Existing Zig compiler for static aarch64-linux-musl cross-build")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--smoke", action="store_true", help="Run host simulation only, no real endpoints")
    parser.add_argument("--cmake", default=shutil.which("cmake"))
    args = parser.parse_args()
    if not args.cmake:
        parser.error("CMake is required.")
    if args.zig and not args.device:
        parser.error("--zig requires --device")
    if args.zig and (args.test or args.smoke or args.sanitize):
        parser.error("Cross-build cannot claim host test/simulation/sanitizer execution.")
    lock = json.loads((DEVICE / "dependencies.lock").read_text())["lvgl"]
    dependency = ROOT / ".build/cardputerzero-deps/lvgl"
    if not dependency.exists():
        dependency.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--no-checkout", "--filter=blob:none", lock["url"], str(dependency)], check=True)
        subprocess.run(["git", "-C", str(dependency), "fetch", "--depth", "1", "origin", lock["revision"]], check=True)
        subprocess.run(["git", "-C", str(dependency), "checkout", "--detach", lock["revision"]], check=True)
    revision = subprocess.check_output(["git", "-C", str(dependency), "rev-parse", "HEAD"], text=True).strip()
    if revision != lock["revision"]:
        raise RuntimeError("LVGL checkout differs from dependency lock; refusing to overwrite it.")
    subprocess.run(["git", "--no-pager", "-C", str(dependency), "diff", "--exit-code", "HEAD", "--"], check=True)
    build = (args.build_dir or ROOT / (".build/cardputerzero-arm64" if args.zig else ".build/cardputerzero-host")).resolve()
    command = [args.cmake, "-S", str(DEVICE), "-B", str(build), "-G", "Ninja",
               "-DQB_LVGL_DIR=" + str(dependency), "-DQB_DESKTOP=" + ("OFF" if args.device else "ON"),
               "-DQB_SANITIZERS=" + ("ON" if args.sanitize else "OFF"),
               "-DCMAKE_BUILD_TYPE=" + ("Debug" if args.sanitize else "Release")]
    if args.zig:
        command += ["-DCMAKE_TOOLCHAIN_FILE=" + str(DEVICE / "cmake/zig-aarch64.cmake"),
                    "-DQB_ZIG_EXECUTABLE=" + str(args.zig.resolve()), "-DBUILD_TESTING=OFF"]
    env = dict(os.environ)
    if os.uname().sysname == "Darwin":
        try:
            directory = subprocess.check_output(["pkg-config", "--variable=libdir", "sdl3"], text=True).strip()
            if directory:
                env["DYLD_FALLBACK_LIBRARY_PATH"] = directory
        except (OSError, subprocess.CalledProcessError):
            pass
    subprocess.run(command, check=True, env=env)
    subprocess.run([args.cmake, "--build", str(build), "--parallel", "2" if args.zig else "4"], check=True, env=env)
    if args.test:
        ctest = str(Path(args.cmake).resolve().with_name("ctest"))
        subprocess.run([ctest, "--test-dir", str(build), "--output-on-failure"], check=True, env=env)
    if args.smoke:
        if args.device:
            parser.error("Smoke mode is only allowed for a host simulator build.")
        subprocess.run([str(build / "quadband-cardputerzero"), "--simulate", "--headless", "--seconds", "1",
                        "--screenshot", str(build / "simulator-home.ppm")], check=True, env=env, timeout=10)
    binary = build / "quadband-cardputerzero"
    sources = [p for p in DEVICE.rglob("*") if p.is_file() and
               not any(part in {".build", "build", "__pycache__"} for part in p.relative_to(DEVICE).parts)]
    sources += [ROOT / "firmware/cores3/main" / name for name in ("bridge.hpp", "bridge.cpp", "local_model.hpp")]
    sources += list((ROOT / "firmware/cores3/main/standalone").glob("*.hpp"))
    sources += [ROOT / "firmware/cores3/main/standalone" / name for name in
                ("controller.cpp", "hf.cpp", "ndef.cpp", "uhf.cpp", "lf.cpp")]
    info = {"version": "0.3.0", "target": "linux-arm64" if args.zig else "linux" if args.device else "host-simulator",
            "lvgl": lock, "hardwareValidated": False,
            "binary": {"bytes": binary.stat().st_size, "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()},
            "sources": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(sources)}}
    (build / "BUILD-INFO.json").write_text(json.dumps(info, indent=2) + "\n")
    print(f"Built {info['target']}: {binary} (no deployment)")


if __name__ == "__main__":
    main()
