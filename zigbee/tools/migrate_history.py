"""Rebuild the old 2 MiB LittleFS history as a verified 1 MiB image.

Offline only: this program never opens a serial port or writes a device. Install
``littlefs-python==0.19.0`` in a separate virtual environment, then run:

    python zigbee/tools/migrate_history.py flash-backup.bin migration-output

The backup must be a complete 4 MiB read of the old standalone layout. Keep it
outside Git: it contains the device's Wi-Fi credentials and Zigbee network data.
The output directory must not exist. No input or existing output is overwritten.
"""

import argparse
from hashlib import md5, sha256
import importlib.metadata
import json
import math
from pathlib import Path, PurePosixPath
import re
import struct

from littlefs import LittleFS, UserContext
from littlefs.errors import LittleFSError


FLASH_SIZE = 0x400000
OLD_HISTORY_OFFSET = 0x200000
OLD_HISTORY_SIZE = 0x200000
NEW_HISTORY_OFFSET = 0x300000
NEW_HISTORY_SIZE = 0x100000
PACKAGE_VERSION = "0.19.0"
GEOMETRY = dict(block_size=4096, read_size=128, prog_size=128,
                cache_size=512, lookahead_size=128, block_cycles=512,
                disk_version=0x00020001)
RECORD = struct.Struct("<IIQIIqqqq24sffII")
assert RECORD.size == 96


class MigrationError(ValueError):
    pass


class ReadOnlyContext(UserContext):
    """Reject any library attempt to change the source filesystem."""

    def __init__(self, image):
        super().__init__(buffer=bytearray(image))
        self.write_attempts = 0

    def prog(self, cfg, block, off, data):
        self.write_attempts += 1
        return -5  # LFS_ERR_IO

    def erase(self, cfg, block):
        self.write_attempts += 1
        return -5


def require_dependency():
    if importlib.metadata.version("littlefs-python") != PACKAGE_VERSION:
        raise MigrationError(f"Use littlefs-python=={PACKAGE_VERSION} for this migration")


