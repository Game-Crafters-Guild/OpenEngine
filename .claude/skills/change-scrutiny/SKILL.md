---
name: change-scrutiny
description: The pass every change runs on itself before its pull request opens — reuse (the engine's types, containers, utilities, controls, the job system), placement (the owner file and module), names, the public surface (whether a symbol should exist, be public, or be an existing API changed instead), the style pass (the whole of CodingStyle.md read against the diff), and the UI/UX pass (whether a user understands and can operate every surface the change adds or alters). Produces the "Reuse and surface" table the pull request body carries. Use after the code compiles and the suites are green, before `gh pr create`; use again at every fold that adds a symbol, a file or a user-facing surface.
---

# Change scrutiny

A change is reviewed twice: once by its author with this pass, once by the reviewer with the review rules. The author's pass is the cheaper one, so it runs first and its table goes into the pull request body under "Reuse and surface". The rules it enforces are `CodingStyle.md` (all of it: the Mathematics Types, Existing First and Public Surface sections carry the reuse and surface questions, section 5 below walks the rest) and `CLAUDE.md`'s engineering values, with section 6 asking whether a user understands what the change shows them; this skill is the method, not a second rule set.

## 1. Inventory the change

From `git diff --stat origin/main...HEAD` and the diff itself, list:

- every file added, with its owner (the module and the class it holds);
- every type, function, constant and setting added or made public (a symbol in a public include folder, a `public:` member, a `*ScriptApi.h` declaration, a setting registered, a component field reflected);
- every piece of behavior written from scratch (a loop over strings, a container, a math expression, a thread, a drawing routine, a UI assembly, a hash);
- every surface a user sees or operates that the change adds or alters, in the editor or the Player: a panel, an inspector section, a setting, a dialog, a menu, a notice, a control, a line of user-facing text.

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
- The file lives with its owner: the module that owns the concept, under the folder its siblings use (`UI/Controls`, `UI/Inspectors`, `Systems`, `Shaders/<Area>`), and inside the feature's own folder when the feature has more than one file (`Panels/ProjectPicker/`, not loose in `Panels/`); a change that adds a second file to a feature creates the folder and moves the first file with it. The table names the folder of every added file.
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
- **Logging and comments.** Nothing verbose on a hot path, warnings and errors only, no enter and exit lines; Doxygen on public APIs; a comment states the invariant, the unit, the convention or the trap as it is now, never history, never what a value used to be, never a word to a reviewer, no author or date stamp; no commented-out code, no vague TODO. Every comment the diff adds or touches is one to three lines of Simplified Technical English (the `writing-simplified-technical-english` skill) and says what the code is, not what changed: a comment that narrates a change, a former value, a removed path or the review that asked for it is deleted; a comment over three lines is cut to its invariant.
- **Tests.** A test name states the behavior; the file sized like its neighbors; scratch checks not committed.
- **Migrate on contact.** Old-style code in a function the change edits is brought to these rules in the same change; an untouched neighbor is left alone (that is a sweep, asked for separately).

Before the line is written, run these over the diff's touched files and paste the result into the pass (the line is a claim; three pull requests in one day carried a false one):

```
git diff --name-only origin/main...HEAD -- '*.cpp' '*.h' '*.py' '*.cs' > /tmp/touched.txt
grep -n -E '\b(ln|rel|m|n|rx|idx|tmp|cnt|mgr|ptr|buf|str|val|res|ctx|obj|elem|i|j|k)\b\s*[=;,)]' $(cat /tmp/touched.txt)     # short names (locals in a loop body of three lines are the only allowed exception: say which)
grep -n -E '\bfloat\s+\w+\[(3|4|9|16)\]|std::array<float,\s*(3|4|9|16)>' $(cat /tmp/touched.txt)              # raw arrays where Vector3, Vector4, Matrix4x4, ColorLinear fit
grep -n -E '^#include' $(cat /tmp/touched.txt)                                                                     # the order: own header, module headers, standard library, third-party
grep -n -E '\b(std::thread|std::async|std::jthread|CreateThread|_beginthread)\b' $(cat /tmp/touched.txt)            # a thread of the change's own
grep -n -E '\[&\]' $(cat /tmp/touched.txt)                                                                        # a lambda capturing everything: a named function instead
grep -n -E '\bTODO\b|\bFIXME\b' $(cat /tmp/touched.txt)                                                          # vague TODOs
grep -n -i -E "^\s*(//|\*|#).*\b($(paste -sd'|' Tools/CleanupGate/history_words.txt))\b" $(cat /tmp/touched.txt)   # change narration in comments: the word list of the cleanup gate's history_comment
```

Fix what the pass and the greps find before the pull request opens. A reviewer walks the same list and runs the same greps, and a style item it finds is a should-fix, not taste. The "Reuse and surface" section of the body ends with one line that names what was checked: `Style pass: names, raw arrays, include order, threads, lambdas, comments checked over N files; nothing left`, or the items left with their reason (a layout a boundary pins, a third-party signature). A line that claims nothing left over a diff with a miss is the finding at the top of the review.

## 6. The UI/UX pass: every surface a user sees or operates

Run it on every change whose inventory lists a user-facing surface. Answer each line for each surface, in writing, against the change as built; a line answered from memory of the design is not answered. Open the surface in an isolated editor or Player, or read its capture, while you answer.

- **Understood at first sight.** A user with no explanation knows what the control does and what each option means. The label is a name a user would say, not an engine-internal noun (not "observer" where the meaning is "seen by the eye or a camera"). The unit and the range show where they matter.
- **The control fits the choice.** A choice among presets is a dropdown, with a thumbnail per preset when they look different; a mode with two values is a toggle or two radio choices; a number with a useful range is a slider with a number box; a list of many is searchable. Four buttons are not a preset picker.
- **Defaults that work.** The default is what most users want on day one. A dial whose non-default values are all worse is deleted. A setting nobody would reach for in a week does not ship.
- **Does what a user expects.** Copy gives feedback. Right-click on an item opens a menu with the actions a user expects there. Double-click does the obvious thing, and the tooltip says what it does. Escape cancels. A long item has a detail view. Undo covers every edit.
- **Consistent with the editor.** The same words, icons, row heights, fonts and spacing as the neighboring panels: the editor's text styles and palette, no new font or color for one panel. A new control is a class in the UI module (or the owning module's UI folder) with XML or typed C++ DSL layout and CSS, never a one-off.
- **Shown, not described.** A capture of each changed surface at rest and in use (the menu open, the notice visible, the long item selected), with the pose or state on the record line, taken against the editor UI checklist in `GameEngine-Workbench/agent/evidence-checklists.md`. The visual critics judge the captures for intuitiveness and look, not only for pixels. A surface the owner asked for is shown to her before it merges.
- **Text.** American spelling, plain words, no other engine's names, and the thing as it is now.

