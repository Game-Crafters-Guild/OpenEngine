---
name: merge-main
description: The merge of main into a branch before it lands, its checks, and the test that lets the last fold skip the merge and its rebuild. Use at the end of a branch's last fold, before any decision not to merge main, and when a review or a merge checks a "no merge rebuild" line.
---

# The merge with main

Merge `origin/main` into the branch once, at the end of its last fold, never mid-stream, and build the merged tree before calling a change ready. A clean textual merge does not mean the result compiles, and a clean `git merge-tree` exit does not mean the merge is only your change: `git diff --stat origin/main <merged tree>` lists the branch's own files and nothing else, or the branch undoes something main gained.

A file in common that is only the cleanup gate's `baseline.json` does not force a merge rebuild when `git merge-tree --write-tree` exits 0 and the cleanup gate passes on the merged tree; the body says so.

## The skip test

At the end of the last fold, the branch skips this merge and its rebuild when the two change sets do not overlap, so the two build stacked. Every line below holds:

- `git merge-tree --write-tree origin/main <head>` exits 0.
- `git diff --name-only <merge-base> origin/main` lists no file the branch touches.
- No header (`.h`, `.hpp`, `.inl`), shader include (`.glsl`), schema or generated-binding source (`*.schema*`, the `Managed/` C# the binding generator reads) that main changed is included by a file the branch touched, directly or through a public header of its own or another module that includes it. The check is `git grep -l` of each changed header's name and of its module's include prefix over the branch's touched files, then once more for each public header that includes it. A changed header under `Foundation`, `Types` or `Mathematics` reaches every branch and counts as overlap. A changed header under `Engine/Include` counts as overlap only when that `git grep` names it from a branch file or from a public header a branch file includes.
- No CMake file main changed (`CMakeLists.txt`, `*.cmake`, `CMakePresets.json`) belongs to a target that compiles a branch file or to the top level, and main changed no `vcpkg.json` and no C# project file (`*.csproj`, `*.props`, `*.targets`) of a branch target.
- Main changed no test asset or staged data file (`Tests/Assets`, the editor's staged XML and CSS) that a focused suite of the branch reads.

A changed shader stage or render graph (`.frag`, `.vert`, `.comp`, `.rendergraph`, `.shaderpkg`) overlaps only a branch that touches shaders or the Rendering module.

The pull request body then states "main's delta <base>..<main SHA>: N files in <modules>; no file in common, and none of its headers, shaders, schemas or build files reach a branch file; no merge rebuild", and the squash merge takes the head.
