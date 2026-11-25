#!/bin/sh
# Builds "Need for Speed Undercover Setup.app" in dist/: the Setup program plus its payload in Contents/Resources
# (the port's source, the built-in mods with their SHA-256 list, the speed profile) - no game files.
# Usage: ./make_setup_app.sh            (from this folder)
set -eu
cd "$(dirname "$0")"
# Source paths in the program are written relative to "~" (no personal folder names in the released app).
# (CARGO_ENCODED_RUSTFLAGS: flags separated by 0x1f, so a folder name with spaces stays one flag.)
CARGO_ENCODED_RUSTFLAGS="$(printf -- '--remap-path-prefix=%s=.\037--remap-path-prefix=%s=~' "$PWD" "$HOME")" \
    cargo build --release
NAME="Need for Speed Undercover Setup"
APP="dist/$NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp target/release/nfsu-setup "$APP/Contents/MacOS/Setup"
cp -c payload/source.tar.gz "$APP/Contents/Resources/" 2>/dev/null || cp payload/source.tar.gz "$APP/Contents/Resources/"
cp -c payload/mods_to_add.sha256 "$APP/Contents/Resources/" 2>/dev/null || cp payload/mods_to_add.sha256 "$APP/Contents/Resources/"
cp -cR payload/mods_to_add "$APP/Contents/Resources/" 2>/dev/null || cp -R payload/mods_to_add "$APP/Contents/Resources/"
for f in pgo/game.profdata rebased/PoliceCareer.asi; do
    mkdir -p "$APP/Contents/Resources/$(dirname "$f")"
    cp -c "payload/$f" "$APP/Contents/Resources/$f" 2>/dev/null || cp "payload/$f" "$APP/Contents/Resources/$f"
done
find "$APP" -name .DS_Store -delete
VERSION="$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -1)"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>Setup</string>
  <key>CFBundleIdentifier</key><string>io.github.nfsu-native-mac.setup</string>
  <key>CFBundleName</key><string>NFSU Setup</string>
  <key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>By DayvuJoness, made with Claude and Craft Anything. Based on the Need for Speed Carbon Setup app by vasist15-source.</string>
</dict></plist>
EOF
codesign --force --sign - "$APP"
echo "$APP ($(du -sh "$APP" | cut -f1))"
