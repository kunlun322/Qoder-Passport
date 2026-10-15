#!/usr/bin/env python3
"""Verify the merged ESP32-C3 firmware layout produced by idf.py merge-bin."""

from __future__ import annotations

import hashlib
import shlex
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


REQUIRED_IMAGES = (
    "bootloader/bootloader.bin",
    "partition_table/partition-table.bin",
    "FoloToy-AI-Passport.bin",
)

FLASH_SIZE = 8 * 1024 * 1024
PARTITION_TABLE_SIZE = 0xC00
PARTITION_TABLE_SECTOR_SIZE = 0x1000
ENTRY = struct.Struct("<HBBII16sI")


@dataclass(frozen=True)
class Partition:
    kind: int
    subtype: int
    offset: int
    size: int
    label: str

    @property
    def end(self) -> int:
        return self.offset + self.size


def parse_partition_table(
    raw: bytes, minimum_partition_offset: int = 0x9000
) -> tuple[list[Partition], bool]:
    """Parse an ESP-IDF table and verify its optional MD5 marker."""
    if len(raw) < PARTITION_TABLE_SIZE:
        raise ValueError("partition table is truncated")

    partitions: list[Partition] = []
    found_md5 = False
    for cursor in range(0, PARTITION_TABLE_SIZE, ENTRY.size):
        magic = int.from_bytes(raw[cursor : cursor + 2], "little")
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:
            expected = hashlib.md5(raw[:cursor]).digest()
            actual = raw[cursor + 16 : cursor + 32]
            if actual != expected:
                raise ValueError("partition table MD5 marker does not match")
            found_md5 = True
            break
        if magic != 0x50AA:
            raise ValueError(f"invalid partition entry at table offset 0x{cursor:x}")

        _, kind, subtype, offset, size, label_raw, _ = ENTRY.unpack_from(raw, cursor)
        label = label_raw.split(b"\0", 1)[0].decode("ascii", "strict")
        if (
            not label
            or not size
            or offset < minimum_partition_offset
            or offset + size > FLASH_SIZE
        ):
            raise ValueError(f"invalid partition bounds for {label!r}")
        partitions.append(Partition(kind, subtype, offset, size, label))

    if not partitions:
        raise ValueError("partition table is empty")
    return partitions, found_md5


def parse_flash_args(raw: str) -> dict[str, int]:
    """Return image paths and offsets from ESP-IDF's line-oriented flash_args."""
    images: dict[str, int] = {}
    for line in raw.splitlines():
        fields = shlex.split(line)
        if not fields:
            continue
        try:
            offset = int(fields[0], 0)
        except ValueError as error:
            if fields[0].startswith("--"):
                continue
            raise ValueError(f"invalid image offset in flash_args: {fields[0]}") from error
        if len(fields) != 2:
            raise ValueError(f"invalid image entry in flash_args: {line}")
        if fields[1] in images:
            raise ValueError(f"duplicate image in flash_args: {fields[1]}")
        images[fields[1]] = offset
    return images


def verify_flash_images(
    merged: bytes, build_dir: Path, image_offsets: dict[str, int]
) -> dict[str, int]:
    """Verify every configured image, not just the minimum bootable set."""
    image_sizes: dict[str, int] = {}
    for name, offset in image_offsets.items():
        image_path = build_dir / name
        if not image_path.is_file():
            raise ValueError(f"missing image {image_path}")
        size = image_path.stat().st_size
        if not size:
            raise ValueError(f"image {name} is empty")
        if offset < 0 or offset + size > FLASH_SIZE:
            raise ValueError(f"image {name} is outside the 8 MB flash bounds")
        image_sizes[name] = size

    ordered = sorted(image_offsets, key=image_offsets.__getitem__)
    for left, right in zip(ordered, ordered[1:]):
        if image_offsets[left] + image_sizes[left] > image_offsets[right]:
            raise ValueError(f"images {left!r} and {right!r} overlap")

    for name, offset in image_offsets.items():
        image = (build_dir / name).read_bytes()
        if merged[offset : offset + len(image)] != image:
            raise ValueError(f"{name} differs at merged offset 0x{offset:x}")
        print(f"Verified {name}: {len(image)} bytes at 0x{offset:x}")
    return image_sizes


