# Build Guide

How to build the editor from a fresh clone on **Windows** (primary), **macOS** and **Linux**, and the Player for **Steam Deck**. Third-party dependencies come from vcpkg in manifest mode: the first configure bootstraps vcpkg into `dependencies/vcpkg/` and fills a per-user binary cache shared by every clone on the machine. You never run vcpkg by hand.

## Contents

- [Quick start](#quick-start)
  - [Windows (Visual Studio)](#windows-visual-studio)
  - [Windows (Ninja)](#windows-ninja)
  - [macOS](#macos)
  - [Linux](#linux)
- [Prerequisites](#prerequisites)
- [Platform notes](#platform-notes)
- [Steam Deck](#steam-deck)
- [Build configurations](#build-configurations)
- [Disk footprint](#disk-footprint)
- [Tests](#tests)
- [Standalone CLI (gameenginecli)](#standalone-cli-gameenginecli)
- [Sanitizers](#sanitizers)
- [Build options](#build-options)
- [Version](#version)
- [vcpkg manifest features](#vcpkg-manifest-features)
- [Troubleshooting](#troubleshooting)

## Quick start

Install the [prerequisites](#prerequisites), then copy the block for your platform. `Editor` also builds `Player`, the runtime the editor exports games with. `McpServer` builds the `mcp/` server that `node mcp/ge.mjs` and AI tools drive the editor with; it exists only when Node.js was on PATH at configure, so skip that line without it. The same tools build as a terminal command with `GameEngineCli` ([Standalone CLI](#standalone-cli-gameenginecli)). On first launch the editor asks you to create or open a project: [Your first project](docs/getting-started/first-project.mdx).

The first configure compiles about 60 third-party libraries (tens of minutes) unless the binary cache has them; later configures take about a minute. When a dependency version moves (a `git pull` that changes `vcpkg.json` or `cmake/ports/`), the configure installs the new version in place and marks its headers as changed, so the next build recompiles the files that include them. When a dependency is rebuilt at the same version with a different ABI (a Visual Studio update, or a `cmake/VcpkgPin.cmake` change that moves the vcpkg checkout), no file would recompile against it, so the configure stops and tells you to delete `build/<preset>` and reconfigure.

### Windows (Visual Studio)

Run in a **Developer Command Prompt for VS 2026** (or Developer PowerShell), which puts Visual Studio's CMake and Ninja on PATH. `-j 12` is the number of parallel compile jobs; use your core count or lower. The prompt opens inside the Visual Studio folder, so move to where the clone should live first: any folder with no spaces in its path, outside `Program Files`.

```bat
mkdir C:\Dev
cd /d C:\Dev
git clone https://github.com/Game-Crafters-Guild/OpenEngine.git
cd OpenEngine
cmake --preset vs2026-x64-local
cmake --build --preset vs2026-x64-local --config DebugFast --target Editor -j 12
cmake --build --preset vs2026-x64-local --target McpServer
build\vs2026-x64-local\bin\DebugFast\Apps\Editor\Editor.exe
```

To use the IDE, open `build\vs2026-x64-local\GameEngine.slnx` and set **Editor** as the startup project.

### Windows (Ninja)

Faster incremental builds than MSBuild with the same compiler. `cl.exe` must be on PATH, so run in an **x64 Native Tools Command Prompt for VS 2026**. The tree is separate from the Visual Studio one and can live beside it.

```bat
mkdir C:\Dev
cd /d C:\Dev
git clone https://github.com/Game-Crafters-Guild/OpenEngine.git
cd OpenEngine
cmake --preset ninja-x64-local
cmake --build --preset ninja-x64-local --config DebugFast --target Editor -j 12
cmake --build --preset ninja-x64-local --target McpServer
build\ninja-x64-local\bin\DebugFast\Apps\Editor\Editor.exe
```

### macOS

Xcode generator; the commands below build the Debug configuration (the presets build DebugFast when `--config` is omitted). On an Intel Mac use `macos-x64-local`. The `Editor` build also places a copy of `Player.app` inside the editor's bundle as its export template.

```bash
git clone https://github.com/Game-Crafters-Guild/OpenEngine.git
cd OpenEngine
cmake --preset macos-arm64-local
cmake --build --preset macos-arm64-local --config Debug --target Editor -j 8
cmake --build --preset macos-arm64-local --target McpServer
open build/macos-arm64-local/bin/Debug/Apps/Editor/Editor.app
```

### Linux

Ninja, single configuration: the build type is set at configure.

```bash
git clone https://github.com/Game-Crafters-Guild/OpenEngine.git
cd OpenEngine
cmake --preset linux-x64-local -DCMAKE_BUILD_TYPE=Debug
cmake --build --preset linux-x64-local --target Editor
cmake --build --preset linux-x64-local --target McpServer
./build/linux-x64-local/bin/Debug/Apps/Editor/Editor
```

## Prerequisites

The engine hosts .NET 10: the managed assemblies target `net10.0` and run on the newest installed 10.0 patch. Other .NET SDKs beside it are fine (`dotnet --list-sdks`); an older SDK alone fails with `NETSDK1045`, and without `dotnet` the configure stops and names the fix. A build without C# scripting needs no .NET at all: see [Building without .NET](#building-without-net).

### Windows

| Requirement | Notes | Where |
|---|---|---|
| **A clone path with no spaces, outside `Program Files`** (for example `C:\Dev\OpenEngine`) | Several third-party build systems break on a space in the path, and `Program Files` is write-protected. | — |
| **Visual Studio 2026** with the *Desktop development with C++* workload | Includes MSVC, **CMake 3.25 or newer**, **Ninja** and Git. A standalone CMake and Ninja on PATH also work. Visual Studio 2022 works too: swap `vs2026-` for `vs2022-` in preset names. | [visualstudio.microsoft.com/downloads](https://visualstudio.microsoft.com/downloads/) |
| **.NET 10 SDK** | C# scripting host and managed assemblies; not needed with `-DENABLE_SCRIPTING=OFF` ([Building without .NET](#building-without-net)). | `winget install Microsoft.DotNet.SDK.10` |
| **Vulkan-capable GPU driver** | Runtime requirement (Vulkan 1.3). | GPU vendor |
| **Vulkan SDK** *(recommended)* | Not needed to compile. Supplies the validation layers Debug builds enable, and `spirv-val`. | [vulkan.lunarg.com](https://vulkan.lunarg.com/) |
| **Node.js 20 or newer** *(optional)* | Builds the `McpServer` and `GameEngineCli` targets; without it configure omits both, and `node mcp/ge.mjs` and `publish_game.py build` cannot drive the editor. | [nodejs.org](https://nodejs.org/) |
| **Python 3.10 or newer** *(optional)* | Runs `Tools/Scripts/publish_game.py` and `build_deck.py`. Without it, `GamePublishingTests` is not registered. | [python.org](https://www.python.org/downloads/) |

### macOS

| Requirement | Notes | Where |
|---|---|---|
| **Xcode**, full install | The presets use ad-hoc signing; no developer account needed. | Mac App Store |
| **CMake 3.25 or newer** and **Ninja** | | `brew install cmake ninja` |
| **Vulkan SDK for macOS** | Bundles MoltenVK. Detected under `~/VulkanSDK/<version>`; set `VULKAN_SDK` if it is elsewhere. Without it the build succeeds with the Vulkan backend disabled. | [vulkan.lunarg.com](https://vulkan.lunarg.com/) |
| **.NET 10 SDK** | C# scripting host and managed assemblies; the arm64 build on Apple Silicon. Not needed with `-DENABLE_SCRIPTING=OFF` ([Building without .NET](#building-without-net)). | [dotnet.microsoft.com](https://dotnet.microsoft.com/download/dotnet/10.0) |
| **Node.js 20 or newer** *(optional)* | Same use as on Windows. | `brew install node` |
| **Python 3.10 or newer** *(optional)* | Same use as on Windows, and the macOS editor's Steam Deck export. The `python3` that ships with Xcode is 3.9, which is too old. | `brew install python` |

Homebrew installs into `/opt/homebrew` on Apple Silicon; add `eval "$(/opt/homebrew/bin/brew shellenv)"` to `~/.zprofile` so new shells find its tools. To check:

```bash
xcode-select -p && cmake --version && ninja --version && dotnet --list-sdks
node --version && python3 --version    # optional tools
```

### Linux

| Requirement | Notes | Where |
|---|---|---|
| **C++20 toolchain** (recent GCC or Clang), **CMake 3.25 or newer**, **Ninja**, **pkg-config**, **Git** | | your distribution's packages |
| **NASM (x86/x64)** | Required by FFmpeg when the video feature is enabled. | `sudo apt install nasm` |
| **Windowing packages** | Required by vcpkg's `glfw3`. | `sudo apt install libxinerama-dev libxcursor-dev xorg-dev libglu1-mesa-dev libwayland-dev libxkbcommon-dev pkg-config` |
| **Vulkan driver and tools** | Runtime requirement, plus the validation layers. | `sudo apt install mesa-vulkan-drivers vulkan-tools vulkan-validationlayers` |
| **.NET 10 SDK** | C# scripting host and managed assemblies; not needed with `-DENABLE_SCRIPTING=OFF` ([Building without .NET](#building-without-net)). | [dotnet.microsoft.com](https://dotnet.microsoft.com/download/dotnet/10.0) |
| **Node.js 20 or newer** *(optional)* | Same use as on Windows. | your distribution's `nodejs` package, or [nodejs.org](https://nodejs.org/) |
| **Python 3.10 or newer** *(optional)* | Same use as on Windows. | your distribution's `python3` package |

On Fedora (checked on Fedora 44), these two commands install everything above, including the system packages the vcpkg ports build with (Perl modules for OpenSSL, `nasm`, `autoconf-archive`):

```bash
sudo dnf group install development-tools c-development
sudo dnf install cmake ninja-build git pkgconf-pkg-config \
  perl-IPC-Cmd perl-FindBin perl-Time-Piece nasm autoconf-archive \
  libX11-devel libxcb-devel libXinerama-devel libXcursor-devel libXrandr-devel libXi-devel \
  libxkbcommon-devel wayland-devel mesa-libGLU-devel \
  mesa-vulkan-drivers vulkan-tools vulkan-validation-layers \
  dotnet-sdk-10.0 nodejs python3
```

If a port needs another system package, the vcpkg configure error prints the install command. `vcpkg.json` builds the Vulkan loader and GLFW with X11 and Wayland support. Install your distribution's Wayland development packages when configuring on Linux; see the [GLFW dependency guide](https://www.glfw.org/docs/latest/compile.html#compile_deps). The Editor preserves compositor window decorations and displays its application menus inside the windowing environment.

## Platform notes

On every platform (`cmake --list-presets` lists every configuration):

- After a `git pull` that touched `mcp/`, build `McpServer` again.
- Skip a heavy optional dependency through a [manifest feature](#vcpkg-manifest-features), for example FFmpeg: `-DVCPKG_MANIFEST_FEATURES="physics;nav;ui-extras;tests"`.
- On an offline machine, prime the binary cache by hand with `./Tools/Scripts/fetch-vcpkg-cache.ps1` (`.sh` on macOS and Linux). Online, the configure fetches the prebuilt archives itself when GitHub CLI is logged in; `GE_NO_CACHE_FETCH=1` turns that off.

### Windows

- `vs2026-x64-local` has no unity build, so an edit recompiles only the files it touched; `vs2026-x64-local-unity` compiles faster cold and slower per edit.
- Windows ARM64: `vs2026-arm64-local` (Visual Studio 2026 only).
- The Ninja tree emits `build/<preset>/compile_commands.json` for clangd and VS Code. `ninja-x64-sccache` adds sccache, and needs an external vcpkg checkout (`VCPKG_ROOT`) and `sccache` installed.

### macOS

- To open a project directly: `build/macos-arm64-local/bin/Debug/Apps/Editor/Editor.app/Contents/MacOS/Editor --project ~/GameProjects/MyGame`.
- IDE: `open build/macos-arm64-local/*.xcodeproj`, select the **Editor** scheme, press **⌘R**.
- The Ninja presets (`macos-arm64-ninja`, `macos-x64-ninja`) are faster for command-line iteration and emit `compile_commands.json`. If you delete the `.app` in a Ninja tree, configure again to regenerate its `Info.plist`.
- The Editor build stages the Vulkan SDK's MoltenVK into `Editor.app`. To stage your own build, configure with `-DGE_MOLTENVK_SOURCE_LIB=<path>/libMoltenVK.dylib -DGE_MOLTENVK_SOURCE_ICD=<path>/MoltenVK_icd.json`.

### Linux

- Pass `RelWithDebInfo` or `Release` as `CMAKE_BUILD_TYPE` for an optimized build. `linux-x64-local-unity` compiles faster cold.
- From Windows without a Linux machine, `.\Tools\Scripts\run-linux-build.ps1` (Docker Desktop running) builds the `Editor` inside a container from `Tools/Docker/Dockerfile.linux-dev`, in `build-linux-docker`, with scripting off.

### Building without .NET

`-DENABLE_SCRIPTING=OFF` builds the Editor and the Player with no .NET SDK or runtime on the machine: the configure does not look for `dotnet`, no C# project is built, and the component reflection the engine compiles is generated by the native `ComponentScanner` tool (`Tools/ComponentScanner`). On Debian 13 or Ubuntu, the Linux prerequisites above without the .NET SDK are:

```bash
sudo apt install build-essential cmake ninja-build pkg-config git ca-certificates nasm \
  curl zip unzip tar python3 autoconf autoconf-archive automake libtool \
  libxinerama-dev libxcursor-dev xorg-dev libglu1-mesa-dev libwayland-dev libxkbcommon-dev \
  mesa-vulkan-drivers vulkan-tools vulkan-validationlayers
cmake --preset linux-x64-local -DENABLE_SCRIPTING=OFF
cmake --build --preset linux-x64-local --target Editor Player
```

What that build does not have: C# scripting (C# components, entity systems and game systems), C# hot reload and the compile server, the C# play-mode driver, and the C# steps of game publishing. The status bar reads "C# scripting is off in this build"; a project's C# components stay in its scenes with their values, marked Not Registered, and apply when the project opens in a build with scripting; a project without C# scripts packages as in a full build, and packaging a project that has C# scripts is refused with that reason. Native C++ scripting, the editor and every engine feature written in C++ work as in a full build. Test targets that link `GameEngine.Native` (the UI test family and `FilePatternTests` among them) do not link in this build, so build the test targets you need by name. The same flag is what the Steam Deck and web presets set.

## Steam Deck

Linux x64, Player only (`BUILD_EDITOR=OFF`, `ENABLE_SCRIPTING=OFF`, `RelWithDebInfo`):

```bash
cmake --preset steamdeck-x64-local
cmake --build --preset player-steamdeck -j 8
# output: build/steamdeck-x64-local/bin/RelWithDebInfo/Apps/Player/
```

From a Mac, `./Tools/Scripts/build-steamdeck-docker.sh` produces the same build through Docker (`--unity` for a faster cold build) and packages a tarball to copy onto the Deck. To build with scripting on the Deck, add `-DENABLE_SCRIPTING=ON` with the .NET 10 SDK installed.

## Build configurations

- **Debug**: `/Od /Zi /MDd /RTC1`, iterator debugging, static analysis as warnings, Vulkan validation layers on. The slowest; use it when you want every check firing.
- **DebugFast**: the default and the daily configuration. Same source-level debugging as Debug without the debug CRT (`/MD`, no `/RTC1`, no static analysis), validation layers off by default, asserts live, compiler warnings treated as errors by default on MSVC, GCC and Clang (including AppleClang).
- **RelWithDebInfo**: `/O2` with symbols, asserts compiled out. For performance checks with usable crash stacks.
- **Release**: optimized; the shipping configuration, and the only one a performance number comes from.

MSVC treats compiler warnings as errors in Debug, DebugFast and RelWithDebInfo. Non-MSVC compilers use the same configurations when `ENABLE_WERROR_NON_MSVC` is ON. A fresh configure defaults to ON for GCC, Clang and AppleClang. Existing build caches retain their saved value; use `-DENABLE_WERROR_NON_MSVC=ON` to enable it there. An explicit `OFF` opts out on non-MSVC compilers. Release does not use this option.

Choosing one:

- Visual Studio, Xcode and Ninja Multi-Config trees list DebugFast first, so the IDE opens on it and `cmake --build --preset <preset>` builds it. Pass `--config Debug`, `--config Release` or another configuration to build that one instead.
- A build that names neither a preset nor `--config` (`cmake --build build/<preset>`) builds Debug in a Visual Studio tree: CMake picks Debug there, not the project.
- Single-configuration trees (Ninja: Linux, `macos-arm64-ninja`, `macos-x64-ninja`) fix the configuration at configure time: DebugFast unless `-DCMAKE_BUILD_TYPE=Debug`, `Release` or another one is passed.
- `vs2026-x64-local-asan`, `macos-arm64-local-asan` and `macos-arm64-local-tsan` build Debug, the configuration the sanitizers instrument. `wasm-debug` builds Debug, `wasm-release` Release and the `steamdeck` presets RelWithDebInfo.

`GE_VK_VALIDATION=1` forces the Vulkan validation layers on in any configuration, `GE_VK_NO_VALIDATION=1` forces them off, and `GE_VK_VALIDATION_VERBOSE=1` turns on the verbose messenger.

## Disk footprint

Plan for about 20 GB free before a first Editor build on Windows or macOS, and about 35 GB on macOS if you also build every test target.

| Windows x64, fresh clone and DebugFast Editor build | Size |
|---|---|
| Clone | ~0.9 GB |
| `dependencies/vcpkg` after configure | ~1.3 GB |
| `build/<preset>/vcpkg_installed` | ~2.9 GB |
| Per-user binary cache | ~0.6 GB |
| `build/<preset>` after the Editor build | ~10.7 GB |

| macOS arm64 | Size |
|---|---|
| `build/macos-arm64-local` after a Debug Editor build, no tests | ~13 GB |
| A Ninja Debug unity tree after the Editor, the Player and every test target | ~28 GB |

- Each additional configuration adds its own objects and staged runtime, about 6-9 GB on Windows. Build only the one you use.
- Test targets add the most: build only the suites you run.
- `git clone --filter=blob:none <url>` halves the clone; history is fetched on demand.
- Safe to delete at any time: `build/<preset>/` (third-party libraries come back from the binary cache), the per-user vcpkg downloads folder (`%LOCALAPPDATA%\vcpkg\downloads`), and `dependencies/vcpkg/downloads/`.

## Tests

Tests are off by default. Configure with `-DBUILD_TESTING=ON`, build the test targets you need, and run each test executable directly: one process runs the whole suite. Executables land in `bin/<Config>/Tests/` and run from the build tree.

Windows:

```bat
cmake --preset vs2026-x64-local -DBUILD_TESTING=ON
cmake --build --preset vs2026-x64-local --config DebugFast --target ECSCoreTests -j 12
build\vs2026-x64-local\bin\DebugFast\Tests\ECSCoreTests.exe
build\vs2026-x64-local\bin\DebugFast\Tests\ECSCoreTests.exe --gtest_filter="ECSCoreTest.*"
```

macOS (the Xcode presets need `PRE_TEST` discovery: Xcode signs a test executable only after its post-build steps, and macOS kills an unsigned one, which fails the build with `Subprocess killed`; the Ninja presets do not need it):

```bash
cmake --preset macos-arm64-local -DBUILD_TESTING=ON -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST
cmake --build --preset macos-arm64-local --config Debug --target ECSCoreTests
build/macos-arm64-local/bin/Debug/Tests/ECSCoreTests
```

Linux:

```bash
cmake --preset linux-x64-local -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build --preset linux-x64-local --target ECSCoreTests
./build/linux-x64-local/bin/Debug/Tests/ECSCoreTests
```

On a machine with OBS Studio installed, set `DISABLE_VULKAN_OBS_CAPTURE=1` before you run a GPU suite directly, so that OBS's Vulkan layer stays out of the test process; ctest sets it for every test it runs.

Through ctest: `ctest --test-dir build/<preset> -C <Config> -R "^ECSCoreTest\." --output-on-failure`. Some suites register under their executable's name and others register each test case (`ECSCoreTest.SomeCase`), so `ctest --test-dir build/<preset> -C <Config> -N -R <pattern>` first lists what a filter matches; a filter that lists nothing runs nothing.

**Which suites to run.** The ones that exercise the code you changed. Test targets are declared in the `CMakeLists.txt` beside their sources (`Engine/Modules/<Module>/Tests/`, `Tests/`, `Engine/Tests/`, `Apps/Editor/Tests/`); `git grep -l <TypeOrFunction> -- Tests Engine/Tests "Engine/Modules/*/Tests" Apps/Editor/Tests` lists the test sources that use what you changed, and the `CMakeLists.txt` in the same folder names their target.

## Standalone CLI (gameenginecli)

Every MCP tool as a command in one native executable; Node.js is needed to build it, not to run it.

```bat
cmake --build --preset vs2026-x64-local --config DebugFast --target GameEngineCli
build\vs2026-x64-local\bin\DebugFast\gameenginecli.exe list
build\vs2026-x64-local\bin\DebugFast\gameenginecli.exe get_editor_state
build\vs2026-x64-local\bin\DebugFast\gameenginecli.exe get_log --min-level warning --pattern "shader|pipeline"
```

Usage, argument shapes and exit codes: [docs/getting-started/mcp-setup.mdx](docs/getting-started/mcp-setup.mdx).

## Sanitizers

Each sanitizer instruments the **Debug** configuration in a tree of its own, with tests on. AddressSanitizer (memory bugs) on Windows, with the ASan runtime DLLs staged beside the executables:

```bat
cmake --preset vs2026-x64-local-asan
cmake --build --preset vs2026-x64-local-asan --config Debug --target <Target> -j 12
```

AddressSanitizer and ThreadSanitizer (data races) on macOS (Apple Silicon); the two cannot share a tree:

```bash
cmake --preset macos-arm64-local-asan
cmake --build --preset macos-arm64-local-asan --target <Target>
cmake --preset macos-arm64-local-tsan
cmake --build --preset macos-arm64-local-tsan --target <Target>
```

Run the test executables from `build/<preset>/bin/Debug/Tests/` as usual; a sanitizer report goes to stderr and fails the run.

## Build options

| Option | Default | Effect |
|---|---|---|
| `BUILD_TESTING` | OFF | Test targets and ctest registration |
| `BUILD_EDITOR` | ON | Editor application target |
| `BUILD_EXAMPLES` | OFF | Standalone demos (`Examples/`, UI module examples) |
| `BUILD_TOOLS` | OFF | Developer tools |
| `BUILD_BENCHMARKS` | OFF | Benchmark executables |
| `ENABLE_SCRIPTING` | ON | CoreCLR hosting and C# integration; OFF needs no .NET ([Building without .NET](#building-without-net)) |
| `PLAYER_MOVIE_RECORDER` | OFF | `--write-movie` capture and encode in the Player |
| `ENABLE_ASAN` | OFF | AddressSanitizer (Debug configuration) |
| `ENABLE_TSAN` | OFF | ThreadSanitizer (Debug configuration; clang or gcc, not with `ENABLE_ASAN`) |
| `RENDERING_ENABLE_GPU_CULLING` | OFF | GPU culling examples and tests |

Example: `cmake --preset vs2026-x64-local -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON`. `cmake --fresh` resets cached variables, so pass your `-D` options again.

## Version

One version, in the `VERSION` file at the repository root: `<year>.<month>.<patch>` with a `-alpha.<n>` or `-beta.<n>` suffix in prerelease (`2026.10.0-alpha.4`). Configure reads it, so the editor's title bar, the macOS bundles, the managed assemblies and the web package carry the same version. A release changes that line and tags the commit `v` plus the version; the web package build refuses a tag that differs from the file.

## vcpkg manifest features

The heavier optional dependencies are manifest features, all on by default except `docs`:

| Feature | Ports | Without it |
|---|---|---|
| `video` | `ffmpeg` | `VideoPlayerNull`; FFmpeg is the heaviest port (~8 min cold) |
| `physics` | `joltphysics` | Physics compiles as a stub backend |
| `nav` | `recastnavigation` | Pathfinding compiles as a stub backend |
| `ui-extras` | `lexbor`, `thorvg` | No HTML parsing or SVG rasterization; degrades gracefully |
| `tests` | `gtest`, `benchmark` | Pair with `BUILD_TESTING` / `BUILD_BENCHMARKS` |
| `docs` | `graphviz` | Off by default |

Select a subset with `-DVCPKG_MANIFEST_FEATURES="physics;nav;ui-extras;tests"` (skips FFmpeg), or `""` for the minimum.

## Troubleshooting

- **`cmake` is not recognized** (Windows): the shell is not a Visual Studio developer environment. Open the Developer Command Prompt for VS 2026, or install a standalone CMake.
- **Ninja: `cl` not found**: open the *x64 Native Tools Command Prompt for VS 2026*, or run `vcvars64.bat` first.
- **`McpServer` or `GameEngineCli` target not found**: npm was not on PATH at configure. Install Node.js 20 or newer and configure again.
- **"Target not found"** for a test or example: the option gating it is off; configure again with `-DBUILD_TESTING=ON`, `-DBUILD_EXAMPLES=ON` and so on.
- **Configure stops because dependencies moved, or a link fails with `LNK1104: cannot open ...lib`** after a pull: a dependency version moved; see the note in [Quick start](#quick-start).
- **Export in a configuration of the other CRT flavor fails with "the vcpkg installed tree has no ... runtime DLLs"**: a Debug export from a release-CRT editor (or the reverse) takes that flavor's runtime DLLs from the engine build's vcpkg tree, which an editor outside its build tree cannot find. Run it from `build/<preset>/bin/<Config>/`, set `build.vcpkgInstalledDir` in the project's `.Editor/ProjectSettings.json` to that tree's `vcpkg_installed` folder, or export in a configuration of the editor's own flavor.
- **`C1083: Cannot open compiler generated file: ''`**: the clone sits too deep and object paths overflow Windows' 260-character limit. Clone near a drive root (for example `C:\Dev\OpenEngine`).
- **Editor misbehaves after a build**: a build overwrites the staged runtime in place; restart any editor that was running from that build directory.
- **Example window opens then closes**: run it from `build/<preset>/bin/<Config>/Demos`; shaders resolve relative to the working directory.
- **`Failed to create window surface! Error: -7`**: the Vulkan instance has no window-surface extension. On macOS, install the Vulkan SDK and configure again (set `VULKAN_SDK` if it is not under `~/VulkanSDK/<version>`). On Linux, install the windowing packages from *Prerequisites*; for a pure Wayland session see the note there.
- **macOS: `[PipeTransport] Socket path too long` or `Timeout waiting for CompileServerHost`**: the C# compile server's socket path is too long for macOS. Before launching the editor: `export GE_PIPE_PATH=/tmp/gameengine-compile-server`.
