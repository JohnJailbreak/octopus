"""Inspect real DEB/tar/plist bytes using an independent archive reader."""
import gzip
import hashlib
import importlib.util
import io
from pathlib import Path
import plistlib
import struct
import sys
import tarfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("package_native", ROOT / "tools/package_native.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def synthetic_dylib():
    # Structurally signed test fixture only: never installed or claimed executable.
    header = struct.pack("<8I", 0xFEEDFACF, 0x100000C, 0x80000002, 6, 1, 16, 0, 0)
    directory = struct.pack(">II", 0xFADE0C02, 44) + bytes(36)
    signature = struct.pack(">IIIII", 0xFADE0CC0, 64, 1, 0, 20) + directory
    return header + struct.pack("<4I", 0x1D, 16, 48, len(signature)) + signature


def read_ar(data):
    assert data[:8] == b"!<arch>\n"
    members, offset = {}, 8
    while offset < len(data):
        header = data[offset:offset + 60]
        assert len(header) == 60 and header[-2:] == b"`\n"
        name = header[:16].decode().strip().rstrip("/")
        length = int(header[48:58])
        members[name] = data[offset + 60:offset + 60 + length]
        offset += 60 + length + length % 2
    assert offset == len(data)
    return members


def read_tar(data):
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data)), mode="r:") as archive:
        return {m.name.removeprefix("./"): (m, archive.extractfile(m).read() if m.isfile() else b"")
                for m in archive.getmembers()}


class NativePackageTests(unittest.TestCase):
    def test_package_payload_metadata_permissions_and_determinism(self):
        dylib = synthetic_dylib()
        built, identity = package.build_package(dylib, "0.4.10~experimental")
        self.assertEqual(built, package.build_package(dylib, "0.4.10~experimental")[0])
        self.assertEqual(identity["sha256"], hashlib.sha256(dylib).hexdigest())
        members = read_ar(built)
        self.assertEqual(list(members), ["debian-binary", "control.tar.gz", "data.tar.gz"])
        self.assertEqual(members["debian-binary"], b"2.0\n")
        control = read_tar(members["control.tar.gz"])
        self.assertEqual(set(control), {"control", "md5sums"})  # no restart/install scripts
        metadata = control["control"][1].decode()
        self.assertIn("Architecture: iphoneos-arm64\n", metadata)
        self.assertIn("firmware (<< 17.1), ellekit\n", metadata)
        payload = read_tar(members["data.tar.gz"])
        files = {name: value for name, value in payload.items() if value[0].isfile()}
        self.assertEqual(set(files), {package.PREFIX + "/OctopusNative.dylib",
                                     package.PREFIX + "/OctopusNative.plist"})
        self.assertEqual(files[package.PREFIX + "/OctopusNative.dylib"][1], dylib)
        self.assertEqual(files[package.PREFIX + "/OctopusNative.dylib"][0].mode, 0o755)
        plist = files[package.PREFIX + "/OctopusNative.plist"]
        self.assertEqual(plist[0].mode, 0o644)
        self.assertEqual(plistlib.loads(plist[1]), {"Filter": {"Executables": ["ptpd"]}})
        for name, (entry, data) in payload.items():
            self.assertFalse(name.startswith("/") or ".." in name.split("/"))
            self.assertEqual((entry.uid, entry.gid, entry.mtime), (0, 0, 0))
            if entry.isdir():
                self.assertEqual(entry.mode, 0o755)
            else:
                self.assertIn(f"{hashlib.md5(data).hexdigest()}  {name}\n", control["md5sums"][1].decode())

    def test_rejects_wrong_arch_unsigned_and_malformed(self):
        original = synthetic_dylib()
        malformed = [b"", original[:31]]
        for offset, value in [(0, 0), (8, 0), (12, 2), (16, 2), (20, 0xFFFFFFFF),
                              (32, 0), (36, 9), (40, 0), (44, 0xFFFFFFFF), (56, 0xFFFFFFFF)]:
            bad = bytearray(original)
            struct.pack_into("<I", bad, offset, value)
            malformed.append(bytes(bad))
        for bad in malformed:
            with self.subTest(bytes=bad[:48].hex()), self.assertRaises(ValueError):
                package.build_package(bad, "0.4.10")

    def test_rejects_version_injection(self):
        for version in ["", "../x", "x", "0.4\nDepends: injected", "0.4/../../x"]:
            with self.subTest(version=version), self.assertRaises(ValueError):
                package.build_package(synthetic_dylib(), version)


if __name__ == "__main__":
    unittest.main(verbosity=2)
