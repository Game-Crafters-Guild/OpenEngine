// The reflection document the web library hands a page (ge_reflection_json), built from the
// engine's real field tables. One component's entry is pinned as text: the field kinds, the
// offsets, the sizes and the element counts a page indexes the component's bytes with.

#include "PageReflection.h"

#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ComponentRegistry.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace
{

// The Light entry as the page reads it, one field per line.
constexpr const char* kLightPage =
    R"({"name":"Light","typeId":"3570210769200010143","fields":[)"
    R"({"name":"Type","tsName":"type","kind":"UInt32","offset":0,"size":4,"count":1,"enum":[{"name":"Directional","value":0},{"name":"Point","value":1},{"name":"Spot","value":2},{"name":"Ambient","value":3},{"name":"Area","value":4},{"name":"Volume","value":5}]},)"
    R"({"name":"Color","tsName":"color","kind":"Float","offset":4,"size":12,"count":3},)"
    R"({"name":"Intensity","tsName":"intensity","kind":"Float","offset":16,"size":4,"count":1},)"
    R"({"name":"Range","tsName":"range","kind":"Float","offset":20,"size":4,"count":1},)"
    R"({"name":"InnerAngle","tsName":"innerAngle","kind":"Float","offset":24,"size":4,"count":1},)"
    R"({"name":"OuterAngle","tsName":"outerAngle","kind":"Float","offset":28,"size":4,"count":1},)"
    R"({"name":"AreaShape","tsName":"areaShape","kind":"UInt32","offset":32,"size":4,"count":1,"enum":[{"name":"Rectangle","value":0},{"name":"Disc","value":1},{"name":"Sphere","value":2},{"name":"Cylinder","value":3}]},)"
    R"({"name":"AreaWidth","tsName":"areaWidth","kind":"Float","offset":36,"size":4,"count":1},)"
    R"({"name":"AreaHeight","tsName":"areaHeight","kind":"Float","offset":40,"size":4,"count":1},)"
    R"({"name":"AreaRadius","tsName":"areaRadius","kind":"Float","offset":44,"size":4,"count":1},)"
    R"({"name":"Falloff","tsName":"falloff","kind":"UInt32","offset":48,"size":4,"count":1,"enum":[{"name":"PhysicalInverseSquare","value":0},{"name":"Linear","value":1},{"name":"SmoothRange","value":2},{"name":"Custom","value":3}]},)"
    R"({"name":"Decay","tsName":"decay","kind":"Float","offset":52,"size":4,"count":1},)"
    R"({"name":"FogContribution","tsName":"fogContribution","kind":"Float","offset":56,"size":4,"count":1},)"
    R"({"name":"FogDensityBoost","tsName":"fogDensityBoost","kind":"Float","offset":60,"size":4,"count":1},)"
    R"({"name":"FogAnisotropy","tsName":"fogAnisotropy","kind":"Float","offset":64,"size":4,"count":1},)"
    R"({"name":"FogOriginFade","tsName":"fogOriginFade","kind":"Float","offset":68,"size":4,"count":1},)"
    R"({"name":"CastsLight","tsName":"castsLight","kind":"Bool","offset":72,"size":1,"count":1},)"
    R"({"name":"CastsShadows","tsName":"castsShadows","kind":"Bool","offset":73,"size":1,"count":1},)"
    R"({"name":"CascadeCount","tsName":"cascadeCount","kind":"UInt32","offset":76,"size":4,"count":1},)"
    R"({"name":"ShadowResolutionTier","tsName":"shadowResolutionTier","kind":"UInt32","offset":80,"size":4,"count":1,"enum":[{"name":"Inherit","value":0},{"name":"Low","value":1},{"name":"Medium","value":2},{"name":"High","value":3}]},)"
    R"({"name":"ShadowAngularDiameter","tsName":"shadowAngularDiameter","kind":"Float","offset":84,"size":4,"count":1},)"
    R"({"name":"UseColorTemperature","tsName":"useColorTemperature","kind":"Bool","offset":88,"size":1,"count":1},)"
    R"({"name":"ColorTemperature","tsName":"colorTemperature","kind":"Float","offset":92,"size":4,"count":1},)"
    R"({"name":"IntensityUnit","tsName":"intensityUnit","kind":"UInt32","offset":96,"size":4,"count":1,"enum":[{"name":"Unitless","value":0},{"name":"Lux","value":1},{"name":"Lumen","value":2},{"name":"Candela","value":3}]})"
    R"(]})";

// A runtime component's fields: an opaque byte run (the kind a scripted component's untyped
// field reflects as, with no element width) counts its bytes; a float counts one.
constexpr const char* kOpaqueProbeFields =
    R"([{"name":"Blob","tsName":"blob","kind":"Bytes","offset":0,"size":12,"count":12},)"
    R"({"name":"Weight","tsName":"weight","kind":"Float","offset":12,"size":4,"count":1}])";

// The page document's entry for the component named `name`, or null.
const nlohmann::ordered_json* FindComponentPage(const nlohmann::ordered_json& document, std::string_view name)
{
    const nlohmann::ordered_json& components = document.at("components");
    const auto found = std::find_if(components.begin(), components.end(), [name](const nlohmann::ordered_json& component) {
        return component.at("name").get<std::string_view>() == name;
    });
    return found == components.end() ? nullptr : &*found;
}

} // namespace

TEST(PageReflection, TheLightComponentPageIsPinned)
{
    const nlohmann::ordered_json document =
        nlohmann::ordered_json::parse(GameEngine::WebLibrary::BuildReflectionJson());
    const nlohmann::ordered_json* light = FindComponentPage(document, "Light");
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->dump(), std::string(kLightPage));
}

TEST(PageReflection, AnOpaqueFieldCountsItsBytes)
{
    using namespace GameEngine::ECS;
    // Registered the way a scripted component is: a blob with an owned field table.
    const ComponentTypeId typeId = ComponentRegistry::RegisterBlobComponent("S1aOpaqueProbe", 16);
    ASSERT_NE(typeId, 0u);
    const std::array<FieldInfo, 2> fields{{
        {"Blob", 0, 12, FieldTypeId::Bytes},
        {"Weight", 12, 4, FieldTypeId::Float},
    }};
    ComponentFieldRegistry::RegisterOwned(typeId, fields, "S1aOpaqueProbe");

    const nlohmann::ordered_json document =
        nlohmann::ordered_json::parse(GameEngine::WebLibrary::BuildReflectionJson());
    const nlohmann::ordered_json* probe = FindComponentPage(document, "S1aOpaqueProbe");
    ComponentFieldRegistry::UnregisterForTests(typeId);
    ComponentRegistry::UnregisterComponentForTests(typeId);
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe->at("fields").dump(), std::string(kOpaqueProbeFields));
}
