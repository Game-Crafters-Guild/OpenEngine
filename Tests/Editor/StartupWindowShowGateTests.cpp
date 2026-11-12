// The hidden main window shows after the first present whose Scene View shows an image.

#include "Startup/StartupWindowShowGate.h"

#include <gtest/gtest.h>

namespace
{
using GameEngine::Editor::Startup::StartupWindowShowGate;

TEST(StartupWindowShowGateTests, TheWindowShowsAfterTheFirstPresentWithASceneImage)
{
    StartupWindowShowGate gate;
    EXPECT_TRUE(gate.OnPresent(true)) << "a present with the scene image already shows the settled scene";
}

TEST(StartupWindowShowGateTests, APresentWithoutASceneImageKeepsTheWindowHidden)
{
    StartupWindowShowGate gate;
    EXPECT_FALSE(gate.OnPresent(false)) << "the empty placeholder viewport is never shown";
    EXPECT_TRUE(gate.OnPresent(true));
}

TEST(StartupWindowShowGateTests, ALayoutWithoutASceneImageShowsAfterTheFallbackPresents)
{
    StartupWindowShowGate gate;
    for (uint32_t present = 1; present < StartupWindowShowGate::kFallbackPresents; ++present)
        EXPECT_FALSE(gate.OnPresent(false)) << "present " << present;
    EXPECT_TRUE(gate.OnPresent(false));
}
} // namespace
