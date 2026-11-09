#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build="$(mktemp -d "${TMPDIR:-/tmp}/octopus-dataset-tests.XXXXXX")"
trap 'rm -rf "$build"' EXIT
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror \
  Octopus/Native/oct_native_dataset.c Octopus/Native/Tests/DatasetTests.c \
  -o "$build/dataset-tests"
"$build/dataset-tests"
