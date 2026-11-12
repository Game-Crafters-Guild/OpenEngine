#include "TerrainHeightFunction/TerrainHeightFunctionRegistry.h"

#include "Components/Terrain/Terrain.h"
#include "Logger/Logger.h"

#include <algorithm>

namespace GameEngine::TerrainECS
{

bool RegisterTerrainHeightFunction(std::string_view name, TerrainHeightFunctionFactory factory)
{
    return TerrainHeightFunctionRegistry::Get().Register(name, factory);
}

TerrainHeightFunctionRegistry& TerrainHeightFunctionRegistry::Get()
{
    static TerrainHeightFunctionRegistry registry;
    return registry;
}

bool TerrainHeightFunctionRegistry::Register(std::string_view name, TerrainHeightFunctionFactory factory)
{
    if (name.empty() || name.size() >= Components::kTerrainHeightFunctionNameCapacity || factory == nullptr)
    {
        LOG_ERROR("Terrain height function '{}' is not registered: a name is 1 to {} characters and the type "
                     "is not null. Shorten the name in GE_REGISTER_TERRAIN_HEIGHT_FUNCTION.",
                     name, Components::kTerrainHeightFunctionNameCapacity - 1u);
        return false;
    }
    const StringId id = HashStringId(name);
    const auto existing = std::ranges::find(m_Entries, id, &Entry::Name);
    Entry& entry = existing != m_Entries.end() ? *existing : m_Entries.emplace_back();
    entry.Name = id;
    entry.Text = name;
    entry.Registration = {factory, m_NextGeneration++, ECS::GetActiveRegistrationModule()};
    return true;
}

std::optional<TerrainHeightFunctionRegistration> TerrainHeightFunctionRegistry::Find(StringId name) const
{
    const auto entry = std::ranges::find(m_Entries, name, &Entry::Name);
    if (entry == m_Entries.end())
        return std::nullopt;
    return entry->Registration;
}

std::vector<std::string> TerrainHeightFunctionRegistry::Names() const
{
    std::vector<std::string> names;
    names.reserve(m_Entries.size());
    for (const Entry& entry : m_Entries)
        names.push_back(entry.Text);
    std::ranges::sort(names);
    return names;
}

std::size_t TerrainHeightFunctionRegistry::RemoveModuleGenerations(std::string_view moduleId, uint64 firstGeneration,
                                                                   uint64 endGeneration)
{
    return std::erase_if(m_Entries, [&](const Entry& entry) {
        return IsOwnedByModuleGenerations(entry.Registration, moduleId, firstGeneration, endGeneration);
    });
}

bool TerrainHeightFunctionRegistry::IsOwnedByModuleGenerations(const TerrainHeightFunctionRegistration& registration,
                                                               std::string_view moduleId, uint64 firstGeneration,
                                                               uint64 endGeneration)
{
    const ECS::ModuleRegistrationStamp& owner = registration.Owner;
    return owner.IsSet() && owner.ModuleId == moduleId && owner.Generation >= firstGeneration &&
           owner.Generation < endGeneration;
}

} // namespace GameEngine::TerrainECS
