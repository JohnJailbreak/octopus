#!/bin/bash
# Build the optional iOS 17.0 arm64e companion. Never installs or restarts ptpd.
set -euo pipefail
cd "$(dirname "$0")/.."
version="${1:?Usage: bash tools/build_native.sh PACKAGE_VERSION}"
[[ "$(uname -s)" == Darwin ]] || { echo "Native companion requires macOS and the Xcode iPhoneOS SDK" >&2; exit 1; }
command -v ldid >/dev/null || { echo "ldid is required (brew install ldid)" >&2; exit 1; }
sdk="$(xcrun --sdk iphoneos --show-sdk-path)"
out=build/native
mkdir -p "$out/objects"
flags=(-arch arm64e -isysroot "$sdk" -miphoneos-version-min=17.0 -O2 -g -fPIC -Wall -Wextra -Werror)
objects=()
# Use the same C implementations as the app. oct_link's clock references its service
# implementation; link the complete core so dependency changes cannot silently fork it.
for source in Octopus/Core/*.c Octopus/Protocol/*.c Octopus/Native/*.c; do
  object="$out/objects/$(basename "${source%.c}").o"
  xcrun --sdk iphoneos clang "${flags[@]}" -std=c11 -c "$source" -o "$object"
  objects+=("$object")
done
for source in Octopus/Native/OctopusRuntime.m Octopus/Native/OctopusObserver.m Octopus/Native/OctopusProvider.m; do
  object="$out/objects/$(basename "${source%.m}").o"
  xcrun --sdk iphoneos clang "${flags[@]}" -fobjc-arc -fblocks -c "$source" -o "$object"
  objects+=("$object")
done
xcrun --sdk iphoneos clang "${flags[@]}" -dynamiclib "${objects[@]}" \
  -framework Foundation -framework CoreFoundation \
  -Wl,-install_name,/var/jb/usr/lib/TweakInject/OctopusNative.dylib \
  -o "$out/OctopusNative.dylib"
ldid -S "$out/OctopusNative.dylib"
python3 tools/package_native.py --dylib "$out/OctopusNative.dylib" --version "$version" \
  --output "$out/com.parv.octopus.native_${version}_iphoneos-arm64.deb"
echo "Built optional companion in $out; no device was changed."
