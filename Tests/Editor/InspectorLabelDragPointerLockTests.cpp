// The inspector label drags (InspectorDrag::SetupLabelDragFloat, SetupLabelDragInt, SetupLabelDragSlider)
// through a real UIManager: a drag begins past kInspectorDragThresholdPx, and only then the value moves
// and the pointer locks. Escape or a right press puts back the start value with one commit.

#include "Inspectors/InspectorDragHelpers.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Rendering/Core/Device.h"
#include "SharedHeadlessDevice.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/IntField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Slider.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

constexpr float kFloatStart = 1.0f;
constexpr int kIntStart = 5;
constexpr float kSliderStart = 4.0f;
constexpr float kSliderMax = 10.0f;
// Far enough past the threshold that every helper moves its value.
constexpr float kDragDistancePx = 40.0f;

// One label row: the label the drag starts on, and the number of commits its onChanged saw.
struct DragRow
{
    Label* RowLabel = nullptr;
    int Changed = 0;
};

class InspectorLabelDragPointerLockTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = SharedHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "Device init failed";

        UIRegistration::RegisterBuiltInControls();

        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        m_FloatField = AddRow<FloatField>(*root, m_FloatRow);
        m_IntField = AddRow<IntField>(*root, m_IntRow);
        m_Slider = AddRow<Slider>(*root, m_SliderRow);
        m_FloatField->SetValue(kFloatStart);
        m_IntField->SetValue(kIntStart);
        m_Slider->SetMin(0.0f);
        m_Slider->SetMax(kSliderMax);
        m_Slider->SetValueWithoutNotify(kSliderStart);
        InspectorDrag::SetupLabelDragFloat(m_FloatRow.RowLabel, m_FloatField, nullptr, [this] { ++m_FloatRow.Changed; });
        InspectorDrag::SetupLabelDragInt(m_IntRow.RowLabel, m_IntField, nullptr, [this] { ++m_IntRow.Changed; });
        InspectorDrag::SetupLabelDragSlider(m_SliderRow.RowLabel, m_Slider, nullptr, [this] { ++m_SliderRow.Changed; });

        m_Ui = std::make_unique<UIManager>(m_Device);
        m_Ui->SetRoot(std::move(root));
        m_Ui->SetPointerLockCallback([this](bool locked) { m_LockEvents.push_back(locked); });

        m_CssPath = std::filesystem::temp_directory_path() /
                    ("inspector_label_drag_lock_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".css");
        {
            std::ofstream css(m_CssPath);
            css << "#root { display: flex; flex-direction: column; width: 400px; height: 120px; }\n"
                   ".drag-row { display: flex; flex-direction: row; width: 400px; height: 30px; }\n"
                   ".drag-label { width: 100px; height: 26px; font-size: 14px; }\n"
                   ".drag-value { width: 200px; height: 26px; font-size: 14px; }\n";
        }
        ASSERT_TRUE(m_Ui->AttachStyleFromFile(m_CssPath.string()));

        m_Rg = std::make_unique<UiRgHarness>(m_Device);
        for (int i = 0; i < 4; ++i)
            Pump();
        for (const DragRow* row : Rows())
            ASSERT_GT(row->RowLabel->GetLayoutWidth(), 0.0f);
    }

    void TearDown() override
    {
        InspectorDrag::ClearLabelDragState();
        m_Rg.reset();
        m_Ui.reset();
        std::error_code ignored;
        std::filesystem::remove(m_CssPath, ignored);
    }

    template <typename FieldType>
    static FieldType* AddRow(UIElement& root, DragRow& row)
    {
        auto container = std::make_unique<UIElement>();
        container->AddClass("drag-row");
        auto label = std::make_unique<Label>();
        row.RowLabel = label.get();
        label->AddClass("drag-label");
        label->SetText("Value");
        auto field = std::make_unique<FieldType>();
        FieldType* fieldPtr = field.get();
        field->AddClass("drag-value");
        container->AddChild(std::move(label));
        container->AddChild(std::move(field));
        root.AddChild(std::move(container));
        return fieldPtr;
    }

    std::vector<DragRow*> Rows() { return {&m_FloatRow, &m_IntRow, &m_SliderRow}; }

    void Pump()
    {
        m_Ui->Update(0.016f, /*interactive=*/true);
        DriveUiRender(*m_Ui, *m_Rg);
    }

    static float CentreX(const Label* label) { return label->GetLayoutX() + label->GetLayoutWidth() * 0.5f; }
    static float CentreY(const Label* label) { return label->GetLayoutY() + label->GetLayoutHeight() * 0.5f; }

    void PressOn(const Label* label)
    {
        m_Ui->OnMouseMove(CentreX(label), CentreY(label));
        Pump();
        m_Ui->OnMouseButton(0, true);
        Pump();
    }

    void MoveBy(const Label* label, float deltaX)
    {
        m_Ui->OnMouseMove(CentreX(label) + deltaX, CentreY(label));
        Pump();
    }

    void Release()
    {
        m_Ui->OnMouseButton(0, false);
        Pump();
    }

    void ExpectStartValues() const
    {
        EXPECT_EQ(m_FloatField->GetValue(), kFloatStart);
        EXPECT_EQ(m_IntField->GetValue(), kIntStart);
        EXPECT_EQ(m_Slider->GetValue(), kSliderStart);
    }

    // First member: destroyed last, after the UIManager released its buffers.
    SharedDeviceReleaseRetirement m_ReleaseRetirement;
    IDevice* m_Device = nullptr;
    std::unique_ptr<UIManager> m_Ui;
    std::unique_ptr<UiRgHarness> m_Rg;
    DragRow m_FloatRow;
    DragRow m_IntRow;
    DragRow m_SliderRow;
    FloatField* m_FloatField = nullptr;
    IntField* m_IntField = nullptr;
    Slider* m_Slider = nullptr;
    std::vector<bool> m_LockEvents;
    std::filesystem::path m_CssPath;
};

} // namespace

