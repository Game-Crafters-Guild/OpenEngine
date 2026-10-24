# Log Channels Design

Status: proposal, not implemented. Facts about the current code were read at `58357141`.

## Problem

All log output today goes into one stream. The only filters are severity and a text search. On a busy frame, a person who wants to see what terrain streaming is doing has two options:

- Lower the global level to Debug and get every module's Debug output.
- Use a text search that depends on each call site writing its own prefix. Only about 124 calls do, for example `HZBBuildNode:` and `EditorDebugServer:`.

There is no way to say "Debug for terrain, Warning for everything else", and no way to show only one subsystem in the Log panel or in `get_log`.

## Goals

- Every log message belongs to a **channel**: a named area, usually a subsystem.
- Each channel has its own level. Changing one channel's level does not change any other's.
- Channels form a hierarchy through dotted names (`Rendering`, `Rendering.Shadows`). A level set on a parent applies to every channel below it that has no rule of its own.
- The Log panel, the console, the log file and `get_log` show the channel and can filter by it.
- A call site does not have to name its channel. The channel comes from the code's area: its module by default, or a namespace or class that declares one.
- The level check on the hot path stays one relaxed atomic load, as it is today.

## Non-goals

- A second verbosity axis next to severity, such as the Essential/Detailed scale some consoles use. Severity is the verbosity axis.
- A binary log format or streaming logs between processes. "Identity and storage" says what such a format would need.
- Channel-specific sinks, for example a separate file per channel. A sink can filter by channel itself if it ever needs to.

## Concepts

| Term | Meaning |
|---|---|
| Channel name | A dotted path of segments: `[A-Za-z][A-Za-z0-9_-]*` joined by `.`. The dash is there for package names. Case-sensitive. This is the channel's identity. |
| Channel id | A `uint16` index assigned the first time a name is registered in a process. It is a handle for the hot path and for in-memory records only. |
| Level rule | A pair (name prefix, level). The empty prefix is the root rule, which is the global level. |
| Effective level | The lowest severity a channel passes: the level of the longest matching rule. |

A parent channel does not have to be registered for a child to exist. `Rendering.Shadows` can log even if nothing ever logs to `Rendering` itself. A rule on `Rendering` still applies to it.

Prefix matching works on whole segments: the rule `Render` does not match `Rendering`.

## Writing a log call

### The call site

```cpp
LOG_INFO("Streamed {} tiles in {} ms", count, ms);            // channel from the enclosing area
LOG_WARNING_TO("Rendering.Shadows", "Cascade {} overflow", i);  // explicit channel
```

There is one `_TO` macro per level. A separate name is needed because a macro cannot tell a channel argument apart from a format string.

The `LOG_*` macros become the only way to log. `Log::Trace`, `Log::Debug`, `Log::Info`, `Log::Warning`, `Log::Error` and `Log::Critical` stop being public, because a function call cannot see the scope it was called from. "Migration" below covers the existing direct calls.

### Declaring the channel for an area

```cpp
namespace GameEngine::Ocean
{
GE_LOG_CHANNEL("Ocean");          // in a header of the area; applies to code inside this namespace
}

class ScreenSpaceReflectionsNode
{
    GE_LOG_CHANNEL("Rendering.SSR");  // applies to member functions, also those defined in the .cpp
    // ...
};
```

### Resolution order

The closest declaration wins:

| Order | Source | Declared by |
|---|---|---|
| 1 | Explicit at the call | `LOG_<LEVEL>_TO(name, ...)` |
| 2 | Class | `GE_LOG_CHANNEL(name)` in the class body. Derived classes get it through normal member lookup unless they declare their own. |
| 3 | Namespace | `GE_LOG_CHANNEL(name)` at namespace scope. Inner namespaces get it unless they declare their own. |
| 4 | Module default | The `GE_LOG_MODULE` compile definition of the target that compiles the file |
| 5 | Fallback | `Default`, when the target defines no `GE_LOG_MODULE` |

There is no file-scope form. A declaration in an anonymous namespace would collide in the `-unity` presets, where several .cpp files become one translation unit.

### How resolution works

The macros call `GeLogChannel(Logger::ChannelLookup{})` without a namespace prefix. C++ name lookup then finds the declaration in the innermost enclosing class or namespace, which gives the order above for free.

