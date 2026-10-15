#!/usr/bin/env python3
"""Host tests for matching, immutable firmware/debug artifact archives."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import archive_firmware as ARCHIVE  # noqa: E402


class FirmwareArchiveTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="ai-passport-archive-tests-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.build = self.root / "build-input"
        self.output = self.root / "archives"
        self.build.mkdir()
        self.app_name = "FoloToy-AI-Passport.bin"
        self.elf_name = "FoloToy-AI-Passport.elf"
        self.map_name = "FoloToy-AI-Passport.map"
        self.full_name = "FoloToy-AI-Passport-full.bin"
        self.offsets = {
            "bootloader/bootloader.bin": 0,
            "partition_table/partition-table.bin": 0x8000,
            self.app_name: 0x10000,
        }

        elf = bytearray(96)
        elf[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<H", elf, 18, 243)  # EM_RISCV
        self.write(self.elf_name, bytes(elf))
        self.write(self.map_name, b"synthetic linker map\n")

        app = bytearray(24 + 8 + 256 + 16)
        app[0:2] = b"\xe9\x01"
        struct.pack_into("<H", app, 12, 5)  # ESP32-C3 image chip ID
        struct.pack_into("<II", app, 24, 0x3C000020, 256)
        struct.pack_into("<I", app, 32, 0xABCD5432)
        app[48:80] = b"fixture-version".ljust(32, b"\x00")
        app[80:112] = b"FoloToy-AI-Passport".ljust(32, b"\x00")
        app[144:176] = b"v5.5.3".ljust(32, b"\x00")
        app[176:208] = hashlib.sha256(elf).digest()
        self.write(self.app_name, bytes(app))
        self.write("bootloader/bootloader.bin", b"\xe9" + b"synthetic bootloader")

        table = bytearray(b"\xff" * 0xC00)
        struct.pack_into(
            "<HBBII16sI", table, 0, 0x50AA, 0, 0, 0x10000, 0x7F0000,
            b"factory".ljust(16, b"\x00"), 0,
        )
        struct.pack_into("<H", table, 32, 0xEBEB)
        table[48:64] = hashlib.md5(table[:32]).digest()
        self.write("partition_table/partition-table.bin", bytes(table))
        self.write(
            "flash_args",
            b"--flash_mode dio --flash_freq 80m --flash_size 8MB\n"
            b"0x0 bootloader/bootloader.bin\n"
            b"0x8000 partition_table/partition-table.bin\n"
            b"0x10000 FoloToy-AI-Passport.bin\n",
        )
        self.merge()
        quiet = contextlib.redirect_stdout(io.StringIO())
        quiet.__enter__()
        self.addCleanup(quiet.__exit__, None, None, None)

    def write(self, name: str, data: bytes) -> None:
        path = self.build / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def merge(self) -> None:
        merged = bytearray(b"\xff" * 0x11000)
        for name, offset in self.offsets.items():
            image = (self.build / name).read_bytes()
            merged[offset : offset + len(image)] = image
        self.write(self.full_name, bytes(merged))

    def destination(self) -> Path:
        digest = hashlib.sha256((self.build / self.full_name).read_bytes()).hexdigest()
        return self.output / digest

    def create(self) -> Path:
        return ARCHIVE.create_archive(self.build, self.output)

    def symlink(self, link: Path, target: Path, *, directory: bool = False) -> None:
        try:
            link.symlink_to(target, target_is_directory=directory)
        except NotImplementedError:
            self.skipTest("This platform does not support symlinks")
        except OSError as error:
            if getattr(error, "winerror", None) != 1314:
                raise
            self.skipTest("Windows symlink tests require Developer Mode or elevation")

    def test_round_trip_records_only_real_artifacts_and_descriptor_metadata(self) -> None:
        self.write("sdkconfig", b"synthetic private configuration")
        self.write("build.log", b"synthetic private log")
        directory = self.create()
        self.assertEqual(directory, self.destination())
        manifest = ARCHIVE.verify_archive(directory)
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["target"], "esp32c3")
        self.assertEqual(manifest["flash_size_bytes"], 8 * 1024 * 1024)
        self.assertEqual(manifest["image_offsets"], self.offsets)
        self.assertEqual(manifest["app_descriptor"]["version"], "fixture-version")
        self.assertEqual(manifest["app_descriptor"]["idf_version"], "v5.5.3")
        self.assertEqual(manifest["app_descriptor"]["project_name"], "FoloToy-AI-Passport")
        self.assertEqual(manifest["app_elf_sha256"], manifest["app_descriptor"]["embedded_elf_sha256"])
        self.assertEqual(set(manifest["files"]), set(ARCHIVE.ARTIFACTS))
        for name in ARCHIVE.ARTIFACTS:
            data = (self.build / name).read_bytes()
            self.assertEqual((directory / name).read_bytes(), data)
            self.assertEqual(manifest["files"][name], {
                "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            })
        self.assertFalse((directory / "sdkconfig").exists())
        self.assertFalse((directory / "build.log").exists())

    def test_repeat_verifies_without_rewriting_existing_archive(self) -> None:
        directory = self.create()
        before = {path: (path.stat().st_ino, path.stat().st_mtime_ns) for path in directory.rglob("*")}
        self.assertEqual(self.create(), directory)
        self.assertEqual(before, {path: (path.stat().st_ino, path.stat().st_mtime_ns) for path in directory.rglob("*")})

    def test_dedicated_archive_subdirectory_inside_build_is_supported(self) -> None:
        directory = ARCHIVE.create_archive(self.build, self.build / "firmware")
        ARCHIVE.verify_archive(directory)

    def test_same_firmware_with_different_map_reuses_verified_original_map(self) -> None:
        directory = self.create()
        original = (directory / self.map_name).read_bytes()
        manifest = (directory / "manifest.json").read_bytes()
        self.write(self.map_name, b"different map from a different build")
        self.assertEqual(self.create(), directory)
        self.assertEqual((directory / self.map_name).read_bytes(), original)
        self.assertEqual((directory / "manifest.json").read_bytes(), manifest)
        ARCHIVE.verify_archive(directory)

    def test_different_flash_args_cannot_replace_same_firmware_archive(self) -> None:
        directory = self.create()
        original = (self.build / "flash_args").read_bytes()
        self.write("flash_args", original.replace(b"80m", b"40m"))
        with self.assertRaisesRegex(ValueError, "different build artifacts"):
            self.create()
        self.assertEqual((directory / "flash_args").read_bytes(), original)

    def test_missing_and_empty_inputs_fail_before_creating_archive(self) -> None:
        for name in ARCHIVE.ARTIFACTS:
            path = self.build / name
            original = path.read_bytes()
            for empty in (False, True):
                with self.subTest(name=name, empty=empty):
                    if empty:
                        path.write_bytes(b"")
                    else:
                        path.unlink()
                    with self.assertRaises((ValueError, OSError)):
                        self.create()
                    self.assertFalse(self.output.exists())
                    path.write_bytes(original)

    def test_mismatched_elf_is_rejected(self) -> None:
        elf = bytearray((self.build / self.elf_name).read_bytes())
        elf[-1] ^= 1
        self.write(self.elf_name, bytes(elf))
        with self.assertRaisesRegex(ValueError, "ELF SHA256 does not match"):
            self.create()
        self.assertFalse(self.output.exists())

    def test_invalid_elf_header_is_rejected(self) -> None:
        self.write(self.elf_name, b"not an ELF")
        with self.assertRaisesRegex(ValueError, "RISC-V ELF"):
            self.create()

    def test_invalid_or_truncated_application_descriptor_is_rejected(self) -> None:
        original = (self.build / self.app_name).read_bytes()
        for offset, value, error in ((32, 0, "magic"), (12, 0, "ESP32-C3"), (28, 0, "truncated")):
            with self.subTest(offset=offset):
                app = bytearray(original)
                struct.pack_into("<I", app, offset, value)
                self.write(self.app_name, bytes(app))
                self.merge()
                with self.assertRaisesRegex(ValueError, error):
                    self.create()
                self.assertFalse(self.output.exists())
        self.write(self.app_name, original[:100])
        self.merge()
        with self.assertRaisesRegex(ValueError, "descriptor is truncated"):
            self.create()

    def test_mixed_merged_image_is_rejected(self) -> None:
        self.write("bootloader/bootloader.bin", b"different bootloader")
        with self.assertRaisesRegex(ValueError, "differs in merged firmware"):
            self.create()
        self.assertFalse(self.output.exists())

    def test_custom_partition_payload_remains_in_full_image_without_separate_copy(self) -> None:
        data_offset = 0x20000
        payload = b"synthetic private user partition"
        self.write("data.bin", payload)
        table = bytearray(b"\xff" * 0xC00)
        for index, (kind, subtype, offset, size, label) in enumerate((
            (0, 0, 0x10000, 0x10000, b"factory"),
            (1, 0x40, data_offset, 0x1000, b"user_data"),
        )):
            struct.pack_into(
                "<HBBII16sI", table, index * 32, 0x50AA, kind, subtype,
                offset, size, label.ljust(16, b"\x00"), 0,
            )
        struct.pack_into("<H", table, 64, 0xEBEB)
        table[80:96] = hashlib.md5(table[:64]).digest()
        self.write("partition_table/partition-table.bin", bytes(table))
        self.merge()
        merged = (self.build / self.full_name).read_bytes()
        self.write(self.full_name, merged.ljust(data_offset, b"\xff") + payload)
        args = (self.build / "flash_args").read_bytes()
        self.write("flash_args", args + b"0x20000 data.bin\n")
        directory = self.create()
        manifest = ARCHIVE.verify_archive(directory)
        self.assertEqual(manifest["image_offsets"]["data.bin"], data_offset)
        self.assertFalse((directory / "data.bin").exists())
        self.assertNotIn("data.bin", manifest["files"])
        archived_full = (directory / self.full_name).read_bytes()
        self.assertEqual(archived_full[data_offset : data_offset + len(payload)], payload)
        self.assertEqual(archived_full, (self.build / self.full_name).read_bytes())

    def test_unsafe_flash_args_are_rejected_without_execution(self) -> None:
        original = (self.build / "flash_args").read_bytes()
        marker = self.root / "must-not-exist"
        for line in (
            "0x1000 ../outside.bin", "0x1000 /outside.bin", "0x1000 C:/outside.bin",
            "0x1000 a/../outside.bin", "0x1000 './outside.bin'", "0x1000 'a\\b.bin'",
            "0x1000 FoloToy-AI-Passport.bin", "-1 outside.bin", "0x800000 outside.bin",
            f"$(touch {marker})", f"0x1000 '$(touch {marker}).bin'", "--flash_size 4MB",
            "--unknown value", "--flash_freq '80m;exit'", "0x1000 x.bin; exit 1",
        ):
            with self.subTest(line=line):
                self.write("flash_args", original + line.encode() + b"\n")
                with self.assertRaises(ValueError):
                    self.create()
                self.assertFalse(self.output.exists())
                self.assertFalse(marker.exists())

    def test_archive_rechecks_every_file_digest(self) -> None:
        directory = self.create()
        for name in ARCHIVE.ARTIFACTS:
            with self.subTest(name=name):
                path = directory / name
                original = path.read_bytes()
                path.write_bytes(original + b"tampered")
                with self.assertRaises(ValueError):
                    ARCHIVE.verify_archive(directory)
                path.write_bytes(original)

    def test_duplicate_manifest_keys_and_metadata_tampering_fail(self) -> None:
        directory = self.create()
        path = directory / "manifest.json"
        original = path.read_text(encoding="utf-8")
        path.write_text('{"schema_version": 1,' + original[1:], encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate manifest key"):
            ARCHIVE.verify_archive(directory)
        data = json.loads(original)
        data["app_descriptor"]["version"] = "invented version"
        path.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "manifest does not match"):
            ARCHIVE.verify_archive(directory)

    def test_archive_name_must_match_full_image(self) -> None:
        directory = self.create()
        renamed = directory.with_name("a" * 64)
        directory.rename(renamed)
        with self.assertRaisesRegex(ValueError, "directory name does not match"):
            ARCHIVE.verify_archive(renamed)

    def test_existing_partial_or_unrelated_directories_are_preserved(self) -> None:
        directory = self.destination()
        directory.mkdir(parents=True)
        for unrelated in (None, directory / "keep-me"):
            with self.subTest(unrelated=unrelated):
                if unrelated:
                    unrelated.write_bytes(b"unrelated content")
                with self.assertRaises(OSError):
                    self.create()
                self.assertTrue(directory.is_dir())
                if unrelated:
                    self.assertEqual(unrelated.read_bytes(), b"unrelated content")
                self.assertFalse((directory / "manifest.json").exists())

    def test_unexpected_file_in_existing_package_is_rejected_and_preserved(self) -> None:
        directory = self.create()
        unknown = directory / "unexpected.txt"
        unknown.write_bytes(b"unrelated content")
        with self.assertRaisesRegex(ValueError, "unexpected archive entry"):
            self.create()
        self.assertEqual(unknown.read_bytes(), b"unrelated content")

    def test_symlinked_source_file_is_rejected(self) -> None:
        original = self.build / self.map_name
        saved = self.root / "original.map"
        original.rename(saved)
        self.symlink(original, saved)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.create()
        self.assertFalse(self.output.exists())

    def test_symlinked_source_directory_and_parent_are_rejected(self) -> None:
        alias = self.root / "source-alias"
        self.symlink(alias, self.build, directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            ARCHIVE.create_archive(alias, self.output)
        bootloader = self.build / "bootloader"
        saved = self.root / "original-bootloader"
        bootloader.rename(saved)
        self.symlink(bootloader, saved, directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.create()

    def test_exact_system_directory_alias_is_resolved_but_nested_links_are_rejected(self) -> None:
        alias = self.root / "system-tmp-alias"
        self.symlink(alias, self.root, directory=True)
        with patch.object(ARCHIVE, "SYSTEM_DIRECTORY_ALIASES", {alias: self.root}):
            directory = ARCHIVE.create_archive(alias / self.build.name, alias / self.output.name)
            ARCHIVE.verify_archive(directory)
            nested_alias = self.build / "nested-symlink"
            self.symlink(nested_alias, self.build, directory=True)
            with self.assertRaisesRegex(ValueError, "symlink"):
                ARCHIVE.safe_path(alias / self.build.name / nested_alias.name)
        with patch.object(ARCHIVE, "SYSTEM_DIRECTORY_ALIASES", {alias: self.build}):
            with self.assertRaisesRegex(ValueError, "symlink"):
                ARCHIVE.safe_path(alias / self.build.name)

    def test_symlinked_archive_root_or_destination_is_rejected(self) -> None:
        external = self.root / "external"
        external.mkdir()
        self.symlink(self.output, external, directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.create()
        self.output.unlink()
        self.output.mkdir()
        self.symlink(self.destination(), external, directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.create()
        self.assertEqual(list(external.iterdir()), [])

    def test_symlinked_archive_file_is_rejected_even_when_content_matches(self) -> None:
        directory = self.create()
        path = directory / self.map_name
        path.unlink()
        self.symlink(path, self.build / self.map_name)
        with self.assertRaisesRegex(ValueError, "symlink"):
            ARCHIVE.verify_archive(directory)

    def test_dangerous_archive_roots_and_traversal_are_rejected(self) -> None:
        for destination in (Path("/"), Path.home(), ROOT, Path.cwd(), self.build, self.root,
                            self.root / "missing" / ".." / "output"):
            with self.subTest(destination=destination), self.assertRaisesRegex(ValueError, "unsafe"):
                ARCHIVE.create_archive(self.build, destination)

    def test_interrupted_copy_does_not_publish_manifest_or_delete_unrelated_file(self) -> None:
        original_open = Path.open
        destination = self.destination()
        unrelated = destination / "keep-me"

        def fail_write(path: Path, mode: str = "r", *args, **kwargs):
            if path == destination / self.map_name and mode == "xb":
                unrelated.write_bytes(b"another writer's content")
                raise OSError("synthetic write failure")
            return original_open(path, mode, *args, **kwargs)

        with patch.object(Path, "open", fail_write):
            with self.assertRaisesRegex(OSError, "synthetic write failure"):
                self.create()
        self.assertFalse((self.destination() / "manifest.json").exists())
        self.assertEqual(unrelated.read_bytes(), b"another writer's content")
        self.assertEqual(list(self.destination().iterdir()), [unrelated])

    def test_post_copy_validation_failure_has_no_manifest(self) -> None:
        original = ARCHIVE.inspect_build

        def reject_copy(directory: Path):
            if directory != self.build:
                raise ValueError("synthetic copied artifact mismatch")
            return original(directory)

        with patch.object(ARCHIVE, "inspect_build", reject_copy):
            with self.assertRaisesRegex(ValueError, "copied artifact mismatch"):
                self.create()
        self.assertFalse(self.destination().exists())

    def test_cli_create_verify_and_failed_verification(self) -> None:
        self.assertEqual(ARCHIVE.main(["create", str(self.build), "--archive-root", str(self.output)]), 0)
        self.assertEqual(ARCHIVE.main(["verify", str(self.destination())]), 0)
        (self.destination() / self.map_name).write_bytes(b"tampered")
        with contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(ARCHIVE.main(["verify", str(self.destination())]), 1)
        self.assertIn("ERROR:", output.getvalue())


if __name__ == "__main__":
    unittest.main()
