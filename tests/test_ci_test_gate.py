"""Regression checks for CI's fail-closed native-test report validator."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('ci_test_gate', ROOT / 'scripts/ci/assert-ctest.py')
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class StrictCTestGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.plan = Path(self.temp.name) / 'plan.json'
        self.report = Path(self.temp.name) / 'report.xml'
        names = re.findall(r'add_test\(\s*NAME\s+(\w+)', (ROOT / 'tests/CMakeLists.txt').read_text())
        self.plan.write_text(json.dumps({'tests': [{'name': name} for name in names]}))
        self.root = ET.Element('testsuite')
        for name in names:
            ET.SubElement(self.root, 'testcase', name=name, status='run')

    def verify(self):
        ET.ElementTree(self.root).write(self.report)
        with contextlib.redirect_stdout(io.StringIO()):
            GATE.verify(self.plan, self.report)

    def test_complete_suite_passes(self):
        self.verify()

    def test_skip_failure_and_error_are_rejected(self):
        for tag in ('skipped', 'failure', 'error'):
            with self.subTest(tag=tag):
                node = ET.SubElement(self.root[0], tag)
                with self.assertRaises(ValueError):
                    self.verify()
                self.root[0].remove(node)

    def test_missing_test_is_rejected(self):
        self.root.remove(self.root[0])
        with self.assertRaises(ValueError):
            self.verify()

    def test_duplicate_test_is_rejected(self):
        self.root.append(self.root[0])
        with self.assertRaises(ValueError):
            self.verify()

    def test_incomplete_registration_is_rejected(self):
        self.plan.write_text(json.dumps({'tests': []}))
        with self.assertRaises(ValueError):
            self.verify()
