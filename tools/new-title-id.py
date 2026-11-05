#!/usr/bin/env python3
"""Pick a fresh title ID and write it through param.json.

Every build gets a new identity so the console treats it as a new title and
never serves a stale cached icon, name or save folder for an older build.

The three identity fields in param.json have to agree or the Shell refuses
the folder, so they are written together:

    titleId    PPSAnnnnn
    conceptId  nnnnn                      (the same five digits)
    contentId  UP9000-PPSAnnnnn_00-<suffix>

The suffix is exactly 16 characters, which is what the content ID format
expects; it is padded rather than truncated so a short name still validates.

Usage:
    new-title-id.py PARAM_JSON [--name NAME] [--title-id PPSAnnnnn]
                               [--record PATH]
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys
import tempfile
from pathlib import Path

TITLE_ID_RE = re.compile(r"^PPSA\d{5}$")
CONTENT_SUFFIX_LENGTH = 16


def random_title_id(rng: random.Random) -> str:
    """A PPSA id in the homebrew range.

    00000 is avoided because an all-zero concept id reads as "unset" and
    90000+ is left alone because the boilerplate ships PPSA99999 as its
    placeholder; colliding with it would make a stale install hard to spot.
    """
    return f"PPSA{rng.randint(10000, 89999):05d}"


def content_suffix(name: str) -> str:
    cleaned = re.sub(r"[^A-Z0-9]", "", name.upper())
    if not cleaned:
        cleaned = "GHOSTCONTROL"
    return (cleaned + "0" * CONTENT_SUFFIX_LENGTH)[:CONTENT_SUFFIX_LENGTH]


def apply_identity(param: dict, title_id: str, name: str) -> dict:
    param["titleId"] = title_id
    param["conceptId"] = title_id[4:]
    param["contentId"] = f"UP9000-{title_id}_00-{content_suffix(name)}"
    localized = param.setdefault("localizedParameters", {})
    localized.setdefault("defaultLanguage", "en-US")
    localized.setdefault(localized["defaultLanguage"], {})
    # The installed product has one name, including on testers' non-English
    # consoles. Preserve other localized metadata, never a stale test suffix.
    for fields in localized.values():
        if isinstance(fields, dict):
            fields["titleName"] = name
    # Toolbox is an app. Inheriting a game's metadata causes the Shell's
    # PlayStation Store bag overlay and a misleading Games-category install.
    param["applicationCategoryType"] = 65536
    param["contentBadgeType"] = 2
    param.pop("gameIntent", None)
    return param


def write_atomically(path: Path, text: str) -> None:
    """Replace the file in one step so an interrupted run cannot leave a
    half-written param.json that the Shell would reject."""
    handle = tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, prefix=path.name, suffix=".tmp",
        delete=False)
    try:
        with handle as out:
            out.write(text)
        os.replace(handle.name, path)
    except BaseException:
        try:
            os.unlink(handle.name)
        except OSError:
            pass
        raise


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("param", type=Path, help="path to sce_sys/param.json")
    parser.add_argument("--name", default="Ghostcontrol Toolbox")
    parser.add_argument("--title-id", default=None,
                        help="use this id instead of a random one")
    parser.add_argument("--record", type=Path, default=None,
                        help="also write the chosen id here, one line")
    args = parser.parse_args(argv)

    if args.title_id is not None and not TITLE_ID_RE.match(args.title_id):
        parser.error(f"title id must look like PPSA12345, got {args.title_id!r}")

    if not args.param.is_file():
        print(f"param.json not found: {args.param}", file=sys.stderr)
        return 2
    try:
        param = json.loads(args.param.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        print(f"param.json is not valid JSON: {error}", file=sys.stderr)
        return 2

    # Seeded from the OS so repeated builds in the same second still differ.
    rng = random.SystemRandom()
    title_id = args.title_id or random_title_id(rng)
    apply_identity(param, title_id, args.name)
    write_atomically(args.param, json.dumps(param, indent=2) + "\n")

    if args.record is not None:
        args.record.parent.mkdir(parents=True, exist_ok=True)
        write_atomically(args.record, title_id + "\n")

    print(title_id)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
