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
    # A block comment becomes the line breaks it contained, so line numbers in messages still point
    # at the real line rather than one shifted up by every comment above it.
    text = re.sub(r"/\*.*?\*/", lambda m: " " + "\n" * m.group(0).count("\n"), text, flags=re.S)
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


# ---- Stand-in encodings must match what was recorded from the device ---------------------------
#
# The provider and observer tests run against stand-ins for Apple's classes, and OCTRuntimeHook
# refuses to hook a method whose type encoding differs from the one recorded from ptpd. A stand-in
# declared with the wrong type is therefore not hooked at all, and the test then exercises the
# stand-in rather than the provider. That happened with appendObjectData:...: the device records
# its content type as an unsigned int, I32, and the stand-in declared an int, i32, so the upload
# hook never installed and the routing assertion failed on CI. Checked here, from the declarations.

ENCODINGS = None


def recorded_encodings():
    global ENCODINGS
    if ENCODINGS is None:
        import json
        data = json.loads((ROOT / "docs/usb-research/evidence/ptpd/method-encodings.json").read_text())
        items = data if isinstance(data, list) else data.get("methods", [])
        ENCODINGS = {}
        for item in items:
            if isinstance(item, dict) and item.get("selector"):
                ENCODINGS.setdefault(item["selector"], set()).add(item.get("encoding"))
    return ENCODINGS


# Objective-C type codes and their sizes in a method encoding on arm64. Arguments narrower than an
# int occupy an int's slot, which is why an unsigned short at 24 is followed by the next at 28.
TYPES = {
    "id": ("@", 8), "instancetype": ("@", 8), "NSData *": ("@", 8), "NSString *": ("@", 8),
    "NSArray *": ("@", 8), "SEL": (":", 8), "void": ("v", 0), "BOOL": ("B", 4), "bool": ("B", 4),
    "unsigned long long": ("Q", 8), "uint64_t": ("Q", 8), "long long": ("q", 8),
    "unsigned int": ("I", 4), "uint32_t": ("I", 4), "int": ("i", 4), "int32_t": ("i", 4),
    "unsigned short": ("S", 4), "uint16_t": ("S", 4), "short": ("s", 4),
    "unsigned short *": ("^S", 8), "uint16_t *": ("^S", 8), "uint32_t *": ("^I", 8),
    "unsigned int *": ("^I", 8), "int64_t": ("q", 8), "void *": ("^v", 8),
}


def encode(return_type, argument_types):
    """The encoding clang emits for a method: return code, frame size, then each argument."""
    code, _ = TYPES[return_type]
    offset, parts = 16, ["@0", ":8"]     # self, then _cmd
    for argument in argument_types:
        arg_code, size = TYPES[argument]
        parts.append(f"{arg_code}{offset}")
        offset += size
    return f"{code}{offset}" + "".join(parts)


def stand_in_methods(path):
    """(selector, return type, argument types) for each method a test's @interface declares."""
    text = strip_comments(path.read_text())
    found = []
    for interface in re.findall(r"@interface\s+\w+[^\n]*\n(.*?)@end", text, flags=re.S):
        for declaration in re.findall(r"^-\s*\(([^)]+)\)\s*([^;]+);", interface, flags=re.M):
            return_type, rest = declaration[0].strip(), declaration[1]
            pieces = re.findall(r"(\w+):\s*\(([^)]+)\)\s*\w+", rest)
            if pieces:
                selector = "".join(f"{name}:" for name, _ in pieces)
                arguments = [kind.strip() for _, kind in pieces]
            else:
                selector, arguments = rest.strip(), []
            found.append((selector, return_type, arguments))
    return found


class StandInEncodings(unittest.TestCase):
    def test_every_stand_in_matches_the_device(self):
        recorded = recorded_encodings()
        for test in ("Octopus/Native/Tests/ProviderTests.m", "Octopus/Native/Tests/ObserverTests.m"):
            for selector, return_type, arguments in stand_in_methods(ROOT / test):
                if selector not in recorded:
                    continue   # not one of Apple's methods, so nothing to match
                if return_type not in TYPES or any(a not in TYPES for a in arguments):
                    continue   # a type this table does not model; not a judgement either way
                with self.subTest(test=test, selector=selector):
                    ours = encode(return_type, arguments)
                    self.assertIn(ours, recorded[selector],
                        f"{selector} in {test} encodes as {ours}, but the device recorded "
                        f"{sorted(recorded[selector])}. OCTRuntimeHook will not hook it, so the "
                        f"test would exercise the stand-in instead of the provider.")

    def test_the_encoder_reproduces_known_recordings(self):
        # Canaries against the real recordings, so a fault in the encoder cannot pass silently.
        self.assertEqual(encode("void", ["uint32_t", "unsigned short *"]), "v28@0:8I16^S20")
        self.assertEqual(encode("unsigned long long", ["id", "unsigned short", "unsigned int", "unsigned int"]),
                         "Q36@0:8@16S24I28I32")
        self.assertEqual(encode("instancetype", ["id", "int32_t"]), "@28@0:8@16i24")
        self.assertEqual(encode("id", []), "@16@0:8")


