#!/usr/bin/env python3
"""Cook every built-in shader stage a web build ships to WGSL.

The stages are the union of the lists builtin_shader_cook.py cooks: the Player
packages, the editor packages, the library packages engine_pack.py adds and the
package-less stage shaders. A stage that naga or tint refuses fails here, at the
gate, instead of at the release package cook. Exit 125 (CTest SKIPPED) when the
toolchain is not provisioned.
"""

from __future__ import annotations

import os
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_shader_cook as cook  # noqa: E402
import shadercook  # noqa: E402  (on the path through builtin_shader_cook)

kCannotRun = 125
kShaderSourceDir = cook.kRepoRoot / "Engine" / "Modules" / "Rendering" / "Shaders"
# Each cook is four short tool processes; a lane's build share bounds the parallelism.
kMaxWorkers = 12


def PackageStages() -> set[tuple[str, tuple[str, ...], bool]]:
    """(GLSL source, extra defines, shared include roots) for every built-in stage.

    A package stage cooks without the shared roots, a stage shader with them, as in builtin_shader_cook.py.
    """
    stages: set[tuple[str, tuple[str, ...], bool]] = set()
    for packages in (cook.kPackages, cook.kEditorPackages, cook.kLibraryPackages):
        for package in packages.values():
            for stageSource in package.values():
                sourceName, defines = (stageSource, []) if isinstance(stageSource, str) else stageSource
                stages.add((sourceName, tuple(defines), False))
    for sourceName, defines in cook.kEditorStageShaders.values():
        stages.add((sourceName, tuple(defines), True))
    return stages


def CookStage(stage: tuple[str, tuple[str, ...], bool], toolchain, scratchRoot: Path) -> str | None:
    """Return None when the stage cooks, else a one-line failure."""
    sourceName, defines, sharedRoots = stage
    glslc, naga, tint = toolchain
    source = cook.ResolveShaderSource(sourceName, kShaderSourceDir)
    stageName = cook.kStageByExtension.get(source.suffix)
    if stageName is None:
        return f"{sourceName}: cannot infer a stage"
    scratch = Path(tempfile.mkdtemp(dir=scratchRoot))
    includeDirs = [kShaderSourceDir, source.parent] + (cook.kSharedShaderRoots if sharedRoots else [])
    try:
        shadercook.CookFile(source, stageName, glslc, naga, tint, True,
                            cook.kWebDefines + list(defines), includeDirs,
                            scratch / f"{source.stem}.wgsl", scratch)
    except shadercook.CookError as error:
        detail = " | ".join(line for line in str(error).splitlines()[1:4] if line.strip())
        return f"{sourceName} {list(defines)}: {detail}"
    return None


def main() -> int:
    try:
        toolchain = cook.FindToolchain()
    except RuntimeError as error:
        print(f"SKIP: {error}")
        return kCannotRun
    if toolchain[2] is None:
        print("SKIP: tint not found - run Tools/ShaderCook/toolchain/setup.py to fetch it")
        return kCannotRun

    stages = sorted(PackageStages())
    with tempfile.TemporaryDirectory() as tempDir:
        workers = min(kMaxWorkers, os.cpu_count() or 1)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(lambda stage: CookStage(stage, toolchain, Path(tempDir)), stages))
    failures = [result for result in results if result is not None]
    for failure in failures:
        print(f"FAIL {failure}")
    print(f"{len(stages) - len(failures)} of {len(stages)} built-in stages cook to WGSL")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
