#!/bin/sh
# Construit l'application « FM-1 Transfert » (envoi de morceaux au FM-1 par USB).
#   tracker/mac/build_app.sh  ->  tracker/mac/build/FM-1 Transfert.app
set -e
ICI="$(cd "$(dirname "$0")" && pwd)"
APP="$ICI/build/FM-1 Transfert.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
swiftc -O -swift-version 5 -parse-as-library -target arm64-apple-macos13 \
  -framework CoreMIDI -framework SwiftUI -framework AppKit \
  "$ICI/Transfert.swift" -o "$APP/Contents/MacOS/FM-1 Transfert"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>FM-1 Transfert</string>
  <key>CFBundleDisplayName</key><string>FM-1 Transfert</string>
  <key>CFBundleIdentifier</key><string>local.opl3tracker.fm1-transfert</string>
  <key>CFBundleExecutable</key><string>FM-1 Transfert</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign --force -s - "$APP"
echo "$APP"
