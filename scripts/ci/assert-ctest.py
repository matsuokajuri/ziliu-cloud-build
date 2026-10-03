#!/usr/bin/env python3
"""Fail closed on missing, failed, skipped, or unregistered CTest results."""
import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def verify(plan_path, report_path):
    plan = json.loads(Path(plan_path).read_text(encoding="utf-8-sig"))
    expected = {test["name"] for test in plan["tests"]}
    required = {
        "ziliu_core_tests", "ziliu_rime_engine_tests",
        "ziliu_rime_dictionary_epoch_tests", "ziliu_rime_response_binding_tests",
        "ziliu_rime_fresh_profile_tests", "ziliu_rime_preexisting_cache_tests",
        "ziliu_rime_source_profile_tests", "ziliu_rime_user_profile_tests",
        "ziliu_ipc_tests", "ziliu_ui_preview_window_tests",
    }
    if not required <= expected or len(expected) < 33:
        raise ValueError("Incomplete registered test suite")
    cases = list(ET.parse(report_path).getroot().iter("testcase"))
    actual = [case.get("name") for case in cases]
    if len(actual) != len(set(actual)) or set(actual) != expected:
        raise ValueError("JUnit test identities differ from complete CTest plan")
    for case in cases:
        if case.get("status", "run") in {"notrun", "disabled"} or any(
            case.find(tag) is not None for tag in ("skipped", "failure", "error")
        ):
            raise ValueError(f"Test did not pass: {case.get('name')}")
    print(f"Strict CTest gate: {len(cases)} executed, zero failed/skipped")


if __name__ == "__main__":
    verify(*sys.argv[1:])
