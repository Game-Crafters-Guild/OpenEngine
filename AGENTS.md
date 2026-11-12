# AGENTS.md

Entry point for coding agents working in this repository, whichever tool runs them: what this is, how to build, test and land a change, and a map of where the rest lives. Read what the map points at when the task touches it, not everything up front.

## Map

- `CLAUDE.md` for the engineering values, the conventions that invert other engines' habits, the debugging instruments, code style and working discipline; where files disagree, it wins.
- `CONTRIBUTING.md` for the short form of the rules, written for people, and the module layers.
- `.claude/rules/rendering.md` when touching the renderer, shaders or GPU debugging; `ecs.md` for ECS components and systems; `editor-ui.md` for editor UI, CSS, settings pages and asset mounts; `scripting.md` for the managed side; `documentation.md` for `docs/`.
- `CodingStyle.md` for naming and layout; `BUILD.md` for building from a fresh clone on every platform, the configurations and the sanitizers.
- The workbench, the team's private workflow repository (sibling `../GameEngine-Workbench`, or `GE_WORKBENCH`; nothing in the engine build reads it, and a contributor without it follows `CONTRIBUTING.md`): `agent/pr-review-rules.md` before opening or reviewing a pull request; `test-map/path-to-suite.md` for which suites gate a path; `agent/evidence-checklists.md` for the captures visible work ships with; `agent/coordination.md` when coordinating lanes, reviewing or merging.

## What this is

A C++20 game engine: Vulkan-first GPU-driven renderer, archetype ECS, CoreCLR (.NET 10) hot-reload scripting, GUID-identified assets with pluggable parsers, a dependency graph for targeted hot reload and async streaming, and a CSS/XML-driven editor. Windows is the primary platform (Visual Studio 2026, CMake 3.25+, vcpkg in manifest mode); macOS, Linux and Steam Deck presets exist. `Engine/Modules/` holds every subsystem as its own library, `Engine/Include/` and `Engine/Source/` the engine-level services, `Engine/ECSModules/` the shared ECS components, `Apps/Editor/` the editor, `Apps/Player/` the runtime, `Managed/` the C# side, `Engine/Tests/` and `Tests/` the engine-level and integration tests, `docs/` user-facing documentation only.

## Build

```bash
cmake --preset vs2026-x64-local -DBUILD_TESTING=ON  # tests are OFF by default
cmake --build --preset vs2026-x64-local --config DebugFast --target <Target> -j 12
```

