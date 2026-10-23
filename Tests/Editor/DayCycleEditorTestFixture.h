// The fixture of the Day Cycle editor tests: a real asset manager (a project source, and an editor
// source holding the shipped profiles), the real undo service and change notifications, a world with
// a sky, a headless renderer and the production sky system.

#pragma once

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/DayCycleProfile.h"
#include "Components/Name.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "DayCycle/DayCycleEditorServices.h"
#include "DayCycle/DayCycleProfileInspector.h"
#include "DayCycle/DayCycleWorkingCopy.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"
#include "EditorChangeNotifications.h"
#include "Engine/Rendering/RenderServices.h"
#include "InspectorRegistry.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace GameEngine::DayCycleEditorTesting
{

constexpr float kRadiansToDegrees = 57.29577951308232f;

inline std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

template <typename T>
inline void Collect(UIElement& node, std::vector<T*>& out)
{
    for (const auto& child : node.GetChildren())
    {
        if (auto* typed = dynamic_cast<T*>(child.get()))
            out.push_back(typed);
        Collect(*child, out);
    }
}

template <typename T>
inline std::vector<T*> All(UIElement& root)
{
    std::vector<T*> out;
    Collect(root, out);
    return out;
}

inline bool HasText(UIElement& root, const std::string& text)
{
    for (Label* label : All<Label>(root))
        if (label->GetText().find(text) != std::string::npos)
            return true;
    return false;
}

inline bool HasButton(UIElement& root, const std::string& text)
{
    for (Button* button : All<Button>(root))
        if (button->GetText() == text)
            return true;
    return false;
}

inline Components::Name NameOf(const char* text)
{
    Components::Name name{};
    std::strncpy(name.value, text, sizeof(name.value) - 1);
    return name;
}

class DayCycleEditorTests : public ::testing::Test
{
  protected:
    std::filesystem::path Root;
    AssetManager Assets;
    Editor::UndoRedoService Undo;
    Editor::EditorChangeNotifications Notifications;
    ECS::World World;
    Engine::Renderer::RenderServices Render;
    Engine::Renderer::SkyEnvironmentSystem Sky{&Render};
    ECS::EntityHandle SkyEntity;
    std::vector<ECS::EntityHandle> Selected;
    bool Playing = false;

    void SetUp() override
    {
        Editor::DayCycleWorkingCopy::CloseAll();
        Root = TestUtils::MakeUniqueTempDirectory("ge_daycycle_editor");
        std::error_code ec;
        std::filesystem::create_directories(Root / "Project", ec);
        std::filesystem::create_directories(Root / "Editor" / "DayCycles", ec);
        for (const char* file : {"Default.daycycle.json", "ClearDay.daycycle.json"})
            std::filesystem::copy_file(TestPaths::ExecutableDirectory() / "Assets" / "DayCycles" / file,
                                       Root / "Editor" / "DayCycles" / file, ec);
        ASSERT_TRUE(Assets.Initialize(Root / "Project", nullptr, Root / "AssetDatabase.assetdb", Root / "Cache"));
        AssetSourceDesc editor{};
        editor.Alias = "editor";
        editor.Root = Root / "Editor";
        ASSERT_TRUE(Assets.RegisterSource(editor));

        SkyEntity = World.CreateEntity();
        Components::SkyEnvironment sky{};
        sky.TimeOfDayHours = 10.0f;
        World.AddComponentImmediate(SkyEntity, sky);
    }

    void TearDown() override
    {
        Editor::DayCycleWorkingCopy::CloseAll();
        Assets.Shutdown();
        std::error_code ec;
        std::filesystem::remove_all(Root, ec);
    }

    Editor::DayCycleEditorServices Services()
    {
        Editor::DayCycleEditorServices services;
        services.Assets = &Assets;
        services.Undo = &Undo;
        services.Notifications = &Notifications;
        services.World = [this]() { return &World; };
        services.Render = [this]() { return &Render; };
        services.Sky = [this]() { return &Sky; };
        services.Selection = [this]() { return Selected; };
        services.IsPlaying = [this]() { return Playing; };
        return services;
    }

    std::filesystem::path ShippedDefault() const { return Root / "Editor" / "DayCycles" / "Default.daycycle.json"; }

    std::filesystem::path WriteProjectProfile(const std::string& text)
    {
        const std::filesystem::path path = Root / "Project" / "DayCycles" / "Marsh.daycycle.json";
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary) << text;
        return path;
    }

    // The asset for the file at `path` as the asset browser hands it to the inspector: the GUID the
    // registry gives the file and the path the registry stores for it, which need not be spelled as
    // `path` is (the registry folds case and separators).
    DayCycleProfile RegisteredAsset(const std::filesystem::path& path)
    {
        const GUID guid = Assets.ResolveAssetGuid(path);
        AssetMetadata metadata;
        EXPECT_TRUE(Assets.GetRegistry().TryGetAssetMetadata(guid, metadata)) << path;
        return DayCycleProfile(guid, metadata.Path);
    }

    // The profile's inspector under `root`, built with the context the inspector panel gives an asset
    // inspector: no refresh callback lists (InspectorPanel.cpp, the asset dispatch).
    void BuildInspector(UIElement& root, DayCycleProfile& asset)
    {
        ASSERT_TRUE(asset.Load());
        InspectorContext context{};
        context.Parent = &root;
        context.Object = &asset;
        context.Undo = &Undo;
        context.ChangeNotifications = &Notifications;
        Editor::BuildDayCycleProfileInspector(context, Services());
    }

    Components::SkyEnvironment SkyNow() const { return *World.GetComponent<Components::SkyEnvironment>(SkyEntity); }

    void SetSky(const Components::SkyEnvironment& sky) { World.AddComponentImmediate(SkyEntity, sky); }
};

constexpr const char* kMarshProfile = R"({ "name": "Marsh", "description": "", "tracks": {
  "exposureOffset": { "keys": "-11,0,0,0,1,1;-6,-1.5,0,0,1,1;0,0,0,0,1,1" } } })";
constexpr const char* kEmptyProfile = R"({ "name": "Fog only", "description": "", "tracks": {} })";

} // namespace GameEngine::DayCycleEditorTesting
