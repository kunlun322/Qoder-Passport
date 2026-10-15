#!/usr/bin/env python3
"""Check or explicitly install repository-local AI Passport skill links."""

from __future__ import annotations

import argparse
import os
import stat
import sys
from pathlib import Path


# Anchor discovery to this script, including when invoked from another directory.
ROOT = Path(__file__).resolve().parents[1]
SKILL_NAMES = (
    "passport-develop",
    "passport-setup",
    "passport-build",
    "passport-device-test",
    "passport-debug",
)


def parent_errors(root: Path) -> list[str]:
    """Reject symlink/non-directory installation parents before following them."""
    for path in (root / ".agents", root / ".agents" / "skills"):
        try:
            mode = path.lstat().st_mode
        except FileNotFoundError:
            return []
        except OSError as error:
            return [f"cannot inspect {path}: {error}"]
        if stat.S_ISLNK(mode):
            return [f"installation parent is a symlink: {path}"]
        if not stat.S_ISDIR(mode):
            return [f"installation parent is not a directory: {path}"]
    return []


def inspect_installation(root: Path) -> tuple[list[str], list[str], list[str]]:
    """Read all sources and destinations; return installed, missing, and errors."""
    installed: list[str] = []
    missing: list[str] = []
    errors = parent_errors(root)
    parents_valid = not errors

    for name in SKILL_NAMES:
        source = root / "skills" / name
        source_valid = True
        try:
            resolved_source = source.resolve(strict=True)
            if resolved_source != source or not source.is_dir():
                raise ValueError("source must be a real repository directory without symlinks")
            document = source / "SKILL.md"
            resolved_document = document.resolve(strict=True)
            if not resolved_document.is_relative_to(root) or not document.is_file():
                raise ValueError("SKILL.md must be a file inside the repository")
            document.read_text(encoding="utf-8")
        except (OSError, RuntimeError, UnicodeError, ValueError) as error:
            errors.append(f"invalid source skills/{name}/SKILL.md: {error}")
            source_valid = False

        if not parents_valid or not source_valid:
            continue
        destination = root / ".agents" / "skills" / name
        try:
            mode = destination.lstat().st_mode
        except FileNotFoundError:
            missing.append(name)
            continue
        except OSError as error:
            errors.append(f"cannot inspect {destination}: {error}")
            continue

        if not stat.S_ISLNK(mode):
            errors.append(f"destination already exists and is not a symlink: {destination}")
            continue
        try:
            target = Path(os.readlink(destination))
            if target.is_absolute() or destination.resolve(strict=True) != source:
                raise ValueError(f"expected a relative link to skills/{name}, found {target}")
            # Check readability through the actual discovery path as well.
            (destination / "SKILL.md").read_text(encoding="utf-8")
        except (OSError, RuntimeError, UnicodeError, ValueError) as error:
            errors.append(f"invalid destination {destination}: {error}")
            continue
        installed.append(name)

    return installed, missing, errors


def report_errors(errors: list[str]) -> None:
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)


def manual_reading_hint(root: Path) -> None:
    print(
        "If directory symlinks are unavailable (for example on Windows), "
        "ask the agent to read the canonical files directly:",
        file=sys.stderr,
    )
    for name in SKILL_NAMES:
        print(f"  {root / 'skills' / name / 'SKILL.md'}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check", action="store_true", help="read-only verification (the default)"
    )
    mode.add_argument(
        "--install", action="store_true", help="create missing repository-local skill links"
    )
    args = parser.parse_args(argv)
    root = ROOT
    print(f"Repository: {root}")
    installed, missing, errors = inspect_installation(root)

    if not args.install:
        for name in installed:
            print(f"OK: .agents/skills/{name} -> skills/{name}")
        for name in missing:
            print(f"MISSING: .agents/skills/{name}")
        report_errors(errors)
        if errors or missing:
            print("Check failed; no files were changed.", file=sys.stderr)
            return 1
        print(f"All {len(SKILL_NAMES)} skill links and SKILL.md files are readable.")
        return 0

    if errors:
        report_errors(errors)
        print("Installation preflight failed; no files were changed.", file=sys.stderr)
        return 1

    created: list[str] = []
    created_directories: list[Path] = []
    try:
        if missing:
            for directory in (root / ".agents", root / ".agents" / "skills"):
                issues = parent_errors(root)
                if issues:
                    raise OSError("; ".join(issues))
                if not directory.exists():
                    directory.mkdir()
                    created_directories.append(directory)

        for name in missing:
            issues = parent_errors(root)
            if issues:
                raise OSError("; ".join(issues))
            destination = root / ".agents" / "skills" / name
            destination.symlink_to(
                Path("..") / ".." / "skills" / name, target_is_directory=True
            )
            created.append(name)
            print(f"CREATED: .agents/skills/{name} -> ../../skills/{name}")
    except (OSError, NotImplementedError) as error:
        print(f"ERROR: installation stopped: {error}", file=sys.stderr)
        print(
            f"Created {len(created)} of {len(missing)} missing links; "
            "created links and directories were kept. Rerun --check to inspect the state.",
            file=sys.stderr,
        )
        for directory in created_directories:
            print(f"Created directory: {directory}", file=sys.stderr)
        manual_reading_hint(root)
        return 1

    _, remaining, errors = inspect_installation(root)
    report_errors(errors)
    if errors or remaining:
        for name in remaining:
            print(f"MISSING after installation: .agents/skills/{name}", file=sys.stderr)
        print("Post-installation verification failed; created links were kept.", file=sys.stderr)
        return 1
    print(f"All {len(SKILL_NAMES)} skill links and SKILL.md files are readable.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
