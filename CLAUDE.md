# CLAUDE.md

@AGENTS.md

Guidance beyond the shared entry point imported above; its Map names where the rest lives, and this file wins where files disagree. An area's rules file under `.claude/rules/` loads by itself only when you open a matching file in the session's own checkout: in any other worktree, read the file the Map names for the area you touch. The workbench is added to a session with `--add-dir`.

## Engineering values

The bar for every change: high performance, high code quality, clean architecture, high usability. Aim for the best end state for the engine, not the fastest thing that works.

- **Fix causes, not symptoms.** When a bug exposes a design flaw, the design is the bug. Prefer the scoped refactor that leaves the engine simpler; if the right fix is bigger than the ask, say so and propose it.
- **No legacy by default.** There are no users or released projects to preserve. For every API, saved format and removal: update all current callers and committed assets, delete the obsolete path, keep only the supported implementation. No backward compatibility, saved-data migrations, deprecated aliases, fallback readers, or checks whose only purpose is detecting removed legacy data; ordinary validation of supported data stays. Preserve compatibility only when explicitly requested for a concrete consumer.
- **Performance is a feature.** Hot paths get data-oriented designs (SoA, contiguous iteration, flat maps over node-based containers). Performance claims are measured before and after, in Release; "should be neutral" is a hypothesis.
- **Code lives where it belongs.** Before adding to a file, ask whether that file owns the responsibility; extract a controller, service or system with a clear owner instead of appending to whatever file is open. Feature code never lands in a hub (`EditorApplication`, `EditorContext`, `SettingsPanel`, `InspectorPanel`, `HierarchyPanel`, `SceneViewPanel`, `RenderServices`, `UIManager`, `UIElement`, `core.css`); the feature pulls what it needs through the app's API and the hub never learns its name. A feature with more than one file lives in a folder named for the feature (`Panels/ProjectPicker/`, `SceneView/Gizmos/`); a new file joins its feature's folder or starts one, never a flat folder of unrelated features.
- **Register, don't accrete.** Extensible surfaces take entries by registration from the owning module: settings categories through `Editor::EditorSettingsRegistry`, inspectors, post effects, dockables and search items through their registries. Migrate legacy hand-built pages on contact.
- **Know what exists before you write.** A second implementation of anything the engine has is a defect. Every subsystem is a module under `Engine/Modules/` (list the directory); engine services live under `Engine/Include/` and `Engine/Source/`; the editor's shared pieces under `Apps/Editor/Include/`. Before writing math, noise, hashing, a container, a string or time utility, a drawing helper or a UI control, look in `Mathematics`, `Noise`, `Types`, `Foundation`, `Platform`, `UI/Include/UI/Controls`, the gizmo API (`Apps/Editor/Include/SceneView/SceneViewGizmos.h`) and the inspector building blocks (`Apps/Editor/Include/UI/InspectorSection.h`). If the shared piece is missing or lacking, add it there in its own change, then use it. A feature owns no private copy of machinery the renderer shares (culling, level of detail, density by projected size, indirect-draw building, per-view buffers); `.claude/rules/rendering.md` states the rule and its exception.
- **Usability counts as quality.** APIs that are hard to misuse, defaults that work, error messages that state the fix, editor UX that does not fight the user. Few settings with high-quality defaults: a dial whose non-default values are all worse is deleted, not shipped. A performance change with a visible cost ships a dial with a sensible default: the engine exposes the trade, and a game or a machine profile chooses. Anything a user sees or operates passes the `change-scrutiny` skill's UI/UX pass before its pull request opens.
- **One engine, many kinds of games.** The engine serves first-person and third-person games, role-playing and open-world games, strategy, flight and space simulation, and their cutscenes at close-up quality. A game built on it is a customer and a source of test content, never the specification: design a system for the range, expose the difference as presets and tuning a game chooses, and do not cap quality or scope at what one game's camera needs. A measurement taken in one game's scene says so and names the scenes that would represent the others.

## Debugging

Debug to root cause: get facts from a debugger, a capture or the live editor before theorizing, and confirm the instrument is reachable before reporting what it said. The `root-cause-debugging` skill holds the method and this engine's instruments (cdb, the sanitizer presets, the live editor's tools); the GPU instruments are in `.claude/rules/rendering.md` and the `renderdoc-gpu-debug` skill (a third-party skill installed per machine, not part of the repository).

