// Log channels: registration by name, the level a channel passes, and the source every GE_LOG record carries.
// The channel table lives for the whole process, so each test registers names no other test uses.

#include "LogChannelTestLog.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{

using namespace Logger;
using namespace LoggerTests;

class LogChannelTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Config config;
        config.GlobalMinLevel = LogLevel::Trace;
        Log::Initialize(config);
        Log::ClearSinks();
        auto sink = std::make_unique<CallbackSink>();
        sink->RegisterCallback([records = m_Records](const LogMessage& message) { records->push_back(message); });
        Log::AddSink(std::move(sink));
    }

    void TearDown() override { Log::Shutdown(); }

    std::shared_ptr<std::vector<LogMessage>> m_Records = std::make_shared<std::vector<LogMessage>>();
};

TEST_F(LogChannelTest, RecordBelowTheChannelLevelIsDroppedAndOneAboveCarriesItsSource)
{
    GE_LOG(QuietTestLog, Info, "dropped {}", 1);
    const int line = __LINE__ + 1;
    GE_LOG(QuietTestLog, Warning, "lost {} frames", 3);
    Log::Flush();

    ASSERT_EQ(m_Records->size(), 1u);
    const LogMessage& record = m_Records->front();
    EXPECT_EQ(record.Message, "lost 3 frames");
    EXPECT_EQ(record.Level, LogLevel::Warning);
    EXPECT_EQ(record.Channel, QuietTestLog().Index);
    EXPECT_EQ(Log::GetChannelName(record.Channel), "Tests.Quiet");
    EXPECT_EQ(record.SourceFile, __FILE__);
    EXPECT_EQ(record.SourceLine, line);
    EXPECT_NE(record.Function.find("LogChannelTest_RecordBelowTheChannelLevelIsDroppedAndOneAboveCarriesItsSource"),
              std::string::npos)
        << record.Function;
    EXPECT_NE(record.Function.find("TestBody"), std::string::npos) << record.Function;
}

TEST_F(LogChannelTest, TwoRegistrationsOfOneNameShareAnIndex)
{
    const LogChannel first = RegisterChannel("Tests.Shared", LogLevel::Info);
    const LogChannel second = RegisterChannel("Tests.Shared", LogLevel::Error);
    const LogChannel other = RegisterChannel("Tests.Other", LogLevel::Info);

    EXPECT_EQ(first.Index, second.Index);
    EXPECT_EQ(second.DefaultLevel, LogLevel::Info) << "the first registration's default level stays";
    EXPECT_NE(other.Index, first.Index);
    EXPECT_NE(first.Index, CoreLog().Index);
    EXPECT_EQ(Log::GetChannelName(CoreLog().Index), "Core");
}

TEST_F(LogChannelTest, HeaderChannelIsOneChannelInEveryTranslationUnit)
{
    EXPECT_EQ(&QuietTestLogInSecondUnit(), &QuietTestLog());

    Log::SetChannelLevel(QuietTestLog().Index, LogLevel::Debug);
    LogDebugInSecondUnit(5);
    Log::SetChannelLevel(QuietTestLog().Index, std::nullopt);
    Log::Flush();

    ASSERT_EQ(m_Records->size(), 1u) << "the level set through the index reaches the second unit";
    EXPECT_EQ(m_Records->front().Message, "second unit 5");
    EXPECT_EQ(m_Records->front().Channel, QuietTestLog().Index);
}

TEST_F(LogChannelTest, ChannelSettingWinsUntilClearedThenTheGlobalLevelApplies)
{
    const LogChannel channel = RegisterChannel("Tests.Setting", LogLevel::Info);
    EXPECT_FALSE(Enabled(channel, LogLevel::Debug)) << "the default level is a floor under the global Trace";

    Log::SetChannelLevel(channel.Index, LogLevel::Debug);
    EXPECT_TRUE(Enabled(channel, LogLevel::Debug));
    EXPECT_EQ(Log::GetChannelLevelWithoutSetting(channel.Index), LogLevel::Info);

    Log::SetLogLevel(LogLevel::Error);
    EXPECT_TRUE(Enabled(channel, LogLevel::Debug)) << "a channel's own setting wins over the global level";
    EXPECT_FALSE(Enabled(CoreLog(), LogLevel::Warning));

    Log::SetChannelLevel(channel.Index, std::nullopt);
    EXPECT_FALSE(Enabled(channel, LogLevel::Warning));
    EXPECT_TRUE(Enabled(channel, LogLevel::Error));
    EXPECT_EQ(Log::GetChannelSetting(channel.Index), std::nullopt);
}

} // namespace
