#!/bin/bash
# Builds the update helper, puts it inside Octopus.app, signs both with their entitlements, and zips Octopus.tipa, the
# same file as Octopus.ipa, and SHA256SUMS, as Caz and Speaka do. Run it after building the app for device into
# build/DerivedData (see the workflow, or "Building and testing" in the README). Needs macOS with Xcode and ldid
# (brew install ldid).
set -euo pipefail
cd "$(dirname "$0")/.."

APP="build/DerivedData/Build/Products/Release-iphoneos/Octopus.app"
[ -d "$APP" ] || { echo "Octopus.app not found at $APP"; exit 1; }

mkdir -p build
xcrun --sdk iphoneos clang -arch arm64 -mios-version-min=17.0 -O2 -Wall \
  -framework CoreFoundation -o build/octopushelper Octopus/RootHelper/main.c
cp build/octopushelper "$APP/octopushelper"
rm -rf "$APP/_CodeSignature" "$APP/embedded.mobileprovision"

# The entitlements TrollStore keeps: the app may start the helper as root, and the helper may run TrollStore's installer.
ldid -SOctopus/RootHelper/entitlements.plist "$APP/octopushelper"
ldid -SOctopus/Octopus.entitlements "$APP/Octopus"
chmod 755 "$APP/Octopus" "$APP/octopushelper"

rm -rf build/package build/Octopus.tipa build/Octopus.ipa
mkdir -p build/package/Payload
cp -R "$APP" build/package/Payload/
(cd build/package && zip -qry ../Octopus.tipa Payload)
cp build/Octopus.tipa build/Octopus.ipa   # the same file; some tools only open .ipa
(cd build && shasum -a 256 Octopus.tipa Octopus.ipa > SHA256SUMS)
echo "Built build/Octopus.tipa and build/Octopus.ipa ($(du -h build/Octopus.tipa | cut -f1))"
