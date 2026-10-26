#!/usr/bin/env python3
"""Tools/Scripts/migrate_blueprints_to_prefabs.py on a project that holds every scene blueprint form
the loader read: the result is Tests/PrefabMigration/expected byte for byte."""
import filecmp
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "Tools/Scripts/migrate_blueprints_to_prefabs.py"
FIXTURE = ROOT / "Tests/PrefabMigration"


def run(project: Path, *flags: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(SCRIPT), *flags, str(project)], capture_output=True, text=True)


def tree(folder: Path) -> dict[str, bytes]:
    return {p.relative_to(folder).as_posix(): p.read_bytes() for p in sorted(folder.rglob("*")) if p.is_file()}


class PrefabMigrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix="ge_prefab_migration_"))
        self.project = self.temp / "project"
        shutil.copytree(FIXTURE / "input", self.project)

    def tearDown(self):
        shutil.rmtree(self.temp, ignore_errors=True)

    def test_converts_the_project_to_the_expected_files_and_a_second_run_changes_nothing(self):
        result = run(self.project)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("changed: Scenes/Lakeside.scene", result.stdout)
        self.assertIn("renamed: Blueprints/bp.blueprint -> Blueprints/bp.prefab", result.stdout)
        self.assertEqual(tree(self.project), tree(FIXTURE / "expected"))

        again = run(self.project)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn("0 prefab(s) renamed, 0 file(s) changed.", again.stdout)

    def test_a_dry_run_writes_nothing(self):
        result = run(self.project, "--dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("would rename: Blueprints/bp.blueprint -> Blueprints/bp.prefab", result.stdout)
        self.assertEqual(tree(self.project), tree(FIXTURE / "input"))

    @unittest.skipIf(shutil.which("git") is None, "git is not on PATH")
    def test_refuses_a_work_tree_with_uncommitted_changes(self):
        subprocess.run(["git", "init", "-q", str(self.project)], check=True)
        result = run(self.project)
        self.assertEqual(result.returncode, 2)
        self.assertIn("uncommitted change(s). Commit or stash them", result.stderr)
        self.assertTrue(filecmp.cmp(self.project / "Blueprints/bp.blueprint", FIXTURE / "input/Blueprints/bp.blueprint",
                                    shallow=False))


if __name__ == "__main__":
    unittest.main()
