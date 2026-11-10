#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'Native provider boundary tests require macOS Foundation and clang.' >&2
  exit 77
fi
build="$(mktemp -d "${TMPDIR:-/tmp}/octopus-provider-tests.XXXXXX")"
trap 'rm -rf "$build"' EXIT
# `xcrun --find clang` only returns the binary's path; running that path directly sets no SDK, so
# clang has no sysroot and can't find stdlib.h. `xcrun --sdk macosx clang` runs it with the macOS
# SDK selected, which these host-side tests need for libc and Foundation.
cc=(xcrun --sdk macosx clang)
common=(-g -O1 -Wall -Wextra -Werror -DOCT_NATIVE_TEST=1)
# Every core, protocol and native C file, rather than a list that has to be remembered. Naming
# five files here is what broke the build when the provider gained two new dependencies
# (oct_native_lease_root and oct_readonly_store_open): the compile succeeded and the link did not.
for unit in Octopus/Core/*.c Octopus/Protocol/*.c Octopus/Native/*.c; do
  "${cc[@]}" "${common[@]}" -std=c11 -c "$unit" -o "$build/$(basename "$unit").o"
done
"${cc[@]}" "${common[@]}" -fobjc-arc -fblocks -framework Foundation \
  Octopus/Native/OctopusRuntime.m Octopus/Native/OctopusProvider.m \
  Octopus/Native/Tests/ProviderTests.m "$build"/*.o -o "$build/provider-tests"
"$build/provider-tests"
