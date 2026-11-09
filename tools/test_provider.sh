#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'Native provider boundary tests require macOS Foundation and clang.' >&2
  exit 77
fi
build="$(mktemp -d "${TMPDIR:-/tmp}/octopus-provider-tests.XXXXXX")"
trap 'rm -rf "$build"' EXIT
cc="$(xcrun --find clang)"
common=(-g -O1 -Wall -Wextra -Werror -DOCT_NATIVE_TEST=1)
for unit in Octopus/Core/oct_store.c Octopus/Core/oct_backend_memory.c Octopus/Protocol/oct_ptp.c Octopus/Protocol/oct_responder.c; do
  "$cc" "${common[@]}" -std=c11 -c "$unit" -o "$build/$(basename "$unit").o"
done
"$cc" "${common[@]}" -fobjc-arc -fblocks -framework Foundation \
  Octopus/Native/OctopusRuntime.m Octopus/Native/OctopusProvider.m \
  Octopus/Native/Tests/ProviderTests.m "$build"/*.o -o "$build/provider-tests"
"$build/provider-tests"
