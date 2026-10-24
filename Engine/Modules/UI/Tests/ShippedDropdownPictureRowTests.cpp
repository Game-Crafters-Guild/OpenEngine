// A dropdown whose options carry pictures, as the editor ships it: the row layout
// comes from UI/controls/DropdownPictureRow.uxml and the control's own sheet from
// UI/controls/Dropdown.css, both through the asset manager over the staged Assets/
// tree, with the theme loaded globally the way the editor loads it. Each popup row
// holds the picture at its fixed size with the name and one line of text beside it,
// the closed header keeps the height of every other field, and a list without
// pictures keeps its text rows.

#include "IsolatedUIFixture.h"
#include "UIRgTestHarness.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/ApplicationPaths.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{

// The picture frame controls/Dropdown.css gives a picture row, in logical px.
constexpr float kPictureWidth = 72.0f;
constexpr float kPictureHeight = 48.0f;
// The picture row's vertical padding and the gap after the picture.
constexpr float kPictureRowPadding = 4.0f;
constexpr float kPictureGap = 10.0f;
// The header's picture, sized to its text line.
constexpr float kHeaderPictureWidth = 24.0f;
constexpr float kHeaderPictureHeight = 16.0f;
constexpr float kTolerance = 0.5f;

std::string ReadStagedFile(const char* relative)
{
    std::ifstream in(ApplicationPaths::GetExecutableDirectory() / "Assets" / "UI" / relative, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

UIElement* ChildWithClass(const UIElement& parent, const char* className)
{
    for (const auto& child : parent.GetChildren())
    {
        if (child->HasClass(className))
            return child.get();
    }
    return nullptr;
}

// Two inspector-width fields side by side under a manager over the staged Assets/
// tree, mounted as the editor source: a picture list (through Dropdown::SetOptions,
// the way an inspector row declares one) and a text list, both open.
class PictureDropdownFixture
{
  public:
    // False with the reason when the host has no device (a skip) or the staged
    // files are missing (a failure: the test would measure nothing).
    bool Init(std::string* skipReason, std::string* failure)
    {
        m_Device = SharedHeadlessDevice();
        if (!m_Device)
        {
            *skipReason = "No Vulkan device available";
            return false;
        }
        UIRegistration::RegisterBuiltInControls();

        namespace fs = std::filesystem;
        const fs::path assetRoot = ApplicationPaths::GetExecutableDirectory() / "Assets";
        const fs::path scratch = fs::temp_directory_path() / "ui_dropdown_picture_row_tests";
        std::error_code ec;
        fs::remove_all(scratch, ec);
        fs::create_directories(scratch, ec);
        if (ReadStagedFile("controls/DropdownPictureRow.uxml").empty() || ReadStagedFile("controls/Dropdown.css").empty())
        {
            *failure = "controls/DropdownPictureRow.uxml or controls/Dropdown.css is not staged next to the test exe";
            return false;
        }

        m_Pool = std::make_unique<JobSystem::WorkStealingThreadPool>(2);
        if (!m_Assets.Initialize(assetRoot, m_Pool.get(), scratch / "AssetDatabase.assetdb",
                                 scratch / ".Cache" / "AssetDatabase"))
        {
            *failure = "AssetManager init failed";
            return false;
        }
        AssetSourceDesc editorSource{};
        editorSource.Alias = std::string(kAssetSourceAliasEditor);
        editorSource.Root = assetRoot;
        if (!m_Assets.RegisterSource(editorSource))
        {
            *failure = "editor source registration failed";
            return false;
        }
        m_Assets.WaitForStartupScan(kAssetSourceAliasProject);
        m_Assets.WaitForStartupScan(kAssetSourceAliasEditor);

        m_Rg = std::make_unique<UiRgHarness>(m_Device);
        m_Ui = std::make_unique<UIManager>(m_Device, &m_Assets);
        auto root = std::make_unique<UIElement>();
        root->SetId("root");
        m_Ui->SetRoot(std::move(root));

        // The theme is global in the editor; the control's own sheet comes through the control.
        const std::string theme = ReadStagedFile("theme/tokens.css") + "\n" + ReadStagedFile("theme/widgets.css") + R"(
#root { display: flex; flex-direction: row; width: 800px; height: 600px; }
.picture-host { width: 300px; }
.plain-host { width: 300px; margin-left: 40px; }
)";
        Stylesheet sheet{};
        if (!UIParsing::CSSParser::ParseStylesFromString(theme, sheet))
        {
            *failure = "theme stylesheet did not parse";
            return false;
        }
        m_Ui->AddStylesheet(std::make_shared<const Stylesheet>(sheet));

        auto pictures = std::make_unique<Dropdown>();
        pictures->AddClass("picture-host");
        pictures->SetOptions({
            {"vivid", "Vivid", {}, {}, "probe-picture-vivid", "Bright stars glow, with four thin spikes."},
            {"physical", "Physical", {}, {}, "probe-picture-physical"},
        });
        Pictures = pictures.get();
        m_Ui->GetRootElement()->AddChild(std::move(pictures));
        auto plain = std::make_unique<Dropdown>();
        plain->AddClass("plain-host");
        plain->SetOptionsFromString("Point,Sparkle");
        Plain = plain.get();
        m_Ui->GetRootElement()->AddChild(std::move(plain));
        Settle();
        Pictures->OpenMenuUi();
        Plain->OpenMenuUi();
        Settle();
        return true;
    }

    void Settle()
    {
        for (int frame = 0; frame < 4; ++frame)
        {
            m_Ui->Update(0.016f, /*interactive=*/true);
            DriveUiRender(*m_Ui, *m_Rg);
        }
    }

    Dropdown* Pictures = nullptr;
    Dropdown* Plain = nullptr;

  private:
    // First member: destroyed last, after this fixture's UIManager released its buffers.
    SharedDeviceReleaseRetirement m_ReleaseRetirement;
    Rendering::IDevice* m_Device = nullptr;
    std::unique_ptr<JobSystem::WorkStealingThreadPool> m_Pool;
    AssetManager m_Assets;
    std::unique_ptr<UiRgHarness> m_Rg;
    std::unique_ptr<UIManager> m_Ui;
};

#define PICTURE_DROPDOWN_FIXTURE(fx)                                                                                   \
    PictureDropdownFixture fx;                                                                                         \
    {                                                                                                                  \
        std::string skipReason;                                                                                        \
        std::string failure;                                                                                           \
        if (!fx.Init(&skipReason, &failure))                                                                           \
        {                                                                                                              \
            if (!skipReason.empty())                                                                                   \
                GTEST_SKIP() << skipReason;                                                                            \
            FAIL() << failure;                                                                                         \
        }                                                                                                              \
    }

} // namespace

