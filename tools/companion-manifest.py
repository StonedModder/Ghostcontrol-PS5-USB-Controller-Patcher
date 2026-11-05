#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package gate for the GhosttoothAPI companion the app carries (APP-SVC-04).

generate     write GhosttoothAPI.manifest.json beside a staged ELF
verify       check one ELF + manifest pair
verify-tree  find and check the pair under assets/ghostcontrol of a staged,
             composed or dist tree
verify-zip   check the pair inside a release archive

The manifest records the exact byte count and SHA-256, the companion version
(GC_APP_SERVICE_VERSION in core/src/gc_app_service.c), the STATUS protocol
the app checks at runtime (schema 1, service_kind app-native), source
provenance (selected checkout revision/dirty state, not ELF build linkage, or
"unknown"), and the notice files shipped with it.

The app refuses to push an ELF that does not match its manifest. A manifest
travelling beside the ELF proves the two are consistent, not that either is
authentic; that needs a pinned reviewed hash (--expect-sha256) at release.

--release additionally requires: a known, clean source revision, at least one
notice file present beside the ELF, and a mandatory externally reviewed hash. Which
notice files are legally required is the project owner's decision; this tool
only refuses to call a package releasable without any.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import zipfile
import os
import stat


def plain_name(name):
    return isinstance(name, str) and name not in ("", ".", "..") and all(
        33 <= ord(c) <= 126 and c not in "/\\\\:" for c in name)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise GateError("duplicate JSON key")
        result[key] = value
    return result


def bounded_json(value, depth=0):
    if depth > 32:
        raise GateError("JSON nesting exceeds 32")
    if isinstance(value, dict):
        for child in value.values(): bounded_json(child, depth + 1)
    elif isinstance(value, list):
        for child in value: bounded_json(child, depth + 1)


def regular(path):
    return path.is_file() and not path.is_symlink() and path.stat().st_nlink == 1

ARTIFACT = "GhosttoothAPI.elf"
MANIFEST = "GhosttoothAPI.manifest.json"
ASSET_DIR = ("assets", "ghostcontrol")
COMPANION_SOURCES = ("core/src", "GhosttoothAPI")
EM_X86_64 = 62


class GateError(Exception):
    pass


def elf_check(data: bytes) -> None:
    """Structural bounds only: 64-bit little-endian x86-64 ELF whose program
    header table fits. Section headers are not required (stripped is fine)."""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise GateError("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise GateError("not a 64-bit little-endian ELF")
    machine = int.from_bytes(data[18:20], "little")
    if machine != EM_X86_64:
        raise GateError(f"ELF machine {machine} is not x86-64")
    phoff = int.from_bytes(data[32:40], "little")
    phentsize = int.from_bytes(data[54:56], "little")
    phnum = int.from_bytes(data[56:58], "little")
    if phnum == 0 or phentsize < 56 or phoff + phnum * phentsize > len(data):
        raise GateError("ELF program headers are missing or out of bounds")


def companion_version(source_root: pathlib.Path) -> str:
    text = (source_root / "core/src/gc_app_service.c").read_text(encoding="utf-8")
    match = re.search(r'#define\s+GC_APP_SERVICE_VERSION\s+"([^"\\]+)"', text)
    if not match:
        raise GateError("GC_APP_SERVICE_VERSION not found in core/src/gc_app_service.c")
    return match.group(1)


def provenance(source_root: pathlib.Path) -> tuple[str, bool | None]:
    try:
        revision = subprocess.run(["git", "-C", str(source_root), "rev-parse", "HEAD"],
                                  capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(source_root), "status", "--porcelain", "--",
                                *COMPANION_SOURCES], capture_output=True, text=True,
                               check=True).stdout.strip() != ""
    except (OSError, subprocess.CalledProcessError):
        return "unknown", None
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        return "unknown", None
    return revision, dirty


def build_manifest(data: bytes, version: str, revision: str, dirty: bool | None,
                   notices: list[str]) -> dict:
    return {
        "schema": 1,
        "artifact": ARTIFACT,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "version": version,
        "status_schema": 1,
        "service_kind": "app-native",
        "source_revision": revision,
        "source_dirty": dirty,
        "notices": notices,
    }


