#!/usr/bin/env python3
"""Deterministic rootless companion DEB; standard-library only, no device access.

The payload is deliberately limited to the supplied signed dylib and ptpd filter.
No installation scripts, Apple binaries, leases or persistent services are added.
"""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import plistlib
import re
import struct
import tarfile

PACKAGE = "com.parv.octopus.native"
PREFIX = "var/jb/usr/lib/TweakInject"


def validate_dylib(data):
    if len(data) < 32:
        raise ValueError("truncated Mach-O header")
    magic, cpu, subtype, kind, count, size, _, _ = struct.unpack_from("<8I", data)
    if (magic, cpu, subtype & 0xFFFFFF, kind) != (0xFEEDFACF, 0x100000C, 2, 6):
        raise ValueError("expected a thin arm64e MH_DYLIB")
    if size > len(data) - 32 or count > size // 8:
        raise ValueError("invalid Mach-O load-command bounds")
    pos, end, signatures = 32, 32 + size, []
    for _ in range(count):
        if end - pos < 8:
            raise ValueError("truncated load command")
        cmd, length = struct.unpack_from("<II", data, pos)
        if length < 8 or length % 8 or length > end - pos:
            raise ValueError("invalid load-command length")
        if cmd == 0x1D:
            if length != 16:
                raise ValueError("invalid code-signature command")
            offset, span = struct.unpack_from("<II", data, pos + 8)
            if offset < end or span < 12 or offset > len(data) or span > len(data) - offset:
                raise ValueError("invalid embedded code-signature bounds")
            magic, blob_size, blobs = struct.unpack_from(">III", data, offset)
            if magic != 0xFADE0CC0 or blob_size > span or blobs > (blob_size - 12) // 8:
                raise ValueError("invalid embedded signature superblob")
            found_directory = False
            for index in range(blobs):
                slot, relative = struct.unpack_from(">II", data, offset + 12 + 8 * index)
                if relative < 12 + blobs * 8 or relative > blob_size - 8:
                    raise ValueError("invalid signature blob offset")
                blob_magic, length2 = struct.unpack_from(">II", data, offset + relative)
                if length2 < 8 or length2 > blob_size - relative:
                    raise ValueError("invalid signature blob length")
                if slot == 0 and blob_magic == 0xFADE0C02:
                    if length2 < 44:
                        raise ValueError("truncated CodeDirectory")
                    found_directory = True
            if not found_directory:
                raise ValueError("missing CodeDirectory; sign with ldid first")
            signatures.append((offset, span))
        pos += length
    if pos != end or len(signatures) != 1:
        raise ValueError("expected exactly one embedded signature and complete load commands")
    return {"architecture": "arm64e", "mach_o_type": "MH_DYLIB",
            "sha256": hashlib.sha256(data).hexdigest(),
            "signature": "embedded CodeDirectory present; trust and runtime acceptance unverified"}


def tar_gz(files):
    buffer = io.BytesIO()
    # USTAR plus explicit ownership/mode/mtime makes archives reproducible on Windows and macOS.
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        directories = set()
        for name in files:
            parts = name.split("/")
            directories.update("/".join(parts[:i]) for i in range(1, len(parts)))
        for name in sorted(directories):
            info = tarfile.TarInfo("./" + name)
            info.type, info.mode, info.uid, info.gid, info.mtime = tarfile.DIRTYPE, 0o755, 0, 0, 0
            archive.addfile(info)
        for name, (data, mode) in sorted(files.items()):
            info = tarfile.TarInfo("./" + name)
            info.size, info.mode, info.uid, info.gid, info.mtime = len(data), mode, 0, 0, 0
            archive.addfile(info, io.BytesIO(data))
    compressed = io.BytesIO()
    with gzip.GzipFile(fileobj=compressed, mode="wb", filename="", mtime=0) as stream:
        stream.write(buffer.getvalue())
    return compressed.getvalue()


def ar(members):
    result = bytearray(b"!<arch>\n")
    for name, data in members:
        header = f"{name + '/':<16}{0:<12}{0:<6}{0:<6}{'100644':<8}{len(data):<10}`\n"
        result.extend(header.encode("ascii"))
        result.extend(data)
        if len(data) % 2:
            result.extend(b"\n")
    return bytes(result)


def build_package(dylib, version):
    if not re.fullmatch(r"[0-9][A-Za-z0-9.+:~_-]*", version):
        raise ValueError("invalid Debian package version")
    identity = validate_dylib(dylib)
    filter_bytes = plistlib.dumps({"Filter": {"Executables": ["ptpd"]}}, sort_keys=True)
    files = {PREFIX + "/OctopusNative.dylib": (dylib, 0o755),
             PREFIX + "/OctopusNative.plist": (filter_bytes, 0o644)}
    control = (f"Package: {PACKAGE}\nName: Octopus Native (Experimental)\nVersion: {version}\n"
               "Architecture: iphoneos-arm64\nMaintainer: Octopus contributors\nSection: Tweaks\n"
               "Depends: firmware (>= 17.0), firmware (<< 17.1), ellekit\n"
               "Description: Optional Octopus PTP companion for Dopamine rootless\n"
               " Requires matching Octopus app and explicit expiring authorization.\n"
               " No automatic service restart. Physical USB verification is required.\n")
    checksums = "".join(f"{hashlib.md5(data).hexdigest()}  {name}\n"  # Debian integrity list, not authentication.
                        for name, (data, _) in sorted(files.items()))
    archive = ar([("debian-binary", b"2.0\n"),
                  ("control.tar.gz", tar_gz({"control": (control.encode(), 0o644),
                                             "md5sums": (checksums.encode(), 0o644)})),
                  ("data.tar.gz", tar_gz(files))])
    return archive, identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dylib", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        archive, identity = build_package(args.dylib.read_bytes(), args.version)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(archive)
    sha = hashlib.sha256(archive).hexdigest()
    args.output.with_suffix(".deb.sha256").write_text(f"{sha}  {args.output.name}\n", encoding="utf8")
    identity.update(package=PACKAGE, version=args.version, package_sha256=sha,
                    payload=list([PREFIX + "/OctopusNative.dylib", PREFIX + "/OctopusNative.plist"]))
    args.output.with_suffix(".deb.json").write_text(json.dumps(identity, indent=2) + "\n", encoding="utf8")
    print(f"Built {args.output} ({len(archive)} bytes); not installed")


if __name__ == "__main__":
    main()