```cpp
namespace Logger
{
struct ChannelLookupFallback {};
struct ChannelLookup : ChannelLookupFallback {};
}

// Logger.h, global scope. Internal linkage: each target's translation units compile
// their own copy with their own GE_LOG_MODULE, which keeps this ODR-clean.
// `inline` keeps GCC and Clang from warning about an unused static function,
// which warnings-as-errors would turn into a build failure.
static inline Logger::LogChannelId GeLogChannel(Logger::ChannelLookupFallback)
{
    static const Logger::LogChannelId id = Logger::Log::RegisterChannel(GE_LOG_MODULE);
    return id;
}

#define GE_LOG_CHANNEL(name)                                                         \
    static inline Logger::LogChannelId GeLogChannel(Logger::ChannelLookup)           \
    {                                                                                \
        static const Logger::LogChannelId id = Logger::Log::RegisterChannel(name);   \
        return id;                                                                   \
    }
```

The same macro works in a class body, where `static` makes a static member function, and at namespace scope, where `static` gives internal linkage. Each translation unit keeps its own function-local static and registers once. Registration looks the name up, so all of them get the same id.

Why the tag types matter:

- **`using namespace`.** Without the tags, `using namespace GameEngine::Ocean;` in a .cpp file would make the global fallback and `Ocean::GeLogChannel` equally good matches, and every log call in that file would fail to compile as ambiguous. With them, the scoped declaration takes `ChannelLookup` exactly and the fallback needs a derived-to-base conversion, so the scoped one wins.
- **Two scoped declarations at once.** If two scoped declarations are visible together (two namespaces, both brought in with `using`), the call is still ambiguous. That is correct: the file must choose, with `_TO` or a class declaration.
- **Argument-dependent lookup.** The tag lives in namespace `Logger`, so lookup also searches `Logger`. Nothing named `GeLogChannel` may be declared there.

Known limits:

- **Dependent base classes.** A class template does not see a declaration in a base class that depends on a template parameter. Such code falls back to its namespace or module channel.
- **Naming rule.** Do not declare `GE_LOG_CHANNEL` in the broad `GameEngine` namespace, which about 250 module source files use. It would replace the module default for all of them. `Logger.cpp` refuses nothing here; the convention is documented next to the macro.

### Module defaults in CMake

Every module in `GE_ENGINE_MODULES` (`Engine/CMakeLists.txt`) is an `OBJECT` library, and the `foreach` over that list already visits each one. A new helper, `ge_set_log_channel(<target> [<name>])`, adds `GE_LOG_MODULE="<name>"` as a **PRIVATE** compile definition. PRIVATE matters because the same loop re-exports each module's `INTERFACE_COMPILE_DEFINITIONS` through `Engine`. The name defaults to the target name. Targets whose names are not good channel names pass one:

| Target | Channel |
|---|---|
| `GameEngineRendering` | `Rendering` |
| `RenderServices` | `Rendering.Services` |
| `PlatformProvider<backend>` | `Platform.Provider` |
| `Engine` (its own sources) | `Engine` |
| `Editor`, `Player` | `Editor`, `Player` |

Modules outside the list (MeshPicking, test executables) call the same helper. Test executables can keep the `Default` fallback. Package modules and game modules get their default in a different way; see the next section.

### Packages and game modules

Package modules (`Packages/*/package.json`, `"lang": "Cpp"`) and a project's own native game modules are separate DLLs. They link `Engine.dll`, through `target_link_libraries(... Engine)` in `ge_add_engine_package_module` and through the engine import library in the generated project (`UserProjectGenerator.cpp`). Their `GeLogChannel` fallback is compiled into their own DLL, but it calls `Log::RegisterChannel` exported from `Engine.dll`. So they share the one registry without a `RedirectToSharedState` call of their own.

The resolution order is the same as for engine modules. Only the source of the default differs.

| Code | Default channel | Example |
|---|---|---|
| A package's modules, runtime and editor kinds alike | `Packages.<package name>` | `Packages.eztree`, `Packages.git-vcs` |
| A project's native game modules | `Game.<module name>` | `Game.Gameplay` |
| C# scripts | `Scripts.<namespace>` | See [Log Channels for C# Scripts](LogChannelsCSharpDesign.md) |

