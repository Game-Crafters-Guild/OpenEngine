// The terrain inspector's planet rows. A Planet Radius commit rebuilds the inspector, so the sculpt
// refusal notice, which reads the radius at build time, shows without a new selection. The Height
// Layer row is read-only.
//
// BOUNDARY: the inspector is built in process over a live World and the field commit goes through
// FloatField::NotifyValueChanged, as in TerrainGrassRegionRowsTests. The refusal text itself is
// covered by TerrainRegionBakeTests; the rebuild is the editor's InspectorPanel and is not run here.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Terrain/TerrainInspector.h"

#include "AssetCore/GUID.h"
#include "Components/Terrain/Terrain.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "UI/AssetField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <string>

using namespace GameEngine;

namespace
{

template<typename T>
T* FindFirst(UIElement& element)
{
    if (auto* found = dynamic_cast<T*>(&element))
        return found;
    for (const auto& child : element.GetChildren())
        if (T* found = FindFirst<T>(*child))
            return found;
    return nullptr;
}

UIElement* FindFirstWithClass(UIElement& element, const std::string& className)
{
    if (element.HasClass(className))
        return &element;
    for (const auto& child : element.GetChildren())
        if (UIElement* found = FindFirstWithClass(*child, className))
            return found;
    return nullptr;
}

// The field of type T on the inspector row whose label reads `text`.
template<typename T>
T* FindRowField(UIElement& element, const std::string& text)
{
    if (auto* label = dynamic_cast<Label*>(&element); label && label->GetText() == text)
    {
        UIElement* row = label->GetParent() ? label->GetParent()->GetParent() : nullptr;
        return row ? FindFirst<T>(*row) : nullptr;
    }
    for (const auto& child : element.GetChildren())
        if (T* found = FindRowField<T>(*child, text))
            return found;
    return nullptr;
}

class TerrainPlanetRowsTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void BuildPlanet(float radius, const GUID& heightLayer = GUID::Null(), float samplesPerMeter = 2.0f)
    {
        RegisterTerrainInspector();
        Components::Terrain terrain{};
        terrain.Domain = Components::TerrainDomain::Spherical;
        terrain.PlanetRadius = radius;
        terrain.SamplesPerMeter = samplesPerMeter;
        if (!heightLayer.IsNull())
            terrain.HeightLayerGuid.Set(heightLayer);
        entity = world.Create(terrain).GetHandle();
        InspectorFn* inspector = InspectorRegistry::Get().TryGetComponentInspector<Components::Terrain>();
        ASSERT_NE(inspector, nullptr);
        InspectorContext ctx;
        ctx.Parent = &parent;
        ctx.World = &world;
        ctx.Entity = entity;
        ctx.RequestInspectorRefresh = [this] { ++refreshes; };
        (*inspector)(ctx);
    }

    ECS::World world{nullptr};
    ECS::EntityHandle entity{};
    UIElement parent{"div"};
    int refreshes = 0;
};

TEST_F(TerrainPlanetRowsTest, APlanetRadiusCommitRebuildsTheInspector)
{
    BuildPlanet(60000.0f);
    FloatField* radius = FindRowField<FloatField>(parent, "Planet Radius");
    ASSERT_NE(radius, nullptr) << "a planet shows no Planet Radius row";
    radius->SetValue(61000.0f);
    radius->NotifyValueChanged();
    EXPECT_FLOAT_EQ(world.GetComponent<Components::Terrain>(entity)->PlanetRadius, 61000.0f);
    EXPECT_EQ(refreshes, 1) << "the sculpt refusal notice reads the radius when the inspector is built";
}

// The row's maximum is the largest planet the page store holds, so a 600 km planet keeps its radius.
TEST_F(TerrainPlanetRowsTest, APlanetRadiusCommitKeepsARadiusThePageStoreHolds)
{
    BuildPlanet(600000.0f);
    FloatField* radius = FindRowField<FloatField>(parent, "Planet Radius");
    ASSERT_NE(radius, nullptr) << "a planet shows no Planet Radius row";
    radius->SetValue(600000.0f);
    radius->NotifyValueChanged();
    EXPECT_FLOAT_EQ(world.GetComponent<Components::Terrain>(entity)->PlanetRadius, 600000.0f);
}

// With Samples Per Meter at 0 the page store sets no maximum, so a commit keeps the radius.
TEST_F(TerrainPlanetRowsTest, APlanetRadiusCommitWithNoSampleSpacingKeepsTheRadius)
{
    BuildPlanet(600000.0f, GUID::Null(), 0.0f);
    FloatField* radius = FindRowField<FloatField>(parent, "Planet Radius");
    ASSERT_NE(radius, nullptr) << "a planet shows no Planet Radius row";
    radius->SetValue(600000.0f);
    radius->NotifyValueChanged();
    EXPECT_FLOAT_EQ(world.GetComponent<Components::Terrain>(entity)->PlanetRadius, 600000.0f);
}

// Only a save sets the Height Layer reference: the row shows it read-only, with no clear button.
TEST_F(TerrainPlanetRowsTest, TheHeightLayerRowIsReadOnlyWithNoClearButton)
{
    BuildPlanet(60000.0f, GUID::Generate());
    AssetField* layer = FindRowField<AssetField>(parent, "Height Layer");
    ASSERT_NE(layer, nullptr) << "a planet shows no Height Layer row";
    EXPECT_FALSE(layer->IsEnabled());
    UIElement* clear = FindFirstWithClass(*layer, "asset-field-clear");
    ASSERT_NE(clear, nullptr) << "the reference field layout did not load";
    EXPECT_TRUE(clear->HasClass("hidden")) << "the row offers a clear that it cannot do";
}

} // namespace
