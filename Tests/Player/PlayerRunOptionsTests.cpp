// The Player's run options: --headless, its fixed step, --quit-after and the audio
// output (--silent-audio over GE_AUDIO_OUTPUT). The refusals
// matter most: a headless server that silently ignored a bad frame count or a flag it
// cannot honor would run forever or produce nothing, with nobody watching a window.

#include "PlayerRunOptions.h"

#include <gtest/gtest.h>

#include <optional>
#include <utility>
#include <vector>

using GameEngine::ParsePlayerRunOptions;
using GameEngine::PlayerRunOptions;

namespace
{
namespace Audio = GameEngine::Audio;

std::optional<PlayerRunOptions> Parse(std::vector<const char*> args, const char* audioOutputEnvironment = nullptr)
{
    args.insert(args.begin(), "Player.exe");
    return ParsePlayerRunOptions(static_cast<int>(args.size()), args.data(), audioOutputEnvironment);
}

Audio::AudioOutput OutputOf(std::vector<const char*> args, const char* audioOutputEnvironment)
{
    const std::optional<PlayerRunOptions> options = Parse(std::move(args), audioOutputEnvironment);
    EXPECT_TRUE(options);
    return options ? options->AudioOutput : Audio::AudioOutput::Device;
}
} // namespace

TEST(PlayerRunOptions, HeadlessStepsAtSixtyAndRunsUntilAskedToQuitByDefault)
{
    const std::optional<PlayerRunOptions> options = Parse({"--headless"});
    ASSERT_TRUE(options);
    EXPECT_TRUE(options->Headless);
    EXPECT_DOUBLE_EQ(options->HeadlessFps, 60.0);
    EXPECT_EQ(options->QuitAfterFrames, 0u);
}

TEST(PlayerRunOptions, HeadlessTakesTheFixedStepAndTheFrameCount)
{
    const std::optional<PlayerRunOptions> options = Parse({"--headless", "--fixed-fps", "30", "--quit-after", "600"});
    ASSERT_TRUE(options);
    EXPECT_DOUBLE_EQ(options->HeadlessFps, 30.0);
    EXPECT_EQ(options->QuitAfterFrames, 600u);
}

TEST(PlayerRunOptions, AQuitAfterThatIsNotAFrameCountIsRefusedInEveryRun)
{
    EXPECT_FALSE(Parse({"--quit-after", "abc"}));
    EXPECT_FALSE(Parse({"--quit-after", "-5"}));
    EXPECT_FALSE(Parse({"--headless", "--quit-after"}));
}

TEST(PlayerRunOptions, AHeadlessStepThatIsNotAPositiveRateIsRefused)
{
    EXPECT_FALSE(Parse({"--headless", "--fixed-fps", "0"}));
    EXPECT_FALSE(Parse({"--headless", "--fixed-fps", "fast"}));
    EXPECT_FALSE(Parse({"--headless", "--fixed-fps"}));
}

TEST(PlayerRunOptions, EveryFlagThatNeedsARenderDeviceIsRefusedUnderHeadless)
{
    for (const char* flag : {"--write-movie", "--screenshot", "--hdr", "--hdr-mode", "--hdr-bit-depth",
                             "--hdr-swapchain-bit-depth", "--allow-hdr-toggle"})
    {
        EXPECT_FALSE(Parse({"--headless", flag})) << flag;
        EXPECT_TRUE(Parse({flag})) << flag << " is accepted in a windowed run";
    }
}

TEST(PlayerRunOptions, SilentAudioComesFromTheFlagOrTheEnvironmentAndTheFlagWins)
{
    EXPECT_EQ(OutputOf({}, nullptr), Audio::AudioOutput::Device);
    EXPECT_EQ(OutputOf({}, ""), Audio::AudioOutput::Device);
    EXPECT_EQ(OutputOf({"--silent-audio"}, nullptr), Audio::AudioOutput::Silent);
    EXPECT_EQ(OutputOf({}, "silent"), Audio::AudioOutput::Silent);
    EXPECT_EQ(OutputOf({}, " Silent\r"), Audio::AudioOutput::Silent);
    EXPECT_EQ(OutputOf({"--silent-audio"}, "device"), Audio::AudioOutput::Silent);
    EXPECT_EQ(OutputOf({"--headless", "--silent-audio"}, nullptr), Audio::AudioOutput::Silent);
}

TEST(PlayerRunOptions, AnUnrecognizedAudioOutputIsIgnoredAndTheDevicePlays)
{
    EXPECT_EQ(OutputOf({}, "mute"), Audio::AudioOutput::Device);
}