- **DebugFast** is the daily configuration: asserts live, warnings are errors. **Release** is the only configuration a performance number may come from, on both arms. `BUILD.md` (Build configurations) has the others and the reasons.
- Presets: `vs2026-x64-local` is the daily driver (no unity build, one edit recompiles one file); the `-unity` variants compile faster cold and slower per edit; `-asan` for memory bugs; `ninja-x64-local`, from a Visual Studio developer shell, is the fastest for incremental iteration in a tree you keep. The build wrapper below sets `-j 12`: a command through it never carries a `-j`.
- Build the targets the change needs, never the whole tree: every target that compiles a touched file, which means the focused suites' targets and `Editor` or `Player` for an application source that no test target compiles, live check or not.
- Build through `bash $GE_WORKBENCH/agent/tools/ge-build.sh <tree> <preset> <config> <log> <targets...>` and run a suite through `bash $GE_WORKBENCH/agent/tools/ge-test.sh <exe> <xml>`: the full log goes to a file in the evidence folder and only the summary enters the conversation (through the MCP server or the `ge` CLI, the same summary comes from `build_engine` for a CMake build, `get_build_status` for the editor's game build and `run_tests` for a suite). Those summaries and the capture tools' record lines are the record of a run. Never read a whole build log, suite log, capture listing or saved tool output; read a tail, a count, a line range or one grep for the thing needed.
- Never build in a checkout someone else is working in, and never while an editor runs from that build's output: the build overwrites DLLs the editor has loaded (a long-lived editor session runs from an isolated copy of the staged tree). Use one git worktree per change and keep it until the change merges. A fresh worktree configures with the shared toolchain path and its own vcpkg root (`-DCMAKE_TOOLCHAIN_FILE=<primary checkout>/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake -DBUILD_TESTING=ON`, no `VCPKG_INSTALLED_DIR`); ports come from the machine-wide binary cache in about a minute, the engine compiles cold once. A configured tree is never renamed or moved and a build directory is never copied between trees: the cache and project files hold absolute paths.
- Before every build, suite and editor start, run `bash $GE_WORKBENCH/agent/wait-ab-lock.sh` as its own call, again while it has not exited 0, and start the command only after it has (never chain it with `;`), or run both as one background command through `agent/tools/gate-then.sh` (the workbench's `agent/lanes.md`). The gate blocks while a timing measurement holds the machine (the `AB-LOCK` file beside the worktrees). Builds and test runs never take that lock; only ms/fps measurements do, with `take-ab-lock.sh <id> <cap minutes> <"gates a merge"|"window">` in the same folder (it claims the lock, lets running builds, suites and editors finish, then marks it taken), `AB_LOCK_HOLDER=<id>` exported for the holder's own launches, and `release-ab-lock.sh <id>` within the cap. The lock is never written by hand.
- Runtime files live next to the executable under `build/<preset>/bin/<config>/`, staged by the build (`StageEditorAssets`, `ge_stage_runtime_dependencies`, `StageTestAssets`). Never resolve a runtime path back into the repository, and never use the repository root as a project root: use `Apps/Editor/Player/` or a dedicated test project.

## Test

```bash
./build/<preset>/bin/DebugFast/Tests/<Suite>.exe --gtest_output=xml:<path>  # one process per suite
```

Google Test, one executable per module or responsibility; `ctest --test-dir build/<preset> -C <config> -N` lists everything. Tests run from the staged output with the build directory as working directory.

- Gate a change with its focused suites: the module-mapped suites in the workbench test map that exercise the touched code (a test reads or calls it). A suite that merely compiles a touched header is built, not run. Each focused suite runs whole, in one process: no `--gtest_filter` inside it at a gate. A filter is for a red-first run, a mutant and a repeat of one failing test; a filter that matches nothing is void, not a pass.
- Report declared = ran + disabled and name every skip with its file:line reason (skips = ran − passed); a suite that only skips is a false instrument. The gtest XML, attached to the change's evidence, is the record of a run.
- Each step of a multi-step branch and each fold round after a review runs the focused suites only; fix failures the change caused and rerun without asking. A fold that changes only comments, documentation or test code runs only the suite that holds a changed test. A code fold that can change what renders or how long a frame takes adds a focused capture pair or timing at the affected pose, never the full evidence set.
- The full mapped set is not a per-pull-request gate: it runs as the pre-release validation and as one batched run on main each day.
- CI is off on purpose: the local runs above are the only gate, an absent CI run is not a defect, and nobody proposes to turn CI on.
- A behavioural claim ships with a test that is red without the change and green with it; say which run was red. Keep tests to what the task asks for, sized like the neighbouring test files; scratch checks are not committed.
- At the end of a branch's last fold, follow the `merge-main` skill (`.claude/skills/merge-main/SKILL.md`): merge `origin/main` into the branch once, never mid-stream, and build the merged tree before calling a change ready, or skip the merge and its rebuild when the skill's test shows that main's delta cannot reach the branch, and say so in the body.
- Every path handed to a Windows program is a `C:/...` path: `--gtest_output=xml:`, log files, `APPDATA` redirects. Git Bash does not convert a `/c/...` path inside such an argument, so it lands under `C:\c\`, a tree nothing reads; a run that produced one is void until rerun.
- Anything visible ships with recorded-pose screenshots (build SHA and full pose on every capture) from an isolated editor instance on a verified-free port that the lane checks is still its own before every call (a freed port is reused by the next launch), stopped by its recorded process id; never kill an editor you did not launch.
- The owner works on this machine while agents run: never synthesise operating-system input (`SetCursorPos`, `keybd_event`, `SendInput`, `mouse_event`) or capture the desktop; a synthetic keystroke lands in whatever window has the focus. Drive the editor through its debug port and the Player through its own flags and log, and take captures through the render graph readback. The debug server's `focus_view`, tab activation and `move_window` ask the operating system for the foreground (issue #2064): do not call them; start the editor with the view you need active. A check that needs more is reported as not verified.
- An evidence folder holds evidence: captures, test results, logs, notes. Scratch output (build logs, exit codes, draft pull request bodies) goes there too, never beside the checkouts, into the repository or into a worktree. Never configure a build or copy an editor build into one. Isolated editor copies live in a scratch folder of their own and are deleted when the editor that ran from them stops; the final report lists what is left on disk, with sizes.

## Land

- Before opening the pull request, run the `change-scrutiny` skill over the diff and carry its "Reuse and surface" table in the body; a reviewer reads it first.
- A change is complete when it builds in DebugFast with warnings as errors, its focused suites are green at the merging head on the merged tree, its live check (if owed) is recorded, and the pull request body states the user's problem in plain steps, the change, the verification table, and what is not verified. Impersonal phrasing.
- IMPORTANT: no attribution lines, tool signatures or session links in commit messages or pull request bodies. The repository's commit hook strips trailers by design.
- One purpose per pull request and per commit, and the purpose complete: a pull request is a whole reviewable step of a feature (a design row a user can see or a test can exercise), never a fix bundled with a refactor and never the thinnest commit that compiles. An arc whose intermediate steps would leave main with a half-feature lives on a feature branch (the workbench's `agent/coordination.md`, Sizing a slice). Shader and GPU changes that each need a timing window, and fixes another team waits on, stay small. Branch from `origin/main`; plain commits after review, never a force-push of a shared branch.
- A change taken over from a public pull request carries "(contributed by <name>, OpenEngine #N)" in its title; a squash merge's author is the merger, so the title is where the credit lives.
- A pre-existing bug or improvement you notice is reported as a follow-up, not folded into the change, unless the requested behaviour cannot work without it.
- A change that makes authored data newly refused (a pipeline, a scene or a material that loaded yesterday and is rejected today) says so in the pull request body with the one-line fix for the data.
- User-facing text (labels, tooltips, notices, documentation) uses American spelling and never names another engine; import features keep their names.
