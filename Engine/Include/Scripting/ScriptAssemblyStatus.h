#pragma once

#include <cstdint>

namespace GameEngine
{
/// Where the bound project's C# scripts assembly stands, as `ScriptManager` reports it.
enum class ScriptAssemblyState : std::uint8_t
{
    NotLoaded, ///< No assembly of the bound project is loaded and none is being built.
    Building,  ///< A compile and load of the project's scripts is in flight.
    Loaded,    ///< The project's assembly is loaded; its types are registered.
    Failed,    ///< The last compile or load failed; the Log holds the errors.
    Disabled,  ///< This build has no C# scripting (ENABLE_SCRIPTING=OFF): no assembly ever loads.
};

/// A snapshot of the managed scripts side for status displays.
struct ScriptAssemblyStatus
{
    ScriptAssemblyState State = ScriptAssemblyState::NotLoaded;
    /// C# components and systems the loaded assembly defines; meaningful when State is Loaded.
    std::uint32_t TypeCount = 0;

    bool operator==(const ScriptAssemblyStatus&) const = default;
};
} // namespace GameEngine