- **Names with dashes.** Package names use dashes (`fidelityfx-dof`), so channel-name segments allow `-` after the first character: `[A-Za-z][A-Za-z0-9_-]*`.
- **One root for packages.** All packages sit under `Packages`, so their names cannot collide with engine channels (`rendering` against `Rendering`), and a single rule turns all package output up or down.
- **Packages that belong to an engine area.** A package that extends an engine area declares its own channel in its namespace or classes. For example, `fidelityfx-dof` declares `GE_LOG_CHANNEL("Rendering.DoF")`. No manifest field is added for this; the scope declarations already cover it.
- **Both build paths must agree.** A package module is compiled in two places: `ge_add_engine_package_module` in the engine build, which produces the shipped prebuilt, and the editor's source build through the generated project, which is the fallback when the prebuilt's `engine_abi` marker does not match. Both must emit the same `GE_LOG_MODULE`, derived from the manifest's `name`.
- **Kept out of the ABI digest.** The define does not change the ABI, so it stays out of the package defines that feed the `engine_abi` digest. Otherwise renaming a channel would invalidate every shipped prebuilt. In CMake it is a separate `target_compile_definitions` call, not part of `_gepkg_defines`. In the generator it is a separate field, not part of `CompileDefinitions`, if that list feeds the digest; this needs checking (open question 3).
- **Unload and hot reload.** Package and game modules unload and reload. `RegisterChannel` copies the name into the registry's arena, so an unloaded DLL leaves no dangling name, which is the same rule the logger already follows for source-file names. The reloaded DLL registers the same name again and gets the same id, so level rules and the Log panel's filters carry over across a reload.

## Levels

### Two kinds of filtering

| | Level rules | View filters |
|---|---|---|
| Applied | At the log call, before anything is formatted | When a viewer reads what was recorded |
| Set by | `SetLogLevel`, `SetChannelLevel`, the Logging settings, `GE_LOG_LEVELS`, `set_log_level`, the Log panel's level menu | The Log panel's channel tree, severity toggles and search box; `get_log --channel`, `--min-level` and `--pattern` |
| A message below it | Is never created. No sink sees it, and it cannot be recovered. | Stays in every sink and reappears when the filter changes. |
| Scope | The whole process: every sink and every viewer | One viewer |

Level rules decide what is recorded, and view filters decide what one viewer shows of that. Turning a channel up affects messages from then on. It cannot show Debug output that was logged while the channel was at Info, because that output never existed.

That is the point of the level gate: a filtered-out call costs one load and no formatting. A mode that records everything and filters only at view time would pay formatting and queueing on every call. It is not offered.

The Log panel keeps the two apart:

- The level menu on a channel says it changes what is recorded.
- The channel tree and the toggles say they only hide entries.
- When a channel's level rule is stricter than the panel's severity toggles, the tree marks that channel. That way an empty view is not mistaken for a quiet subsystem.

### Rules and effective levels

- The level state is a set of rules keyed by name prefix. `Log::SetLogLevel(level)` sets the root rule.
- `Log::SetChannelLevel(prefix, level)` sets one rule, and `Log::ClearChannelLevel(prefix)` removes it.
- When a rule changes, every registered channel's effective level is recomputed. Rules change rarely, so the cost is O(channels × rules) and does not matter.
- When a channel registers, its effective level is computed from the rules that exist at that moment. A rule therefore applies to channels that first log after it was set.
- `Off` on a channel silences it completely, Critical included. That matches what `Off` does globally today.

### The hot path

- The effective levels live in a fixed-capacity array of `std::atomic<LogLevel>` in `LoggerState`, indexed by channel id.
- The array never moves, so a read needs no lock: `ShouldLog(channel, level)` is `level >= levels[channel].load(relaxed)`.
- The capacity is 1,024 channels, which is 1 KB. A registration past the capacity returns the `Default` channel and logs one warning naming the channel that was refused.
- The other new cost per call is the guard check on the function-local static in `GeLogChannel`. That is one acquire load and a branch once the static is set. "Performance" below says how to measure it.

### Sink levels