def check_pair(elf: bytes, manifest_text: str, notice_present, release: bool,
               expect_sha256: str | None) -> dict:
    try:
        manifest = json.loads(manifest_text, object_pairs_hook=unique_object,
                              parse_constant=lambda _: (_ for _ in ()).throw(GateError("non-JSON number")))
        bounded_json(manifest)
    except (ValueError, RecursionError) as error:
        raise GateError(f"manifest is not JSON: {error}") from None
    required = {"schema", "artifact", "bytes", "sha256", "version", "status_schema", "service_kind", "source_revision", "source_dirty", "notices"}
    if not isinstance(manifest, dict) or not required <= manifest.keys():
        raise GateError("missing required manifest fields")
    if type(manifest.get("schema")) is not int or manifest.get("schema") != 1:
        raise GateError("unsupported manifest schema")
    if manifest.get("artifact") != ARTIFACT:
        raise GateError("manifest names another artifact")
    if type(manifest.get("status_schema")) is not int or manifest.get("status_schema") != 1 or manifest.get("service_kind") != "app-native":
        raise GateError("manifest protocol is not STATUS schema 1 / app-native")
    if not isinstance(manifest.get("version"), str) or not manifest["version"]:
        raise GateError("manifest has no version")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+\-]{0,127}", manifest["version"]):
        raise GateError("manifest version is not a bounded version token")
    revision = manifest["source_revision"]
    if not isinstance(revision, str) or (revision != "unknown" and not re.fullmatch(r"[0-9a-fA-F]{40}", revision)):
        raise GateError("source_revision must be 40 hex or unknown")
    if manifest["source_dirty"] is not None and type(manifest["source_dirty"]) is not bool:
        raise GateError("source_dirty must be bool or null")
    elf_check(elf)
    if type(manifest.get("bytes")) is not int or manifest.get("bytes") != len(elf):
        raise GateError(f"byte count differs: manifest {manifest.get('bytes')}, file {len(elf)}")
    digest = hashlib.sha256(elf).hexdigest()
    if manifest.get("sha256") != digest:
        raise GateError("SHA-256 differs from the manifest")
    notices = manifest.get("notices")
    if not isinstance(notices, list) or not all(plain_name(n) for n in notices) or len(notices) != len(set(notices)):
        raise GateError("notices must be unique plain file names")
    missing = [n for n in notices if not notice_present(n)]
    if missing:
        raise GateError(f"listed notice files are missing: {', '.join(missing)}")
    if release:
        if not isinstance(expect_sha256, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", expect_sha256):
            raise GateError("release requires an externally provided reviewed SHA-256")
        if digest != expect_sha256.lower():
            raise GateError("SHA-256 is not the pinned reviewed hash")
        if not re.fullmatch(r"[0-9a-fA-F]{40}", revision) or manifest.get("source_dirty") is not False:
            raise GateError("release needs a known, clean companion source revision")
        if not notices:
            raise GateError("release needs at least one notice file beside the companion")
    return manifest


def cmd_generate(args) -> None:
    elf_path = pathlib.Path(args.elf)
    data = elf_path.read_bytes()
    elf_check(data)
    notices = []
    for notice in args.notice or []:
        path = pathlib.Path(notice)
        if not path.is_file():
            raise GateError(f"notice file not found: {notice}")
        target = elf_path.parent / path.name
        if target.resolve() != path.resolve():
            target.write_bytes(path.read_bytes())
        notices.append(path.name)
    root = pathlib.Path(args.source_root)
    revision, dirty = provenance(root)
    manifest = build_manifest(data, companion_version(root), revision, dirty, sorted(set(notices)))
    out = elf_path.parent / MANIFEST
    out.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    print(f"==> [manifest] {out} {manifest['bytes']} bytes sha256 {manifest['sha256']} "
          f"version {manifest['version']} revision {revision}{' (dirty)' if dirty else ''}")


def verify_dir(directory: pathlib.Path, release: bool, expect: str | None) -> dict:
    elf, manifest = directory / ARTIFACT, directory / MANIFEST
    if not regular(elf):
        raise GateError(f"{elf} is missing")
    if not regular(manifest):
        raise GateError(f"{manifest} is missing")
    return check_pair(elf.read_bytes(), manifest.read_text(encoding="utf-8"),
                      lambda name: regular(directory / name), release, expect)


def cmd_verify(args) -> None:
    result = verify_dir(pathlib.Path(args.dir), args.release, args.expect_sha256)
    print(f"==> [manifest] verified {args.dir}: {result['sha256']}")


def cmd_verify_tree(args) -> None:
    root = pathlib.Path(args.root)
    if not root.is_dir() or root.is_symlink():
        raise GateError("tree root must be a real directory")
    found = set()
    for current, dirs, files in os.walk(root, followlinks=False):
        directory = pathlib.Path(current)
        for name in dirs + files:
            path = directory / name
            if path.is_symlink():
                raise GateError(f"symlink in package tree: {path}")
            if name in (ARTIFACT, MANIFEST):
                if directory.parts[-2:] != ASSET_DIR or not regular(path):
                    raise GateError(f"invalid companion path/type: {path}")
                found.add(directory)
    if not found:
        raise GateError(f"no assets/ghostcontrol/{MANIFEST} under {root}")
    for directory in sorted(found):
        parts = directory.relative_to(root).parts
        allowed = parts == ASSET_DIR or (root.name == "assets" and parts == ("ghostcontrol",)) or (
            len(parts) == 3 and re.fullmatch(r"PPSA[0-9]{5}", parts[0]) and tuple(parts[1:]) == ASSET_DIR)
        if not allowed: raise GateError(f"invalid title/asset boundary: {directory}")
        verify_dir(directory, args.release, args.expect_sha256)
        print(f"==> [manifest] verified {directory}")


def cmd_verify_zip(args) -> None:
    with zipfile.ZipFile(args.zip) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise GateError(f"{args.zip} has duplicate entries; which copy installs is ambiguous")
        prefixes = set()
        regular_names = set()
        normalized = {}
        for info in archive.infolist():
            name = info.filename
            parts = name.rstrip("/").split("/")
            if name.startswith("/") or any(not plain_name(p) for p in parts):
                raise GateError(f"unsafe archive path: {name}")
            key = name.rstrip("/")
            if key in normalized: raise GateError(f"ambiguous file/directory path: {name}")
            normalized[key] = info.is_dir()
            mode = info.external_attr >> 16
            kind = stat.S_IFMT(mode)
            if kind not in (0, stat.S_IFREG, stat.S_IFDIR) or (info.is_dir() != (kind == stat.S_IFDIR) and kind != 0):
                raise GateError(f"ambiguous archive entry type: {name}")
            if not info.is_dir(): regular_names.add(name)
            if parts[-1] in (ARTIFACT, MANIFEST):
                if len(parts) not in (3, 4) or tuple(parts[-3:-1]) != ASSET_DIR or (len(parts) == 4 and not re.fullmatch(r"PPSA[0-9]{5}", parts[0])) or info.is_dir():
                    raise GateError(f"invalid companion archive boundary: {name}")
                prefixes.add(name[: -len(parts[-1])])
        for name in normalized:
            parts = name.split("/")
            for n in range(1, len(parts)):
                if normalized.get("/".join(parts[:n])) is False:
                    raise GateError(f"archive regular file is also a parent: {name}")
        if not prefixes:
            raise GateError(f"{args.zip} carries no companion pair")
        for prefix in sorted(prefixes):
            if not all(prefix + n in regular_names for n in (ARTIFACT, MANIFEST)):
                raise GateError(f"{args.zip}: orphan companion pair at {prefix}")
            check_pair(archive.read(prefix + ARTIFACT), archive.read(prefix + MANIFEST).decode("utf-8"),
                       lambda n, p=prefix: (p + n) in regular_names, args.release, args.expect_sha256)
            print(f"==> [manifest] verified {args.zip}:{prefix}")


def cmd_verify_dist(args):
    cmd_verify_tree(args)
    archives = sorted(pathlib.Path(args.root).rglob("*.zip"))
    if not archives:
        raise GateError("distribution has no release archives")
    for path in archives:
        if not regular(path): raise GateError(f"invalid archive file type: {path}")
        args.zip = str(path)
        cmd_verify_zip(args)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--elf", required=True)
    g.add_argument("--source-root", required=True)
    g.add_argument("--notice", action="append")
    for name in ("verify", "verify-tree", "verify-zip", "verify-dist"):
        p = sub.add_parser(name)
        p.add_argument({"verify": "--dir", "verify-tree": "--root", "verify-zip": "--zip", "verify-dist": "--root"}[name], required=True)
        p.add_argument("--release", action="store_true")
        p.add_argument("--expect-sha256")
    args = parser.parse_args(argv)
    try:
        {"generate": cmd_generate, "verify": cmd_verify, "verify-tree": cmd_verify_tree,
         "verify-zip": cmd_verify_zip, "verify-dist": cmd_verify_dist}[args.command](args)
    except (GateError, OSError, UnicodeError, zipfile.BadZipFile) as error:
        print(f"companion-manifest: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
