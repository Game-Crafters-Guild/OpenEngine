#include "Platform/SpeechInput.h"

#include <gtest/gtest.h>

using GameEngine::Platform::SpeechInput;

// Speech input is the Speech framework's, so only macOS offers it; a panel hides its microphone
// wherever this is false.
TEST(SpeechInput, IsAvailableOnMacOSOnly)
{
#if defined(__APPLE__)
    EXPECT_TRUE(SpeechInput::IsAvailable());
#else
    EXPECT_FALSE(SpeechInput::IsAvailable());
#endif
}

// A session that never started is not running, and Finish and Cancel on it do nothing (they never
// ask for permission and never call back).
TEST(SpeechInput, AnUnstartedSessionIsIdleAndStopsSafely)
{
    SpeechInput input;
    EXPECT_FALSE(input.IsRunning());
    input.Finish();
    input.Cancel();
    EXPECT_FALSE(input.IsRunning());
}
