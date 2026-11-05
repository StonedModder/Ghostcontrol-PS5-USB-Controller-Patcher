#!/usr/bin/env python3
"""Upload a built app folder to /data/homebrew over FTP.

Written rather than delegated because the upstream deploy script reads the
title ID out of *its own* sce_sys/param.json, which is the template's
placeholder. Our identity is regenerated per build and lives in our param.json,
so delegating would publish every build under the same placeholder ID and
defeat the point of randomising it.

What this guarantees, and checks before touching the console:

  * the folder being uploaded, its param.json titleId, and the requested ID
    all agree -- a mismatch is a failed build, not something to publish;
  * each file lands under a temporary name and is renamed only once its
    transfer finished, so a dropped connection cannot leave a half-written
    file that the Shell might try to run;
  * eboot.bin and sce_sys/param.json are published last, so the title does
    not become launchable until everything it needs is already there.

Usage:
    deploy-app.py --host H [--port 2120] [--bind 192.168.69.23] --dist DIR
                  [--title-id PPSAnnnnn] [--user anonymous] [--password x] [--dry-run]
"""
from __future__ import annotations

import argparse
import ftplib
import json
import posixpath
import re
import sys
from pathlib import Path

TITLE_ID_RE = re.compile(r"^PPSA\d{5}$")
HOMEBREW_ROOT = "/data/homebrew"
# Published last, in this order: the Shell needs param.json to list the title
# and eboot.bin to launch it.
PUBLISH_LAST = ("eboot.bin", "sce_sys/param.json")
TEMP_PREFIX = ".uploading-"

# An FTP upload lands without the execute bit, and the Shell answers
# "cannot start the game or app" (CE-107750-0) when the title's files are not
# open to every user. The template's packaging fix (2f672d1 / zip-open-modes)
# stores every ZIP entry as 0777 for the same reason. Matching that here —
# directories and every file, not just eboot — is what makes FTP-deployed
# titles launch after unpack or direct folder publish.
MODE_DIR = "777"
MODE_EXEC = "777"
MODE_DATA = "777"
EXECUTABLE_NAMES = frozenset({"eboot.bin"})
EXECUTABLE_SUFFIXES = (".prx", ".elf", ".bin")


