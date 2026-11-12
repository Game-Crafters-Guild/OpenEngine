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
Every shader source the pack's program index names that a third-party port covers
(test_build_web_package.kShaderPortNotices) has its notice in the list a game and the web
editor ship, and the port table's shipped marks agree with the index.

    python3 Tools/Web/engine_pack.py --out C:/scratch/opengine-core.gepak --build-tree build/vs2026-x64-local
    python3 Tools/Web/test_engine_pack.py --pack C:/scratch/opengine-core.gepak

`--program-index <cache>/program-index.bin` checks only the notices of a cooked cache that is
not in a pack (the web editor's). The index names a source as `engine/<path>` (the engine
shader tree) or `<alias>/<path>`; `--source-root <alias>=<dir>` maps each alias the cook
mounted, and an alias no root maps fails. An index with no source, or a source that is not a
file under its root, fails. The index does not record a source under a --package-shaders
folder of the cook, so the check does not see a port there. With no pack and no index the
script runs its own tests.

Exit 0 = the pack matches its layout; 1 = it does not.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import struct
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_web_package  # noqa: E402
import builtin_shader_cook  # noqa: E402
import engine_pack  # noqa: E402
import gepak  # noqa: E402
import test_build_web_package as ports  # noqa: E402


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


kProgramIndexPath = ".workspace/.Cache/Shaders/program-index.bin"
# kShaderProgramIndexVersion (ShaderProgramIndex.h): the layout ProgramIndexSources reads.
kProgramIndexVersion = 3


def ProgramIndexSources(index: bytes) -> list[str]:
    """The source identities a program index records (layout: ShaderProgramIndex.cpp)."""
    magic, version, inputsLength = struct.unpack_from("<8sII", index, 0)
    if magic != b"GEPRGIDX" or version != kProgramIndexVersion:
        raise ValueError(f"not a version {kProgramIndexVersion} program index (magic {magic!r}, version "
                         f"{version}): read the layout ShaderProgramIndex.cpp writes")
    offset = 16 + inputsLength
    (count,) = struct.unpack_from("<Q", index, offset)
    offset += 8
    identities = []
    for _ in range(count):
        (length,) = struct.unpack_from("<I", index, offset)
        identities.append(index[offset + 4:offset + 4 + length].decode("utf-8"))
        offset += 4 + length + 8  # the identity, then its u64 content hash
    return identities


def PortSources(identities: list[str], roots: dict[str, Path]) -> tuple[dict[str, list[str]], list[str]]:
    """The indexed sources of each port row of the table, as repository paths, and one message per
    source label that no root maps."""
    unmapped = sorted({identity.partition("/")[0] for identity in identities} - roots.keys())
    problems = [f"the program index names sources under '{label}', which no --source-root maps"
                for label in unmapped]
    if not identities:
        problems.append("the program index names no source: cook the materials again")
    files = [roots[label] / relative for label, _, relative in (identity.partition("/") for identity in identities)
             if label in roots]
    problems += [f"the program index names {file}, which is not a file: check the --source-root of its alias"
                 for file in files if not file.is_file()]
    sources = [file.resolve().relative_to(engine_pack.kRepoRoot).as_posix() for file in files]
    named = {notice: sorted(source for source in sources
                            if any(fnmatch.fnmatchcase(source, pattern) for pattern in patterns))
             for notice, patterns in ports.kShaderPortNotices.items()}
    return named, problems


def UnlistedPortNotices(named: dict[str, list[str]]) -> list[str]:
    """The port rows whose sources the index names and whose notice a game and the web editor do not ship."""
    listed = {notice.Path for notice in build_web_package.kRepoNotices if not notice.PackageOnly}
    return [f"the program index names {', '.join(sources)}, which {notice} covers, and "
            "build_web_package.kRepoNotices does not ship that notice"
            for notice, sources in named.items() if sources and notice not in listed]


def MarksDisagreeingWithIndex(named: dict[str, list[str]]) -> list[str]:
    """The port rows whose shipped mark in test_build_web_package the index contradicts."""
    problems = [f"{notice} is marked shipped (kShippedPorts), and the program index names none of its sources"
                for notice in sorted(ports.kShippedPorts) if not named[notice]]
    problems += [f"{notice} is marked not shipped (kUnshippedPorts), and the program index names "
                 f"{', '.join(named[notice])}"
                 for notice in sorted(ports.kUnshippedPorts) if named[notice]]
    return problems


def SourceRoot(value: str) -> tuple[str, Path]:
    """An --source-root value, `<alias>=<dir>`, as the alias and the folder."""
    label, separator, directory = value.partition("=")
    if not separator or not label or not directory:
        raise argparse.ArgumentTypeError(f"'{value}' is not <alias>=<dir>")
    return label, Path(directory).resolve()


def ProgramIndexFixture(identities: list[str], version: int = kProgramIndexVersion) -> bytes:
    """A program index that records `identities` and no entry."""
    def String(text: str) -> bytes:
        return struct.pack("<I", len(text)) + text.encode("utf-8")
    sources = b"".join(String(identity) + struct.pack("<Q", 0) for identity in identities)
    return (b"GEPRGIDX" + struct.pack("<I", version) + String("inputs") + struct.pack("<Q", len(identities))
            + sources + struct.pack("<Q", 0))


