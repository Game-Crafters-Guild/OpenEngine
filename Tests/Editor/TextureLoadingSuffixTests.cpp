// The inspector header's state for a selected texture whose first load has not landed: it says
// the texture is preparing while its import cook runs, and nothing otherwise, which leaves the
// default "Loading" line.

#include "Inspectors/TextureLoadingSuffix.h"

#include "InspectorRegistry.h"

#include "AssetCore/GUID.h"
#include "Assets/TextureCookActivity.h"

#include <gtest/gtest.h>

using GameEngine::GUID;
using GameEngine::TextureCookActivity;
using GameEngine::Editor::TextureLoadingSuffix;

TEST(TextureLoadingSuffix, ReadsPreparingWhileTheTexturesCookRunsAndNothingAfter)
{
    TextureCookActivity activity;
    const GUID texture = GUID::Generate();
    const GUID other = GUID::Generate();

    EXPECT_EQ(TextureLoadingSuffix(activity, texture), "") << "a queued load is still loading";
    {
        const TextureCookActivity::Scope cooking = activity.Begin(texture);
        EXPECT_EQ(TextureLoadingSuffix(activity, texture), "preparing…");
        EXPECT_EQ(TextureLoadingSuffix(activity, other), "") << "another texture's cook says nothing here";
    }
    EXPECT_EQ(TextureLoadingSuffix(activity, texture), "") << "the cook ended";
}

// The inspector asks the selected asset's type for its pending-load suffix: the suffix registered
// for that type, and nothing for a type that registered none.
TEST(InspectorAssetLoadingSuffix, IsTheSuffixRegisteredForTheAssetsType)
{
    auto& registry = GameEngine::InspectorRegistry::Get();
    registry.RegisterAssetLoadingSuffix(GameEngine::AssetType::Texture,
                                        [](const GUID&) { return std::string("preparing…"); });
    const GUID texture = GUID::Generate();
    EXPECT_EQ(registry.AssetLoadingSuffix(GameEngine::AssetType::Texture, texture), "preparing…");
    EXPECT_EQ(registry.AssetLoadingSuffix(GameEngine::AssetType::Audio, texture), "")
        << "a type with no registered suffix leaves the default line";
}
