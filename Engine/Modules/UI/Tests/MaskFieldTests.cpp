#include <gtest/gtest.h>

#include "Core/ApplicationPaths.h"
#include "Mathematics/Rect.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/MaskField.h"
#include "UI/Internal/MaskFieldAccess.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/ITextMeasurable.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/StyleProperties.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "IsolatedUIFixture.h"
#include "SharedHeadlessDevice.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;

namespace
{

void SendMouse(UIElement& element, EventId id, float x, float y)
{
    UIEvent event{};
    event.Id = id;
    event.X = x;
    event.Y = y;
    event.Button = 0;
    event.Target = &element;
    event.CurrentTarget = &element;
    element.DispatchEvent(event);
}

void Click(UIElement& element)
{
    UILayoutAccess::SetLastLayoutRect(element, 0.0f, 0.0f, 100.0f, 20.0f);
    SendMouse(element, kEventMouseDown, 5.0f, 5.0f);
    SendMouse(element, kEventMouseUp, 5.0f, 5.0f);
}

// Options with their own bits, not their index: the particle flags' shape.
std::vector<MaskOption> SevenOptions()
{
    return {{0x01u, "A"}, {0x02u, "B"}, {0x08u, "C"}, {0x10u, "D"}, {0x40u, "E"}, {0x100u, "F"}, {0x400u, "G"}};
}

// The control's own sheet as the build stages it next to the test exe; empty when it is not staged.
std::string ReadStagedMaskFieldSheet()
{
    std::ifstream in(ApplicationPaths::GetExecutableDirectory() / "Assets" / "UI" / "controls" / "MaskField.css",
                     std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The name of an option row: the checkbox's label.
const Label* OptionRowLabel(const UIElement& row)
{
    for (const auto& child : row.GetChildren())
    {
        if (child->HasClass("checkbox-label"))
            return dynamic_cast<const Label*>(child.get());
    }
    return nullptr;
}

struct BitsEvent
{
    uint32 Bits = 0u;
    bool On = false;
};

} // namespace

TEST(MaskFieldTests, SummaryNamesTheValueByTheRules)
{
    MaskField field;
    const std::vector<MaskOption> options = SevenOptions();
    field.SetOptions(options);
    field.SetSummaryNoun("layers");

    field.SetValueWithoutNotify(0u);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "Nothing");
    field.SetValueWithoutNotify(0x55Bu);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "Everything");
    field.SetValueWithoutNotify(0x08u);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "C");
    field.SetValueWithoutNotify(0x08u | 0x400u);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "C, G");
    field.SetValueWithoutNotify(0x55Bu & ~0x10u);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "All but D");
    field.SetValueWithoutNotify(0x55Bu & ~0x10u & ~0x01u);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "All but A, D");
    field.SetValueWithoutNotify(0x01u | 0x02u | 0x08u);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "3 layers");
    field.SetMixedBits(0x02u);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "Mixed");
}

TEST(MaskFieldTests, NothingShowsTheCallersTooltip)
{
    MaskField field;
    const std::vector<MaskOption> options = SevenOptions();
    field.SetOptions(options);
    field.SetNothingTooltip("Sees no layer.");
    field.SetTooltip("The layers this camera draws.");
    field.SetValueWithoutNotify(0u);
    EXPECT_EQ(MaskFieldAccess::SummaryLabel(field)->GetTooltip(), "Sees no layer.");
    EXPECT_TRUE(MaskFieldAccess::SummaryLabel(field)->HasClass("mask-field-nothing"));
    field.SetValueWithoutNotify(0x01u);
    EXPECT_EQ(MaskFieldAccess::SummaryLabel(field)->GetTooltip(), "");
    EXPECT_EQ(field.GetTooltip(), "The layers this camera draws.") << "the field's own tooltip stays";
}

TEST(MaskFieldTests, ARowClickRaisesOneEventWithThatOptionsBit)
{
    MaskField field;
    const std::vector<MaskOption> options = SevenOptions();
    field.SetOptions(options);
    field.SetValueWithoutNotify(0x01u);
    std::vector<BitsEvent> events;
    field.SetOnBitsChanged([&events](uint32 bits, bool on) { events.push_back({bits, on}); });

    MaskFieldAccess::OpenPopup(field);
    ASSERT_EQ(MaskFieldAccess::OptionRows(field).size(), options.size());
    Click(*MaskFieldAccess::OptionRows(field)[2]);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].Bits, 0x08u);
    EXPECT_TRUE(events[0].On);
    EXPECT_EQ(field.GetValue(), 0x01u | 0x08u) << "the other bits stay as they were";

    Click(*MaskFieldAccess::OptionRows(field)[0]);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].Bits, 0x01u);
    EXPECT_FALSE(events[1].On);
    EXPECT_EQ(field.GetValue(), 0x08u);
}

