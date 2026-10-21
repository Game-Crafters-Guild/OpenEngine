#!/usr/bin/env python3
"""Assert a built opengine-core.gepak holds what engine_pack.py's docstring says it holds.

Every file in the pack must match a pattern of the docstring's layout, and every pattern
marked `!` must match at least one file: a pack that drops the render pipeline, an engine
shader directory, the cooked packages, the font or the variant cache fails here instead of as
an empty canvas in a browser. Every built-in package the pack's tables name
(builtin_shader_cook.kPackages and kLibraryPackages) must be in it, so a pass the library's
pipeline runs never finds its program missing at runtime.
No cooked fragment stage copies its material's texture transform rows into a private array,
which a mobile GPU keeps in memory for every pixel, and the pipeline has the pass a camera's
render scale crosses back to the display through.

    python3 Tools/Web/engine_pack.py --out C:/scratch/opengine-core.gepak --build-tree build/vs2026-x64-local
    python3 Tools/Web/test_engine_pack.py --pack C:/scratch/opengine-core.gepak

Exit 0 = the pack matches its layout; 1 = it does not.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builtin_shader_cook  # noqa: E402
import engine_pack  # noqa: E402
import gepak  # noqa: E402


def PatternRegex(pattern: str) -> re.Pattern[str]:
    parts = []
    for token in re.split(r"(\*\*/|\*\*|\*)", pattern):
        if token == "**/":
            parts.append(r"(?:.*/)?")
        elif token == "**":
            parts.append(r".*")
        elif token == "*":
            parts.append(r"[^/]*")
        else:
            parts.append(re.escape(token))
    return re.compile("".join(parts) + r"\Z")


def CheckPack(paths: list[str], layout: list[tuple[str, bool]]) -> list[str]:
    """The ways `paths` departs from `layout`, one message each; empty when it matches."""
    regexes = [(pattern, required, PatternRegex(pattern)) for pattern, required in layout]
    problems = [f"{path} matches no layout pattern" for path in paths
                if not any(regex.match(path) for _, _, regex in regexes)]
    problems += [f"no file matches the required pattern {pattern}" for pattern, required, regex in regexes
                 if required and not any(regex.match(path) for path in paths)]
    return problems


def MissingPackages(paths: list[str]) -> list[str]:
    """The built-in packages the pack's tables name that the pack does not hold, one message each."""
    present = set(paths)
    names = {**builtin_shader_cook.kPackages, **builtin_shader_cook.kLibraryPackages}
    return [f"the table names {name} but the pack has no Assets/Shaders/{name}.shaderpkg"
            for name in sorted(names) if f"Assets/Shaders/{name}.shaderpkg" not in present]


def ShaderPkgChunk(package: bytes, name: str) -> bytes | None:
    """One named chunk of a .shaderpkg (layout: ShaderPackageContainer.h), or None."""
    magic, _, count = struct.unpack_from("<8sII", package, 0)
    if magic != b"GESHDRPK":
        return None
    for i in range(count):
        chunkName, _, _, offset, size = struct.unpack_from("<16sIIQQ", package, 16 + i * 40)
        if chunkName.rstrip(b"\0").decode() == name:
            return package[offset:offset + size]
    return None


# A function-scope array of eight vec4s in a material's fragment stage. The Mali compiler keeps
# such an array in memory, about half the forward pass on a Mali-G78, so the compat profile never
# copies the texture transform rows (the material's TextureST) into one: it copies them as whole
# arrays (surface_io.glsl, adapter_forward.glsl), and a cooked pack must hold none. The pattern
# matches any array of that shape, not only the rows.
kPrivateTextureTransformRows = re.compile(r"\bvar \w+: array<vec4<f32>, 8>;")


def PrivateTextureTransformCopies(entries: list[gepak.PackEntry]) -> list[str]:
    """The cooked variants whose fragment stage copies texture transform rows per pixel, one message each."""
    problems = []
    for entry in entries:
        if not entry.Path.startswith(".workspace/.Cache/Shaders/"):
            continue
        fragment = ShaderPkgChunk(entry.Data, "fs-wgsl")
        if fragment is not None and kPrivateTextureTransformRows.search(fragment.decode("utf-8", "replace")):
            problems.append(f"{entry.Path} declares an 8 x vec4 private array in its fragment stage "
                            "(such as the texture transform rows copied per pixel)")
    return problems


def MissingRenderScaleCrossing(entries: list[gepak.PackEntry]) -> list[str]:
    """The pipeline lacks the pass a camera's render scale crosses back to the display through."""
    pipeline = next((entry for entry in entries if entry.Path == engine_pack.kPipeline.as_posix()), None)
    if pipeline is None:
        return []
    # RenderPipeline ignores a view's render scale on a graph without a RenderScaleUpscale node.
    passes = json.loads(pipeline.Data.decode("utf-8"))["passes"]
    if not any(p.get("type") == "RenderScaleUpscale" and p.get("enabled", True) for p in passes):
        return [f"{pipeline.Path} has no RenderScaleUpscale pass: Camera.renderScale does nothing on the web"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pack", type=Path, required=True, help="a built opengine-core.gepak")
    args = parser.parse_args()

    layout = engine_pack.Layout()
    if len(layout) < 10 or not any(required for _, required in layout):
        print(f"FAIL: engine_pack.py's docstring layout did not parse ({len(layout)} patterns)")
        return 1
    entries = gepak.Unpack(args.pack.read_bytes())
    paths = sorted(entry.Path for entry in entries)
    problems = (CheckPack(paths, layout) + MissingPackages(paths) + PrivateTextureTransformCopies(entries)
                + MissingRenderScaleCrossing(entries))
    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"test_engine_pack: {len(paths)} files against {len(layout)} patterns, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