- Several editors are often alive: verify PID and path before attaching, and never kill an editor you did not launch.
- Live editor: the MCP tools (`mcp__gameengine__*`, setup in `docs/getting-started/mcp-setup.mdx`) and the CLI `node mcp/ge.mjs <tool>` (`--help` lists all). The CLI's `create_component` writes into the CLI's own repository instead of the project (#5202): do not use it until that is fixed.

## Conventions

Convention traps that invert habits from other engines, always called out when in play: left-handed, +Z forward, +Y up; reverse-Z with a D32 depth buffer, clear 0.0 and `GreaterOrEqual`; negative shadow bias; negative viewport height; descriptor indexing and image-layout transitions. The full list is in `.claude/rules/rendering.md`.

## Code style

`CodingStyle.md` is canonical: functions and types PascalCase, private members `m_PascalCase`, public data PascalCase, locals camelCase, constants `kPascalCase`, full words, `enum class`, fixed-width integers for layout, `std::chrono` for time, early returns over nesting, forward declarations in headers with the includes in the .cpp. The rules that bite most:

- Existing first (`CodingStyle.md`, Mathematics Types and Existing First, holds the full rules): a position, color, matrix or quaternion is the engine's type, never a `float[N]` or `std::array<float, N>`; strings, numbers and paths go through the `Types` utilities; `FlatMap` over node-based maps; free functions live in their module's utility header, never a header of their own; a UI control or a static layout is a class with a `.uxml` layout and a `.css` style in the owning module's UI folder, and `UI::Element<T>` is only for trees built at run time from data; parallel or deferred work goes through the job system, never a thread of its own. A reviewer names each as a Should-fix in every touched file.
- Minimal API surface: private by default, no convenience overloads, and every added function has a production caller in the same change. An API that a game or a script calls is judged by what a game developer needs, not by who calls it inside the engine: it ships with its user documentation and a test, and "nothing subscribes yet" is not a reason to delete it.
- No speculative abstraction: no interface, virtual dispatch, template or registry for a consumer that does not exist; the second concrete use tells you where the seam belongs. This is not licence to skip design: decomposition, naming and single responsibility are structure, expected for one consumer.
- Named functions over lambdas: a lambda is a short adapter handed to an API; anything longer, captured across a screen, defined then called once, or needed twice becomes a named function in the owning file.
- One class per file; a file that outgrows its responsibility is split in the change that grows it. No dead code, no commented-out code, no vague TODOs, no magic numbers. Migrate old-style code on contact, never in unprompted bulk.
- Comments are one to three lines of Simplified Technical English (the `writing-simplified-technical-english` skill; a term of art only when it is the engine's own and there is no plain word for it) that describe the code as it is now: the invariant, the unit or coordinate convention, the trap and the rule. Never narrate history, a change or a former value, never address reviewers, no author or date stamps; a comment that explains a change instead of the code is deleted, and a long comment is cut to its invariant. Doxygen on public APIs.

## Landing a change

Commit verified work to a feature branch early; main is strict. A clean compile plus green unit tests is not proof for rendering, GPU or runtime changes: confirm in a running editor first and say what to look for (fps, flicker, the interaction). Never push an unverified rendering, GPU or runtime change to main; when verification is hard, stage and ask.

## Coordination

Whoever coordinates lanes, reviews or merges runs the `coordinator` skill (`/coordinator`); it names the workbench files (the workbench is `GE_WORKBENCH`, default `../GameEngine-Workbench`) that hold the project's values and the process. A lane that only implements needs this file, `AGENTS.md` and its brief.

## Working discipline

- The brief is a hypothesis: verify the premise (reproduce the defect, trace the cited paths at their current state) before implementing; if evidence contradicts it, stop and report rather than improvise.
- Comments are claims; verify against code and fix any comment your change falsifies.
- "Is X enabled" and "did we fix Y" are chains, not values: enumerate every layer with file:line, date it with `git log`, present the table before the verdict (the `trace-the-chain` skill). A reversed answer means the chain was incomplete; enumerate, don't re-sample.
- Tier every claim: verified, inferred, assumed. Other agents' reports, the owner's claims and your own under challenge are all hypotheses: re-verify, never retract on weaker evidence than you asserted on, never accept an unchecked correction. Absence of evidence is not evidence of absence.
- Relay a decision by its meaning and its optionality, without strengthening it; carry the tier of a lane's finding ("lane X reports Y, unverified by me") or verify first. Ownership, provenance and version claims (whose process, whose worktree, which file version) are checked in one command before they are stated, above all when they would justify not acting; the session-start git snapshot is not a check.
- Never reason from timestamps or from a wrapper's output; verify by exit code, fresh files and running the result. Missing test scaffolding is work, not an excuse.
- Finish the whole task. A turn that ends with a plan or a question the request already answers leaves the work undone: do the next step, and stop only when blocked on input only the owner can give, before a destructive action, or to wait on one background command (the workbench's `agent/lanes.md`). Keep a checklist (`STATUS.md` in the evidence folder) current after every step: it survives compaction, and a fresh lane can continue from it.
- A script other processes may be running is never edited in place: write the new file and move it over the old one.

## Reporting

End every long run under three headings: **Blocked on me** (decisions or input only the owner can give), **Changed** (files, commits, pushes), **Found** (defects, gaps or risks that were not the point of the task). Omit an empty heading. When compacting, preserve the owner's decisions, live lane ids and worktree paths, branch heads and SHAs, process ids of anything still running, and every open item.

## Working with the owner

The owner is a senior graphics engineer (Unity URP/HDRP and SRP internals, UE5 engine-side rendering, and extensive hands-on work in this engine's Vulkan renderer, terrain and ECS). Skip the basics, lead with conclusions and evidence, be direct about trade-offs and about verified versus assumed. A one-sentence Unity or UE cross-reference is the fastest bridge for a genuinely new concept; anchor, don't lecture. Ask only for visual or quality judgement, product direction, or an editor-level GPU run; everything else is decided and split autonomously.

## Documentation

`docs/` is for users of the engine (usage, authored formats, concepts). Designs, decisions, investigations, measurements, plans, post-mortems and review records live in the workbench and are cited from code by name only; `.claude/rules/documentation.md` holds the format and the one-canonical-file rule.