// The rows come from the Checkbox registration: each row carries the registered tag and the registered defaults.
TEST(MaskFieldTests, OptionRowsAreRegisteredCheckboxesWithTheOptionLabels)
{
    MaskField field;
    const std::vector<MaskOption> options = SevenOptions();
    field.SetOptions(options);
    MaskFieldAccess::OpenPopup(field);

    const std::vector<Checkbox*>& rows = MaskFieldAccess::OptionRows(field);
    ASSERT_EQ(rows.size(), options.size());
    const StringId checkboxTag = UIRegistration::ElementFactoryRegistry::Instance().GetTagId("checkbox");
    for (size_t index = 0; index < rows.size(); ++index)
    {
        EXPECT_EQ(rows[index]->GetTagId(), checkboxTag);
        EXPECT_EQ(rows[index]->GetText(), options[index].Label);
        EXPECT_TRUE(rows[index]->HasClass("dropdown-item"));
        EXPECT_TRUE(rows[index]->HasClass("mask-field-option"));
        EXPECT_EQ(rows[index]->GetFocusProxy(), &field);
    }
}

TEST(MaskFieldTests, OptionsThatShrinkWhileThePopupIsClosedGiveOneRowEachAtTheNextOpen)
{
    MaskField field;
    const std::vector<MaskOption> options = SevenOptions();
    field.SetOptions(options);
    MaskFieldAccess::OpenPopup(field);
    MaskFieldAccess::ClosePopup(field);

    field.SetOptions(std::span<const MaskOption>(options.data(), 3));
    field.SetValueWithoutNotify(0x08u);
    MaskFieldAccess::OpenPopup(field);

    const std::vector<Checkbox*>& rows = MaskFieldAccess::OptionRows(field);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0]->GetText(), "A");
    EXPECT_EQ(rows[2]->GetText(), "C");
    EXPECT_TRUE(rows[2]->GetValue());
}

TEST(MaskFieldTests, AMixedBitDrawsMixedAndAClickSetsIt)
{
    MaskField field;
    const std::vector<MaskOption> options = SevenOptions();
    field.SetOptions(options);
    // The primary entity has B and another selected entity does not: the row shows B checked and mixed.
    field.SetValueWithoutNotify(0x01u | 0x02u);
    field.SetMixedBits(0x02u);
    std::vector<BitsEvent> events;
    field.SetOnBitsChanged([&events](uint32 bits, bool on) { events.push_back({bits, on}); });

    MaskFieldAccess::OpenPopup(field);
    Checkbox* mixedRow = MaskFieldAccess::OptionRows(field)[1];
    EXPECT_TRUE(mixedRow->IsMixed());
    EXPECT_TRUE(mixedRow->HasClass("mixed"));
    EXPECT_FALSE(MaskFieldAccess::OptionRows(field)[0]->IsMixed());

    Click(*mixedRow);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].Bits, 0x02u);
    EXPECT_TRUE(events[0].On) << "a mixed row resolves to on for the whole selection";
    EXPECT_FALSE(mixedRow->IsMixed());
    EXPECT_NE(MaskFieldAccess::Summary(field), "Mixed");
}

TEST(MaskFieldTests, EverythingAndNothingRaiseOneEventWithEveryOption)
{
    MaskField field;
    const std::vector<MaskOption> options = SevenOptions();
    field.SetOptions(options);
    field.SetValueWithoutNotify(0x08u);
    std::vector<BitsEvent> events;
    field.SetOnBitsChanged([&events](uint32 bits, bool on) { events.push_back({bits, on}); });

    SendMouse(*MaskFieldAccess::EverythingRow(field), kEventMouseDown, 0.0f, 0.0f);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].Bits, 0x55Bu);
    EXPECT_TRUE(events[0].On);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "Everything");

    SendMouse(*MaskFieldAccess::NothingRow(field), kEventMouseDown, 0.0f, 0.0f);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].Bits, 0x55Bu);
    EXPECT_FALSE(events[1].On);
    EXPECT_EQ(field.GetValue(), 0u);
    EXPECT_EQ(MaskFieldAccess::Summary(field), "Nothing");
}

