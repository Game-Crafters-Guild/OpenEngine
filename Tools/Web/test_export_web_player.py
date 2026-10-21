#!/usr/bin/env python3
"""export_web_player.py stages the project content the engine lists and nothing else: a
project's C# and C++ sources and its Editor/ folders sit beside its assets, and only the
files the engine's content list names (StageWebExportContent) reach the web build.

    python Tools/Web/test_export_web_player.py
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_web_player  # noqa: E402


def WriteFile(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class ProjectContentTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="ge_web_export_content_"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.project = self.root / "project"
        for relative, text in {
            "game.config": "{}",
            "Assets/Scenes/Main.scene": "[scene name=\"Main\"]",
            "Assets/Textures/Rock.png": "PNGDATA",
            "Assets/Scripts/Player.cs": "class Player {}",
            "Assets/Scripts/Native/AI.cpp": "int Think();",
            "Assets/Editor/MyTool.cs": "class MyTool {}",
        }.items():
            WriteFile(self.project / relative, text)
        self.staging = self.root / "staging"
        self.staging.mkdir()
        self.contentList = self.root / "content.json"

    def WriteContentList(self, paths: list[str]) -> None:
        files = [{"source": str(self.project / path), "path": path} for path in paths]
        self.contentList.write_text(json.dumps({"files": files}))

    def testOnlyTheListedContentIsStaged(self):
        self.WriteContentList(["Assets/Scenes/Main.scene", "Assets/Textures/Rock.png"])
        export_web_player.CopyProjectContent(self.project, self.contentList, self.staging)
        self.assertEqual(export_web_player.StagedRelativePaths(self.staging),
                         ["Assets/Scenes/Main.scene", "Assets/Textures/Rock.png", "game.config"])

    def testAPathOutsideTheBuildIsRefused(self):
        outside = self.root / "outside.txt"
        forms = ["../outside.txt", "Assets/../../outside.txt", "Assets/..", outside.as_posix()]
        if os.name == "nt":
            # The host's own path rules: backslashes, drives, UNC shares and the drive root.
            drive, rest = os.path.splitdrive(str(outside))
            forms += ["..\\outside.txt", "Assets\\..\\..\\outside.txt", str(outside),
                      f"\\\\localhost\\{drive[0]}${rest}", "\\outside.txt", f"{drive}outside.txt"]
        for form in forms:
            with self.subTest(path=form):
                self.contentList.write_text(json.dumps(
                    {"files": [{"source": str(self.project / "Assets/Textures/Rock.png"), "path": form}]}))
                with self.assertRaisesRegex(export_web_player.ExportError, "outside the build"):
                    export_web_player.CopyProjectContent(self.project, self.contentList, self.staging)
                self.assertFalse(outside.exists())


if __name__ == "__main__":
    unittest.main()
