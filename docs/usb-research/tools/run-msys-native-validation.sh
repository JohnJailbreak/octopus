#!/usr/bin/env bash
# Run from the repository root in a workspace-local MSYS2 environment.
# This changes no production code or machine-wide setting. It temporarily mounts
# build/native-validation-tmp at MSYS /tmp with NTFS ACL support and restores it
# on exit. Do not run if /tmp is already an explicit mount.
set -u
export PATH=/usr/bin:$PATH
while IFS= read -r entry; do
    case "$entry" in *' on /tmp type '*) printf '%s\n' 'Refusing to replace an existing explicit /tmp mount.' >&2; exit 1;; esac
done < <(mount)
mkdir -p build/native-validation-tmp build/native-validation-logs
native_tmp=$(cygpath -w "$PWD/build/native-validation-tmp")
mount -o acl,binary,posix=1 "$native_tmp" /tmp || exit 1
trap 'umount /tmp' EXIT
export TMPDIR=/tmp TMP=/tmp TEMP=/tmp MSYS=winsymlinks:sys
make test > build/native-validation-logs/test.log 2>&1
test_result=$?
make analyze > build/native-validation-logs/analyze.log 2>&1
analyze_result=$?
make sanitize > build/native-validation-logs/sanitize.log 2>&1
sanitize_result=$?
make tsan > build/native-validation-logs/tsan.log 2>&1
tsan_result=$?
cc -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wvla -Wstrict-prototypes -Wmissing-prototypes -Werror build/plain/Core/*.o build/plain/Protocol/*.o Octopus/Tests/CoreTests/fuzz_responder.c -lpthread -o build/native-fuzz-plain > build/native-validation-logs/fuzz.log 2>&1
fuzz_result=$?
if test "$fuzz_result" = 0; then
    ./build/native-fuzz-plain 300000 >> build/native-validation-logs/fuzz.log 2>&1
    fuzz_result=$?
fi
printf 'test=%s analyze=%s sanitize=%s tsan=%s fuzz=%s\n' "$test_result" "$analyze_result" "$sanitize_result" "$tsan_result" "$fuzz_result"
# Expected unavailable sanitizer runtimes still produce nonzero status.
test "$test_result" = 0 && test "$analyze_result" = 0 && test "$sanitize_result" = 0 && test "$tsan_result" = 0 && test "$fuzz_result" = 0
