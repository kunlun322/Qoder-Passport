#!/usr/bin/env python3
"""Host tests for safe repository-local AI Passport skill discovery."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "install_passport_skills.py"
SPEC = importlib.util.spec_from_file_location("install_passport_skills", SCRIPT)
assert SPEC and SPEC.loader
INSTALLER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTALLER)


class InstallPassportSkillsTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ai-passport-skill-tests-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        root_patch = patch.object(INSTALLER, "ROOT", self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        for name in INSTALLER.SKILL_NAMES:
            source = self.root / "skills" / name
            source.mkdir(parents=True)
            (source / "SKILL.md").write_text(f"# {name}\n", encoding="utf-8")

    def run_installer(self, *args: str) -> tuple[int, str, str]:
        with (
            contextlib.redirect_stdout(io.StringIO()) as output,
            contextlib.redirect_stderr(io.StringIO()) as errors,
        ):
            code = INSTALLER.main(list(args))
        return code, output.getvalue(), errors.getvalue()

    def symlink(self, link: Path, target: Path, *, directory: bool = True) -> None:
        try:
            link.symlink_to(target, target_is_directory=directory)
        except NotImplementedError:
            self.skipTest("This platform does not support symlinks")
        except OSError as error:
            if getattr(error, "winerror", None) != 1314:
                raise
            self.skipTest("Windows symlink tests require Developer Mode or elevation")

    def make_installation(self) -> Path:
        destination = self.root / ".agents" / "skills"
        destination.mkdir(parents=True)
        for name in INSTALLER.SKILL_NAMES:
            self.symlink(destination / name, Path("..") / ".." / "skills" / name)
        return destination

    def test_missing_installation_and_default_check_do_not_write(self) -> None:
        for args in ((), ("--check",)):
            with self.subTest(args=args):
                code, output, errors = self.run_installer(*args)
                self.assertEqual(code, 1)
                self.assertEqual(output.count("MISSING:"), 5)
                self.assertIn("no files were changed", errors)
                self.assertFalse((self.root / ".agents").exists())

    def test_check_recognizes_all_relative_links(self) -> None:
        self.make_installation()
        code, output, errors = self.run_installer("--check")
        self.assertEqual((code, errors), (0, ""))
        self.assertEqual(output.count("OK:"), 5)
        self.assertIn("All 5 skill links", output)

    def test_install_creates_relative_links_and_is_idempotent(self) -> None:
        code, _, errors = self.run_installer("--install")
        self.assertEqual((code, errors), (0, ""))
        snapshots = {}
        for name in INSTALLER.SKILL_NAMES:
            link = self.root / ".agents" / "skills" / name
            self.assertTrue(link.is_symlink())
            self.assertFalse(Path(os.readlink(link)).is_absolute())
            self.assertEqual(link.resolve(), self.root / "skills" / name)
            snapshots[name] = link.lstat()
        code, output, errors = self.run_installer("--install")
        self.assertEqual((code, errors), (0, ""))
        self.assertNotIn("CREATED:", output)
        for name, before in snapshots.items():
            after = (self.root / ".agents" / "skills" / name).lstat()
            self.assertEqual((after.st_ino, after.st_mtime_ns), (before.st_ino, before.st_mtime_ns))

    def test_missing_final_source_prevents_all_writes(self) -> None:
        name = INSTALLER.SKILL_NAMES[-1]
        (self.root / "skills" / name / "SKILL.md").unlink()
        code, _, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertIn(f"invalid source skills/{name}/SKILL.md", errors)
        self.assertFalse((self.root / ".agents").exists())

    def test_unreadable_source_prevents_all_writes(self) -> None:
        original = Path.read_text
        unreadable = self.root / "skills" / INSTALLER.SKILL_NAMES[-1] / "SKILL.md"

        def read(path: Path, *args, **kwargs):
            if path == unreadable:
                raise PermissionError("synthetic unreadable source")
            return original(path, *args, **kwargs)

        with patch.object(Path, "read_text", read):
            code, _, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertIn("synthetic unreadable source", errors)
        self.assertFalse((self.root / ".agents").exists())

    def test_check_rejects_missing_source_after_installation(self) -> None:
        self.make_installation()
        (self.root / "skills" / INSTALLER.SKILL_NAMES[0] / "SKILL.md").unlink()
        code, _, errors = self.run_installer("--check")
        self.assertEqual(code, 1)
        self.assertIn("invalid source", errors)

    def test_wrong_link_prevents_other_installations_and_is_preserved(self) -> None:
        destination = self.root / ".agents" / "skills"
        destination.mkdir(parents=True)
        name = INSTALLER.SKILL_NAMES[-1]
        link = destination / name
        wrong = Path("..") / ".." / "skills" / INSTALLER.SKILL_NAMES[0]
        self.symlink(link, wrong)
        code, _, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertIn("invalid destination", errors)
        self.assertEqual(os.readlink(link), str(wrong))
        self.assertEqual(list(destination.iterdir()), [link])

    def test_absolute_link_is_rejected(self) -> None:
        destination = self.root / ".agents" / "skills"
        destination.mkdir(parents=True)
        name = INSTALLER.SKILL_NAMES[0]
        self.symlink(destination / name, self.root / "skills" / name)
        code, _, errors = self.run_installer("--check")
        self.assertEqual(code, 1)
        self.assertIn("expected a relative link", errors)

    def test_broken_link_is_rejected_and_preserved(self) -> None:
        destination = self.root / ".agents" / "skills"
        destination.mkdir(parents=True)
        link = destination / INSTALLER.SKILL_NAMES[-1]
        self.symlink(link, Path("missing-target"))
        code, _, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertIn("invalid destination", errors)
        self.assertEqual(os.readlink(link), "missing-target")
        self.assertEqual(list(destination.iterdir()), [link])

    def test_existing_directory_and_file_conflicts_are_preserved(self) -> None:
        destination = self.root / ".agents" / "skills"
        destination.mkdir(parents=True)
        directory = destination / INSTALLER.SKILL_NAMES[-1]
        directory.mkdir()
        marker = directory / "user-content"
        marker.write_text("preserve", encoding="utf-8")
        file = destination / INSTALLER.SKILL_NAMES[-2]
        file.write_text("preserve file", encoding="utf-8")
        code, _, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertEqual(errors.count("not a symlink"), 2)
        self.assertEqual(marker.read_text(encoding="utf-8"), "preserve")
        self.assertEqual(file.read_text(encoding="utf-8"), "preserve file")
        self.assertEqual(set(destination.iterdir()), {directory, file})

    def test_external_parent_symlink_is_rejected_without_external_writes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ai-passport-skill-external-") as external:
            target = Path(external)
            self.symlink(self.root / ".agents", target)
            for args in ("--check", "--install"):
                code, _, errors = self.run_installer(args)
                self.assertEqual(code, 1)
                self.assertIn("installation parent is a symlink", errors)
                self.assertEqual(list(target.iterdir()), [])

    def test_internal_skills_parent_symlink_is_rejected(self) -> None:
        target = self.root / "local-skills"
        target.mkdir()
        (self.root / ".agents").mkdir()
        self.symlink(self.root / ".agents" / "skills", target)
        code, _, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertIn("installation parent is a symlink", errors)
        self.assertEqual(list(target.iterdir()), [])

    def test_parent_file_conflict_is_preserved(self) -> None:
        parent = self.root / ".agents"
        parent.write_text("preserve parent", encoding="utf-8")
        code, _, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertIn("installation parent is not a directory", errors)
        self.assertEqual(parent.read_text(encoding="utf-8"), "preserve parent")

    def test_existing_other_skill_is_preserved(self) -> None:
        other = self.root / ".agents" / "skills" / "user-skill" / "SKILL.md"
        other.parent.mkdir(parents=True)
        other.write_text("user-owned skill", encoding="utf-8")
        before = other.stat()
        code, _, errors = self.run_installer("--install")
        self.assertEqual((code, errors), (0, ""))
        self.assertEqual(other.read_text(encoding="utf-8"), "user-owned skill")
        after = other.stat()
        self.assertEqual((after.st_ino, after.st_mtime_ns), (before.st_ino, before.st_mtime_ns))

    def test_source_document_cannot_escape_repository(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ai-passport-skill-external-") as external:
            target = Path(external) / "SKILL.md"
            target.write_text("outside", encoding="utf-8")
            source = self.root / "skills" / INSTALLER.SKILL_NAMES[0] / "SKILL.md"
            source.unlink()
            self.symlink(source, target, directory=False)
            code, _, errors = self.run_installer("--install")
            self.assertEqual(code, 1)
            self.assertIn("SKILL.md must be a file inside the repository", errors)
            self.assertFalse((self.root / ".agents").exists())

    def test_partial_installation_failure_reports_kept_links_and_manual_paths(self) -> None:
        original = Path.symlink_to
        failing = INSTALLER.SKILL_NAMES[2]

        def symlink(path: Path, *args, **kwargs):
            if path.name == failing:
                raise OSError("synthetic symlink failure")
            return original(path, *args, **kwargs)

        with patch.object(Path, "symlink_to", symlink):
            code, output, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertIn("synthetic symlink failure", errors)
        self.assertIn("Created 2 of 5 missing links", errors)
        self.assertIn("created links and directories were kept", errors)
        self.assertEqual(output.count("CREATED:"), 2)
        for name in INSTALLER.SKILL_NAMES:
            self.assertIn(str(self.root / "skills" / name / "SKILL.md"), errors)
        self.assertEqual(
            {path.name for path in (self.root / ".agents" / "skills").iterdir()},
            set(INSTALLER.SKILL_NAMES[:2]),
        )

    def test_unsupported_symlinks_give_manual_reading_instructions(self) -> None:
        with patch.object(Path, "symlink_to", side_effect=NotImplementedError("unavailable")):
            code, _, errors = self.run_installer("--install")
        self.assertEqual(code, 1)
        self.assertIn("Created 0 of 5 missing links", errors)
        self.assertIn("Windows", errors)
        self.assertIn("read the canonical files directly", errors)

    def test_cli_uses_script_repository_instead_of_working_directory(self) -> None:
        script = self.root / "tools" / "install_passport_skills.py"
        script.parent.mkdir()
        shutil.copyfile(SCRIPT, script)
        self.make_installation()
        with tempfile.TemporaryDirectory(prefix="ai-passport-skill-cwd-") as unrelated:
            result = subprocess.run(
                [sys.executable, "-B", str(script), "--check"],
                cwd=unrelated,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(str(self.root), result.stdout)
            self.assertEqual(list(Path(unrelated).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
