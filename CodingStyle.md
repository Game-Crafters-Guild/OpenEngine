# Coding Style

This project adopts a consistent, modern C++ naming and API style across the entire repository. These conventions are canonical and apply to all modules (ECS, Engine, Rendering, UISystem, Assets, Tools, Examples, Tests). New code must follow them. Existing code will be migrated incrementally.

## Naming Conventions (Global)

- Functions/methods: PascalCase
  - Examples: ProcessCommands, GetEntityCount, Clear, View, Each, Parallel, Adaptive
- Private member variables: m_PascalCase
  - Example: m_CommandQueue, m_JobSystem
- Public data members (in plain-old-data structs/classes where appropriate): PascalCase (no m_ prefix)
  - Example: Position.X, PlayerStats.Health
- Local variables: camelCase (function scope) and parameters: camelCase
  - Example: entityCount, chunkSize, startIndex
- File-scope or static internal variables (module-private): s_PascalCase for static storage, camelCase for non-static
  - Example: s_ThorvgInitFlag, s_Inited, cacheSize, defaultWidth
- Local variables: camelCase
  - Example: entityCount, chunkSize
- Constants: kPrefix with PascalCase, at every scope — file, class and function-local
  alike. A named constant inside a function is still a constant, not a local variable.
  - Example: kMaxJobs, kDefaultBatchSize
- Types: PascalCase
  - Example: Entity, World, Archetype, Query, EntityHandle

## ECS Public API (Canonical)

Prefer these PascalCase entry points. Older camelCase names may still exist internally but should not be used in new code.

- World
  - ProcessCommands()
  - GetEntityCount() / GetArchetypeCount() / GetAllArchetypes()
  - Clear()
  - View<...>() returning a Query
  - Create(), CreateEntity(), CreateHandle(), CreateBatch(count), CreateBatchHandle(count)
  - SetBulkComponents(handles, components)
  - DestroyEntity(...), DestroyEntityImmediate(...)
  - ReserveEntities(count), ReserveArchetypeCapacity<...>(count)

- Query
  - Each(lambda)
  - Parallel(lambda, minBatchSize)
  - ForEachChunk(lambda)
  - ParallelChunks(lambda)
  - Adaptive(lambda) and adaptive(lambda) both exist; prefer Adaptive in examples going forward
  - Count()
  - Filters: With<...>(), Without<...>() — one call each, naming every type

- Entity (fluent API)
  - Set<T>(), Get<T>(), Has<T>(), Remove<T>()
  - Enable(), Disable(), IsEnabled()
  - Destroy(), Clone(), IsValid(), GetHandle()

## Migration Guidance

- Use PascalCase in all user-facing code (Examples, Tests, tools). If a PascalCase alias does not exist yet, add it alongside the existing camelCase implementation.
- Do not maintain duplicate semantics long-term; once usage is updated and tested, remove the camelCase entry.
- Avoid verbose logging; reserve logs for errors/warnings or debugging.

## Formatting

- Brace style: Allman or project default from .editorconfig/IDE settings.
- Keep functions short and focused; prefer small helpers over giant functions.
- Avoid public data members in complex objects; encapsulate behind accessors. For simple POD/aggregate types used as components or data carriers, public fields are allowed and should be PascalCase (no m_ prefix).


## Headers, Includes, and Organization

- One class or primary type per file where practical
- Section order: public, protected, private
- Prefer forward declarations in headers; include minimal headers, use includes in .cpp
- Include order: own header, related module headers, standard library, third-party
- Use angle brackets for external libraries and quotes for project headers

## Lambdas

- A lambda is a short adapter passed to an API: an `Each` body, a sort key, a small event handler. It captures what it needs by name.
- A lambda that grows past a handful of lines, captures `[&]` across unrelated state, is assigned to a local and called once, or is needed twice becomes a named function (file-local in an anonymous namespace, or a member) in the file that owns the concept.
- Do not build a function out of nested local lambdas; the reader has to hold every capture in mind to follow the control flow.

## Namespaces

- Code should reside in project namespaces; no global-scope symbols in headers
- Do not use `using namespace` in headers; prefer narrow using declarations in .cpp only

## Enums

- Prefer `enum class` for type safety; use explicit underlying types (e.g., `enum class Foo : uint32_t`) when data size/layout matters
- For plain C-compatible flags/bitfields where needed, document the rationale and ensure explicit widths

## Literals and Types

- Use fixed-width integer types where layout matters (uint32_t, int16_t, etc.)
- Prefer std::chrono for time and std::filesystem for paths

## Mathematics Types

- A position, direction, scale, color, quaternion, matrix or bounding box is the engine's type from `Engine/Modules/Mathematics` (`Vector2`, `Vector3`, `Vector4`, `Quaternion`, `Matrix4x4`, `BoundingBox`, and `Color` and `ColorLinear` from `Types`), never `float x[3]`, `float m[16]`, `std::array<float, N>` or a tuple of floats; a run of them is a `std::span` or a container of the engine type.
- The raw form is allowed only at a boundary the engine does not own: a third-party API's signature, a GPU upload struct that pins its layout, the managed ABI. It lives one line from the conversion, and a comment names the boundary.
- A function that takes or returns one of these takes or returns the engine type; free functions over raw arrays are the sign that the owner type is missing or bypassed. Migrate on contact: a change that touches a raw-array site converts it.

## Existing First

The engine has one implementation of each of these; use it, extend it in its own file when it lacks what you need, and never write a second one beside it.