Fix what the pass finds before the pull request opens. The "Reuse and surface" section ends with a second line after the style pass line: `UI/UX pass: first sight, control fit, defaults, expected behavior, consistency, captures, text checked over N surfaces; nothing left`, or the items left with their reason. A change with no user-facing surface writes `UI/UX pass: no user-facing surface`. A reviewer answers the same lines for a UI change; a line that claims nothing left over a surface with a miss is the finding at the top of the review.

## 7. Duplicates and the gate

- A second implementation of anything the engine has is a defect, in this change or found beside it: the change uses the one piece, or extends it, or says in the table why it could not (the piece is wrong) and files the issue.
- Where the cleanup gate test exists (`CleanupGateTests`, `Tools/CleanupGate/`), run it; a count above the baseline is this change's to fix before the pull request opens.

## 8. The table in the pull request body

```
## Reuse and surface
| Added | Kind | Engine piece used or why not | Public? Caller outside the module | Verdict |
|---|---|---|---|---|
| Foo::Bar() | function | Types/StringUtils.h TrimWhitespace | no (module-internal) | kept |
| BazCurve | type | Mathematics/Interpolation.h lacked Wrap: extended there (commit …) | yes: the Day Cycle window | kept, documented, tested |
| float heights[16] | field | boundary: the GPU upload struct pins its layout (comment names it) | no | kept, allowlisted |
```

A change with nothing public and nothing written from scratch says so in one line, followed by the style pass line and the UI/UX pass line. A reviewer reads this table first; a table that lies is a finding at the top of the review.