def verify_firmware_layout(
    merged: bytes, build_dir: Path, partition_table_offset: int, app_offset: int
) -> list[Partition]:
    """Validate the configured partition table and its application image."""
    table = merged[
        partition_table_offset : partition_table_offset + PARTITION_TABLE_SIZE
    ]
    partitions, found_md5 = parse_partition_table(
        table, partition_table_offset + PARTITION_TABLE_SECTOR_SIZE
    )
    if not found_md5:
        raise ValueError("partition table has no MD5 marker")

    labels = [item.label for item in partitions]
    if len(labels) != len(set(labels)):
        raise ValueError("partition labels must be unique")

    ordered = sorted(partitions, key=lambda item: item.offset)
    for left, right in zip(ordered, ordered[1:]):
        if left.end > right.offset:
            raise ValueError(f"partitions {left.label!r} and {right.label!r} overlap")

    matching_apps = [
        item for item in partitions if item.kind == 0 and item.offset == app_offset
    ]
    if len(matching_apps) != 1:
        raise ValueError(
            f"application offset 0x{app_offset:x} must match exactly one app partition"
        )
    app_partition = matching_apps[0]
    app_path = build_dir / "FoloToy-AI-Passport.bin"
    app_size = app_path.stat().st_size
    if app_size > app_partition.size:
        raise ValueError(
            f"application is {app_size} bytes; partition limit is {app_partition.size}"
        )
    if len(merged) <= app_offset or merged[app_offset] != 0xE9:
        raise ValueError(
            f"merged artifact has no ESP application image at 0x{app_offset:x}"
        )

    print(
        f"Firmware layout: PASS (app {app_size} / {app_partition.size} bytes "
        f"in {app_partition.label!r} at 0x{app_offset:x})"
    )
    return partitions


def verify_extra_image_partitions(
    image_offsets: dict[str, int], image_sizes: dict[str, int],
    partitions: list[Partition],
) -> None:
    """Allow custom images anywhere within one partition, never across its end."""
    for name, offset in image_offsets.items():
        if name in REQUIRED_IMAGES:
            continue
        end = offset + image_sizes[name]
        if not any(part.offset <= offset and end <= part.end for part in partitions):
            raise ValueError(f"image {name} must fit entirely within one partition")


def main() -> int:
    build_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
    merged_path = build_dir / "FoloToy-AI-Passport-full.bin"
    flash_args_path = build_dir / "flash_args"

    if not merged_path.is_file() or not flash_args_path.is_file():
        print("ERROR: merged firmware or flash_args is missing", file=sys.stderr)
        return 1

    try:
        flash_args = flash_args_path.read_text(encoding="utf-8")
        if "--flash_size 8MB" not in flash_args:
            raise ValueError("flash_args does not select the required 8 MB flash size")
        image_offsets = parse_flash_args(flash_args)
        missing = [name for name in REQUIRED_IMAGES if name not in image_offsets]
        if missing:
            raise ValueError(f"flash_args is missing required images: {missing}")
        if image_offsets["bootloader/bootloader.bin"] != 0:
            raise ValueError("the merged bootloader must start at 0x0")
        if merged_path.stat().st_size > FLASH_SIZE:
            raise ValueError("merged firmware exceeds 8 MB")
        merged = merged_path.read_bytes()
        image_sizes = verify_flash_images(merged, build_dir, image_offsets)
        partitions = verify_firmware_layout(
            merged,
            build_dir,
            image_offsets["partition_table/partition-table.bin"],
            image_offsets["FoloToy-AI-Passport.bin"],
        )
        verify_extra_image_partitions(image_offsets, image_sizes, partitions)
    except (OSError, UnicodeDecodeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"Merged firmware: PASS ({len(merged)} bytes, flash at 0x0)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
