#include "AIECS/Components/AiAgent.h"

#include <gtest/gtest.h>

#include <cstddef>

using namespace GameEngine;

// The decision loop reads every due agent's row, so the row stays inside the AI design's 48 bytes:
// ticks, score and seed (16), program and cadence (4), the path (16), the depth and one flag byte (2),
// and the path's program (2): 40 bytes.
TEST(AiAgentLayoutTests, AgentFitsItsByteBudget)
{
    EXPECT_EQ(sizeof(Components::AiAgent), 40u);
    EXPECT_LE(sizeof(Components::AiAgent), 48u);
    EXPECT_EQ(alignof(Components::AiAgent), 4u);
    EXPECT_EQ(offsetof(Components::AiAgent, ActivePath), 20u);
    EXPECT_EQ(sizeof(Components::AiAgent::ActivePath), Components::AiAgent::kMaxPathDepth * sizeof(uint16));
}

TEST(AiAgentLayoutTests, NewAgentHasNoActiveState)
{
    const Components::AiAgent agent;
    EXPECT_EQ(agent.PathDepth, 0);
    EXPECT_FALSE(agent.Scheduled);
    for (uint16 state : agent.ActivePath)
        EXPECT_EQ(state, Components::AiAgent::kNoState);
}
