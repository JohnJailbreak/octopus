#!/usr/bin/env python3
"""Every C function the native Objective-C calls must be defined by a C file that gets linked.

Two CI failures came from this gap, both only visible on macOS and both after a successful
compile. The provider gained oct_native_lease_root and oct_readonly_store_open and the host test
script still named five C files, so the link failed; before that the arm64e build did not compile
Octopus/Native/*.c at all. Neither is a language error, so nothing on the Linux side noticed.

This checks it where it is cheap: find the oct_* functions referenced from the Objective-C, find
what each build and test script links, and require the one to cover the other. It reads the build
inputs rather than trusting a list, so adding a core dependency to the companion either works or
fails here, several minutes before a macOS runner would say so.
"""
import glob
import os
import pathlib
import re
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]

# Scripts that build or test the native component, and the globs each one links.
LINKERS = {
    "tools/build_native.sh": None,
    "tools/test_provider.sh": None,
    "tools/test_observer.sh": None,
}


def c_sources_linked_by(script: str) -> set:
    """The C files a script compiles, read out of its own `for unit in ...` lines."""
    text = (ROOT / script).read_text()
    linked = set()
    for line in text.splitlines():
        match = re.search(r"for source in (.+?); do|for unit in (.+?); do", line)
        if not match:
            continue
        for pattern in (match.group(1) or match.group(2)).split():
            if not pattern.endswith(".c"):
                continue
            for path in glob.glob(str(ROOT / pattern)):
                linked.add(os.path.relpath(path, ROOT))
    return linked


def defined_functions(sources) -> set:
    """Function names each C file defines, taken from nm so the test cannot drift from reality."""
    defined = set()
    for source in sources:
        obj = f"/tmp/octopus-link-check-{pathlib.Path(source).stem}.o"
        compiled = subprocess.run(
            ["cc", "-std=c11", f"-I{ROOT}", "-O0", "-c", str(ROOT / source), "-o", obj],
            capture_output=True,
        )
        if compiled.returncode != 0:
            continue  # a file this host cannot compile cannot be checked here
        listed = subprocess.run(["nm", "--defined-only", obj], capture_output=True, text=True)
        for line in listed.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "TtWw":
                defined.add(parts[2].lstrip("_"))
        if os.path.exists(obj):
            os.remove(obj)
    return defined


def functions_called_by_objc() -> set:
    """oct_* identifiers used as calls in the native Objective-C."""
    called = set()
    for path in (ROOT / "Octopus/Native").glob("*.m"):
        text = path.read_text()
        # Strip block comments so prose about a function is not read as a call.
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        called |= set(re.findall(r"\b(oct_[a-z0-9_]+)\s*\(", text))
    return called


class NativeLinks(unittest.TestCase):
    def setUp(self):
        self.called = functions_called_by_objc()
        self.assertTrue(self.called, "found no oct_* calls in the Objective-C; the scan is broken")

    def test_the_objc_calls_core_functions(self):
        # A canary: if these ever stop being found the scan has silently stopped working.
        for expected in ("oct_native_lease_root", "oct_readonly_store_open", "oct_store_read"):
            self.assertIn(expected, self.called)

    def test_every_script_links_what_the_objc_calls(self):
        for script in LINKERS:
            with self.subTest(script=script):
                sources = c_sources_linked_by(script)
                self.assertTrue(sources, f"{script} links no C sources; the parse is broken")
                defined = defined_functions(sources)
                missing = sorted(name for name in self.called if name not in defined)
                self.assertEqual(
                    missing, [],
                    f"{script} does not link a definition of: {', '.join(missing)}. "
                    f"Add the file that defines them to that script's source list.",
                )

    def test_scripts_glob_rather_than_list(self):
        # A fixed list is what broke twice; a glob picks up a new core file on its own.
        for script in LINKERS:
            with self.subTest(script=script):
                text = (ROOT / script).read_text()
                self.assertIn("Octopus/Core/*.c", text)
                self.assertIn("Octopus/Native/*.c", text)


if __name__ == "__main__":
    unittest.main()
