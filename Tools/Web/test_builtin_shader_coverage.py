#!/usr/bin/env python3
"""Every package a web pass loads ships in the web build, or has a stated reason not to.

A source file that loads one built-in package a web target cooks runs in that target. Each other
package that file loads is then cooked for that target too (a list in builtin_shader_cook.py), or it
is a row of kNotCookedForWeb below with the reason the pass runs without it. A package added to such
a pass without a decision fails here, at the gate, instead of in a browser.

The rule runs once for each target in kTargets. A C++ source counts for every target, because all
targets run one Engine wasm. A render graph counts only for the target that ships it.

The scan reads only the literal "Shaders/<name>.shaderpkg". It does not see a package whose path
the code builds at run time (OceanShaderProgram builds "Shaders/" + name + ".shaderpkg").

    python Tools/Web/test_builtin_shader_coverage.py
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_shader_cook as cook  # noqa: E402

# The trees whose sources and render graphs load built-in packages at run time. Tests are not
# load sites: they name packages that do not exist on purpose.
kSearchRoots = ["Engine", "Apps/Player", "Apps/WebLibrary", "Tools/Web/smoke-project"]
kExcludedPathParts = ("/Tests/", "Tests.cpp", "/test_")
kPackageLiteral = re.compile(r'"Shaders/([A-Za-z0-9_]+)\.shaderpkg"')

# Packages that a pass on the web loads and the web build does not cook, with the reason the
# pass runs without them.
kNotCookedForWeb: dict[str, str] = {
    "ddgi_classify": "the hardware lane's ray queries have no WebGPU equivalent; ddgi_classify_sw runs",
    "ddgi_trace_hw": "the hardware lane's ray queries have no WebGPU equivalent; ddgi_trace_sw runs",
    "ddgi_depth_blend": "the Fine depth tile; the fused ddgi_blend writes the Shared depth tile",
    "ddgi_depth_upload": "the Fine depth tile; the fused ddgi_blend writes the Shared depth tile",
    "ddgi_variability": "the convergence throttle; DDGIProbeFeature checks the kernel before it throttles",
    "hzb_culling": "tint refuses the GPU-driven constructs; culling stays frustum-only",
    "runtime_visibility_aggregate": "tint refuses the GPU-driven constructs; visibility-gated skinning waits",
    "hzb_spd": "binds an array of storage images, which WebGPU cannot bind; HZBBuildNode runs the per-mip chain",
    "hzb_spd_finalize": "binds an array of storage images, which WebGPU cannot bind; HZBBuildNode runs the per-mip chain",
    "hzb_spd_nearest": "binds an array of storage images, which WebGPU cannot bind; HZBBuildNode runs the per-mip chain",
    "hzb_spd_nearest_finalize": "binds an array of storage images, which WebGPU cannot bind; HZBBuildNode runs the "
                                "per-mip chain",
    "sky_render_fog": "SkyRenderer draws the sky without the height fog's sky fill",
    "sky_stars_billboard_fog": "SkyRenderer draws the stars without the height fog's sky fill",
    "volumetric_clouds_noise_normalize": "its ForwardPlus.rendergraph passes declare no WGSL source",
}


# Web target -> the packages its cook ships.
kTargets: dict[str, set[str]] = {
    "player-export": set(cook.kPackages),
    "library-pack": set(cook.kPackages) | set(cook.kLibraryPackages),
    "web-editor": set(cook.kPackages) | set(cook.kEditorPackages),
}
# Render graph folder -> the target that ships the graphs in it.
kGraphTargets = {
    "Tools/Web/smoke-project/Assets/RenderPipelines/": "player-export",
    "Apps/WebLibrary/Assets/RenderPipelines/": "library-pack",
    "Engine/Assets/RenderPipelines/": "web-editor",
}


def TargetsOf(path: str) -> list[str]:
    """The targets that run the loads in the file at path."""
    if not path.endswith(".rendergraph"):
        return list(kTargets)
    return [target for folder, target in kGraphTargets.items() if path.startswith(folder)]


def PackagesBySourceFile() -> dict[str, set[str]]:
    """Source path -> the built-in packages it names, from the committed tree."""
    result = subprocess.run(["git", "-C", str(cook.kRepoRoot), "grep", "-oE", kPackageLiteral.pattern, "--",
                             *kSearchRoots], capture_output=True, text=True, check=True)
    packages: dict[str, set[str]] = {}
    for line in result.stdout.splitlines():
        path, literal = line.split(":", 1)
        if any(part in f"/{path}" for part in kExcludedPathParts):
            continue
        match = kPackageLiteral.fullmatch(literal)
        if match:
            packages.setdefault(path, set()).add(match.group(1))
    return packages


def main() -> int:
    skipped = set(kNotCookedForWeb)
    failures = [f"{target}: {name}: kNotCookedForWeb names a package this target cooks"
                for target, cooked in kTargets.items() for name in sorted(cooked & skipped)]
    named: set[str] = set()
    for path, packages in sorted(PackagesBySourceFile().items()):
        named |= packages
        targets = TargetsOf(path)
        if not targets:
            failures.append(f"{path}: no target ships this render graph: add its folder to kGraphTargets")
        for target in targets:
            cooked = kTargets[target]
            if not packages & cooked:
                continue
            for name in sorted(packages - cooked - skipped):
                failures.append(f"{target}: {path} loads '{name}' beside packages this target cooks: add it to "
                                f"its list in Tools/Web/builtin_shader_cook.py, or to kNotCookedForWeb with the "
                                f"reason the pass runs without it")
    failures += [f"{name}: kNotCookedForWeb names a package no source loads" for name in sorted(skipped - named)]
    for failure in failures:
        print(f"FAIL {failure}")
    print(f"{len(failures)} uncovered built-in package(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
