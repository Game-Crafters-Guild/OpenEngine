// ProgressBar (UI/Controls/ProgressBar.h): the fill's length follows the value's share of the
// range, the value is clamped, an indeterminate bar shows a centered segment, the fill grows toward the
// chosen edge and eases through the transition support. The look is the module's default sheet.

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "UI/Controls/ProgressBar.h"
#include "UI/Parsers/XMLParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/UIElement.h"
#include "UI/UIManager.h"
#include "UIRgTestHarness.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::UIParsing;

namespace
{
constexpr float kTolerance = 0.5f;

class ProgressBarTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Device = SharedHeadlessDevice();
        if (!m_Device)
            GTEST_SKIP() << "Device init failed";
        UIRegistration::RegisterBuiltInControls();
    }

    // Lays out `xml` under the default sheet and, when given, one host sheet.
    void Build(const std::string& xml, const std::string& css = {})
    {
        std::unique_ptr<UIElement> root;
        ASSERT_TRUE(XMLParser::ParseLayoutFromString(xml, root));
        m_Ui = std::make_unique<UIManager>(m_Device);
        m_Ui->SetRoot(std::move(root));
        if (!css.empty())
        {
            const std::filesystem::path path = std::filesystem::temp_directory_path() / "progress_bar_test.css";
            {
                std::ofstream file(path);
                file << css;
            }
            ASSERT_TRUE(m_Ui->AttachStyleFromFile(path.string()));
        }
        Tick(0.0f);
    }

    void Tick(float seconds) { m_Ui->Update(seconds, false); }

    ProgressBar* Bar() { return dynamic_cast<ProgressBar*>(m_Ui->GetRootElement()->FindById("bar"_sid)); }

    static UIElement* Fill(ProgressBar& bar) { return bar.GetChildren().front().get(); }

    SharedDeviceReleaseRetirement m_Retirement;
    IDevice* m_Device = nullptr;
    std::unique_ptr<UIManager> m_Ui;
};

constexpr const char* kHorizontalBar = R"(<uielement>
    <progress id='bar' value='0.25' style='width: 200px; height: 8px;' />
</uielement>)";
} // namespace

TEST_F(ProgressBarTest, AQuarterValueFillsAQuarterOfTheTrack)
{
    Build(kHorizontalBar);
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    UIElement* fill = Fill(*bar);
    EXPECT_NEAR(bar->GetLayoutWidth(), 200.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutWidth(), 50.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutHeight(), 8.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutX(), bar->GetLayoutX(), kTolerance);
}

TEST_F(ProgressBarTest, AValueOutsideTheRangeIsClamped)
{
    Build(kHorizontalBar);
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);

    bar->SetValue(2.0f);
    Tick(0.0f);
    EXPECT_FLOAT_EQ(bar->GetValue(), 1.0f);
    EXPECT_NEAR(Fill(*bar)->GetLayoutWidth(), 200.0f, kTolerance);

    bar->SetValue(-1.0f);
    Tick(0.0f);
    EXPECT_FLOAT_EQ(bar->GetValue(), 0.0f);
    EXPECT_NEAR(Fill(*bar)->GetLayoutWidth(), 0.0f, kTolerance);
}

