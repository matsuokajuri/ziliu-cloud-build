import importlib.util
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("pilot", ROOT / "scripts/evaluate-contextual-ranking-pilot.py")
PILOT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PILOT)


class PilotTests(unittest.TestCase):
    def setUp(self):
        self.pool = [{"source_index": i, "text": t, "comment": ""}
                     for i, t in enumerate(["邮箱", "油箱", "油箱"])]

    def test_empty_context_preserves_duplicates_and_order(self):
        self.assertEqual(PILOT.overlap_order("", self.pool), self.pool)

    def test_overlap_uses_no_labels_and_stable_ties(self):
        ordered = PILOT.overlap_order("油", self.pool)
        self.assertEqual([x["source_index"] for x in ordered], [1, 2, 0])

    def test_exact_match_not_substring(self):
        self.assertIsNone(PILOT.target_rank(self.pool, ["箱"]))
        self.assertEqual(PILOT.target_rank(self.pool, ["油箱"]), 2)

    def test_missing_targets_count_as_failures_neutral_excluded(self):
        rows = [{"expected": ["油箱"], "rime_rank": None, "rime_text": "邮箱", "overlap_text": "邮箱"},
                {"expected": [], "rime_rank": None, "rime_text": "邮箱", "overlap_text": "邮箱"}]
        s = PILOT.summarize(rows)
        self.assertEqual(s["labeled"], 1)
        self.assertEqual(s["absent_within_pool"], 1)
        self.assertEqual(s["overlap"]["neutral_churn"], 0)
        self.assertEqual(s["teacher"]["measured"], 0)

    def test_teacher_prompt_does_not_include_expected_or_case_id(self):
        p = PILOT.teacher_prompt({"id": "SECRET-ID", "context": "前文", "input": "yx",
                                  "expected": ["SECRET-LABEL"]}, self.pool)
        self.assertNotIn("SECRET", p)
        self.assertNotIn("expected", p)
        self.assertIn('"typed_keys": "yx"', p)

    def test_validation_rejects_hash_missing_case_and_bad_input(self):
        cases = [{"id": "one", "input": "yx"}]
        result = {"input_sha256": "abc", "cases": [{"id": "one", "status": "ok", "raw_input": "yx", "candidates": self.pool}]}
        self.assertEqual(PILOT.validate(cases, result, "abc")["one"]["raw_input"], "yx")
        with self.assertRaises(ValueError):
            PILOT.validate(cases, result, "wrong")
        with self.assertRaises(ValueError):
            PILOT.validate(cases, {"input_sha256": "abc", "cases": []}, "abc")
        result["cases"][0]["raw_input"] = "nihao"
        with self.assertRaises(ValueError):
            PILOT.validate(cases, result, "abc")

    def test_fixture_shape(self):
        cases = json.loads((ROOT / "tests/data/contextual-ranking-pilot.json").read_text(encoding="utf-8"))
        self.assertEqual(len(cases), 60)
        self.assertEqual(len({c["id"] for c in cases}), 60)
        self.assertEqual(sum(not c["expected"] for c in cases), 6)
        self.assertTrue(all(c["input"].isascii() and c["input"].isalpha() for c in cases))

    def test_annotation_exclusions_are_explicit_and_keep_raw_fixture(self):
        cases = PILOT.read_json(ROOT / "tests/data/contextual-ranking-pilot.json")
        annotations = PILOT.read_json(ROOT / "tests/data/contextual-ranking-pilot.annotations.json")
        excluded = annotations["exclude_from_label_reviewed_metrics"]
        self.assertEqual(set(excluded), {"gongshi-company-full", "fangan-dislike-initials"})
        self.assertEqual(sum(bool(c["expected"]) for c in cases), 54)
        self.assertEqual(sum(bool(c["expected"]) for c in cases if c["id"] not in excluded), 52)


if __name__ == "__main__":
    unittest.main()
