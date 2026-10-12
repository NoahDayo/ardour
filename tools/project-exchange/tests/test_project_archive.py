# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the native project ZIP code with independently generated archives."""
import argparse
import io
import os
from pathlib import Path
import stat
import struct
import subprocess
import tarfile
import tempfile
import unittest
import warnings
import zipfile


class ProjectArchiveTest(unittest.TestCase):
    probe = None

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ardour-archive-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.archive = self.root / "project.dawproject"

    def command(self, *args, status=0, **env):
        result = subprocess.run([self.probe, *map(str, args)], capture_output=True,
                                encoding="utf-8", timeout=30,
                                env=dict(os.environ, **{k: str(v) for k, v in env.items()}))
        self.assertEqual(result.returncode, status, result.stderr)
        # On every exit path, including errors, temporary siblings must be gone.
        self.assertEqual(list(self.root.rglob("*.part-*")), [])
        return result

    def fixture(self, entries):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(self.archive, "w", zipfile.ZIP_DEFLATED) as output:
                for name, data in entries:
                    output.writestr(name, data)

    def test_independent_zip_round_trip_and_unicode_paths(self):
        work = self.root / "音楽 café"
        work.mkdir()
        media = work / "波形.wav"
        payload = bytes(range(256)) * 4097
        media.write_bytes(payload)
        package = work / "交換.dawproject"
        self.command("write", package, "audio/波形.wav", media)
        media.unlink()
        with zipfile.ZipFile(package) as check:
            self.assertIsNone(check.testzip())
            self.assertEqual(check.namelist(), ["project.xml", "metadata.xml", "empty", "audio/波形.wav"])
            self.assertEqual(check.read("audio/波形.wav"), payload)
            self.assertEqual(check.read("empty"), b"")
        output = work / "restored.wav"
        self.command("copy", package, "audio/波形.wav", output)
        self.assertEqual(output.read_bytes(), payload)

    def test_import_zip_with_explicit_directories_and_utf8(self):
        self.fixture([("audio/", b""), ("audio/音.wav", b"audio"), ("project.xml", b"<Project/>")])
        result = self.command("list", self.archive)
        self.assertEqual(result.stdout.splitlines(), ["5\taudio/音.wav", "10\tproject.xml"])
        self.assertEqual(self.command("read", self.archive, "project.xml").stdout, "<Project/>")

    def test_reject_nonportable_paths(self):
        for name in ["../escape", "/absolute", "C:/absolute", "dir/../../escape", "dir//empty", "./relative",
                     "dir/./file", "dir\\..\\escape", "file:stream", "trailing. ", "trailing.", "CON", "aux.wav",
                     "com1/file", "COM¹.wav", "CON .wav", "bad\nname", "bad?.wav"]:
            with self.subTest(name=name):
                self.fixture([(name, b"payload")])
                self.command("list", self.archive, status=1)

    def test_reject_duplicate_alias_and_file_directory_collisions(self):
        for entries in [["a", "a"], ["A.wav", "a.wav"], ["café", "cafe\u0301"],
                        ["audio/A", "Audio/B"], ["audio", "audio/file"],
                        ["audio/file", "audio"], ["audio/", "audio/"]]:
            with self.subTest(entries=entries):
                self.fixture([(name, b"" if name.endswith("/") else b"x") for name in entries])
                self.command("list", self.archive, status=1)

    def test_zip64_and_archive_order(self):
        with zipfile.ZipFile(self.archive, "w") as output:
            with output.open("audio/stream.wav", "w", force_zip64=True) as media:
                media.write(b"streaming audio")
            output.writestr("audio/", b"")  # explicit directory after implicit parent
            output.writestr("project.xml", b"xml")
        target = self.root / "restored.wav"
        self.command("copy", self.archive, "audio/stream.wav", target)
        self.assertEqual(target.read_bytes(), b"streaming audio")

    def test_encrypted_entries_are_rejected(self):
        self.fixture([("project.xml", b"xml")])
        encrypted = bytearray(self.archive.read_bytes())
        # Set the encryption bit in the local and central headers.
        struct.pack_into("<H", encrypted, 6, struct.unpack_from("<H", encrypted, 6)[0] | 1)
        central = encrypted.index(b"PK\x01\x02")
        struct.pack_into("<H", encrypted, central + 8, struct.unpack_from("<H", encrypted, central + 8)[0] | 1)
        self.archive.write_bytes(encrypted)
        self.command("list", self.archive, status=1)

    def test_reject_links_and_special_files(self):
        for mode in [stat.S_IFLNK, stat.S_IFCHR, stat.S_IFSOCK]:
            with self.subTest(mode=mode):
                info = zipfile.ZipInfo("audio")
                info.create_system = 3
                info.external_attr = (mode | 0o777) << 16
                self.fixture([(info, b"../outside")])
                self.command("list", self.archive, status=1)

    def test_normalized_zip_entries_cannot_create_special_files(self):
        # libarchive treats ZIP FIFOs as regular files. The application copies
        # bytes to its own filename and never restores archive permissions/type.
        info = zipfile.ZipInfo("audio")
        info.create_system = 3
        info.external_attr = (stat.S_IFIFO | 0o777) << 16
        self.fixture([(info, b"data")])
        target = self.root / "normal.wav"
        self.command("copy", self.archive, "audio", target)
        self.assertTrue(stat.S_ISREG(target.stat().st_mode))
        self.assertEqual(target.read_bytes(), b"data")

    def test_size_entry_and_text_limits(self):
        self.fixture([("a", b"A" * 64), ("b", b"B" * 64)])
        self.command("list", self.archive, ARCHIVE_TEST_ENTRY_LIMIT=64, ARCHIVE_TEST_TOTAL_LIMIT=128, ARCHIVE_TEST_COUNT_LIMIT=2)
        for limit in [{"ARCHIVE_TEST_ENTRY_LIMIT": 63}, {"ARCHIVE_TEST_TOTAL_LIMIT": 127}, {"ARCHIVE_TEST_COUNT_LIMIT": 1}]:
            with self.subTest(limit=limit):
                self.command("list", self.archive, status=1, **limit)
        self.command("read", self.archive, "a", 63, status=1)
        self.assertEqual(self.command("read", self.archive, "a", 64).stdout, "A" * 64)

    def test_non_zip_truncated_and_crc_corruption(self):
        with tarfile.open(self.archive, "w") as output:
            info = tarfile.TarInfo("project.xml")
            info.size = 4
            output.addfile(info, io.BytesIO(b"data"))
        self.command("list", self.archive, status=1)
        with zipfile.ZipFile(self.archive, "w", zipfile.ZIP_STORED) as output:
            output.writestr("project.xml", b"abcdef")
        original = self.archive.read_bytes()
        corrupted = bytearray(original)
        name_length, extra_length = struct.unpack_from("<HH", corrupted, 26)
        corrupted[30 + name_length + extra_length] ^= 0x7F
        self.archive.write_bytes(corrupted)
        target = self.root / "copy.xml"
        self.command("copy", self.archive, "project.xml", target, status=1)
        self.assertFalse(target.exists())
        self.archive.write_bytes(original[:40])
        self.command("read", self.archive, "project.xml", status=1)

    def test_existing_destinations_survive(self):
        self.archive.write_bytes(b"existing project")
        self.command("write", self.archive, status=1)
        self.assertEqual(self.archive.read_bytes(), b"existing project")
        self.fixture([("project.xml", b"new")])
        target = self.root / "existing.xml"
        target.write_bytes(b"existing media")
        self.command("copy", self.archive, "project.xml", target, status=1)
        self.assertEqual(target.read_bytes(), b"existing media")

    def test_missing_member_and_failed_writer_leave_no_output(self):
        self.fixture([("project.xml", b"xml")])
        target = self.root / "missing.xml"
        self.command("copy", self.archive, "absent", target, status=1)
        self.assertFalse(target.exists())
        self.archive.unlink()
        for command in ["write", "poison"]:
            self.command(command, self.archive, "audio/missing.wav", self.root / "missing.wav", status=1)
            self.assertFalse(self.archive.exists())
        self.command("abandon", self.archive)
        self.assertFalse(self.archive.exists())

    def test_write_limits_and_member_names(self):
        source = self.root / "source.wav"
        source.write_bytes(b"audio")
        for name in ["../escape", "PROJECT.xml", "audio/file:stream"]:
            self.command("write", self.archive, name, source, status=1)
            self.assertFalse(self.archive.exists())
        self.command("write", self.archive, status=1, ARCHIVE_TEST_COUNT_LIMIT=2)
        self.assertFalse(self.archive.exists())

    def test_cancellation_cleans_up_without_publishing(self):
        source = self.root / "source.wav"
        source.write_bytes(b"a" * 1048576)
        # Vary the cancellation point across metadata, media chunks and finish.
        for point in [0, 2, 6, 9, 20, 39]:
            with self.subTest(point=point):
                self.command("write", self.archive, "audio/source.wav", source, status=3, ARCHIVE_TEST_CANCEL=point)
                self.assertFalse(self.archive.exists())
        self.fixture([("audio/source.wav", source.read_bytes())])
        target = self.root / "restored.wav"
        for point in [0, 2, 4, 10]:
            with self.subTest(point=point):
                self.command("copy", self.archive, "audio/source.wav", target, status=3, ARCHIVE_TEST_CANCEL=point)
                self.assertFalse(target.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, type=Path)
    args = parser.parse_args()
    ProjectArchiveTest.probe = str(args.probe.resolve())
    unittest.main(argv=[__file__], verbosity=2)
