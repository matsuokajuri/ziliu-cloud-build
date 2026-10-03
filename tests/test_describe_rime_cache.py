import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/ci/describe-rime-cache.py"
SPEC = importlib.util.spec_from_file_location("describe_rime_cache", SCRIPT)
DIAG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DIAG)


def yaml_bytes(ending=b"\r\n", timestamp=b"123", final=False):
    data = ending.join([b"__build_info:", b"  rime_version: 1.17.0",
        b"  timestamps:", b"    alpha: " + timestamp, b"    beta: 0", b"body:", b"  value: fixed"])
    return data + ending if final else data


def slow_crc(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xedb88320 if value & 1 else 0)
    return value ^ 0xffffffff


def fixture():
    raw = {name: yaml_bytes() if name in DIAG.YAML_FILES else b"dictionary-data"
           for name in DIAG.FILES}
    for name, schemas in DIAG.PRISM_BINDINGS:
        data = bytearray(DIAG.PRISM_HEADER_BYTES)
        data[:14] = b"Rime::Prism/4.0"
        data[32:36] = (97).to_bytes(4, "little")
        data[36:40] = slow_crc(raw[schemas[-1]]).to_bytes(4, "little")
        raw[name] = bytes(data)
    return raw


class CacheDiagnosticsTests(unittest.TestCase):
    def test_timestamp_normalization_preserves_every_other_byte(self):
        for ending in (b"\n", b"\r\n"):
            for final in (True, False):
                self.assertEqual(DIAG.normalize_yaml(yaml_bytes(ending, b"123", final)),
                                 yaml_bytes(ending, b"0", final))
        self.assertEqual(DIAG.normalize_yaml(yaml_bytes(timestamp=b"18446744073709551615")),
                         yaml_bytes(timestamp=b"0"))

        self.assertEqual(DIAG.normalize_yaml(yaml_bytes(timestamp=b"0" * 5000)),
                         yaml_bytes(timestamp=b"0"))

    def test_malformed_yaml_rejected(self):
        valid = yaml_bytes()
        invalid = [b"", valid.replace(b"1.17.0", b"1.18.0"),
            valid.replace(b"\r\n", b"\n", 1), valid + b"\r",
            valid.replace(b"    beta: 0", b"    alpha: 0"),
            valid.replace(b"    alpha: 123", b"    ../bad: 123"),
            valid.replace(b"123", b"-1"), valid.replace(b"123", b"+1"),
            valid.replace(b"123", b"18446744073709551616"),
            valid.replace(b"body:", b"__build_info:"),
            valid.replace(b"body:", b" body:"),
            valid.replace(b"body:", b"body"),
            b"__build_info:\r\n  rime_version: 1.17.0\r\n  timestamps:\r\nbody:\r\n  x: 0"]
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(DIAG.DiagnosticError):
                DIAG.normalize_yaml(data)

    def test_crc_independent_vectors(self):
        self.assertEqual(DIAG.rime_crc32(b""), 0xffffffff)
        self.assertEqual(DIAG.rime_crc32(b"123456789"), 0xd202d277)
        for data in (bytes(range(256)), yaml_bytes(), b"a" * 4097):
            self.assertEqual(DIAG.rime_crc32(data), slow_crc(data))

    def test_valid_report_and_variants(self):
        raw = fixture()
        result = DIAG.describe_files(raw)
        self.assertEqual(result["status"], "fingerprints_only")
        self.assertEqual(len(result["files"]), 14)
        self.assertEqual(result["files"][0]["timestamp_resource_keys"], ["alpha", "beta"])
        self.assertEqual(result["files"][0]["body_line_count"], 2)
        self.assertTrue(all(item["matches"] for item in result["prism_bindings"]))
        melt = result["files"][1]
        self.assertEqual(melt["prism_header"]["dictionary_crc32"], 97)
        blocks = melt["normalized_block_fingerprints"]
        self.assertEqual(blocks["block_bytes"], 4096)
        self.assertEqual(blocks["sha256"], [melt["normalized_sha256"]])
        variants = result["hypothetical_yaml_variant_aggregates_NOT_ADMISSION"]
        self.assertEqual(result["normalized_aggregate_sha256"], variants["crlf_terminal_absent"])
        self.assertNotEqual(result["normalized_aggregate_sha256"], variants["lf_terminal_absent"])
        self.assertNotEqual(variants["crlf_terminal_present"], variants["crlf_terminal_absent"])
        # Independently construct the aggregate record format.
        import hashlib
        records = []
        for name in DIAG.FILES:
            data = raw[name]
            if name.endswith(".yaml"):
                data = data.replace(b"alpha: 123", b"alpha: 0")
            elif name.endswith(".prism.bin"):
                data = data[:36] + b"\0" * 4 + data[40:]
            records.append(name + " " + hashlib.sha256(data).hexdigest() + "\n")
        expected = hashlib.sha256("".join(records).encode("ascii")).hexdigest()
        self.assertEqual(result["normalized_aggregate_sha256"], expected)
        self.assertNotIn("dictionary-data", json.dumps(result))
        self.assertNotIn("value: fixed", json.dumps(result))

    def test_multiple_normalized_blocks(self):
        raw = fixture()
        raw["melt_eng.prism.bin"] += bytes(range(256)) * 40
        result = DIAG.describe_files(raw)["files"][1]
        blocks = result["normalized_block_fingerprints"]["sha256"]
        self.assertEqual(len(blocks), 3)
        data = raw["melt_eng.prism.bin"]
        self.assertEqual(blocks[-1], DIAG.sha256(data[8192:]))
        self.assertNotEqual(blocks[0], DIAG.sha256(data[:4096]))

    def test_prism_binding_and_header_fail_closed(self):
        for offset in (0, 20, 36):
            raw = fixture()
            data = bytearray(raw["melt_eng.prism.bin"])
            data[offset] ^= 1
            raw["melt_eng.prism.bin"] = bytes(data)
            result = DIAG.describe_files(raw)
            self.assertEqual(result["status"], "invalid_artifact")
            self.assertNotIn("normalized_aggregate_sha256", result)

    def test_filesystem_boundaries_and_cli(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            root = base / "test-state"
            root.mkdir()
            for i in range(14):
                cache = root / f"run-{i:02d}-123" / "build"
                cache.mkdir(parents=True)
                for name, data in fixture().items():
                    (cache / name).write_bytes(data)
            (root / "unrelated-sensitive-name").mkdir()
            report = DIAG.describe_root(root)
            self.assertEqual(len(report["runs"]), 12)
            self.assertEqual(report["omitted_run_count"], 2)
            self.assertNotIn("unrelated-sensitive-name", json.dumps(report))
            output = base / "report.json"
            self.assertEqual(DIAG.main([str(root), str(output)]), 0)
            self.assertEqual(json.loads(output.read_text()), report)
            self.assertEqual(DIAG.main([str(root), str(output)]), 1)
            cache = root / "run-00-123" / "build"
            (cache / "secret-user-data").write_text("never output")
            self.assertEqual(DIAG.describe_cache(cache)["status"], "inventory_mismatch")
            (cache / "secret-user-data").unlink()
            with mock.patch.object(DIAG, "MAX_FILE_BYTES", 2):
                self.assertEqual(DIAG.describe_cache(cache)["status"], "byte_limit")
            with mock.patch.object(DIAG, "MAX_TOTAL_BYTES", 2):
                self.assertEqual(DIAG.describe_cache(cache)["status"], "byte_limit")
            with mock.patch.object(DIAG, "MAX_ENTRIES", 2):
                self.assertEqual(DIAG.describe_root(root)["status"], "entry_limit")

    def test_links_and_hardlinks_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            cache = base / "run-1-1" / "build"
            cache.mkdir(parents=True)
            for name, data in fixture().items():
                (cache / name).write_bytes(data)
            target = cache / "default.yaml"
            outside = base / "outside"
            outside.write_bytes(target.read_bytes())
            target.unlink()
            os.link(outside, target)
            self.assertEqual(DIAG.describe_cache(cache)["status"], "unsafe_file")
            target.unlink()
            try:
                target.symlink_to(outside)
            except OSError:
                return  # Windows may not grant symlink creation; hardlink test ran.
            self.assertEqual(DIAG.describe_cache(cache)["status"], "unsafe_file")
            linked_root = base / "linked"
            linked_root.symlink_to(cache.parent, target_is_directory=True)
            self.assertEqual(DIAG.describe_cache(linked_root / "build")["status"], "unsafe_directory")


if __name__ == "__main__":
    unittest.main()
