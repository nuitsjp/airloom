"""Offline migration tests; requires littlefs-python==0.19.0."""

from hashlib import md5, sha256
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

from littlefs import LittleFS
from littlefs.errors import LittleFSError


SPEC = importlib.util.spec_from_file_location(
    "migrate_history", Path(__file__).resolve().parents[1] / "tools" / "migrate_history.py")
migration = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(migration)


def record(sequence):
    data = bytearray(migration.RECORD.pack(
        0x41495231, 1, sequence, 0x12345678, 7,
        sequence * 1_000_000, 1_800_000_000 + sequence,
        sequence * 1_000_000 - 1000, sequence * 1_000_000,
        b"a4c138e6ca181aea\0", 22.5, 61.25, 0, 0))
    struct.pack_into("<I", data, 88, migration.checksum(data))
    return bytes(data)


def backup_with_files(files, attributes=None):
    fs = LittleFS(mount=False, block_count=512, **migration.GEOMETRY)
    fs.format()
    fs.mount()
    for path in sorted(files):
        parent = str(Path(path).parent).replace("\\", "/")
        if parent != "/":
            fs.makedirs(parent, exist_ok=True)
        with fs.open(path, "wb") as destination:
            destination.write(files[path])
    for path, attrs in (attributes or {}).items():
        for kind, data in attrs.items():
            fs.setattr(path, kind, data)
    fs.unmount()

    entries = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, subtype,
                                   address, size, name.encode(), 0)
                       for name, kind, subtype, address, size in [
                           ("nvs", 1, 2, 0x9000, 0x6000),
                           ("phy_init", 1, 1, 0xF000, 0x1000),
                           ("factory", 0, 0, 0x10000, 0x1F0000),
                           ("history", 1, 0x83, 0x200000, 0x200000)])
    entries += b"\xeb\xeb" + b"\xff" * 14 + md5(entries, usedforsecurity=False).digest()
    flash = bytearray(b"\xff" * migration.FLASH_SIZE)
    flash[0x8000:0x8000 + len(entries)] = entries
    flash[0x9000:0xF000] = bytes(range(256)) * (0x6000 // 256)
    flash[0xF000:0x10000] = b"P" * 0x1000
    flash[migration.OLD_HISTORY_OFFSET:] = fs.context.buffer
    return bytes(flash)


class MigrationTest(unittest.TestCase):
    def test_preserves_full_capacity_files_attributes_and_source(self):
        files = {f"/segment-{index}.bin": b"".join(
            record(10001 + index * 512 + item) for item in range(512))
                 for index in range(8)}
        files["/pending.bin"] = b"interrupted rotation stays untouched"
        files["/notes/source.txt"] = b"extra file must also survive\n"
        attrs = {"/segment-0.bin": {0x74: struct.pack("<q", 1800000000)},
                 "/": {0x12: b"root metadata"},
                 "/notes": {2: b"directory metadata"}}
        original = backup_with_files(files, attrs)
        original_hash = sha256(original).hexdigest()
        image, directories, restored, manifest = migration.rebuild(original)
        self.assertEqual(len(image), migration.NEW_HISTORY_SIZE)
        self.assertEqual(restored, files)
        self.assertIn("/notes", directories)
        self.assertEqual(manifest["history"]["record_count"], 4096)
        self.assertEqual(manifest["history"]["first_sequence"], 10001)
        self.assertEqual(manifest["history"]["last_sequence"], 14096)
        self.assertEqual(manifest["attributes"]["/segment-0.bin"]["116"],
                         attrs["/segment-0.bin"][0x74].hex())
        self.assertEqual(sha256(original).hexdigest(), original_hash)
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            backup = root / "flash.bin"
            backup.write_bytes(original)
            output = root / "migrated"
            migration.migrate(backup, output)
            self.assertEqual(backup.read_bytes(), original)
            self.assertEqual((output / "history-1m.bin").read_bytes(), image)
            for path, data in files.items():
                self.assertEqual((output / "files" / path.lstrip("/")).read_bytes(), data)
            saved = json.loads((output / "migration-manifest.json").read_text())
            self.assertTrue(saved["file_bytes_and_attributes_verified"])
            with self.assertRaises(migration.MigrationError):
                migration.migrate(backup, output)

    def test_rejects_corrupt_or_incomplete_committed_records(self):
        bad = bytearray(record(1))
        bad[80] ^= 1
        for files in [{"/segment-0.bin": bytes(bad)},
                      {"/segment-0.bin": record(1) + b"partial"},
                      {"/segment-0.bin": record(1), "/segment-1.bin": record(1)},
                      {"/segment-0.bin": record(1) + record(3)}]:
            with self.subTest(files=tuple(files)):
                with self.assertRaises(migration.MigrationError):
                    migration.rebuild(backup_with_files(files))

    def test_rejects_wrong_layout_unformatted_history_and_small_input(self):
        flash = backup_with_files({"/segment-0.bin": record(1)})
        wrong = bytearray(flash)
        wrong[0x8000 + 3 * 32 + 4] ^= 0x10
        with self.assertRaises(migration.MigrationError):
            migration.rebuild(bytes(wrong))
        with self.assertRaises(migration.MigrationError):
            migration.rebuild(flash[:-1])
        blank = bytearray(flash)
        blank[migration.OLD_HISTORY_OFFSET:] = b"\xff" * migration.OLD_HISTORY_SIZE
        with self.assertRaises(LittleFSError):
            migration.rebuild(bytes(blank))
        self.assertEqual(blank[migration.OLD_HISTORY_OFFSET:],
                         b"\xff" * migration.OLD_HISTORY_SIZE)

    def test_rejects_path_escape_and_target_capacity_overflow(self):
        with self.assertRaises(migration.MigrationError):
            migration.rebuild(backup_with_files({"/C:escape": b"not a native path"}))
        original = backup_with_files({"/extra.bin": b"X" * 0x120000})
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "full.bin"
            source.write_bytes(original)
            output = root / "too-small"
            with self.assertRaises(LittleFSError):
                migration.migrate(source, output)
            self.assertFalse(output.exists())
            self.assertEqual(source.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