class ProgramIndexTests(unittest.TestCase):
    kRoots = {"engine": engine_pack.kEngineShaders}
    kOcean = "engine/Ocean/ocean_surface.glsl"

    def Named(self, identities: list[str]) -> dict[str, list[str]]:
        named, problems = PortSources(ProgramIndexSources(ProgramIndexFixture(identities)), self.kRoots)
        self.assertEqual(problems, [])
        return named

    def test_an_indexed_port_source_needs_its_notice_in_the_shipped_list(self):
        named = self.Named([self.kOcean, "engine/Includes/camera_relative.glsl"])
        self.assertEqual(named["Engine/Modules/Ocean/NOTICE"],
                         ["Engine/Modules/Rendering/Shaders/Ocean/ocean_surface.glsl"])
        self.assertEqual(UnlistedPortNotices(named), [])
        withoutOcean = tuple(notice for notice in build_web_package.kRepoNotices
                             if notice.Path != "Engine/Modules/Ocean/NOTICE")
        with mock.patch.object(build_web_package, "kRepoNotices", withoutOcean):
            problems = UnlistedPortNotices(named)
        self.assertEqual(len(problems), 1)
        self.assertIn("Engine/Modules/Ocean/NOTICE", problems[0])
        packageOnlyOcean = withoutOcean + (build_web_package.Notice("Ocean", "Engine/Modules/Ocean/NOTICE",
                                                                    PackageOnly=True),)
        with mock.patch.object(build_web_package, "kRepoNotices", packageOnlyOcean):
            self.assertEqual(len(UnlistedPortNotices(named)), 1)

    def test_an_empty_index_or_a_source_that_is_not_a_file_fails(self):
        for identities in ([], ["engine/Ocean/no_such_source.glsl"]):
            with self.subTest(identities=identities):
                _, problems = PortSources(ProgramIndexSources(ProgramIndexFixture(identities)), self.kRoots)
                self.assertEqual(len(problems), 1)

    def test_a_source_alias_no_root_maps_fails(self):
        index = ProgramIndexFixture([self.kOcean, "eztree/Shaders/Surfaces/ez_tree_leaves.glsl"])
        _, problems = PortSources(ProgramIndexSources(index), self.kRoots)
        self.assertEqual(len(problems), 1)
        self.assertIn("'eztree'", problems[0])
        roots = {**self.kRoots, "eztree": engine_pack.kRepoRoot / "Packages" / "eztree" / "Assets"}
        self.assertEqual(PortSources(ProgramIndexSources(index), roots)[1], [])

    def test_the_shipped_marks_agree_with_the_index(self):
        self.assertEqual(MarksDisagreeingWithIndex(self.Named([self.kOcean])), [])
        problems = MarksDisagreeingWithIndex(self.Named(["engine/vhs.frag"]))
        self.assertEqual(len(problems), 2)
        self.assertIn("Engine/Modules/Ocean/NOTICE is marked shipped", problems[0])
        self.assertIn("ThirdParty/PunikontaVhs/LICENSE.md is marked not shipped", problems[1])

    def test_another_index_version_is_refused(self):
        with self.assertRaises(ValueError):
            ProgramIndexSources(ProgramIndexFixture([self.kOcean], version=kProgramIndexVersion - 1))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    inputs = parser.add_mutually_exclusive_group()
    inputs.add_argument("--pack", type=Path, help="a built opengine-core.gepak")
    inputs.add_argument("--program-index", type=Path, help="a cooked cache's program-index.bin")
    parser.add_argument("--source-root", type=SourceRoot, action="append", default=[], metavar="ALIAS=DIR",
                        help="the repository folder a source alias of the index names")
    args, remaining = parser.parse_known_args()
    if args.pack is None and args.program_index is None:
        unittest.main(argv=[sys.argv[0], *remaining])
    if remaining:
        parser.error(f"unrecognized arguments: {' '.join(remaining)}")
    roots = {"engine": engine_pack.kEngineShaders, **dict(args.source_root)}

    if args.program_index is not None:
        try:
            identities = ProgramIndexSources(args.program_index.read_bytes())
        except (OSError, ValueError, struct.error) as error:
            print(f"FAIL: {args.program_index} is not a readable program index: {error}")
            return 1
        named, problems = PortSources(identities, roots)
        problems += UnlistedPortNotices(named)
        for problem in problems:
            print(f"FAIL: {problem}")
        print(f"test_engine_pack: {args.program_index}, {len(identities)} sources, {len(problems)} problems")
        return 1 if problems else 0

    layout = engine_pack.Layout()
    if len(layout) < 10 or not any(required for _, required in layout):
        print(f"FAIL: engine_pack.py's docstring layout did not parse ({len(layout)} patterns)")
        return 1
    entries = gepak.Unpack(args.pack.read_bytes())
    paths = sorted(entry.Path for entry in entries)
    problems = (CheckPack(paths, layout) + MissingPackages(paths) + PrivateTextureTransformCopies(entries)
                + MissingRenderScaleCrossing(entries))
    index = next((entry.Data for entry in entries if entry.Path == kProgramIndexPath), None)
    if index is None:
        problems.append(f"the pack has no {kProgramIndexPath}")
    else:
        named, indexProblems = PortSources(ProgramIndexSources(index), roots)
        problems += indexProblems + UnlistedPortNotices(named) + MarksDisagreeingWithIndex(named)
    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"test_engine_pack: {len(paths)} files against {len(layout)} patterns, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
