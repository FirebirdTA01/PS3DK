"""Registration-copy RED/acceptance tests. No compiler, target or loader run."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import runpy
import struct
import tempfile
import unittest
from unittest import mock

from registration_fixture import Fixture, malformed_cases

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("registration_elf", HERE.parent / "tools/registration_elf.py")
CONVERTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONVERTER)
# Existing independent strict ELF acceptance checker, not conversion implementation.
ELF_CHECK = runpy.run_path(str(HERE.parents[3] / "tests/host/ps3dk-prof-spu-int/i3b_elf.py"))["oracle"]


class RegistrationTests(unittest.TestCase):
    def test_fixture_oracle_control_raw_refuses_expected_accepts(self):
        f = Fixture()
        self.assertIsNone(ELF_CHECK(f.raw))
        bid, records = ELF_CHECK(f.expected())
        self.assertEqual(len(records), 5)
        self.assertEqual([r[0] for r in records], [f.DESC + i * 16 for i in range(5)])
        self.assertEqual([r[1] for r in records], list(range(10, 15)))
        self.assertEqual([r[2] for r in records], list(f.NAMES))
        self.assertEqual([r[3] for r in records], [b"job.c"] * 5)
        self.assertEqual(bid[0], 2)  # whole-registration-copy FNV identity

    def test_conversion_nulls_exactly_one_header(self):
        f = Fixture()
        before = hashlib.sha256(f.raw).digest()
        actual = CONVERTER.normalize(f.raw)
        self.assertEqual(actual, f.expected())
        self.assertEqual(hashlib.sha256(f.raw).digest(), before)
        self.assertNotEqual(hashlib.sha256(actual).digest(), before)

    def test_actual_conversion_accepted_by_independent_strict_oracle(self):
        f = Fixture()
        actual = CONVERTER.normalize(f.raw)
        self.assertIsNotNone(ELF_CHECK(actual), "converter output must be accepted, not unchanged raw bytes")
        self.assertEqual(ELF_CHECK(actual), ELF_CHECK(f.expected()))

    def test_postchecker_rejects_unchanged_copy(self):
        f = Fixture()
        with self.assertRaises(ValueError):
            CONVERTER.verify_conversion(f.raw, f.raw)

    def test_postchecker_accepts_exact_expected_conversion(self):
        f = Fixture()
        CONVERTER.verify_conversion(f.raw, f.expected())

    def test_postchecker_rejects_any_other_byte_change(self):
        f = Fixture()
        # Header, phdr, load text, descriptor, fixup, relocation payload,
        # symbol table, and another section header are all protected.
        for offset in (24, f.PH + 4, f.LOAD, f.DESC, f.FIXUP, f.RELA, f.SYM + 16, f.SH + 40):
            with self.subTest(offset=offset):
                b = bytearray(f.expected())
                b[offset] ^= 1
                with self.assertRaises(ValueError):
                    CONVERTER.verify_conversion(f.raw, bytes(b))

    def test_malformed_inputs_refused(self):
        for label, raw in malformed_cases():
            with self.subTest(label=label):
                with self.assertRaises(ValueError):
                    CONVERTER.normalize(raw)

    def test_all_load_and_fixup_bytes_preserved(self):
        f = Fixture()
        actual = CONVERTER.normalize(f.raw)
        self.assertEqual(actual[f.LOAD:f.LOAD + f.LOAD_SIZE], f.raw[f.LOAD:f.LOAD + f.LOAD_SIZE])
        self.assertEqual(actual[f.FIXUP:f.FIXUP + 24], f.raw[f.FIXUP:f.FIXUP + 24])
        self.assertEqual(actual[f.PH:f.PH + 32], f.raw[f.PH:f.PH + 32])

    def test_relocated_indices_and_line_words_are_base_invariant(self):
        f = Fixture()
        original = [list(r) for r in f.records]
        for base in (0x4c00, 0x8000):
            records = [r.copy() for r in original]
            previous_base = 0
            for destination in (base, base):
                delta = destination - previous_base
                for i in range(5):
                    encoded = struct.unpack_from(">I", f.raw, f.FIXUP + 4 * i)[0]
                    self.assertEqual(encoded & 15, 11)
                    for word in range(4):
                        if encoded & (8 >> word):
                            records[i][word] += delta
                    self.assertEqual((records[i][0] - (f.DESC + destination)) >> 4, i)
                    self.assertEqual(records[i][1], original[i][1])
                    self.assertEqual(records[i][2:], [v + destination for v in original[i][2:]])
                previous_base = destination


class FileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ.get("SJ_REG_TEST_TMP"))
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "raw.elf"
        self.destination = self.root / "registration.elf"
        self.report = self.root / "registration.elf.json"
        self.source.write_bytes(Fixture().raw)

    def test_fresh_output_and_identity_receipt(self):
        report = CONVERTER.convert_file(self.source, self.destination)
        self.assertEqual(self.destination.read_bytes(), Fixture().expected())
        self.assertEqual(self.source.read_bytes(), Fixture().raw)
        self.assertEqual(json.loads(self.report.read_text()), report)
        self.assertEqual(report["raw_sha256"], hashlib.sha256(Fixture().raw).hexdigest())
        self.assertEqual(report["registration_sha256"], hashlib.sha256(Fixture().expected()).hexdigest())
        self.assertEqual(report["registration_build_id"], ELF_CHECK(Fixture().expected())[0].hex())

    def test_existing_output_refuses_without_replace(self):
        self.destination.write_bytes(b"existing evidence")
        with self.assertRaises(ValueError):
            CONVERTER.convert_file(self.source, self.destination)
        self.assertEqual(self.destination.read_bytes(), b"existing evidence")
        self.assertFalse(self.report.exists())

    def test_existing_receipt_refuses_without_publishing_output(self):
        self.report.write_bytes(b"existing receipt")
        with self.assertRaises(ValueError):
            CONVERTER.convert_file(self.source, self.destination)
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.report.read_bytes(), b"existing receipt")

    def test_replace_is_explicit_and_preserves_raw(self):
        self.destination.write_bytes(b"old owned output")
        self.report.write_bytes(b"old owned receipt")
        CONVERTER.convert_file(self.source, self.destination, replace=True)
        self.assertEqual(self.destination.read_bytes(), Fixture().expected())
        self.assertEqual(self.source.read_bytes(), Fixture().raw)
        self.assertFalse(list(self.root.glob("*.tmp")))

    def test_validation_failure_preserves_prior_output_and_receipt(self):
        self.source.write_bytes(next(iter(malformed_cases()))[1])
        self.destination.write_bytes(b"old output")
        self.report.write_bytes(b"old receipt")
        with self.assertRaises(ValueError):
            CONVERTER.convert_file(self.source, self.destination, replace=True)
        self.assertEqual(self.destination.read_bytes(), b"old output")
        self.assertEqual(self.report.read_bytes(), b"old receipt")
        self.assertFalse(list(self.root.glob("*.tmp")))

    def test_same_file_and_hardlink_alias_refuse_even_with_replace(self):
        with self.assertRaises(ValueError):
            CONVERTER.convert_file(self.source, self.source, replace=True)
        os.link(self.source, self.destination)
        with self.assertRaises(ValueError):
            CONVERTER.convert_file(self.source, self.destination, replace=True)
        self.assertEqual(self.source.read_bytes(), Fixture().raw)

    def test_receipt_alias_refuses(self):
        for path in (self.source, self.destination):
            with self.subTest(path=path):
                with self.assertRaises(ValueError):
                    CONVERTER.convert_file(self.source, self.destination, path, replace=True)
        self.assertFalse(self.destination.exists())

    def test_oversized_input_is_refused_without_outputs(self):
        with self.source.open("wb") as stream:
            stream.truncate(CONVERTER.MAX_SIZE + 1)
        with self.assertRaises(ValueError):
            CONVERTER.convert_file(self.source, self.destination)
        self.assertFalse(self.destination.exists())
        self.assertFalse(self.report.exists())

    def test_growth_after_stat_is_caught_by_bounded_read(self):
        stream = mock.MagicMock()
        stream.__enter__.return_value = stream
        stream.read.return_value = bytes(CONVERTER.MAX_SIZE + 1)
        with mock.patch.object(Path, "open", return_value=stream), \
                mock.patch.object(CONVERTER.os, "fstat", return_value=mock.Mock(st_size=52)):
            with self.assertRaises(ValueError):
                CONVERTER.read_bounded(self.source)
        stream.read.assert_called_once_with(CONVERTER.MAX_SIZE + 1)

    def test_invalid_input_publishes_nothing(self):
        self.source.write_bytes(b"invalid ELF")
        with self.assertRaises(ValueError):
            CONVERTER.convert_file(self.source, self.destination)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ["raw.elf"])


if __name__ == "__main__":
    unittest.main()
