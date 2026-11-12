#pragma once

#include "ECS/ModuleRegistration.h"
#include "TerrainECS/TerrainHeightFunction/ITerrainHeightFunction.h"
#include "Types/StringId.h"
#include "Types/Types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::TerrainECS
{

/// A registered height function: its factory, the generation of its registration and its module.
struct TerrainHeightFunctionRegistration
{
    TerrainHeightFunctionFactory Factory = nullptr;
    /// Moves on every registration of the name, so a reloaded module's function makes the bases built
    /// from the old one stale. Never 0.
    uint64 Generation = 0;
    /// The module load that registered the function (ECS::SetActiveRegistrationModule); not set for
    /// engine code. The factory and the functions it builds are code in that module's image.
    ECS::ModuleRegistrationStamp Owner;
};

/// The process-wide table of height functions by name (GE_REGISTER_TERRAIN_HEIGHT_FUNCTION).
/// Main thread only.
class TerrainHeightFunctionRegistry
{
public:
    static TerrainHeightFunctionRegistry& Get();

    /// Registers `factory` under `name`, stamped with the active registration module. A second
    /// registration of a name replaces the first and takes a new generation: the newest module load
    /// wins. False when the name is empty or too long for Components::Terrain::HeightFunction.
    bool Register(std::string_view name, TerrainHeightFunctionFactory factory);
    /// The registration of the name whose id (HashStringId) is `name`; empty when no module registers it.
    std::optional<TerrainHeightFunctionRegistration> Find(StringId name) const;
    /// The registered names, sorted.
    std::vector<std::string> Names() const;
    /// Removes every entry that a load of `moduleId` with a generation in [firstGeneration,
    /// endGeneration) registered, before that load's image unmaps. Returns the count.
    std::size_t RemoveModuleGenerations(std::string_view moduleId, uint64 firstGeneration, uint64 endGeneration);

    /// True when a load of `moduleId` with a generation in [firstGeneration, endGeneration) made
    /// `registration`.
    static bool IsOwnedByModuleGenerations(const TerrainHeightFunctionRegistration& registration,
                                           std::string_view moduleId, uint64 firstGeneration, uint64 endGeneration);

private:
    struct Entry
    {
        StringId Name = 0;
        std::string Text;
        TerrainHeightFunctionRegistration Registration;
    };

    std::vector<Entry> m_Entries;
    uint64 m_NextGeneration = 1;
};

} // namespace GameEngine::TerrainECS
