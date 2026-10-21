// Manual reflection for CubeLutEffect.
//
// The build-time scanner skips AssetRef<> fields because its lightweight C++
// parser cannot resolve templates. Registering the complete component here
// gives it the same ComponentFactory/default-bytes contract as every other
// effect offered by the Add Post FX picker.

#include "Components/ComponentRegistration.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"

GE_REGISTER_COMPONENT_BEGIN(GameEngine::Components::CubeLutEffect)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, Enabled)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, Intensity)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, Stage)
    GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::CubeLutEffect, LutAssetGuid)
GE_REGISTER_COMPONENT_END(GameEngine::Components::CubeLutEffect)

// The Stage's names (Components::kCubeLutStageNames), so tools and the debug server read and write
// it by name, as a scene does.
GE_REFLECT_ENUM_TABLE_BEGIN(CubeLutStage)
    GE_REFLECT_ENUM_TABLE_VALUE("Finish", 0)
    GE_REFLECT_ENUM_TABLE_VALUE("Grade", 1)
GE_REFLECT_ENUM_TABLE_END()
GE_REFLECT_ENUM_FIELD(GameEngine::Components::CubeLutEffect, Stage, CubeLutStage);