TEST_F(InspectorLabelDragPointerLockTests, AClickThatJittersWithinTheThresholdKeepsTheValueAndDoesNotLock)
{
    for (DragRow* row : Rows())
    {
        PressOn(row->RowLabel);
        MoveBy(row->RowLabel, InspectorDrag::kInspectorDragThresholdPx);
        Release();
        EXPECT_EQ(row->Changed, 0);
    }
    ExpectStartValues();
    EXPECT_TRUE(m_LockEvents.empty());
}

TEST_F(InspectorLabelDragPointerLockTests, ADragPastTheThresholdLocksThePointerUntilTheRelease)
{
    PressOn(m_FloatRow.RowLabel);
    MoveBy(m_FloatRow.RowLabel, InspectorDrag::kInspectorDragThresholdPx + 7.0f);
    ASSERT_EQ(m_LockEvents, (std::vector<bool>{true}));

    Release();
    EXPECT_EQ(m_LockEvents, (std::vector<bool>{true, false}));
    EXPECT_EQ(m_FloatRow.Changed, 1);
}

TEST_F(InspectorLabelDragPointerLockTests, EscapePutsBackTheStartValueWithOneCommit)
{
    for (DragRow* row : Rows())
    {
        PressOn(row->RowLabel);
        MoveBy(row->RowLabel, kDragDistancePx);
        EXPECT_TRUE(m_Ui->OnKey(Input::kKeyCode_Escape, Input::kKeyActionPress, 0));
        Pump();
        Release();
        EXPECT_EQ(row->Changed, 1);
    }
    ExpectStartValues();
    EXPECT_EQ(m_LockEvents, (std::vector<bool>{true, false, true, false, true, false}));
}

TEST_F(InspectorLabelDragPointerLockTests, ARightPressPutsBackTheStartValueWithOneCommit)
{
    for (DragRow* row : Rows())
    {
        PressOn(row->RowLabel);
        MoveBy(row->RowLabel, kDragDistancePx);
        EXPECT_TRUE(m_Ui->OnMouseButton(Input::kMouseButton_Right, true));
        Pump();
        m_Ui->OnMouseButton(Input::kMouseButton_Right, false);
        Release();
        EXPECT_EQ(row->Changed, 1);
    }
    ExpectStartValues();
    EXPECT_EQ(m_LockEvents, (std::vector<bool>{true, false, true, false, true, false}));
}