def mount_image(image):
    context = ReadOnlyContext(image)
    # LittleFS(mount=True) formats automatically after a failed mount. Never use
    # that convenience behavior for a source image or for verification.
    fs = LittleFS(context=context, mount=False,
                  block_count=len(image) // GEOMETRY["block_size"], **GEOMETRY)
    fs.mount()
    return fs, context


def verify_old_layout(flash):
    if len(flash) != FLASH_SIZE:
        raise MigrationError("Input must be exactly one complete 4 MiB flash backup")
    entries = {}
    table = flash[0x8000:0x9000]
    md5_found = False
    for offset in range(0, len(table), 32):
        entry = table[offset:offset + 32]
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0xFFFF:
            break
        if magic == 0xEBEB:
            if entry[16:] != md5(table[:offset], usedforsecurity=False).digest():
                raise MigrationError("Partition table MD5 does not match")
            md5_found = True
            break
        if magic != 0x50AA:
            raise MigrationError("Invalid partition table entry")
        _, kind, subtype, address, size, label, _ = struct.unpack("<HBBII16sI", entry)
        label = label.split(b"\0", 1)[0].decode("ascii")
        if label in entries:
            raise MigrationError("Duplicate partition label")
        entries[label] = (kind, subtype, address, size)
    expected = {
        "nvs": (1, 2, 0x9000, 0x6000),
        "phy_init": (1, 1, 0xF000, 0x1000),
        "factory": (0, 0, 0x10000, 0x1F0000),
        "history": (1, 0x83, OLD_HISTORY_OFFSET, OLD_HISTORY_SIZE),
    }
    if not md5_found or entries != expected:
        raise MigrationError("Backup is not the expected old standalone partition layout")


def read_attributes(fs, path):
    attributes = {}
    for attr_type in range(256):
        try:
            attributes[attr_type] = fs.getattr(path, attr_type)
        except LittleFSError as error:
            if error.code != -61:  # LFS_ERR_NOATTR
                raise
    return attributes


def read_tree(fs):
    directories = ["/"]
    files = {}
    attributes = {"/": read_attributes(fs, "/")}
    for root, dirs, names in fs.walk("/"):
        for name in sorted(dirs + names):
            path = str(PurePosixPath(root) / name)
            if (not name or name in (".", "..") or
                    any(char in name for char in "/\\:") or
                    any(ord(char) < 32 for char in name)):
                raise MigrationError("Unsafe filename in filesystem")
            attributes[path] = read_attributes(fs, path)
        for name in sorted(dirs):
            directories.append(str(PurePosixPath(root) / name))
        for name in sorted(names):
            path = str(PurePosixPath(root) / name)
            with fs.open(path, "rb") as source:
                files[path] = source.read()
    return sorted(directories), files, attributes


def checksum(data):
    value = 2166136261
    for byte in data[:88]:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def validate_records(files):
    sequences = []
    segment_records = {}
    for path, data in sorted(files.items()):
        if not re.fullmatch(r"/segment-[0-7]\.bin", path):
            continue
        if len(data) % RECORD.size:
            raise MigrationError(f"Incomplete history record in {path}; source preserved")
        count = len(data) // RECORD.size
        if count > 512:
            raise MigrationError(f"Too many records in {path}")
        previous = None
        for offset in range(0, len(data), RECORD.size):
            raw = data[offset:offset + RECORD.size]
            fields = RECORD.unpack(raw)
            magic, version, sequence, _, flags, recorded_us = fields[:6]
            valid = (magic == 0x41495231 and version == 1 and sequence != 0
                     and fields[12] == checksum(raw) and b"\0" in fields[9]
                     and recorded_us >= 0
                     and (not flags & 1 or math.isfinite(fields[10]))
                     and (not flags & 2 or math.isfinite(fields[11])))
            if not valid or (previous is not None and sequence != previous + 1):
                raise MigrationError(f"Invalid history record in {path} at byte {offset}")
            sequences.append(sequence)
            previous = sequence
        segment_records[path] = count
    if len(sequences) != len(set(sequences)):
        raise MigrationError("History segments have overlapping sequence numbers")
    return {
        "record_count": len(sequences),
        "first_sequence": min(sequences) if sequences else None,
        "last_sequence": max(sequences) if sequences else None,
        "segment_records": segment_records,
    }


def rebuild(flash):
    require_dependency()
    verify_old_layout(flash)
    original = flash[OLD_HISTORY_OFFSET:OLD_HISTORY_OFFSET + OLD_HISTORY_SIZE]
    source, source_context = mount_image(original)
    try:
        source_stat = source.fs_stat()
        directories, files, attributes = read_tree(source)
    finally:
        source.unmount()
    if source_context.write_attempts or bytes(source_context.buffer) != original:
        raise MigrationError("Source filesystem unexpectedly attempted a write")
    records = validate_records(files)

    target = LittleFS(mount=False,
                      block_count=NEW_HISTORY_SIZE // GEOMETRY["block_size"],
                      name_max=source_stat.name_max, file_max=source_stat.file_max,
                      attr_max=source_stat.attr_max, **GEOMETRY)
    target.format()
    target.mount()
    try:
        for directory in sorted(directories, key=lambda path: (path.count("/"), path)):
            if directory != "/":
                target.mkdir(directory)
        for path, data in sorted(files.items()):
            with target.open(path, "wb") as destination:
                destination.write(data)
        # Preserve LittleFS attributes too, including ESP-IDF's mtime attribute.
        for path, items in attributes.items():
            for attr_type, data in items.items():
                target.setattr(path, attr_type, data)
    finally:
        target.unmount()
    image = bytes(target.context.buffer)
    verified, verification_context = mount_image(image)
    try:
        new_directories, new_files, new_attributes = read_tree(verified)
        new_stat = verified.fs_stat()
    finally:
        verified.unmount()
    if (new_directories != directories or new_files != files or
            new_attributes != attributes or verification_context.write_attempts):
        raise MigrationError("Reopened image does not preserve every file and attribute")
    if validate_records(new_files) != records:
        raise MigrationError("Reopened image changed committed history records")
    if len(image) != NEW_HISTORY_SIZE or new_stat.block_count != 256:
        raise MigrationError("Rebuilt filesystem is not the required 1 MiB size")

    manifest = {
        "littlefs_python_version": PACKAGE_VERSION,
        "source_flash_sha256": sha256(flash).hexdigest(),
        "nvs_sha256": sha256(flash[0x9000:0xF000]).hexdigest(),
        "phy_sha256": sha256(flash[0xF000:0x10000]).hexdigest(),
        "source_history_sha256": sha256(original).hexdigest(),
        "target_history_sha256": sha256(image).hexdigest(),
        "source_history_offset": OLD_HISTORY_OFFSET,
        "source_history_size": OLD_HISTORY_SIZE,
        "target_history_offset": NEW_HISTORY_OFFSET,
        "target_history_size": NEW_HISTORY_SIZE,
        "geometry": GEOMETRY,
        "directories": directories,
        "files": {path: {"bytes": len(data), "sha256": sha256(data).hexdigest()}
                  for path, data in sorted(files.items())},
        "attributes": {path: {str(kind): data.hex() for kind, data in items.items()}
                       for path, items in attributes.items() if items},
        "history": records,
        "file_bytes_and_attributes_verified": True,
    }
    return image, directories, files, manifest


def migrate(backup, output):
    backup = Path(backup)
    output = Path(output)
    if output.exists():
        raise MigrationError("Output directory already exists; refusing to overwrite it")
    image, directories, files, manifest = rebuild(backup.read_bytes())
    output.mkdir(parents=True)
    extracted = output / "files"
    extracted.mkdir()
    for directory in directories:
        if directory != "/":
            (extracted / directory.lstrip("/")).mkdir(parents=True, exist_ok=True)
    for path, data in files.items():
        destination = extracted / path.lstrip("/")
        if not destination.resolve().is_relative_to(extracted.resolve()):
            raise MigrationError("Extracted path would escape the new output directory")
        destination.write_bytes(data)
    (output / "history-1m.bin").write_bytes(image)
    (output / "migration-manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backup", type=Path, help="complete 4 MiB old-layout flash backup")
    parser.add_argument("output", type=Path, help="new directory for image, files, and manifest")
    args = parser.parse_args()
    try:
        manifest = migrate(args.backup, args.output)
    except (MigrationError, LittleFSError, OSError) as error:
        parser.exit(1, f"Migration stopped without changing the backup or device: {error}\n")
    print(json.dumps({"output": str(args.output), "files": len(manifest["files"]),
                      **manifest["history"], "verified": True}))


if __name__ == "__main__":
    main()