TEST(ShippedDropdownPictureRowTests, APictureRowShowsThePictureWithTheTextBesideIt)
{
    PICTURE_DROPDOWN_FIXTURE(fixture);

    const auto& rows = fixture.Pictures->GetItemsContainer()->GetChildren();
    ASSERT_EQ(rows.size(), 2u);
    const char* pictures[] = {"probe-picture-vivid", "probe-picture-physical"};
    const char* names[] = {"Vivid", "Physical"};
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const UIElement& row = *rows[i];
        EXPECT_TRUE(row.HasClass("dropdown-item-picture")) << "row " << i;
        const UIElement* picture = ChildWithClass(row, "dropdown-item-picture-image");
        const UIElement* column = ChildWithClass(row, "dropdown-item-text");
        ASSERT_NE(picture, nullptr) << "row " << i;
        ASSERT_NE(column, nullptr) << "row " << i;
        EXPECT_TRUE(picture->HasClass(pictures[i])) << "row " << i;
        EXPECT_FALSE(picture->GetResolvedStyle().Visual.BackgroundImage.HasTint) << "row " << i << " draws untinted";
        auto* name = dynamic_cast<const Label*>(ChildWithClass(*column, "dropdown-item-label"));
        ASSERT_NE(name, nullptr) << "row " << i;
        EXPECT_EQ(name->GetText(), names[i]);

        EXPECT_NEAR(picture->GetLayoutWidth(), kPictureWidth, kTolerance) << "row " << i;
        EXPECT_NEAR(picture->GetLayoutHeight(), kPictureHeight, kTolerance) << "row " << i;
        // The picture sets the row's height; the text does not stretch it.
        EXPECT_NEAR(row.GetLayoutHeight(), kPictureHeight + 2.0f * kPictureRowPadding, kTolerance) << "row " << i;
        EXPECT_NEAR(picture->GetLayoutY(), row.GetLayoutY() + kPictureRowPadding, kTolerance) << "row " << i;

        // The text sits beside the picture, inside the row, centered on it.
        EXPECT_NEAR(column->GetLayoutX(), picture->GetLayoutX() + kPictureWidth + kPictureGap, kTolerance) << "row " << i;
        EXPECT_LE(column->GetLayoutX() + column->GetLayoutWidth(), row.GetLayoutX() + row.GetLayoutWidth() + kTolerance);
        const float columnMiddle = column->GetLayoutY() + 0.5f * column->GetLayoutHeight();
        const float pictureMiddle = picture->GetLayoutY() + 0.5f * kPictureHeight;
        EXPECT_NEAR(columnMiddle, pictureMiddle, 1.0f) << "row " << i;
    }

    // The name above the line that says what the option does, each on one line.
    const UIElement& column = *ChildWithClass(*rows[0], "dropdown-item-text");
    const UIElement* name = ChildWithClass(column, "dropdown-item-label");
    auto* description = dynamic_cast<const Label*>(ChildWithClass(column, "dropdown-item-description"));
    ASSERT_NE(name, nullptr);
    ASSERT_NE(description, nullptr);
    EXPECT_EQ(description->GetText(), "Bright stars glow, with four thin spikes.");
    EXPECT_GE(description->GetLayoutY(), name->GetLayoutY() + name->GetLayoutHeight() - kTolerance);
    EXPECT_LT(description->GetLayoutHeight(), 1.5f * name->GetLayoutHeight()) << "the description wrapped";
    EXPECT_GE(description->GetResolvedStyle().Visual.FontSize, 12.0f);
    EXPECT_EQ(ChildWithClass(*ChildWithClass(*rows[1], "dropdown-item-text"), "dropdown-item-description"), nullptr)
        << "an option without a description draws no empty line";
}

