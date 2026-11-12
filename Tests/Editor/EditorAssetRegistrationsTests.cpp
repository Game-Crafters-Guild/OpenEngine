#include "Assets/EditorAssetRegistrations.h"

#include <gtest/gtest.h>

using namespace GameEngine;

// A type factory takes precedence over the asset's parser. UIStyleAssetParser gives a UI module
// control sheet the Control origin, so a UIStyle factory would make every sheet Author.
TEST(EditorAssetRegistrations, RegistersNoUIStyleFactory)
{
    AssetTypeRegistry registry;
    RegisterEditorAssetTypes(registry);
    EXPECT_TRUE(registry.IsAssetTypeRegistered(AssetType::UILayout));
    EXPECT_FALSE(registry.IsAssetTypeRegistered(AssetType::UIStyle))
        << "an editor UIStyle factory bypasses UIStyleAssetParser and the control sheet origin";
}
