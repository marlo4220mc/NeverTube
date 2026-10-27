#!/usr/bin/env python3
"""Fill docs/release/notes.md and append the checksum block. Used by the release.

The notes are a file of the repository (they are documentation and they are
reviewed like the rest); this only replaces the placeholders and appends the
SHA256SUMS of the build that is being published. Everything it does is
deterministic: the same inputs give the same text, byte for byte.

    tools/release_notes.py --version 1.0.0-alpha.1 --tag v1.0.0-alpha.1 \
        --date 2026-10-26 --prerelease --checksums out/SHA256SUMS.txt \
        > release-notes.md

Exit code is 1 (with the reason on stderr) if a placeholder would be left
unfilled in the published notes: a release with "@@VERSION@@+" on it is worse
than no release.
"""

import argparse
import pathlib
import re
import sys

PLACEHOLDERS = ("@@VERSION@@", "@@TAG@@", "@@DATE@@", "@@PRERELEASE_NOTE@@")

# Shown only for a pre-release, at the top, because it is the first thing to
# read before installing. Kept short: the details are in "Known limitations".
ALPHA_NOTE = (
    "> **This is an alpha.** It is the first public build: features can\n"
    "> change, some things are rough, and a video can fail because YouTube\n"
    '> changed something. Please read "Known limitations" before installing.'
)


def template_path() -> pathlib.Path:
    """docs/release/notes.md, from the repository this script lives in."""
    return pathlib.Path(__file__).resolve().parent.parent / "docs" / "release" / "notes.md"


def checksums_block(path: pathlib.Path) -> str:
    lines = [ln.rstrip() for ln in path.read_text(encoding="utf-8").splitlines() if ln.strip()]
    if not lines:
        raise SystemExit(f"{path}: no checksums in it")
    return "\n## Checksums\n\n```\n" + "\n".join(lines) + "\n```\n"


def render(version: str, tag: str, date: str, prerelease: bool, checksums: pathlib.Path) -> str:
    text = template_path().read_text(encoding="utf-8")
    values = {
        "@@VERSION@@": version,
        "@@TAG@@": tag,
        "@@DATE@@": date,
        "@@PRERELEASE_NOTE@@": ALPHA_NOTE if prerelease else "",
    }
    for key, value in values.items():
        text = text.replace(key, value)
    left = [p for p in PLACEHOLDERS if p in text]
    if left:
        raise SystemExit(f"the template still has {', '.join(left)}")
    return text.rstrip("\n") + "\n" + checksums_block(checksums)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--prerelease", action="store_true")
    ap.add_argument("--checksums", required=True, type=pathlib.Path)
    args = ap.parse_args()

    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?", args.version):
        print(f"--version '{args.version}' is not X.Y.Z or X.Y.Z-suffix", file=sys.stderr)
        return 1
    if not re.fullmatch(r"v" + re.escape(args.version), args.tag):
        print(f"--tag '{args.tag}' does not match --version '{args.version}' (it must be v<version>)",
              file=sys.stderr)
        return 1

    sys.stdout.write(render(args.version, args.tag, args.date, args.prerelease, args.checksums))
    return 0


if __name__ == "__main__":
    sys.exit(main())