// A popup taller than the room below its field opens upward when the room above is larger, and its option
// list gets that room and scrolls; Everything and Nothing stay in view.
TEST(MaskFieldTests, APopupTallerThanTheWindowOpensUpwardAndScrolls)
{
    auto* device = SharedHeadlessDevice();
    if (!device)
        GTEST_SKIP() << "Device init failed";

    auto root = std::make_unique<UIElement>();
    auto owned = std::make_unique<MaskField>();
    MaskField* field = owned.get();
    root->AddChild(std::move(owned));
    UIManager ui(device);
    ui.SetRoot(std::move(root));
    // A short window: 32 rows need about 850 px.
    UILayoutAccess::SetLastLayoutRect(*ui.GetRootElement(), 0.0f, 0.0f, 400.0f, 300.0f);

    std::vector<MaskOption> layers;
    for (uint32 layer = 0; layer < 32; ++layer)
        layers.push_back({1u << layer, "Layer " + std::to_string(layer)});
    field->SetOptions(layers);

    UILayoutAccess::SetLastLayoutRect(*field, 10.0f, 220.0f, 120.0f, 20.0f);
    MaskFieldAccess::OpenPopup(*field);
    EXPECT_TRUE(field->HasClass("open-upward")) << "220 px above, 60 px below";
    const auto bound = MaskFieldAccess::OptionList(*field)->Style().Get(Style::MaxHeight);
    ASSERT_TRUE(bound.has_value()) << "the option list is bounded and scrolls";
    EXPECT_GT(bound->Value, 0.0f);
    EXPECT_LT(bound->Value, 220.0f);
    MaskFieldAccess::ClosePopup(*field);

    // Three options fit below a field near the top: down, unbounded.
    const std::vector<MaskOption> few = SevenOptions();
    field->SetOptions(std::span<const MaskOption>(few.data(), 3));
    UILayoutAccess::SetLastLayoutRect(*field, 10.0f, 10.0f, 120.0f, 20.0f);
    MaskFieldAccess::OpenPopup(*field);
    EXPECT_FALSE(field->HasClass("open-upward"));
    EXPECT_FALSE(MaskFieldAccess::OptionList(*field)->Style().Get(Style::MaxHeight).has_value());
}

// The dropdown's rule, shared: the popup hangs from the header's left edge at the width of its widest name,
// also past the panel, and a name wider than the window's room ends in an ellipsis.
TEST(MaskFieldTests, ThePopupFitsItsWidestNameAndALongNameEndsInAnEllipsis)
{
    const std::string maskFieldSheet = ReadStagedMaskFieldSheet();
    ASSERT_FALSE(maskFieldSheet.empty()) << "controls/MaskField.css is not staged next to the test exe";
    UITesting::IsolatedUIFixture fixture;
    const bool built = fixture.Build(
        1.0f,
        R"(<uielement id="root"><uielement id="pane" class="panel"><uielement id="label"/><uielement id="host"/></uielement></uielement>)",
        R"(
#root { width: 400px; height: 300px; flex-direction: row; }
#pane { width: 150px; height: 300px; flex-direction: row; }
#label { width: 90px; }
#host { width: 60px; }
.dropdown { min-width: 60px; }
.dropdown-items { position: absolute; top: 100%; left: 0px; width: 100%; }
.dropdown-items.fit-content { width: auto; min-width: 100%; }
.dropdown-item-fit-content { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.dropdown-item { padding: 2px 10px 2px 24px; }
)" + maskFieldSheet);
    if (!fixture.DeviceAvailable())
        GTEST_SKIP() << "No Vulkan device available";
    ASSERT_TRUE(built) << fixture.Diagnostic();

    UIElement* host = fixture.Element("host");
    UIElement* pane = fixture.Element("pane");
    ASSERT_NE(host, nullptr);
    ASSERT_NE(pane, nullptr);
    auto owned = std::make_unique<MaskField>();
    MaskField* field = owned.get();
    host->AddChild(std::move(owned));
    const std::vector<MaskOption> shortNames = {{0x1u, "Distant scenery"}, {0x2u, "Props"}};
    field->SetOptions(shortNames);
    fixture.Settle();
    MaskFieldAccess::OpenPopup(*field);
    fixture.Settle();

    UIElement* popup = MaskFieldAccess::Popup(*field);
    const Mathematics::Rect header = field->GetWindowRect();
    Mathematics::Rect box = popup->GetWindowRect();
    EXPECT_NEAR(box.X, header.X, 0.5f) << "the popup hangs from the header's left edge";
    EXPECT_GT(box.Right(), pane->GetWindowRect().Right()) << "the popup draws over the next panel";
    EXPECT_LT(box.Right(), 398.0f) << "the popup is as wide as its widest name, not as the window";
    const Label* name = OptionRowLabel(*MaskFieldAccess::OptionRows(*field)[0]);
    ASSERT_NE(name, nullptr);
    EXPECT_FALSE(name->GetTextMeasurable()->WasLastRunEllipsized()) << "a name that fits is whole";

    MaskFieldAccess::ClosePopup(*field);
    const std::vector<MaskOption> longName = {{0x1u, std::string(400, 'W')}, {0x2u, "Props"}};
    field->SetOptions(longName);
    fixture.Settle();
    MaskFieldAccess::OpenPopup(*field);
    fixture.Settle();
    box = popup->GetWindowRect();
    EXPECT_NEAR(box.X, header.X, 0.5f) << "the room right of the header is the larger";
    EXPECT_NEAR(box.Right(), 398.0f, 0.5f) << "the popup takes the room up to the window's edge";
    name = OptionRowLabel(*MaskFieldAccess::OptionRows(*field)[0]);
    ASSERT_NE(name, nullptr);
    EXPECT_TRUE(name->GetTextMeasurable()->WasLastRunEllipsized()) << "the long name ends in an ellipsis";
}
