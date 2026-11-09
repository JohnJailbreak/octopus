#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'Native observer boundary tests require macOS Foundation and clang.' >&2
  exit 77
fi
build="$(mktemp -d "${TMPDIR:-/tmp}/octopus-observer-tests.XXXXXX")"
trap 'rm -rf "$build"' EXIT
cc="$(xcrun --find clang)"
common=(-g -O1 -Wall -Wextra -Werror -DOCT_NATIVE_TEST=1)
for unit in Octopus/Core/*.c Octopus/Protocol/*.c; do
  "$cc" "${common[@]}" -std=c11 -c "$unit" -o "$build/$(basename "$unit").o"
done
"$cc" "${common[@]}" -fobjc-arc -fblocks -framework Foundation \
  Octopus/Native/OctopusRuntime.m Octopus/Native/OctopusObserver.m \
  Octopus/Native/Tests/ObserverTests.m "$build"/*.o -o "$build/observer-tests"
"$build/observer-tests"
