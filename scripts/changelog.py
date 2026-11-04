#!/usr/bin/env python3
"""Changelog helpers for the FaceForge release workflow.

    scripts/changelog.py notes VERSION
        Print Markdown release notes for VERSION. The matching
        "## [VERSION]" section of CHANGELOG.md is used when present;
        otherwise the notes are generated from `git log` between the
        previous tag and the current commit.

    scripts/changelog.py add VERSION
        Prepend a new dated section for VERSION to CHANGELOG.md, built
        from `git log` since the previous tag. Run this locally before
        tagging a release if you want curated notes in the file; the
        commit message list is fine as a starting point.

Only the Python standard library is used so the script runs on any CI image.
"""

from __future__ import annotations

import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

SECTION_RE = re.compile(r"^##\s+\[([^\]]+)\][^\n]*\n(.*?)(?=^##\s+\[|\Z)", re.M | re.S)


def run_git(*args: str) -> str:
    return subprocess.run(
        ["git", *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def repo_root() -> Path:
    return Path(run_git("rev-parse", "--show-toplevel").strip())


def norm(value: str) -> str:
    value = value.strip()
    return value[1:] if value.startswith("v") else value


def tag_for(version: str) -> str:
    return "v" + norm(version)


def tag_exists(tag: str) -> bool:
    return (
        subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}"],
            capture_output=True,
            text=True,
        ).returncode
        == 0
    )


def previous_tag(version: str) -> str | None:
    """Most recent tag strictly before the current release."""
    current = tag_for(version)
    ref = f"{current}^" if tag_exists(current) else "HEAD"
    try:
        return run_git("describe", "--tags", "--abbrev=0", ref).strip()
    except subprocess.CalledProcessError:
        return None


def git_notes(version: str) -> str:
    prev = previous_tag(version)
    rng = f"{prev}..HEAD" if prev else "HEAD"
    out = run_git("log", "--no-merges", "--pretty=format:- %s (%h)", rng).strip()
    if out:
        return out
    return "- No changes recorded."


def changelog_section(version: str) -> str | None:
    path = repo_root() / "CHANGELOG.md"
    if not path.is_file():
        return None
    wanted = norm(version)
    for name, body in SECTION_RE.findall(path.read_text(encoding="utf-8")):
        if norm(name).lower() == wanted.lower():
            body = body.strip()
            if body:
                return body
    return None


def cmd_notes(args: argparse.Namespace) -> int:
    # A development build (no real version) always falls back to git.
    if args.version and norm(args.version) != "0.0.0-dev":
        section = changelog_section(args.version)
        if section:
            print(section)
            return 0
    print(git_notes(args.version))
    return 0


def cmd_add(args: argparse.Namespace) -> int:
    root = repo_root()
    path = root / "CHANGELOG.md"
    text = path.read_text(encoding="utf-8") if path.is_file() else "# Changelog\n"
    date = args.date or datetime.date.today().isoformat()
    section = f"## [{norm(args.version)}] - {date}\n\n### Changed\n\n{git_notes(args.version)}\n"
    # Insert right after the "# Changelog" title block, before existing
    # sections, so the newest entry stays at the top.
    match = re.search(r"^##\s+\[", text, re.M)
    if match:
        text = text[: match.start()] + section + "\n" + text[match.start() :]
    else:
        text = text.rstrip() + "\n\n" + section
    path.write_text(text, encoding="utf-8")
    print(f"==> updated {path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_notes = sub.add_parser("notes", help="print release notes for a version")
    p_notes.add_argument("version")
    p_notes.set_defaults(func=cmd_notes)

    p_add = sub.add_parser("add", help="prepend a section to CHANGELOG.md")
    p_add.add_argument("version")
    p_add.add_argument("--date", help="override the ISO date")
    p_add.set_defaults(func=cmd_add)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
