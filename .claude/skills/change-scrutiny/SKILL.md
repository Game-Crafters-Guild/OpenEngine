---
name: change-scrutiny
description: The pass every change runs on itself before its pull request opens — reuse (the engine's types, containers, utilities, controls, the job system), placement (the owner file and module), names, the public surface (whether a symbol should exist, be public, or be an existing API changed instead), and the style pass (the whole of CodingStyle.md read against the diff). Produces the "Reuse and surface" table the pull request body carries. Use after the code compiles and the suites are green, before `gh pr create`; use again at every fold that adds a symbol or a file.
---

# Change scrutiny

A change is reviewed twice: once by its author with this pass, once by the reviewer with the review rules. The author's pass is the cheaper one, so it runs first and its table goes into the pull request body under "Reuse and surface". The rules it enforces are `CodingStyle.md` (all of it: the Mathematics Types, Existing First and Public Surface sections carry the reuse and surface questions, section 5 below walks the rest) and `CLAUDE.md`'s engineering values; this skill is the method, not a second rule set.

## 1. Inventory the change

From `git diff --stat origin/main...HEAD` and the diff itself, list:

- every file added, with its owner (the module and the class it holds);
- every type, function, constant and setting added or made public (a symbol in a public include folder, a `public:` member, a `*ScriptApi.h` declaration, a setting registered, a component field reflected);
- every piece of behavior written from scratch (a loop over strings, a container, a math expression, a thread, a drawing routine, a UI assembly, a hash).

## 2. Reuse: for each piece written from scratch, find the engine's piece

Search before concluding it does not exist; the search is the evidence, named in the table.

| What the change does | Where the engine already does it |
|---|---|
| a position, direction, scale, color, quaternion, matrix, bounding box | `Engine/Modules/Mathematics` (`Vector2/3/4`, `Quaternion`, `Matrix4x4`, `Geometry.h`), `Types/Color.h` (`Color`, `ColorLinear`) |
| lerp, clamp to 0..1, smoothstep, remap, easing, noise | `Mathematics/Interpolation.h` (`Lerp`, `Clamp01`, `SmoothStep`), `Mathematics`, `Noise` |
| split, trim, join, case fold, prefix tests, number and byte formatting, parsing, path joining | `Types/StringUtils.h`, `Types/FormatNumber.h`, `Types/ParseNumber.h`, `Types/PathUtils.h`, `Types/NearestName.h` |
| a map, a set, a handle table, a view over a run, a hash | `Types/FlatMap.h` (and `FlatSet` when it lands), `Foundation/GenerationalVector`, `std::span`, `Types/Fnv1a.h` |
| parallel or deferred work, a worker, a poll, a future | `Engine/Modules/JobSystem` (a job, a batch, a continuation; a pool build like `HeightFieldShapeBuilder`) |
| a log line | `Logger` |
| a pool or arena, a frame allocator | `Engine/Modules/Memory` |
| a render pass, a GPU resource, a barrier | the render graph (`Engine/Source/Engine/Rendering` nodes; the pass declares its resources) |
| a drawn handle, a gizmo, a line in the Scene View | `Apps/Editor/Include/SceneView/SceneViewGizmos.h` |
| an inspector row, a section, a property editor | `Apps/Editor/Include/UI/InspectorSection.h`, the inspector registry |
| a control | `Engine/Modules/UI/Include/UI/Controls`, the module's own `UI/Controls` folder |
| a settings page, a post effect, a dockable, a search item | the registries (`Editor::EditorSettingsRegistry` and the others) |

The search: `ls Engine/Modules/`, then `grep -rn "<verb or noun>" Engine/Modules/<Candidate>/Include` for the function, and the test map for the suite that covers it. A piece that exists is used; a piece that exists but lacks a function is extended in its own file, in its own commit, then used; a piece that does not exist is added beside its siblings, never beside the caller.

## 3. Placement and names

- One class per file, named for the class; a free function in its module's utility header beside its kin, never a header of its own; nothing new in a hub (`EditorApplication`, `EditorContext`, `SettingsPanel`, `InspectorPanel`, `HierarchyPanel`, `SceneViewPanel`, `RenderServices`, `UIManager`, `UIElement`, `core.css`).
- The file lives with its owner: the module that owns the concept, under the folder its siblings use (`UI/Controls`, `UI/Inspectors`, `Systems`, `Shaders/<Area>`).
- Names say what the thing does for the caller, in full words, PascalCase for types and functions, `m_PascalCase` for private members, `kPascalCase` for constants; a name that needs a comment to be understood is the wrong name.

## 4. The public surface, one question set per symbol

Ask these of every symbol the inventory marked public, and write the answer in the table:

1. **Who needs it, outside the module, today?** Name the caller in this change. No caller: the symbol is private, or deleted. "A future caller" is no caller.
2. **Does an existing API already carry this?** A sibling that does nearly the same thing is changed to do this thing, with its callers; a twin (an immediate, fast, asynchronous or overloaded version) is not added.
3. **Is a new symbol the right end state, or does a refactor remove the need?** When the need comes from a shape that is wrong (a flag threaded through, a special case, a pass-through), the shape is fixed instead.
4. **Is it a game-facing API?** Then it ships with its user documentation and a test, judged by what a game developer needs, not by who calls it inside the engine.
5. **Is the name the caller's word for it?** The verb for a function, the noun for a type; the engine's own terms where it has one.
6. **What is its narrowest reach?** Private by default; a `.cpp`-local or an internal header before a public include; a public include only for the consumer named in question 1.

A public symbol is a promise the engine keeps for every future caller; the smallest promise that serves the named caller is the right one.

## 5. The style pass: the whole of `CodingStyle.md` against the diff

Read the diff against every section of the style guide, not from memory. The ones that bite:

- **Names.** Functions and types PascalCase; private members `m_PascalCase`; public data members of a plain aggregate PascalCase; locals and parameters camelCase; file-scope statics `s_PascalCase`; constants `kPascalCase` at every scope, a named constant inside a function included. Full words: no `idx`, `tmp`, `cnt`, `mgr`, `ptr`. A boolean reads as a condition (`IsVisible`, `HasParent`). The verb matches the cost and the contract: `Get` returns what is there without side effects, `Find` may fail, `Compute` and `Build` cost something, `Try` returns whether it did.
- **Files and headers.** One class per file, the file named for it; forward declarations in the header, includes in the `.cpp`; include order own header, module headers, standard library, third-party; quotes for project headers, angle brackets for external ones; `public`, `protected`, `private` in that order; no `using namespace` and no global-scope symbol in a header; everything inside the project's namespaces.
- **Lambdas.** A short adapter handed to an API. One that grows past a handful of lines, captures `[&]` across unrelated state, is assigned to a local and called once, or is needed twice becomes a named function in the owning file; no function built from nested lambdas.
- **Enums, literals and types.** `enum class`, with an explicit underlying type where layout matters; fixed-width integers where layout matters; `std::chrono` for time, `std::filesystem` for paths; `nullptr`; no magic number: a `k` constant whose name or comment states the unit.
- **Control flow and errors.** Early returns over nesting; a hot path straight-line; no exceptions on a hot path, validation and early returns instead; the project's assert macro, never with a side effect inside it.
- **Concurrency.** Thread affinity and ownership stated on a public API; the job system (`Query::Parallel`, `JobCounter`) over a thread of the change's own; immutability first, then a named synchronization primitive.
- **Templates and copies.** A public template constrained by a concept or a `requires` clause; a large template body in an `.inl`; move operations for a heavy type, copy semantics deleted or defined on purpose.
- **Logging and comments.** Nothing verbose on a hot path, warnings and errors only, no enter and exit lines; Doxygen on public APIs; a comment states the invariant, the unit, the convention or the trap as it is now, never history, never what a value used to be, never a word to a reviewer, no author or date stamp; no commented-out code, no vague TODO.
- **Tests.** A test name states the behavior; the file sized like its neighbors; scratch checks not committed.
- **Migrate on contact.** Old-style code in a function the change edits is brought to these rules in the same change; an untouched neighbor is left alone (that is a sweep, asked for separately).

Fix what the pass finds before the pull request opens. A reviewer walks the same list, and a style item it finds is a should-fix, not taste. The "Reuse and surface" section of the body ends with one line: `Style pass: nothing left`, or the items left with their reason (a layout a boundary pins, a third-party signature).

## 6. Duplicates and the gate

- A second implementation of anything the engine has is a defect, in this change or found beside it: the change uses the one piece, or extends it, or says in the table why it could not (the piece is wrong) and files the issue.
- Where the cleanup gate test exists (`CleanupGateTests`, `Tools/CleanupGate/`), run it; a count above the baseline is this change's to fix before the pull request opens.

## 7. The table in the pull request body

```
## Reuse and surface
| Added | Kind | Engine piece used or why not | Public? Caller outside the module | Verdict |
|---|---|---|---|---|
| Foo::Bar() | function | Types/StringUtils.h TrimWhitespace | no (module-internal) | kept |
| BazCurve | type | Mathematics/Interpolation.h lacked Wrap: extended there (commit …) | yes: the Day Cycle window | kept, documented, tested |
| float heights[16] | field | boundary: the GPU upload struct pins its layout (comment names it) | no | kept, allowlisted |
```

A change with nothing public and nothing written from scratch says so in one line, followed by the style pass line. A reviewer reads this table first; a table that lies is a finding at the top of the review.
