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


# ---- The Objective-C side: OCT* functions shared between .m files ------------------------------
#
# The third failure of this kind was not a C symbol at all. OctopusObserver.m called
# OCTProviderSetScope, which was declared only in OctopusProvider.h, a header the observer does not
# import, so the observer did not compile; and the observer's test build stubs the provider rather
# than linking it, so declaring it alone would only have moved the failure to the linker. Both
# halves are checked here.

NATIVE = ROOT / "Octopus/Native"


def objc_compiled_by(script: str) -> list:
    """The .m files a script compiles, read from the script itself."""
    text = (ROOT / script).read_text()
    found = []
    for path in re.findall(r"(Octopus/Native/[\w/]+\.m)", text):
        if path not in found:
            found.append(path)
    return found


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def oct_calls(path: pathlib.Path) -> set:
    """OCT* functions a file calls (not methods, which start with a lowercase letter or a bracket)."""
    return set(re.findall(r"(?<![\w.])(OCT[A-Za-z0-9_]+)\s*\(", strip_comments(path.read_text())))


def oct_definitions(path: pathlib.Path) -> set:
    """OCT* functions a file defines: a return type, the name, a parameter list, then a body."""
    text = strip_comments(path.read_text())
    return set(re.findall(r"^[ \t]*(?:static\s+)?[A-Za-z_][\w \t\*]*?\b(OCT[A-Za-z0-9_]+)\s*\([^;{]*\)\s*\{",
                          text, flags=re.M))


def oct_declarations(path: pathlib.Path) -> set:
    """OCT* prototypes a header declares."""
    text = strip_comments(path.read_text())
    return set(re.findall(r"\b(OCT[A-Za-z0-9_]+)\s*\([^;{]*\)\s*;", text))


def headers_imported_by(path: pathlib.Path, seen=None) -> set:
    """Headers under Octopus/Native a file imports, followed transitively."""
    seen = set() if seen is None else seen
    for name in re.findall(r'#(?:import|include)\s+"([^"]+\.h)"', path.read_text()):
        header = (path.parent / name).resolve()
        if header.exists() and header not in seen and NATIVE in header.parents or header.parent == NATIVE:
            if header in seen:
                continue
            seen.add(header)
            headers_imported_by(header, seen)
    return seen


class ObjCLinks(unittest.TestCase):
    def test_every_oct_function_is_declared_where_it_is_called(self):
        # C99 and later reject an implicit declaration, so a call needs a prototype in scope: from
        # an imported header, or from a definition earlier in the same file.
        for path in sorted(NATIVE.glob("*.m")):
            with self.subTest(file=path.name):
                declared = set().union(*(oct_declarations(h) for h in headers_imported_by(path))) \
                    if headers_imported_by(path) else set()
                declared |= oct_definitions(path)
                missing = sorted(oct_calls(path) - declared)
                self.assertEqual(missing, [],
                    f"{path.name} calls {', '.join(missing)} with no prototype in scope. Declare it "
                    f"in a header {path.name} imports, as OctopusRuntime.h does for the provider.")

    def test_every_script_defines_each_oct_function_it_calls(self):
        # Within one build, every OCT* function called from a compiled .m must be defined by one of
        # them: the real implementation, or a test's stub standing in for a file it does not link.
        for script in LINKERS:
            with self.subTest(script=script):
                compiled = [ROOT / p for p in objc_compiled_by(script)]
                self.assertTrue(compiled, f"{script} compiles no Objective-C; the parse is broken")
                called = set().union(*(oct_calls(p) for p in compiled))
                defined = set().union(*(oct_definitions(p) for p in compiled))
                missing = sorted(called - defined)
                self.assertEqual(missing, [],
                    f"{script} calls {', '.join(missing)} but compiles no definition of it. Link the "
                    f"file that defines it, or give the test a stub as ObserverTests.m does for "
                    f"OCTProviderInstall.")

    def test_the_scan_finds_the_provider_entry_points(self):
        # Canary: the observer calls these; if the scan stops seeing them it has stopped working.
        calls = oct_calls(NATIVE / "OctopusObserver.m")
        for expected in ("OCTProviderInstall", "OCTProviderValidate"):
            self.assertIn(expected, calls)


if __name__ == "__main__":
    unittest.main()