- **Strings, numbers and paths:** the string, number and path utilities in `Types` (`StringUtils.h`: the trim, case-fold, prefix and format functions; `FormatNumber`, `ParseNumber`, `PathUtils`, `NearestName`) for splitting, trimming, joining, case folding, formatting, parsing and path joining. A local `Split` or `Trim` is a defect.
- **Containers and ids:** `FlatMap` over `std::map` and `std::unordered_map`; `GenerationalVector` for handles; `std::span` for a view over a run; `Fnv1a` and the engine's id types for hashing and lookup, never a string compared where an id exists.
- **Files and organization:** one class per file, named for the class. A free function lives in its module's utility header beside its kin, never in a header of its own; a file that holds one function and sits beside a hub is a defect, and so is a header created to avoid finding the owner. A file that outgrows its responsibility is split in the change that grows it.
- **UI controls:** a control is a class on the UI control base, its layout in XML and its styling in CSS, placed in the module that owns its use under that module's UI folder (`Controls`, `Inspectors`, `Panels` as the module needs), registered through the registries. No imperative layout inside a panel, no control assembled from raw elements in place, no control filed under a hub.
- **Threading:** parallel and deferred work goes through the job system (a job, a batch, a continuation). `std::thread`, `std::async`, `std::jthread`, a custom pool or a sleeping poll thread belong only inside `JobSystem` and `Platform` (a process host, a file watcher).
- **Logging, memory, time:** `Logger` for every message (no stream or printf output); the `Memory` module's allocators for pools and arenas, no raw `new` and `delete` outside an owner; `std::chrono` for time.
- **Rendering:** every pass goes through the render graph with its resources declared; no ad-hoc command list, no resource created outside the graph, no barrier placed by hand where the graph owns the state.
- **Math helpers:** `Lerp`, `Clamp01`, `SmoothStep` and the rest of `Mathematics/Interpolation.h`, the geometry helpers and the noise functions from `Mathematics` and `Noise`; never a local copy, never a bare `std::clamp(x, 0.0f, 1.0f)` where `Clamp01` exists.
- **Settings and registration:** settings categories, inspectors, post effects, dockables and search items by registration from the owning module; never a hand-built page or an edit to a hub.

## Public Surface

A public symbol is a promise the engine keeps for every future caller, so the smallest promise that serves a named caller is the right one.

- Private by default: a `.cpp`-local function or an internal header before a public include; a public include only for a consumer outside the module that exists in the same change. A symbol with no caller, or only a test caller, is deleted, not kept for a future that may not come.
- Before a new symbol: can an existing API carry it? Then that API changes, with its callers, and no twin (an immediate, fast, asynchronous or overloaded sibling) is added. Does the need come from a wrong shape (a flag threaded through, a special case, a pass-through)? Then the shape is fixed and the symbol is not needed.
- A game-facing API (one a game or a script calls) is judged by what a game developer needs: it ships with its user documentation and a test, and "nothing subscribes yet" is not a reason to delete it.
- The name is the caller's word for it: the verb for a function, the noun for a type, the engine's own term where it has one, in full words.
- Every pull request carries a "Reuse and surface" table (the `change-scrutiny` skill under `.claude/skills/`): each added symbol, the engine piece it reuses or why not, whether it is public and who outside the module calls it, and a last line stating that the style pass (this whole guide read against the diff) left nothing. A reviewer reads that table first.

## Nulls, Returns, and Control Flow

- Use nullptr, not 0 or NULL
- Return early to avoid deep nesting; keep hot paths straight-line when possible

## Error Handling

- In hot paths avoid exceptions; prefer lightweight validation and early returns
- In non-hot paths/tools, exceptions are acceptable with clear ownership and error contracts

## Concurrency

- Document thread affinity and ownership in public APIs
- Prefer immutability; otherwise protect with clear synchronization primitives

## Templates and Concepts

- Constrain public templates with concepts or requires clauses where practical
- Keep large template definitions in .inl files for clarity

## Move Semantics and Copies

- Provide move operations for heavy types; delete or define copy semantics intentionally

## Logging

- Minimal logging in hot paths; warnings/errors only. Avoid verbose enter/exit logs.

## Comments and Documentation

- Doxygen-style comments for public APIs
- Examples and docs should use canonical PascalCase API only
- A comment is short, one to three lines, in Simplified Technical English (ASD-STE100): one meaning per word, active voice, present tense, short sentences.
- A comment describes the code as it is now: the invariant, the unit or coordinate convention, the trap, the rule. It never describes a change, a former value, a removed path, a review or a reason that no longer applies. A comment that narrates history is deleted on contact.
- A name that needs a comment to be understood is the wrong name; fix the name first.

## Assertions

- Prefer project Assert macro over raw assert; avoid side effects in assertions

## Unit Tests

- Test names should state behavior, not implementation
- For ECS, include performance sanity checks for hot paths

## Threading and Jobs

- Prefer job system helpers and Query::Parallel/ParallelChunks for data-parallel work (synchronous — they join internally).
- For explicit fork-join, use JobSystem::JobCounter: Run(fn, counter) to submit, Wait(counter) to join.

## Error Handling

- Use assertions in tests; in runtime code, prefer lightweight validation and early returns over exceptions in hot paths.

## Documentation

- Add brief comments for non-obvious behavior and any public API that could be misused.
- Update this guide if you introduce a new, widely used pattern.

