#!/usr/bin/env python3
"""Retain and verify the debug artifacts belonging to one checked ESP32-C3 build.

Only the explicit artifact allowlist is copied. flash_args is parsed as data,
never executed; sdkconfig, logs, and extra partition images are not copied as
separate files. The full image can still contain custom NVS or resource data;
this archive does not sanitize firmware contents.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import stat
import struct
import sys
from pathlib import Path

# Standalone verification must not create a tools/__pycache__ as a side effect.
sys.dont_write_bytecode = True
from verify_firmware import FLASH_SIZE, REQUIRED_IMAGES, verify_firmware_layout


APP = "FoloToy-AI-Passport"
FULL_BIN = f"{APP}-full.bin"
ARTIFACTS = (
    f"{APP}.elf",
    f"{APP}.map",
    f"{APP}.bin",
    FULL_BIN,
    "bootloader/bootloader.bin",
    "partition_table/partition-table.bin",
    "flash_args",
)
MANIFEST = "manifest.json"
SHA256 = re.compile(r"[0-9a-f]{64}")
# macOS exposes its system temporary directories through these two aliases.
# Resolve only these exact OS aliases, never arbitrary user-created symlinks.
SYSTEM_DIRECTORY_ALIASES = (
    {Path("/tmp"): Path("/private/tmp"), Path("/var"): Path("/private/var")}
    if sys.platform == "darwin" else {}
)

# ESP-IDF v5.5.3: components/bootloader_support/include/esp_app_format.h
# (24-byte esp_image_header_t, 8-byte esp_image_segment_header_t), and
# components/esp_app_format/include/esp_app_desc.h (256-byte esp_app_desc_t).
# esptool's cmds._parse_app_info uses the same descriptor structure. elf2image
# inserts SHA256 of the entire input ELF at image offset 0xB0 (32 + 144).
IMAGE_HEADER_SIZE = 24
SEGMENT_HEADER = struct.Struct("<II")
APP_DESCRIPTOR = struct.Struct("<II8s32s32s16s16s32s32sHHB3s72s")
APP_DESCRIPTOR_MAGIC = 0xABCD5432
ESP32C3_CHIP_ID = 5


def safe_path(value: str | Path) -> Path:
    """Reject traversal and every existing symlink component before filesystem use."""
    raw = os.fspath(value)
    if (
        not raw
        or "\x00" in raw
        or (os.name != "nt" and "\\" in raw)
        or ".." in Path(raw).parts
    ):
        raise ValueError(f"unsafe path: {raw!r}")
    path = Path(raw).absolute()
    for component in (*reversed(path.parents), path):
        if component.is_symlink():
            system_target = SYSTEM_DIRECTORY_ALIASES.get(component)
            if system_target is not None and component.resolve() == system_target:
                return safe_path(system_target / path.relative_to(component))
            raise ValueError(f"symlink is not allowed: {component}")
    return path


def safe_relative_name(name: str) -> None:
    if (
        not name
        or any(part in ("", ".", "..") for part in name.split("/"))
        or any(char in name for char in "\\:$`;&|<>\x00")
        or any(ord(char) < 32 for char in name)
    ):
        raise ValueError(f"unsafe artifact path: {name!r}")


def read_artifact(directory: Path, name: str) -> bytes:
    safe_relative_name(name)
    path = safe_path(directory / name)
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError(f"artifact is not a regular file: {name}")
    data = path.read_bytes()
    if not data:
        raise ValueError(f"empty artifact: {name}")
    return data


def parse_flash_args(raw: bytes) -> dict[str, int]:
    """Parse the generated flash parameters without invoking a shell or esptool."""
    options: dict[str, str] = {}
    images: dict[str, int] = {}
    for line in raw.decode("utf-8").splitlines():
        fields = shlex.split(line)
        if not fields:
            continue
        if fields[0].startswith("--"):
            if len(fields) % 2:
                raise ValueError("invalid flash_args options")
            for key, value in zip(fields[::2], fields[1::2]):
                if key not in ("--flash_mode", "--flash_freq", "--flash_size") or key in options:
                    raise ValueError(f"unsupported or duplicate flash_args option: {key}")
                if not re.fullmatch(r"[A-Za-z0-9]+", value):
                    raise ValueError("unsafe flash_args option value")
                options[key] = value
            continue
        if len(fields) != 2:
            raise ValueError("invalid image entry in flash_args")
        offset = int(fields[0], 0)
        name = fields[1]
        safe_relative_name(name)
        if not name.endswith(".bin") or name in images or not 0 <= offset < FLASH_SIZE:
            raise ValueError(f"invalid or duplicate flash_args image: {name}")
        images[name] = offset
    if options.get("--flash_size") != "8MB":
        raise ValueError("flash_args must select the required 8 MB flash size")
    if any(name not in images for name in REQUIRED_IMAGES):
        raise ValueError("flash_args is missing required images")
    if images["bootloader/bootloader.bin"] != 0:
        raise ValueError("the merged bootloader must start at 0x0")
    return images


def application_descriptor(app: bytes) -> dict[str, str]:
    descriptor_offset = IMAGE_HEADER_SIZE + SEGMENT_HEADER.size
    if len(app) < descriptor_offset + APP_DESCRIPTOR.size:
        raise ValueError("application descriptor is truncated")
    if app[0] != 0xE9 or not 1 <= app[1] <= 16:
        raise ValueError("invalid ESP application image header")
    if struct.unpack_from("<H", app, 12)[0] != ESP32C3_CHIP_ID:
        raise ValueError("application image is not for ESP32-C3")
    _, segment_size = SEGMENT_HEADER.unpack_from(app, IMAGE_HEADER_SIZE)
    if segment_size < APP_DESCRIPTOR.size or descriptor_offset + segment_size > len(app):
        raise ValueError("application descriptor segment is truncated")
    descriptor = APP_DESCRIPTOR.unpack_from(app, descriptor_offset)
    if descriptor[0] != APP_DESCRIPTOR_MAGIC:
        raise ValueError("invalid application descriptor magic")

    def text_field(value: bytes) -> str:
        text = value.split(b"\x00", 1)[0].decode("utf-8")
        if any(ord(char) < 32 for char in text):
            raise ValueError("application descriptor contains control characters")
        return text

    return {
        "project_name": text_field(descriptor[4]),
        "version": text_field(descriptor[3]),
        "idf_version": text_field(descriptor[7]),
        "embedded_elf_sha256": descriptor[8].hex(),
    }


def inspect_build(directory: Path) -> tuple[dict, dict[str, bytes]]:
    """Read one artifact snapshot and validate the firmware/ELF correspondence."""
    directory = safe_path(directory)
    if not directory.is_dir():
        raise ValueError(f"build directory does not exist: {directory}")
    artifacts = {name: read_artifact(directory, name) for name in ARTIFACTS}
    offsets = parse_flash_args(artifacts["flash_args"])
    # Extra partition images are not copied separately, but their contents may
    # remain in the full image. Reject symlink paths even though these entries
    # are never opened or executed; no firmware contents are sanitized here.
    for name in offsets:
        safe_path(directory / name)
    merged = artifacts[FULL_BIN]
    if len(merged) > FLASH_SIZE:
        raise ValueError("merged firmware exceeds 8 MB")
    for name in REQUIRED_IMAGES:
        offset = offsets[name]
        image = artifacts[name]
        if merged[offset : offset + len(image)] != image:
            raise ValueError(f"{name} differs in merged firmware at 0x{offset:x}")
    verify_firmware_layout(
        merged, directory, offsets["partition_table/partition-table.bin"], offsets[f"{APP}.bin"]
    )
    elf = artifacts[f"{APP}.elf"]
    if len(elf) < 52 or elf[:6] != b"\x7fELF\x01\x01" or struct.unpack_from("<H", elf, 18)[0] != 243:
        raise ValueError("application ELF must be a 32-bit little-endian RISC-V ELF")
    descriptor = application_descriptor(artifacts[f"{APP}.bin"])
    files = {
        name: {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
        for name, data in artifacts.items()
    }
    elf_sha = files[f"{APP}.elf"]["sha256"]
    if descriptor["embedded_elf_sha256"] != elf_sha:
        raise ValueError("application descriptor ELF SHA256 does not match the application ELF")
    return {
        "schema_version": 1,
        "target": "esp32c3",
        "flash_size_bytes": FLASH_SIZE,
        "full_bin_sha256": files[FULL_BIN]["sha256"],
        "app_elf_sha256": elf_sha,
        "app_descriptor": descriptor,
        "image_offsets": offsets,
        "files": files,
    }, artifacts


def unique_json_object(pairs: list[tuple[str, object]]) -> dict:
    result: dict = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate manifest key: {key}")
        result[key] = value
    return result


def verify_archive(directory: Path) -> dict:
    """Check the actual artifacts against the manifest, including embedded ELF SHA."""
    directory = safe_path(directory)
    if not SHA256.fullmatch(directory.name):
        raise ValueError("archive directory name must be the full firmware SHA256")
    expected = json.loads(
        read_artifact(directory, MANIFEST).decode("utf-8"), object_pairs_hook=unique_json_object
    )
    actual, _ = inspect_build(directory)
    if expected != actual:
        raise ValueError("archive manifest does not match its artifacts")
    if directory.name != actual["full_bin_sha256"]:
        raise ValueError("archive directory name does not match the full firmware SHA256")
    allowed = set(ARTIFACTS) | {MANIFEST, "bootloader", "partition_table"}
    for parent, directories, files in os.walk(directory, followlinks=False):
        for name in directories + files:
            child = Path(parent) / name
            safe_path(child)
            if child.relative_to(directory).as_posix() not in allowed:
                raise ValueError(f"unexpected archive entry: {child.relative_to(directory)}")
    return actual


def create_archive(build_dir: Path, archive_root: Path) -> Path:
    """Validate first; publish a manifest only after writing and checking all files."""
    build_dir = safe_path(build_dir)
    archive_root = safe_path(archive_root)
    repository = Path(__file__).resolve().parents[1]
    if (
        archive_root in (Path(archive_root.anchor), Path.home(), Path.cwd(), repository)
        or build_dir.is_relative_to(archive_root)
    ):
        raise ValueError("unsafe archive root: use a separate dedicated archive directory")
    expected, artifacts = inspect_build(build_dir)
    destination = safe_path(archive_root / expected["full_bin_sha256"])
    if destination.exists():
        existing = verify_archive(destination)
        # Linker maps may contain temporary build-directory names. Reuse the
        # first verified map when every executable artifact and flash_args is
        # identical; do not replace it or rewrite the original manifest.
        if any(existing[key] != expected[key] for key in expected if key != "files") or any(
            existing["files"][name] != expected["files"][name]
            for name in ARTIFACTS if name != f"{APP}.map"
        ):
            raise ValueError("existing archive belongs to different build artifacts")
        return destination

    archive_root.mkdir(parents=True, exist_ok=True)
    safe_path(archive_root)
    # Exclusive mkdir also protects an existing empty directory from replacement.
    destination.mkdir()
    created_files: list[tuple[Path, int, int]] = []
    created_directories = [destination]

    def write_file(name: str, data: bytes) -> None:
        path = safe_path(destination / name)
        with path.open("xb") as output:
            identity = os.fstat(output.fileno())
            created_files.append((path, identity.st_dev, identity.st_ino))
            output.write(data)
            output.flush()
            os.fsync(output.fileno())

    try:
        for name, data in artifacts.items():
            parent = destination / name
            if parent.parent != destination and not parent.parent.exists():
                parent.parent.mkdir()
                created_directories.append(parent.parent)
            write_file(name, data)
        actual, _ = inspect_build(destination)
        if actual != expected:
            raise ValueError("copied archive differs from the checked build")
        manifest_data = (json.dumps(expected, indent=2, sort_keys=True) + "\n").encode("utf-8")
        # Linking a complete manifest publishes it atomically without overwriting
        # an unexpected file. Interrupted/failed copies have no success manifest.
        temporary_manifest = ".manifest.json.incomplete"
        write_file(temporary_manifest, manifest_data)
        manifest_path = destination / MANIFEST
        os.link(destination / temporary_manifest, manifest_path)
        identity = manifest_path.stat()
        created_files.append((manifest_path, identity.st_dev, identity.st_ino))
        (destination / temporary_manifest).unlink()
    except BaseException:
        # Remove only files this invocation created, never a pre-existing package
        # or a file replaced by another writer. rmdir preserves unknown contents.
        for path, device, inode in reversed(created_files):
            try:
                current = safe_path(path).lstat()
                if (current.st_dev, current.st_ino) == (device, inode):
                    path.unlink()
            except (OSError, ValueError):
                pass
        for directory in reversed(created_directories):
            try:
                safe_path(directory).rmdir()
            except (OSError, ValueError):
                pass
        raise
    return destination


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create", help="validate and archive one build")
    create.add_argument("build_dir", type=Path)
    create.add_argument("--archive-root", type=Path, default=Path("build/firmware"))
    verify = commands.add_parser("verify", help="verify an existing archive without writing")
    verify.add_argument("archive_dir", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "create":
            directory = create_archive(args.build_dir, args.archive_root)
            print(f"Firmware debug archive: {directory}")
        else:
            verify_archive(args.archive_dir)
            print(f"Firmware debug archive: PASS ({args.archive_dir})")
    except (OSError, UnicodeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