- Today `Log::Initialize` copies `Config::GlobalMinLevel` into the default `ConsoleSink`'s `MinLevel` (`Logger.cpp:108`). With channels, that cap would drop a channel set to Debug while the global level is Info. The console sink created by `Initialize` therefore defaults to `Trace` and relies on the channel gate.
- `RingBufferSink` and `CallbackSink` already default to `Trace`. `FileSink` has no level of its own.
- A sink keeps its own `MinLevel` only as an explicit cap that a host chooses.

### `LOG_TRACE` and `LOG_DEBUG` in Release

These two compile to nothing when `NDEBUG` is defined. Turning a channel up to Trace in a Release build cannot bring them back, and the documentation says so. Whether that stays the right choice once per-channel verbosity exists is an open question below.

## Identity and storage

The channel id comes from registration order, and that order depends on thread timing. An id is therefore not stable between builds or between two runs of one build. The rule: **the id is a process-local handle, and the name is the channel's identity. An id never leaves the process.**

| Where | Holds |
|---|---|
| `LogMessage`, the async queue, `RingBufferSink::StoredMessage`, `LogView`'s list | Id |
| Console and file output | Name: `[Rendering.Shadows]` after the level |
| `get_log` (`LogQueryReport.cpp` builds text lines) | Name in each line. The new filter takes names or prefixes and resolves them when the query arrives. |
| Editor settings, `GE_LOG_LEVELS` | Rules keyed by name prefix |

Everything that leaves the process today is formatted text, so persisting names costs nothing.

- **Typos in rules.** A rule may name a channel that has not registered yet, so a misspelled rule cannot be refused when it loads. The settings page marks rules that match no registered channel.
- **A future binary format** writes a channel-definition record (id → name) the first time each channel appears in that stream. The reader maps ids per stream.

### Names and threads

- Channel names are stored in an arena in `LoggerState` and never move. `Log::GetChannelName(id)` returns a `std::string_view` without a lock, so the drain thread and sinks can read names freely.
- Registration takes a registry mutex: it looks the name up in a `FlatMap<String, LogChannelId>`, appends it if it is new, and computes its effective level.

### Logger state shared across modules

Channels live in `LoggerState`, so the host and `Engine.dll` share one registry through the existing `RedirectToSharedState`. One trap: a channel registered in a host's own logger copy before the redirect would hold an id from the wrong registry. `RedirectToSharedState` therefore fails loudly if the local registry already has a channel, the same way it is documented to be called before anything logs.

## Output and tools

### Message record and sinks

- `LogMessage` gets a `LogChannelId Channel` field, set before the record is queued.
- `LogSink::FormatMessage` gets an `includeChannel` flag, on by default. The line format becomes `<time> [<LEVEL>] [<channel>] <message>`.
- `RingBufferSink::StoredMessage` stores the channel id. `GetMessages` takes an optional set of channel prefixes, resolved to ids before it walks the ring.
- `CallbackSink` passes the full `LogMessage` through, so it already carries the id.

### Editor Log panel (`LogView`)

- A channel filter next to the severity toggles and the search box: a tree of channel names built from the registry, with tri-state checkboxes for parents. Unchecking `Rendering` hides all of `Rendering.*`.
- The channel column shows the name, with a stable color derived from a hash of the top-level segment, so related channels share a hue.
- Changing a channel's level from the panel's context menu writes the same rule the settings page writes.

### Editor settings

- A "Logging" settings category registered through `Editor::EditorSettingsRegistry` by the Log panel's owning code, not added to `SettingsPanel`.
- It lists the level rules (prefix, level) and marks rules that match nothing.
- The rules are stored by name in the editor's settings file.

### `get_log` and the `ge` CLI

- `get_log` gets a `channel` parameter that takes a comma-separated list of prefixes, for example `--channel Terrain,Rendering.Shadows`.
- The debug-server handler and the MCP tool change together, so `EditorIpcMethodDrift` stays green.
- A `set_log_level` tool (channel prefix, level) lets an agent turn one area up while it investigates, then clear the rule again.

### Player and tests

`GE_LOG_LEVELS` holds rules as `prefix=level` pairs separated by commas: `GE_LOG_LEVELS=Terrain=debug,Rendering=warning`. The empty prefix is written `*`. The Player reads it at startup and also accepts the same value through a command-line flag. A malformed pair is refused with a message that quotes the pair and the expected form.

## C# scripts