class CameraOnlyChecks(unittest.TestCase):
    """available() runs the camera's own allStorageIDs and objectForObjectHandle: on its argument,
    so it may only be called with self from a hook installed on PTPVirtualCamera. The upload hooks
    are installed on PTPStorage and PTPFolder, and calling it from them ran camera code against an
    object of another class: in the tests the upload was misrouted to Apple's receiver, and on a
    phone it would have read a storage's memory as a camera's inside the process serving Photos."""

    def test_available_is_only_called_from_camera_hooks(self):
        text = strip_comments((NATIVE / "OctopusProvider.m").read_text())
        functions = [(m.start(), m.group(1))
                     for m in re.finditer(r"^static [^\n(]*?\b(\w+)\s*\(id self", text, re.M)]

        def owner(position):
            found = None
            for start, name in functions:
                if start < position:
                    found = name
            return found

        callers = {owner(m.start()) for m in re.finditer(r"\bavailable\(self\)", text)}
        self.assertTrue(callers, "found no callers of available(); the scan is broken")
        # The mandatory table is installed on the camera in a loop; optional hooks name their class.
        table = text[text.index("static Hook hooks[] = {"):]
        table = table[:table.index("};")]
        hooked = {m.group(1): "PTPVirtualCamera" for m in re.finditer(r"\(IMP\)(\w+)", table)}
        for m in re.finditer(r'OCTRuntimeHook\("(\w+)",[^;]*?\(IMP\)(\w+)', text, re.S):
            hooked.setdefault(m.group(2), m.group(1))
        # appendObject is shared by the two upload hooks, so it inherits their classes.
        for caller in sorted(callers):
            with self.subTest(function=caller):
                self.assertEqual(hooked.get(caller), "PTPVirtualCamera",
                    f"{caller} calls available(self) but is hooked on {hooked.get(caller, 'no class')}; "
                    f"use providerServing() for a receiver that is not the camera.")
        self.assertNotIn("appendObject", callers)
        self.assertNotIn("incomingObjectInfo", callers)


class UseBeforeDeclaration(unittest.TestCase):
    """C99 rejects calling a function with no declaration in scope, so a static helper used above its
    own definition, with no prototype before the use, fails to compile. providerServing was defined
    below the first hook that called it and failed exactly so on CI. The earlier checks here covered
    only OCT* functions shared between files, not a file's own static helpers, which is how it got
    through. Every static function in the native Objective-C is checked: its first call must follow
    its definition or a prototype."""

    def test_static_functions_are_declared_before_use(self):
        for path in sorted(NATIVE.glob("*.m")) + sorted((NATIVE / "Tests").glob("*.m")):
            text = strip_comments(path.read_text())
            # A static function's definition or prototype: return type, name, parameters, then a body
            # or a semicolon. Calls through a variable or a cast never match this.
            declared = {}
            for m in re.finditer(r"^static\s+(?:inline\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;{)]*\)\s*([{;])",
                                 text, re.M):
                declared.setdefault(m.group(1), m.start())
            for name, first_declared in declared.items():
                # The first call that is not the declaration itself: the name followed by "(", not
                # preceded by a word character or a dot, which would make it a member or another name.
                for call in re.finditer(r"(?<![\w.>])" + re.escape(name) + r"\s*\(", text):
                    if call.start() == first_declared or text[:call.start()].rstrip().endswith(("static BOOL", "static bool")):
                        continue
                    line_start = text.rfind("\n", 0, call.start()) + 1
                    if re.match(r"\s*static\b", text[line_start:call.start()]):
                        continue   # this is a declaration line, not a call
                    with self.subTest(file=path.name, function=name):
                        self.assertGreater(call.start(), first_declared,
                            f"{path.name} calls {name}() at line {text.count(chr(10), 0, call.start()) + 1}, "
                            f"before its first declaration at line {text.count(chr(10), 0, first_declared) + 1}. "
                            f"C99 rejects that: move the definition up or add a prototype.")
                    break


