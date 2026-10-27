#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
export CLANG_MODULE_CACHE_PATH="$PWD/.build/ModuleCache"
export SWIFTPM_MODULECACHE_OVERRIDE="$PWD/.build/ModuleCache"
mkdir -p .build/ModuleCache .build/cache .build/config .build/security
BUILD_ARGS=(-c release --arch arm64 --arch x86_64 --disable-sandbox -debug-info-format none
  --cache-path "$PWD/.build/cache" --config-path "$PWD/.build/config" --security-path "$PWD/.build/security")
swift build "${BUILD_ARGS[@]}"
BIN_DIR=$(swift build "${BUILD_ARGS[@]}" --show-bin-path)
APP="$PWD/dist/Quadband Card Studio.app"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN_DIR/MagStripeStudio" "$APP/Contents/MacOS/MagStripeStudio.new"
mv -f "$APP/Contents/MacOS/MagStripeStudio.new" "$APP/Contents/MacOS/MagStripeStudio"
# Copy artwork without Dropbox/Finder metadata, which invalidates a code signature.
rm -f "$APP/Contents/Resources/MidwestGadgets.png"
cp -X Resources/MidwestGadgets.png "$APP/Contents/Resources/"
# The supplied reference script has no established redistribution license.
# Remove a stale private copy from previous local builds; independent controls remain built in.
rm -f "$APP/Contents/Resources/acr1552u_rf_config.py"
cp -X LICENSE "$APP/Contents/Resources/LICENSE.txt"
cp -X BRANDING.txt "$APP/Contents/Resources/BRANDING.txt"
python3 scripts/source-archive.py
cp -X dist/Quadband-Card-Studio-1.0.1-source.zip "$APP/Contents/Resources/Source.zip"
cp Resources/Info.plist "$APP/Contents/Info.plist"
if [ -f Resources/AppIcon.icns ]; then cp Resources/AppIcon.icns "$APP/Contents/Resources/"; fi
codesign --force --sign - "$APP"
codesign --verify --deep --strict "$APP"
ditto -c -k --keepParent "$APP" "$PWD/dist/Quadband-Card-Studio-macOS.zip"
printf 'Built %s\n' "$APP"