TEST(ShippedDropdownPictureRowTests, TheOptionColorLandsOnTheName)
{
    PICTURE_DROPDOWN_FIXTURE(fixture);
    fixture.Pictures->SetOptions({{"a", "Selected", "#ff0000", {}, "probe-picture-vivid"},
                                  {"b", "Resting", "#00ff00", {}, "probe-picture-physical", "One line."}});
    fixture.Pictures->CloseMenuUi();
    fixture.Pictures->OpenMenuUi();
    fixture.Settle();

    const auto& rows = fixture.Pictures->GetItemsContainer()->GetChildren();
    ASSERT_EQ(rows.size(), 2u);
    const UIElement* column = ChildWithClass(*rows[1], "dropdown-item-text");
    ASSERT_NE(column, nullptr);
    auto* restingName = dynamic_cast<Label*>(ChildWithClass(*column, "dropdown-item-label"));
    ASSERT_NE(restingName, nullptr);
    EXPECT_EQ(restingName->Overrides().Get(Style::Color), 0xff00ff00u);
    EXPECT_FALSE(rows[1]->Overrides().Get(Style::Color).has_value()) << "the color belongs on the name, not the row";
}

TEST(ShippedDropdownPictureRowTests, TheHeaderPictureKeepsTheFieldHeight)
{
    PICTURE_DROPDOWN_FIXTURE(fixture);

    const UIElement* header = fixture.Pictures->GetHeaderContainer();
    const UIElement* picture = ChildWithClass(*header, "dropdown-header-picture");
    ASSERT_NE(picture, nullptr);
    EXPECT_NEAR(picture->GetLayoutWidth(), kHeaderPictureWidth, kTolerance);
    EXPECT_NEAR(picture->GetLayoutHeight(), kHeaderPictureHeight, kTolerance);
    EXPECT_FALSE(picture->GetResolvedStyle().Visual.BackgroundImage.HasTint) << "the header picture draws untinted";
    // A picture list sits in an inspector row beside text lists: same height.
    EXPECT_NEAR(header->GetLayoutHeight(), fixture.Plain->GetHeaderContainer()->GetLayoutHeight(), kTolerance);
    // The label follows the picture.
    EXPECT_GE(fixture.Pictures->GetHeaderLabel()->GetLayoutX(), picture->GetLayoutX() + kHeaderPictureWidth);
}

TEST(ShippedDropdownPictureRowTests, AListWithoutPicturesKeepsItsTextRows)
{
    PICTURE_DROPDOWN_FIXTURE(fixture);

    const auto& rows = fixture.Plain->GetItemsContainer()->GetChildren();
    ASSERT_EQ(rows.size(), 2u);
    for (const auto& row : rows)
    {
        EXPECT_FALSE(row->HasClass("dropdown-item-picture"));
        EXPECT_TRUE(row->GetChildren().empty()) << "a text row is its own text";
        // One 13 px line and the row's 2 px padding: far below a picture row.
        EXPECT_LT(row->GetLayoutHeight(), kPictureHeight);
        EXPECT_GT(row->GetLayoutHeight(), 12.0f);
    }
    EXPECT_EQ(ChildWithClass(*fixture.Plain->GetHeaderContainer(), "dropdown-header-picture"), nullptr);
}