class NothingHeavyInTheConstructor(unittest.TestCase):
    """Opening a store on disk inside ptpd's constructor cost that process its entire session on
    0.4.59: it served no host at all, 0 commands and not one hook called, while Windows talked to
    another ptpd. The constructor now installs on the sample tree, which touches no disk, and the
    granted root is applied from the timer a tenth of a second later. A host is told the storage
    list changed once that is done, so nothing is lost by waiting; before the event existed, waiting
    would have meant the volume never appearing at all."""

    def test_the_constructor_installs_on_the_sample_tree(self):
        text = strip_comments((NATIVE / "OctopusObserver.m").read_text())
        self.assertIn("OCTProviderInstall(OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC", text,
            "the provider is installed on a real root, which opens a store inside the constructor.")
        self.assertNotIn("OCTProviderInstall(grantedScope", text)

    def test_the_granted_root_is_applied_outside_the_constructor(self):
        text = strip_comments((NATIVE / "OctopusObserver.m").read_text())
        body = text[text.index("static void pollOnce("):]
        scope = body.index("OCTProviderSetScope(grantedScope")
        guard = body.rfind("insideConstructor", 0, scope)
        self.assertGreater(guard, 0,
            "the granted root is applied without checking it is not the constructor's pass.")
        # And the constructor's pass is the one that sets that flag.
        self.assertIn("insideConstructor=true", text)
        self.assertIn("insideConstructor=false", text)

    def test_the_storage_is_announced_when_a_store_opens(self):
        provider = strip_comments((NATIVE / "OctopusProvider.m").read_text())
        self.assertIn("OCTNativeAnnounceStorage(OCT_NATIVE_STORAGE_ID)", provider,
            "a store opens without telling the host its storage list changed, so the volume is "
            "invisible until the cable is pulled.")
        # StoreAdded has to be advertised or a host ignores the announcement.
        self.assertIn("OCT_EVENT_STORE_ADDED", provider)


class MatchingSignatures(unittest.TestCase):
    """Every declaration, definition, stub and call of an OCT* entry point must agree on its
    arguments. Changing OCTProviderInstall to take a scope left its two headers, the observer's
    stub and two calls in the provider tests on the old one argument, which only a compiler the
    macOS jobs run would have caught."""

    def test_entry_points_agree_on_their_arguments(self):
        sources = sorted(NATIVE.glob("*.m")) + sorted(NATIVE.glob("*.h")) + sorted((NATIVE / "Tests").glob("*.m"))
        counts = {}
        for path in sources:
            text = strip_comments(path.read_text())
            for m in re.finditer(r"\bBOOL\s+(OCTProvider\w+|OCTRuntime\w+)\s*\(([^)]*)\)", text):
                name, arguments = m.group(1), m.group(2).strip()
                if arguments == "void" or not arguments:
                    count = 0
                else:
                    count = len([a for a in arguments.split(",") if a.strip()])
                counts.setdefault(name, {}).setdefault(count, []).append(path.name)
        self.assertTrue(counts, "found no OCT* entry points; the scan is broken")
        for name, by_count in counts.items():
            with self.subTest(entry_point=name):
                self.assertEqual(len(by_count), 1,
                    f"{name} is declared with differing argument counts: "
                    + "; ".join(f"{n} in {sorted(set(where))}" for n, where in sorted(by_count.items())))

    def test_calls_pass_as_many_arguments_as_declared(self):
        declared = {}
        for path in sorted(NATIVE.glob("*.h")):
            for m in re.finditer(r"\bBOOL\s+(OCTProvider\w+)\s*\(([^)]*)\)\s*;", strip_comments(path.read_text())):
                declared[m.group(1)] = len([a for a in m.group(2).split(",") if a.strip()])
        self.assertTrue(declared, "found no declarations to compare calls against")
        for path in sorted(NATIVE.glob("*.m")) + sorted((NATIVE / "Tests").glob("*.m")):
            text = strip_comments(path.read_text())
            for name, expected in declared.items():
                for call in re.finditer(r"(?<![\w.])" + name + r"\s*\(([^;{]*?)\)\s*[;),&]", text):
                    arguments = call.group(1)
                    if re.match(r"\s*(?:BOOL|void|oct_\w+)\b", arguments):
                        continue   # a declaration or definition, not a call
                    passed = len([a for a in re.split(r",(?![^(]*\))", arguments) if a.strip()])
                    with self.subTest(file=path.name, call=name):
                        self.assertEqual(passed, expected,
                            f"{path.name} calls {name} with {passed} arguments; it takes {expected}.")


