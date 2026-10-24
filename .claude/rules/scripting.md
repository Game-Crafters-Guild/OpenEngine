---
paths:
  - "Managed/**"
  - "Engine/Modules/Scripting/**"
  - "Engine/Source/Scripting/**"
  - "Engine/Modules/PluginAPI/**"
  - "**/*.cs"
---

# Scripting and hot reload

- CoreCLR (.NET 10) is embedded in C++. Assembly chain: `CoreBridge` (default load context, never reloaded) → `HotReload` (the orchestrator, never unloaded) → user scripts (a collectible load context, swapped on change). A resident `CompileServerHost` provides incremental Roslyn compilation: file change → compile → unload the old context → load the new one, while the engine keeps running.
- `Engine` is a shared library. The Editor, the Player and `GameEngine.Native.dll` all link its import library and share the single `Engine.dll` and its engine singleton at runtime. Whoever calls `EngineCore::Initialize` first owns the lifecycle; the scripting ABI initializes the engine only in a process where nobody has (standalone hosting such as `dotnet test`). See `Engine/Source/Scripting/DllEngineBootstrap.h`.
- A change to a package that has its own repository goes to that repository as a pull request. Licensed test content stays local, never committed.