TEST_F(ProgressBarTest, TheValueIsAShareOfMaxWhateverTheAttributeOrder)
{
    Build(R"(<uielement>
        <progress id='bar' value='3' max='4' style='width: 200px; height: 8px;' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    EXPECT_FLOAT_EQ(bar->GetValue(), 3.0f);
    EXPECT_NEAR(Fill(*bar)->GetLayoutWidth(), 150.0f, kTolerance);

    bar->SetMax(0.0f);
    EXPECT_FLOAT_EQ(bar->GetMax(), 4.0f) << "a max of 0 is refused";
}

TEST_F(ProgressBarTest, TheLayoutAttributesGiveOneValueInEitherOrder)
{
    const UIRegistration::ElementFactoryRegistry& registry = UIRegistration::ElementFactoryRegistry::Instance();
    const std::unordered_map<std::string, std::string> value{{"value", "3"}};
    const std::unordered_map<std::string, std::string> max{{"max", "4"}};

    ProgressBar valueFirst;
    registry.ApplyAttributes(valueFirst, value, {}, {}, 0);
    registry.ApplyAttributes(valueFirst, max, {}, {}, 0);
    ProgressBar maxFirst;
    registry.ApplyAttributes(maxFirst, max, {}, {}, 0);
    registry.ApplyAttributes(maxFirst, value, {}, {}, 0);

    EXPECT_FLOAT_EQ(valueFirst.GetValue(), 3.0f) << "the value set before max is clamped again to the new max";
    EXPECT_FLOAT_EQ(maxFirst.GetValue(), 3.0f);
    EXPECT_FLOAT_EQ(valueFirst.GetMax(), 4.0f);
    EXPECT_FLOAT_EQ(maxFirst.GetMax(), 4.0f);
}

TEST_F(ProgressBarTest, AMaxBelowTheValueClampsTheValueDown)
{
    Build(R"(<uielement>
        <progress id='bar' max='4' style='width: 200px; height: 8px;' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    bar->SetValue(3.0f);
    bar->SetMax(2.0f);
    Tick(0.0f);
    EXPECT_FLOAT_EQ(bar->GetValue(), 2.0f);
    EXPECT_NEAR(Fill(*bar)->GetLayoutWidth(), 200.0f, kTolerance);
}

TEST_F(ProgressBarTest, ANonFiniteMaxIsRefusedAndANaNValueIsZero)
{
    Build(kHorizontalBar);
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    bar->SetMax(std::numeric_limits<float>::infinity());
    EXPECT_FLOAT_EQ(bar->GetMax(), 1.0f) << "an infinite max is refused";
    bar->SetMax(std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(bar->GetMax(), 1.0f) << "a NaN max is refused";

    bar->SetValue(std::numeric_limits<float>::quiet_NaN());
    Tick(0.0f);
    EXPECT_FLOAT_EQ(bar->GetValue(), 0.0f);
    EXPECT_NEAR(Fill(*bar)->GetLayoutWidth(), 0.0f, kTolerance);
}

TEST_F(ProgressBarTest, AnUnknownDirectionFillsToTheRightAndLogsTheValidWords)
{
    Logger::Log::Initialize({});
    auto sink = std::make_unique<Logger::CallbackSink>();
    Logger::CallbackSink* sinkPointer = sink.get();
    auto warnings = std::make_shared<std::atomic<int>>(0);
    const std::uint64_t callbackId = sinkPointer->RegisterCallback(
        [warnings](const Logger::LogMessage& message)
        {
            if (message.Message.find("'sideways'") != std::string::npos &&
                message.Message.find("right, left, up or down") != std::string::npos)
                warnings->fetch_add(1);
        });
    Logger::Log::AddSink(std::move(sink));

    Build(R"(<uielement>
        <progress id='bar' value='0.25' direction='sideways' style='width: 200px; height: 8px;' />
    </uielement>)");
    Logger::Log::Flush();
    sinkPointer->UnregisterCallback(callbackId);

    EXPECT_EQ(warnings->load(), 1);
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    EXPECT_EQ(bar->GetFillDirection(), ProgressBar::FillDirection::Right);
    EXPECT_NEAR(Fill(*bar)->GetLayoutX(), bar->GetLayoutX(), kTolerance);
}

TEST_F(ProgressBarTest, AnIndeterminateBarShowsACenteredSegmentAtFullOpacity)
{
    Build(R"(<uielement>
        <progress id='bar' value='0.25' indeterminate='true' style='width: 200px; height: 8px;' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    UIElement* fill = Fill(*bar);
    const float segment = 200.0f * ProgressBar::kIndeterminateShare;
    EXPECT_TRUE(bar->IsIndeterminate());
    EXPECT_TRUE(fill->HasClass("progress-bar-fill-indeterminate"));
    EXPECT_NEAR(fill->GetLayoutWidth(), segment, kTolerance);
    EXPECT_NEAR(fill->GetLayoutX() - bar->GetLayoutX(), (200.0f - segment) * 0.5f, kTolerance);
    EXPECT_FLOAT_EQ(fill->GetResolvedStyle().Visual.LocalOpacity, 1.0f) << "a dim fill reads as disabled";

    bar->SetIndeterminate(false);
    Tick(0.0f);
    EXPECT_FALSE(fill->HasClass("progress-bar-fill-indeterminate"));
    EXPECT_NEAR(fill->GetLayoutWidth(), 50.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutX(), bar->GetLayoutX(), kTolerance) << "a value fills from the start edge again";
}

TEST_F(ProgressBarTest, AnIndeterminateSegmentIsCenteredInEveryDirection)
{
    const float segment = 200.0f * ProgressBar::kIndeterminateShare;
    const float offset = (200.0f - segment) * 0.5f;
    Build(R"(<uielement>
        <progress id='bar' indeterminate='true' direction='up' style='width: 10px; height: 200px;' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    UIElement* fill = Fill(*bar);
    EXPECT_NEAR(fill->GetLayoutHeight(), segment, kTolerance);
    EXPECT_NEAR(fill->GetLayoutY() - bar->GetLayoutY(), offset, kTolerance);

    bar->SetFillDirection(ProgressBar::FillDirection::Down);
    Tick(0.0f);
    EXPECT_NEAR(fill->GetLayoutY() - bar->GetLayoutY(), offset, kTolerance);

    Build(R"(<uielement>
        <progress id='bar' indeterminate='true' direction='left' style='width: 200px; height: 8px;' />
    </uielement>)");
    bar = Bar();
    ASSERT_NE(bar, nullptr);
    fill = Fill(*bar);
    EXPECT_NEAR(fill->GetLayoutWidth(), segment, kTolerance);
    EXPECT_NEAR(fill->GetLayoutX() - bar->GetLayoutX(), offset, kTolerance);
    EXPECT_NEAR(fill->GetLayoutHeight(), 8.0f, kTolerance) << "the margins are on the main axis only";
}

TEST_F(ProgressBarTest, AnIndeterminateSegmentDiffersFromTheSameShareAsAValue)
{
    Build(kHorizontalBar);
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    UIElement* fill = Fill(*bar);
    bar->SetValue(ProgressBar::kIndeterminateShare);
    Tick(0.0f);
    const float valueX = fill->GetLayoutX();

    bar->SetIndeterminate(true);
    Tick(0.0f);
    EXPECT_GT(fill->GetLayoutX() - valueX, 1.0f) << "a segment at the start edge reads as a value";
}

TEST_F(ProgressBarTest, TheDefaultTrackAndFillAreTheSliderRailColors)
{
    Build(R"(<uielement>
        <progress id='bar' value='0.25' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    const uint32_t sliderTrack = bar->GetResolvedStyle().GetCustomColor(HashStringId("--slider-track")).value_or(0u);
    const uint32_t sliderFill = bar->GetResolvedStyle().GetCustomColor(HashStringId("--slider-track-fill")).value_or(0u);
    ASSERT_NE(sliderTrack, 0u);
    ASSERT_NE(sliderFill, 0u);
    EXPECT_EQ(bar->GetResolvedStyle().Visual.BackgroundColor, sliderTrack);
    EXPECT_EQ(Fill(*bar)->GetResolvedStyle().Visual.BackgroundColor, sliderFill);
}

TEST_F(ProgressBarTest, TheHiddenClassHidesTheBar)
{
    Build(R"(<uielement>
        <progress id='bar' class='hidden' value='0.25' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    EXPECT_NEAR(bar->GetLayoutWidth(), 0.0f, kTolerance);
    EXPECT_NEAR(bar->GetLayoutHeight(), 0.0f, kTolerance);
}

TEST_F(ProgressBarTest, AnUpBarFillsFromTheBottom)
{
    Build(R"(<uielement>
        <progress id='bar' value='0.25' direction='up' style='width: 10px; height: 200px;' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    UIElement* fill = Fill(*bar);
    EXPECT_NEAR(fill->GetLayoutHeight(), 50.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutWidth(), 10.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutY() - bar->GetLayoutY(), 150.0f, kTolerance);
}

TEST_F(ProgressBarTest, ADownBarFillsFromTheTop)
{
    Build(R"(<uielement>
        <progress id='bar' value='0.25' direction='down' style='width: 10px; height: 200px;' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    UIElement* fill = Fill(*bar);
    EXPECT_NEAR(fill->GetLayoutHeight(), 50.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutWidth(), 10.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutY(), bar->GetLayoutY(), kTolerance);
}

TEST_F(ProgressBarTest, ALeftBarFillsFromTheRightEdge)
{
    Build(R"(<uielement>
        <progress id='bar' value='0.25' direction='left' style='width: 200px; height: 8px;' />
    </uielement>)");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    UIElement* fill = Fill(*bar);
    EXPECT_NEAR(fill->GetLayoutWidth(), 50.0f, kTolerance);
    EXPECT_NEAR(fill->GetLayoutX() - bar->GetLayoutX(), 150.0f, kTolerance);
}

TEST_F(ProgressBarTest, AWidthTransitionEasesTheFillToANewValue)
{
    Build(kHorizontalBar, ".progress-bar-fill { transition: width 1s linear; }");
    ProgressBar* bar = Bar();
    ASSERT_NE(bar, nullptr);
    UIElement* fill = Fill(*bar);
    ASSERT_NEAR(fill->GetLayoutWidth(), 50.0f, kTolerance);

    bar->SetValueWithoutNotify(0.75f);
    Tick(0.0f);
    Tick(0.5f);
    const float halfway = fill->GetLayoutWidth();
    EXPECT_GT(halfway, 60.0f) << "the fill moves";
    EXPECT_LT(halfway, 140.0f) << "the fill eases, it does not jump";

    Tick(1.0f);
    EXPECT_NEAR(fill->GetLayoutWidth(), 150.0f, kTolerance);
}
