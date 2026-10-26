#include "PlayerStartupReadiness.h"

namespace GameEngine
{

void PlayerStartupReadiness::Begin(bool waitForSplineRecipes)
{
    m_Pending = true;
    m_WaitForSplineRecipes = waitForSplineRecipes;
    m_Frames = 0;
    m_Seconds = 0.0;
}

bool PlayerStartupReadiness::Update(bool batchComplete, bool recipesSettled, float64 deltaSeconds)
{
    if (!m_Pending)
        return false;
    ++m_Frames;
    m_Seconds += deltaSeconds;
    if (!batchComplete || (m_WaitForSplineRecipes && !recipesSettled))
        return false;
    m_Pending = false;
    return true;
}

bool PlayerStartupReadiness::IsPending() const
{
    return m_Pending;
}

uint32 PlayerStartupReadiness::Frames() const
{
    return m_Frames;
}

float64 PlayerStartupReadiness::Seconds() const
{
    return m_Seconds;
}

} // namespace GameEngine