class ConstantsInScope(unittest.TestCase):
    """Every OCT_ constant an Objective-C file uses must be defined in that file or in a header it
    reaches through its own includes. The observer used OCT_NATIVE_STORAGE_ID, which only the
    provider's header defines and the observer does not include, and the macOS job rejected it as an
    undeclared identifier. The checks above follow functions across files, not constants, which is
    how it went through. Includes are followed transitively; a system header is not, since no
    OCT_ name comes from one."""

    @staticmethod
    def reachable(path, seen=None):
        seen = set() if seen is None else seen
        path = path.resolve()
        if path in seen or not path.exists():
            return seen
        seen.add(path)
        for include in re.findall(r'#\s*(?:include|import)\s+"([^"]+)"', path.read_text()):
            ConstantsInScope.reachable(path.parent / include, seen)
        return seen

    @staticmethod
    def defined_in(text):
        names = set(re.findall(r"#\s*define\s+(OCT_[A-Z0-9_]+)", text))
        # Enumerators, with or without a value, inside an enum body.
        for body in re.findall(r"\benum\b[^{;]*\{(.*?)\}", text, flags=re.S):
            names.update(re.findall(r"\b(OCT_[A-Z0-9_]+)\s*(?:=|,|$)", body, flags=re.M))
        return names

    def test_every_constant_is_in_scope(self):
        sources = sorted(NATIVE.glob("*.m")) + sorted((NATIVE / "Tests").glob("*.m"))
        self.assertTrue(sources)
        for path in sources:
            text = strip_comments(path.read_text())
            available = set()
            for header in self.reachable(path):
                available |= self.defined_in(strip_comments(header.read_text()))
            # A name tested only by #if, #ifdef or #ifndef is a build flag passed with -D, such as
            # OCT_NATIVE_TEST, and needs no definition anywhere; only code uses count.
            code = "\n".join(line for line in text.split("\n")
                             if not re.match(r"\s*#\s*(?:if|ifdef|ifndef|elif|else|endif)\b", line))
            used = set(re.findall(r"\b(OCT_[A-Z0-9_]+)\b", code))
            for name in sorted(used - available):
                with self.subTest(file=path.name, constant=name):
                    self.fail(f"{path.name} uses {name}, which no header it includes defines. "
                              f"Include the header that defines it, or keep the use in the file that does.")

    def test_the_scan_finds_real_definitions(self):
        # A canary, so a broken scan cannot pass by finding nothing at all.
        observer = NATIVE / "OctopusObserver.m"
        available = set()
        for header in self.reachable(observer):
            available |= self.defined_in(strip_comments(header.read_text()))
        for expected in ("OCT_NATIVE_LEASE_NONE", "OCT_NATIVE_HOOK_DEVICE_INFO", "OCT_NATIVE_WRITE_ADVERTISED"):
            self.assertIn(expected, available)
        self.assertNotIn("OCT_NATIVE_STORAGE_ID", available,
                         "the observer reaches the provider's header after all; the scan is wrong")


class NoInitThroughMsgSend(unittest.TestCase):
    """An initializer must never be called through objc_msgSend cast to a plain function. Under ARC,
    init takes ownership of the object alloc returned, and the compiler cannot see that through a
    cast, so it releases the object again after the call: the result is freed while still in use. It
    compiles cleanly, which is what makes it dangerous; the event announcing a storage was sent this
    way and would have crashed ptpd the first time it ran. Declare the method instead, as the
    observer now does for Apple's event classes, so the init family's rules apply."""

    def test_no_cast_objc_msgsend_in_native_code(self):
        for path in sorted(NATIVE.glob("*.m")):
            code = strip_comments(path.read_text())
            for m in re.finditer(r"\)\s*objc_msgSend\s*\)", code):
                with self.subTest(file=path.name):
                    self.fail(f"{path.name} line {code.count(chr(10), 0, m.start()) + 1} calls "
                              f"objc_msgSend through a cast. Declare the method instead; for an "
                              f"initializer a cast frees the object it returns.")


class NoAnnouncementUnderTheLock(unittest.TestCase):
    """announce() calls Apple's sendEventAddStorageWithID:, which may ask for the storage's info
    while building the event and so reach the storageInfo hook, which takes coreLock. coreLock is
    not recursive, so announcing from code that already holds it would freeze ptpd on that thread.
    The stand-in camera does not call back, so no test would see it; this reads the source instead.
    Neither openStoreLocked, whose name says it runs locked, nor any region between a lock and an
    unlock of coreLock, may call announce()."""

    def test_announce_is_not_called_while_coreLock_is_held(self):
        text = strip_comments((NATIVE / "OctopusProvider.m").read_text())
        opener = text[text.index("static BOOL openStoreLocked("):]
        opener = opener[:opener.index("\n}\n")]
        self.assertNotIn("announce()", opener, "openStoreLocked runs holding coreLock and announces")
        for m in re.finditer(r"pthread_mutex_lock\(&coreLock\);(.*?)pthread_mutex_unlock\(&coreLock\);", text, re.S):
            with self.subTest(at=text.count("\n", 0, m.start()) + 1):
                self.assertNotIn("announce()", m.group(1))
                self.assertNotIn("OCTProviderAnnounceIfNeeded()", m.group(1))


if __name__ == "__main__":
    unittest.main()