Managed scripts get their own `Log` API with channels resolved from class attributes, namespace rules, assembly attributes and the namespace. It is designed in [Log Channels for C# Scripts](LogChannelsCSharpDesign.md). It depends on these parts of this design: the naming rule, the level rules, the fixed effective-level array, which C# reads in place, and registration by name. The scripting ABI changes, including the new `GE_Log` signature, are specified there.

## Migration

| What | Count at `58357141` | Change |
|---|---|---|
| `LOG_*` macro calls | 156 | None. They get their area's channel. |
| Direct `Logger::Log::<Level>(...)` and `Log::<Level>(...)` calls | about 4,855 (Trace 36, Debug 359, Info 1,569, Warning 1,610, Error 1,274, Critical 7) | One mechanical commit rewrites them to the matching `LOG_*` macro. |
| Calls with a hand-written `Name:` prefix | about 124 | Unchanged by the migration. When someone next touches one, they declare a class-scope channel and drop the prefix. |
| `LogWithSource` callers outside Logger | 0 | It becomes private. |

The mechanical rewrite changes behavior in exactly one way: the 395 direct Trace and Debug calls compile out in Release once they are macros, while today they are kept and filtered at runtime.

- That is the intended contract for Trace and Debug.
- Any of the 395 that must reach a Release log become Info.
- The rewrite commit lists them in its description so a reviewer can check the few that matter.

The Logger module's own tests call `Log::Info` directly to exercise the entry points. They switch to the macros or to a test-only friend, so the public surface stays macros only.

## Performance

Measure before and after in Release, both arms:

- A microbenchmark: an enabled log call and a filtered-out log call, each in a tight loop on one thread, ns per call.
- An editor frame at a recorded pose with the default levels, where nearly every call is filtered out. Report CPU frame time. Name the scene; a terrain-heavy open-world scene and a dense interior scene are the two cases that log differently.

The expected cost is the static guard plus an indexed load instead of a load from a fixed address. That is a hypothesis until the measurement exists.

## Tests

In the Logger module's suites, red before the change and green after:

- **Resolution.** A class declaration beats a namespace declaration, which beats the module default. `using namespace` on a declaring namespace still compiles and picks the scoped channel. `_TO` overrides everything.
- **Rules.**
  - The longest prefix wins.
  - Prefixes match whole segments only.
  - A rule set before a channel registers applies to that channel.
  - Clearing a rule falls back to the parent's rule.
  - `Off` silences Critical.
- **Capacity.** Registration past the capacity returns `Default` and warns once.
- **Shared state.** `RedirectToSharedState` with a non-empty local registry fails.
- **Output.**
  - The file sink line carries the name.
  - A ring-buffer query by prefix returns only matching entries.
- **Parsing.** `GE_LOG_LEVELS` parses valid input and refuses malformed pairs with the documented message.

The editor suites cover the `get_log` channel filter. A live check covers the Log panel's channel tree and its level menu, with screenshots.

## Alternatives considered

- **An explicit category at every call, as some engines require.** Precise, but it would mean editing all 5,000 call sites by hand, and it gives the wrong answer in the common case where the module already says enough.
- **Channels by namespace only.** The namespaces are uneven: plain `GameEngine` covers about 250 module source files, so most logs would land in one channel. Namespace declarations remain an override, not the base layer.
- **Channel from `std::source_location` and path rules.** It would need no rewrite, but it means a lookup keyed by file-name pointer on every call, before the level check can even run. That is slow, and pointer-keyed caches break when a module unloads.
- **Stable ids from a hash of the name.** Ids would be the same in every run, but the hot path would need a hash-to-slot lookup instead of a direct index. Every persisted output already carries the name, so nothing needs a stable id.

## Open questions

1. Should Trace and Debug stay compiled out in Release now that a channel can be turned up at runtime? Keeping them costs code size and one gate per call. Removing them lets a shipped game's log be turned up for one subsystem.
2. Are the channel names in the CMake table right, especially `Rendering.Services` and `Platform.Provider`?
3. Which define lists feed the `engine_abi` digest? On the CMake side it is `_gepkg_defines` (`cmake/Packages.cmake`); on the editor side it is not yet traced whether `NativeBuildConfig::CompileDefinitions` is the digest input. `GE_LOG_MODULE` has to stay out of the digest on both paths.
