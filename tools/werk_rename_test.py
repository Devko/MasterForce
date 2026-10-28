#!/usr/bin/env python3
"""Tests for werk_rename.py: each one builds a scratch git repo and runs the script on it as a user would.

    python3 -m unittest tools/werk_rename_test.py -v
"""
import os
import subprocess
import sys
import tempfile
import unittest

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "werk_rename.py")


class WerkRenameTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name
        subprocess.run(["git", "init", "-q", self.repo], check=True)
        subprocess.run(["git", "-C", self.repo, "config", "core.autocrlf", "false"], check=True)

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, rel, data, track=True):
        path = os.path.join(self.repo, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode())
        if track:
            subprocess.run(["git", "-C", self.repo, "add", rel], check=True)

    def get(self, rel):
        with open(os.path.join(self.repo, rel), "rb") as f:
            return f.read().decode()

    def run_script(self, *args):
        env = dict(os.environ, PYTHONIOENCODING="")   # the console's own encoding, as on a Windows terminal
        return subprocess.run([sys.executable, SCRIPT, *args, self.repo], capture_output=True, encoding="utf-8",
                              errors="replace", env=env)

    def test_renames_three_case_forms(self):
        self.put("a.txt", "SubForce subforce SUBFORCE\nMasterForce EffectForce PolyForce DrumForce GrooveForce AmbientForce RackForce\n"
                          "subforce_stages.so SubForceStageTimes\n")
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(self.get("a.txt"), "SubWerk subwerk SUBWERK\nMasterWerk EffectWerk PolyWerk DrumWerk GrooveWerk AmbientWerk RackWerk\n"
                                            "subwerk_stages.so SubWerkStageTimes\n")

    def test_leaves_device_names(self):
        text = "/media/AkaiForce/x the Force force-shadow Akai Force FORCE\n"
        self.put("a.txt", text)
        self.run_script()
        self.assertEqual(self.get("a.txt"), text)

    def test_keep_marker(self):
        text = 'constexpr const char* kLegacyMagic = "subforce ";  // werk-rename: keep\n'
        self.put("state.cpp", text + "SubForce\n")
        self.run_script()
        self.assertEqual(self.get("state.cpp"), text + "SubWerk\n")

    def test_idempotent(self):
        self.put("a.txt", "SubForce\n")
        self.assertIn("1 lines in 1 files", self.run_script().stdout)
        self.assertIn("0 lines in 0 files", self.run_script().stdout)

    def test_check_exit_codes(self):
        self.put("a.txt", "SubForce\n")
        before = self.run_script("--check")
        self.assertEqual(before.returncode, 1)
        self.assertIn("a.txt:1: SubForce", before.stdout)
        self.assertEqual(self.get("a.txt"), "SubForce\n")
        self.run_script()
        self.assertEqual(self.run_script("--check").returncode, 0)

    def test_check_prints_any_text(self):
        self.put("a.md", "a ≤ b − c ✅ SubForce\n")
        result = self.run_script("--check")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("SubForce", result.stdout)

    def test_check_catches_odd_casing(self):
        self.put("a.txt", "Subforce\n")
        self.assertEqual(self.run_script("--check").returncode, 1)

    def test_check_reports_paths(self):
        self.put("presets/subforce.txt", "nothing to rename\n")
        result = self.run_script("--check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("presets/subforce.txt", result.stdout)

    def test_skips_untracked_and_binary(self):
        self.put("untracked.txt", "SubForce\n", track=False)
        self.put("blob.dat", b"SubForce\x00\x01")
        self.run_script()
        self.assertEqual(self.get("untracked.txt"), "SubForce\n")
        self.assertEqual(self.get("blob.dat"), "SubForce\x00\x01")

    def test_skips_plan_docs_and_itself(self):
        self.put("docs/superpowers/plans/plan.md", "SubForce\n")
        self.put("tools/werk_rename.py", "SubForce\n")
        self.run_script()
        self.assertEqual(self.get("docs/superpowers/plans/plan.md"), "SubForce\n")
        self.assertEqual(self.get("tools/werk_rename.py"), "SubForce\n")
        self.assertEqual(self.run_script("--check").returncode, 0)

    def test_keeps_crlf(self):
        self.put("a.txt", "SubForce\r\nx\r\n")
        self.run_script()
        self.assertEqual(self.get("a.txt"), "SubWerk\r\nx\r\n")


if __name__ == "__main__":
    unittest.main()
