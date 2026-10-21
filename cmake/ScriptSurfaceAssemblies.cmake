# The managed assemblies a game's scripts compile against. The generated
# GameEngine.Scripts.csproj references the script surface assemblies beside
# the engine (ScriptingPaths::kScriptSurfaceAssemblyFileNames), so the macOS
# editor bundle and the scripting suite copy each of these directories whole.
#
# GE_SCRIPT_SURFACE_MANAGED_DIRS: directories under Managed/; each one's
# bin/<Config> holds the assembly and its dependencies. Scripting.Runtime is
# last: its copies of Scripting.ABI and ECS.ABI are the ones staged.
# GE_SCRIPT_SURFACE_ASSEMBLY_TARGETS: the targets that build them.
set(GE_SCRIPT_SURFACE_MANAGED_DIRS
  ECS.ABI
  Scripting.Bindings
  Platform.ABI
  Scripting.Runtime)
set(GE_SCRIPT_SURFACE_ASSEMBLY_TARGETS
  EcsAbiAssembly
  ScriptingBindingsAssembly
  PlatformAbiAssembly
  ScriptingRuntimeAssembly)
