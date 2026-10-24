// The registry entries of the navigation, lens flare, UI document, cube LUT, DDGI and ocean
// components: every field's kind, layout, flags, range, label, tooltip and enum names and labels, and the
// component's Add Component and save state, against a committed golden. A registration or a
// marker that goes missing or changes a value fails here with the line that moved.
// Regenerate only for an intended metadata change (GE_REFLECTED_METADATA_GOLDEN_REGEN=1).

#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "Types/FormatNumber.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{

constexpr std::string_view kComponents[] = {
    "GameEngine::Components::CubeLutEffect",
    "GameEngine::Components::DDGIVolume",
    "GameEngine::Components::LensFlareSource",
    "GameEngine::Components::NavigationAgent",
    "GameEngine::Components::NavigationSource",
    "GameEngine::Components::NavigationSurface",
    "GameEngine::Components::OceanAlbedoSource",
    "GameEngine::Components::OceanBuoyancy",
    "GameEngine::Components::OceanClipSource",
    "GameEngine::Components::OceanDepthCacheSource",
    "GameEngine::Components::OceanDepthContributor",
    "GameEngine::Components::OceanFlowMapSource",
    "GameEngine::Components::OceanFlowSource",
    "GameEngine::Components::OceanMeshDepthContributor",
    "GameEngine::Components::OceanPolygonWaterBody",
    "GameEngine::Components::OceanRenderer",
    "GameEngine::Components::OceanSeabed",
    "GameEngine::Components::OceanSplineInput",
    "GameEngine::Components::OceanSurface",
    "GameEngine::Components::OceanUnderwaterExclusionVolume",
    "GameEngine::Components::OceanUnderwaterPortalOccluder",
    "GameEngine::Components::OceanUnderwaterVolume",
    "GameEngine::Components::OceanWaterBody",
    "GameEngine::Components::OceanWaterInteraction",
    "GameEngine::Components::OceanWaveImpulse",
    "GameEngine::Components::OceanWaveMaskTextureSource",
    "GameEngine::Components::OceanWaveSpectrum",
    "GameEngine::Components::UIDocument",
};

// One line per component, then one line per field in registry order.
std::string DescribeComponent(std::string_view name)
{
    const ECS::ComponentTypeId typeId = ECS::ComponentFieldRegistry::FindByName(name);
    if (typeId == 0)
        return std::string(name) + " unregistered\n";

    const std::vector<ECS::ComponentTypeId> addable = ECS::ComponentFactory::RegisteredTypes();
    std::string out = std::string(name);
    out += " addable=" + std::to_string(std::ranges::find(addable, typeId) != addable.end());
    out += " doNotSerialize=" + std::to_string(ECS::ComponentFieldRegistry::IsComponentDoNotSerialize(typeId));
    out += "\n";
    for (const ECS::FieldInfo& field : ECS::ComponentFieldRegistry::Get(typeId))
    {
        out += "  " + std::string(field.Name);
        out += " type=" + std::to_string(static_cast<int>(field.Type));
        out += " offset=" + std::to_string(field.Offset);
        out += " size=" + std::to_string(field.Size);
        out += " category=" + std::to_string(field.AssetCategory);
        out += " flags=" + std::to_string(static_cast<unsigned>(field.Flags));
        if (field.HasRange)
            out += " range=" + FormatFloat(field.MinValue) + ".." + FormatFloat(field.MaxValue);
        if (!field.EnumNames.empty())
        {
            out += " enum=";
            for (const ECS::EnumNameValue& value : field.EnumNames)
            {
                out += std::string(value.Name) + ":" + std::to_string(value.Value);
                if (!value.Label.empty())
                    out += ":" + std::string(value.Label);
                out += ",";
            }
        }
        if (!field.Label.empty())
            out += " label=" + std::string(field.Label);
        if (!field.Tooltip.empty())
            out += " tooltip=" + std::string(field.Tooltip);
        out += "\n";
    }
    return out;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

} // namespace

TEST(ReflectedFieldMetadata, RegistryEntriesMatchTheGolden)
{
    std::string actual;
    for (std::string_view name : kComponents)
        actual += DescribeComponent(name);

    const std::filesystem::path golden =
        std::filesystem::path(GE_REPO_SOURCE_DIR) / "Engine/Tests/Fixtures/ReflectedFieldMetadataGoldens.txt";
    if (std::getenv("GE_REFLECTED_METADATA_GOLDEN_REGEN") != nullptr)
    {
        // Binary, so a Windows regeneration keeps the fixture's LF line ends.
        std::ofstream(golden, std::ios::trunc | std::ios::binary) << actual;
        GTEST_SKIP() << "Regenerated " << golden.string();
    }
    EXPECT_EQ(ReadFile(golden), actual);
}
