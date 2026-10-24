# Log Channels for C# Scripts

Status: proposal, not implemented. Facts about the current code were read at `58357141`.

This design extends [Log Channels](LogChannelsDesign.md) to managed scripts. The channel concepts, the naming rule, level rules, the effective-level array and the identity rules come from that document and are not repeated here.

## Problem

Today scripts have no logging API. `Console.WriteLine` reaches the native logger through `EngineLogWriter`, which `Console.SetOut` and `Console.SetError` install, and every message arrives at Info. That includes `Console.Error` output, because the writer hard-codes the level (`EngineLogWriter.cs:209`). This document adds a script logging API with channels resolved the same way as on the native side.

## The call site

```csharp
using GameEngine;

namespace MyGame.AI
{
    static class Planner
    {
        static readonly LogChannel s_Navmesh = new("AI.Navmesh");

        static void Plan(int count)
        {
            Log.Info($"Planning {count} agents");             // channel from the enclosing area
            Log.Warning(s_Navmesh, $"No path for {count}");   // explicit channel
        }
    }
}
```

- **Explicit channels.** The explicit form takes a `LogChannel` handle, created once and kept in a static field. Registration then happens once per handle, not once per call.
- **Arguments.** Message arguments are interpolated strings only, through the level's interpolated-string handler. There is no `params object[]` overload, because it would box its arguments and could not skip formatting.

## Resolution order

The closest declaration wins:

| Order | Source | Declared by |
|---|---|---|
| 1 | Explicit at the call | The `LogChannel` overload |
| 2 | Class | `[LogChannel("AI.Brain")]` on a `partial` class. Nested classes get the channel of the class that contains them. Only an explicit attribute makes a class channel; a class never gets one by default. |
| 3 | Namespace rule | `[assembly: LogChannelRule("MyGame.AI", "AI")]`. The longest matching namespace prefix wins. C# has no attributes on namespaces, so the assembly declares the rule. |
| 4 | Assembly | `[assembly: LogChannel("Game.Combat")]` |
| 5 | Default | `Scripts.<namespace>`, or `Scripts` for code in the global namespace |

An assembly attribute wins over the namespace default on purpose: writing it chooses one channel for the whole assembly. A namespace rule wins over the assembly attribute because it is more specific.

Namespaces are the default and modules are not, unlike in C++. A project's game scripts usually compile into one assembly, so an assembly default would put all game code in one channel, while namespaces already carry the structure.

## How resolution works: a generated `Log` per scope

A Roslyn incremental generator, next to `EntitySystemGenerator` under `Managed/SourceGenerators/`, runs on every script compile, hot-reload builds in the compile server included. It works the same way as the native lookup:

- **Namespaces.** For every namespace that contains code, it emits an `internal static class Log` in that namespace. C# looks up a simple name in the enclosing namespace's own members before the file's `using` directives, so inside `MyGame.AI`, `Log` means the generated `MyGame.AI.Log`, not `GameEngine.Log`.
- **Classes.** For every class with `[LogChannel]`, it emits a nested `private static class Log`, which takes precedence over the namespace one.
- **The channel.** The generator applies the resolution order at compile time and writes the resulting channel name into each generated class. Nothing about the channel is looked up at runtime.
- **The filtered-out path.** Each generated `Log` method takes a level-specific interpolated-string handler that is generic over the scope, for example `InfoHandler<TScope>`, where `TScope` is a generated type with a `static abstract` `LogChannel` property. The handler's constructor checks `levels[TScope.Channel.Id]` and sets `shouldAppend` to false when the channel is below Info. A filtered-out call therefore never formats or allocates.

```csharp
// Generated for namespace MyGame.AI
namespace MyGame.AI
{
    internal sealed class LogScope : GameEngine.ILogScope
    {
        public static GameEngine.LogChannel Channel { get; } = new("Scripts.MyGame.AI");
    }

    internal static class Log
    {
        public static void Info(ref GameEngine.InfoHandler<LogScope> message) => message.Dispatch();
        public static void Info(GameEngine.LogChannel channel,
            [InterpolatedStringHandlerArgument("channel")] ref GameEngine.ChannelInfoHandler message) => message.Dispatch();
        // ... the other levels
    }
}
```

A prototype of this lookup and the handler check was built and run on the .NET 10.0.112 SDK. It showed three things:

- The namespace `Log` takes precedence over `GameEngine.Log` under `using GameEngine;`.
- A nested class `Log` takes precedence over the namespace one.
- A call whose channel was set to Error did not format its Info message.

The generator reports a diagnostic in these cases:

- `[LogChannel]` on a class that is not `partial`, which needs a nested type added to it.
- User code that declares its own type named `Log` in a namespace that also needs a generated one. The diagnostic names both and suggests the explicit form or a rename.
- A channel name that breaks the naming rule.

### Interceptors rejected

An interceptor generator would replace each `GameEngine.Log.Info(...)` call with one that passes the channel. It cannot keep the formatting skip, though. The compiler builds the interpolated-string handler at the call site, from the original method's signature, before the intercepted method runs, so the handler cannot know the channel. It could only check a process-wide minimum level, and every Debug call would format as soon as any channel was set to Debug.

## The managed hot path

- **Level checks without leaving managed code.** The effective-level array is fixed and never moves (see "The hot path" in [Log Channels](LogChannelsDesign.md)). C# reads it in place: `GE_GetLogLevelTable` returns its address and capacity once. A level check is a `Volatile.Read` of one byte with no P/Invoke call. `Logger.cpp` gains a `static_assert` that `std::atomic<LogLevel>` is always lock-free and one byte wide, which this layout depends on.
- **One way to register.** Generated scopes and explicit handles both register through the `LogChannel` constructor. No separate registration function exists.
- **Static ids.** Each `LogScope.Channel` is a static read-only value. Tiered compilation treats it as a constant once the type is initialized.
- **Formatting and dispatch.** A message that passes the check is formatted into a pooled buffer, converted to UTF-8 and sent with one `GE_Log` call.

## Scripting ABI

| Function | Purpose |
|---|---|
| `GE_RegisterLogChannel(name, nameLen, outId)` | Registers a channel or returns its existing id. A malformed name returns an error result whose message quotes the name and the rule. |
| `GE_GetLogLevelTable(outLevels, outCapacity)` | The effective-level array for in-place reads |
| `GE_Log(level, channel, msg, msgLen)` | Replaces `GE_Log(level, msg, msgLen)` (`ScriptingABI.h:557`) |

Everything that carries the old three-argument shape changes in the same commit, so no old signature is kept:

- `GE_Log` and its definition (`ScriptingABI.h:557`, `ScriptingABI.cpp:463`);
- the `GE_Log_Fn` typedef and the interface table's `Log` field (`ScriptingABI.h:207`, `:242`; set at `ScriptingABI.cpp:752`);
- the managed mirror of that table, `EngineNativeBinding.GE_Log_Delegate` and its `Log` field (`Managed/Common/EngineNativeBinding.cs`);
- the binding `InteropGenerator` emits (`Generator.cs:73`);
- `EngineLogWriter`, both its main path through `binding.Log` and its `NativeFallback`;
- the caller in `Managed/HostRuntime/Program.cs`.

## Hot reload

A script reload creates new types, so `LogScope.Channel` initializes again. It registers the same names, and channel registration is idempotent by name, so it gets the same ids back. Level rules and the Log panel's filters carry over across the reload.

## `Console.WriteLine`

`Console.WriteLine` keeps working. It cannot tell who called it, so `EngineLogWriter` registers one `Scripts` channel and sends `Console.Out` there. It is a fallback for quick output and for code that is not engine-aware. The documentation points script authors at `Log`.

## Documentation

`docs/scripting/initialize-on-load-and-logging.mdx` and the scripting ABI page are updated with the change. The `Log` API is called by games, so it ships with its own user page: the call forms, the attributes, the resolution order and the diagnostics.

## Tests

In the managed test projects:

- **Generator resolution.**
  - A class attribute beats a namespace rule, which beats the assembly attribute, which beats the namespace default.
  - Nested classes get their containing class's channel.
  - Code in the global namespace gets `Scripts`.
- **Generator diagnostics.** A non-partial attributed class, a user type named `Log`, and a malformed name each produce their diagnostic.
- **Filtered-out calls.** Interpolated arguments are not evaluated for formatting when the channel is below the level.
- **Hot reload.** A reload gets the same ids back for the same names.

## Open questions

1. Should `Trace` and `Debug` compile out of Release script builds through `[Conditional("DEBUG")]` on the generated methods? Follow the native answer to open question 1 in [Log Channels](LogChannelsDesign.md), so a channel turned up at runtime behaves the same in C++ and C#.
