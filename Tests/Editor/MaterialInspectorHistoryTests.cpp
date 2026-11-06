#include "Assets/MaterialAsset.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/MaterialInspector.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/UIElement.h"
#include "UndoRedo/UndoRedoService.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace GameEngine;

namespace
{
template <typename TElement>
TElement* FindElement(UIElement& root)
{
    if (auto* match = dynamic_cast<TElement*>(&root))
        return match;
    for (const auto& child : root.GetChildren())
        if (auto* match = FindElement<TElement>(*child))
            return match;
    return nullptr;
}

UIElement* FindRow(UIElement& root, const std::string& labelText)
{
    if (auto* label = dynamic_cast<Label*>(&root); label && label->GetText() == labelText)
    {
        auto* parent = label->GetParent();
        return parent->HasClass("inspector-label-cell") ? parent->GetParent() : parent;
    }
    for (const auto& child : root.GetChildren())
        if (auto* row = FindRow(*child, labelText))
            return row;
    return nullptr;
}

UIElement* FindGrid(UIElement& root)
{
    if (root.HasClass("material-tiling-grid"))
        return &root;
    for (const auto& child : root.GetChildren())
        if (auto* grid = FindGrid(*child))
            return grid;
    return nullptr;
}

void SendMouse(UIElement& element, EventId eventId, float positionX)
{
    UIEvent event{};
    event.Id = eventId;
    event.Target = &element;
    event.CurrentTarget = &element;
    event.Button = 0;
    event.X = positionX;
    element.DispatchEvent(event);
}
} // namespace

class MaterialInspectorHistoryTests : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        m_Path = std::filesystem::temp_directory_path() / (GUID::Generate().ToString() + ".material");
        auto document = MaterialDocument::CreateDefaultPBR("History test");
        document.surfaceShader.clear();
        document.textures["albedoMap"] = "";
        {
            std::ofstream output(m_Path);
            output << SerializeMaterialDocument(document).dump(2);
        }
        m_Material = std::make_unique<MaterialAsset>(GUID::Generate(), m_Path);
        ASSERT_TRUE(m_Material->Load());
        BuildMaterialInspectorUI(&m_Root, m_Material.get(), {}, {}, &m_History);
    }

    void TearDown() override
    {
        InspectorDrag::GetDragState().draggingLabel = nullptr;
        m_Root.RemoveAllChildren();
        m_History.Clear();
        m_Material.reset();
        std::error_code error;
        std::filesystem::remove(m_Path, error);
    }

    template <typename TField>
    void CheckScalarField()
    {
        auto* row = FindRow(m_Root, "Roughness");
        ASSERT_NE(row, nullptr);
        auto* field = FindElement<TField>(*row);
        ASSERT_NE(field, nullptr);
        field->SetValueWithoutNotify(0.6f);
        field->NotifyValueChanging();
        ASSERT_TRUE(m_History.HasOpenInteractiveEdit());
        ASSERT_TRUE(m_History.CommitPendingEdit());
        EXPECT_FALSE(m_History.HasOpenInteractiveEdit());

        field->SetValueWithoutNotify(0.8f);
        field->NotifyValueChanging();
        EXPECT_TRUE(m_History.HasOpenInteractiveEdit());
        ASSERT_TRUE(m_History.CommitPendingEdit());
        field->NotifyValueChanged();
        ASSERT_TRUE(m_History.CommitPendingEdit());
        EXPECT_FLOAT_EQ(std::get<float>(m_Material->GetDocument().properties.at("roughness")), 0.8f);
        ASSERT_EQ(m_History.GetUndoCount(), 1u);
        m_History.Undo();
        EXPECT_FLOAT_EQ(std::get<float>(m_Material->GetDocument().properties.at("roughness")), 0.5f);
        m_History.Redo();
        EXPECT_FLOAT_EQ(std::get<float>(m_Material->GetDocument().properties.at("roughness")), 0.8f);
    }

    std::filesystem::path m_Path;
    std::unique_ptr<MaterialAsset> m_Material;
    Editor::UndoRedoService m_History;
    UIElement m_Root;
};

TEST_F(MaterialInspectorHistoryTests, SliderReacquiresAfterRepeatedSettlingBeforeCommit)
{
    CheckScalarField<Slider>();
}

TEST_F(MaterialInspectorHistoryTests, NumericFieldReacquiresAfterRepeatedSettlingBeforeCommit)
{
    CheckScalarField<FloatField>();
}

TEST_F(MaterialInspectorHistoryTests, TextureFieldReacquiresAcrossSuccessiveEditsAndSettles)
{
    auto* grid = FindGrid(m_Root);
    ASSERT_NE(grid, nullptr);
    auto* field = FindElement<FloatField>(*grid);
    ASSERT_NE(field, nullptr);
    for (float value : {2.0f, 3.0f})
    {
        field->SetValueWithoutNotify(value);
        field->NotifyValueChanging();
        ASSERT_TRUE(m_History.CommitPendingEdit());
        field->NotifyValueChanged();
        ASSERT_TRUE(m_History.CommitPendingEdit());
        EXPECT_FLOAT_EQ(m_Material->GetDocument().textureTransforms.at("albedoMap")[0], value);
    }
    ASSERT_EQ(m_History.GetUndoCount(), 2u);
    m_History.Undo();
    EXPECT_FLOAT_EQ(m_Material->GetDocument().textureTransforms.at("albedoMap")[0], 2.0f);
    m_History.Undo();
    EXPECT_EQ(m_Material->GetDocument().textureTransforms.count("albedoMap"), 0u);
    m_History.Redo();
    EXPECT_FLOAT_EQ(m_Material->GetDocument().textureTransforms.at("albedoMap")[0], 2.0f);
}

TEST_F(MaterialInspectorHistoryTests, TextureHeaderReacquiresOnReleaseAfterSettling)
{
    auto* grid = FindGrid(m_Root);
    ASSERT_NE(grid, nullptr);
    auto* headerRow = FindRow(*grid, "Tiling");
    ASSERT_NE(headerRow, nullptr);
    auto* header = FindElement<Label>(*headerRow);
    ASSERT_NE(header, nullptr);
    auto* field = FindElement<FloatField>(*grid);
    ASSERT_NE(field, nullptr);

    SendMouse(*header, kEventMouseDown, 0.0f);
    SendMouse(*header, kEventMouseMove, 200.0f);
    const float tiling = field->GetValue();
    ASSERT_NE(tiling, 1.0f);
    ASSERT_TRUE(m_History.CommitPendingEdit());
    SendMouse(*header, kEventMouseUp, 200.0f);
    ASSERT_TRUE(m_History.CommitPendingEdit());
    EXPECT_FLOAT_EQ(m_Material->GetDocument().textureTransforms.at("albedoMap")[0], tiling);
    EXPECT_FLOAT_EQ(m_Material->GetDocument().textureTransforms.at("albedoMap")[5], tiling);
    ASSERT_EQ(m_History.GetUndoCount(), 1u);
    m_History.Undo();
    EXPECT_EQ(m_Material->GetDocument().textureTransforms.count("albedoMap"), 0u);
    m_History.Redo();
    EXPECT_FLOAT_EQ(m_Material->GetDocument().textureTransforms.at("albedoMap")[0], tiling);
    EXPECT_FLOAT_EQ(m_Material->GetDocument().textureTransforms.at("albedoMap")[5], tiling);
}
