import ctypes
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "probe_rime_candidates", ROOT / "scripts/probe-rime-candidates.py"
)
PROBE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(PROBE)


class ProbeRimeCandidatesTests(unittest.TestCase):
    def test_librime_struct_sizes_exclude_data_size_field(self):
        for structure in (PROBE.Traits, PROBE.Context, PROBE.Status):
            self.assertEqual(
                PROBE.rime_data_size(structure),
                ctypes.sizeof(structure) - ctypes.sizeof(ctypes.c_int),
            )

    def test_frozen_input_contract_and_hash(self):
        path = ROOT / "tests/data/contextual-ranking-pilot.json"
        cases = PROBE.validate_cases(path)
        self.assertEqual(len(cases), 60)
        self.assertEqual(
            PROBE.sha256(path),
            "16ba1220d695c2beef0aa2179076eb98b0f06bf4b9567bdb071a311dbe6afbda",
        )

    def test_run_directory_is_confined_to_pilot_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            output_root = build / "contextual-ranking-pilot"
            valid = PROBE.validate_run_dir(output_root / "run-1", output_root, build)
            self.assertEqual(valid, output_root / "run-1")
            with self.assertRaises(ValueError):
                PROBE.validate_run_dir(root / "outside", output_root, build)


if __name__ == "__main__":
    unittest.main()