def read_title_id(dist: Path) -> str:
    param = dist / "sce_sys" / "param.json"
    if not param.is_file():
        raise SystemExit(f"no param.json in {dist}")
    try:
        data = json.loads(param.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SystemExit(f"param.json is not valid JSON: {error}")
    title_id = data.get("titleId", "")
    if not TITLE_ID_RE.match(title_id):
        raise SystemExit(f"param.json has an invalid titleId: {title_id!r}")
    concept = data.get("conceptId", "")
    content = data.get("contentId", "")
    # The Shell rejects a folder whose identity fields disagree, and that
    # failure is confusing on the console, so catch it here instead.
    if concept != title_id[4:]:
        raise SystemExit(
            f"conceptId {concept!r} does not match titleId {title_id!r}")
    if title_id not in content:
        raise SystemExit(
            f"contentId {content!r} does not contain titleId {title_id!r}")
    return title_id


def collect(dist: Path) -> list[tuple[str, Path]]:
    """Every file to upload, as (posix relative path, local path)."""
    files = [(p.relative_to(dist).as_posix(), p)
             for p in sorted(dist.rglob("*")) if p.is_file()]
    first = [f for f in files if f[0] not in PUBLISH_LAST]
    last = [f for f in files if f[0] in PUBLISH_LAST]
    # Keep PUBLISH_LAST in its declared order rather than alphabetical.
    last.sort(key=lambda f: PUBLISH_LAST.index(f[0]))
    return first + last


def ensure_dir(ftp: ftplib.FTP, path: str) -> None:
    try:
        ftp.mkd(path)
    except ftplib.error_perm:
        pass  # already there, or the server reports it that way


def remove_if_present(ftp: ftplib.FTP, path: str) -> None:
    try:
        ftp.delete(path)
    except ftplib.error_perm:
        pass


def needs_execute(name: str) -> bool:
    return name in EXECUTABLE_NAMES or name.endswith(EXECUTABLE_SUFFIXES)


def chmod(ftp: ftplib.FTP, path: str, mode: str) -> bool:
    """Best effort: a server without SITE CHMOD is reported, not fatal."""
    try:
        ftp.sendcmd(f"SITE CHMOD {mode} {path}")
        return True
    except ftplib.all_errors:
        return False


def apply_modes(ftp: ftplib.FTP, remote_root: str,
                files: list[tuple[str, Path]]) -> None:
    directories = {remote_root}
    for relative, _ in files:
        parts = relative.split("/")[:-1]
        for depth in range(1, len(parts) + 1):
            directories.add(posixpath.join(remote_root, *parts[:depth]))

    refused = 0
    for directory in sorted(directories):
        refused += not chmod(ftp, directory, MODE_DIR)
    for relative, _ in files:
        name = posixpath.basename(relative)
        mode = MODE_EXEC if needs_execute(name) else MODE_DATA
        refused += not chmod(ftp, posixpath.join(remote_root, relative), mode)

    if refused:
        # Worth saying loudly: without open modes the title will not
        # launch, and the console's error does not say why.
        print(f"    WARNING: {refused} permission change(s) refused; if the "
              f"title will not start, chmod 777 its files by hand "
              f"(CE-107750-0 is usually 0644/0755 leftovers)")
    else:
        print("    permissions set (777 directories and every file; CE-107750-0 fix)")


def upload(ftp: ftplib.FTP, remote_root: str,
           files: list[tuple[str, Path]]) -> None:
    made: set[str] = set()
    for relative, local in files:
        remote = posixpath.join(remote_root, relative)
        directory = posixpath.dirname(remote)
        if directory and directory not in made:
            # Build the path a level at a time; mkd does not create parents.
            parts = directory.strip("/").split("/")
            for depth in range(1, len(parts) + 1):
                ensure_dir(ftp, "/" + "/".join(parts[:depth]))
            made.add(directory)
        temporary = posixpath.join(
            directory, TEMP_PREFIX + posixpath.basename(remote))
        remove_if_present(ftp, temporary)
        with local.open("rb") as handle:
            ftp.storbinary(f"STOR {temporary}", handle)
        remove_if_present(ftp, remote)
        ftp.rename(temporary, remote)
        print(f"    {relative} ({local.stat().st_size} bytes)")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=2120)
    parser.add_argument("--bind", default="192.168.69.23",
                        help="local address to bind; VPN adapters misroute otherwise")
    parser.add_argument("--dist", type=Path, required=True,
                        help="the built dist/<TITLE_ID> folder")
    parser.add_argument("--title-id", default=None,
                        help="the ID this build was meant to publish as")
    parser.add_argument("--user", default="anonymous")
    parser.add_argument("--password", default="ghostcontrol")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    dist = args.dist
    if not dist.is_dir():
        raise SystemExit(f"not a directory: {dist}")

    title_id = read_title_id(dist)
    if dist.name != title_id:
        raise SystemExit(
            f"folder {dist.name!r} does not match param.json titleId "
            f"{title_id!r}")
    if args.title_id and args.title_id != title_id:
        raise SystemExit(
            f"built title ID {title_id!r} is not the requested "
            f"{args.title_id!r}; rebuild before deploying")

    files = collect(dist)
    remote_root = posixpath.join(HOMEBREW_ROOT, title_id)
    print(f"==> [deploy] {title_id} -> ftp://{args.host}:{args.port}"
          f"{remote_root}/  ({len(files)} files)")
    if args.dry_run:
        for relative, local in files:
            print(f"    would send {relative} ({local.stat().st_size} bytes)")
        print("==> [deploy] dry run, nothing sent")
        return 0

    ftp = ftplib.FTP()
    ftp.connect(args.host, args.port, timeout=30,
                source_address=(args.bind, 0))
    try:
        ftp.login(args.user, args.password)
        ensure_dir(ftp, HOMEBREW_ROOT)
        ensure_dir(ftp, remote_root)
        upload(ftp, remote_root, files)
        apply_modes(ftp, remote_root, files)
    finally:
        try:
            ftp.quit()
        except Exception:
            ftp.close()
    print(f"==> [deploy] {title_id} published; launch it from the home screen")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
