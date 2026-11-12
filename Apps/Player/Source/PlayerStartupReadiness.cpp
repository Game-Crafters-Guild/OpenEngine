#include "PlayerStartupReadiness.h"

#include "Engine/Loading/SceneLoadRequest.h"

#include <utility>

namespace GameEngine
{

SceneLoadRequest MakeStartupSceneLoadRequest(std::filesystem::path scene,
                                             std::function<void(SceneLoadInstantiated&)> onInstantiated)
{
    return {.Scene = std::move(scene), .OnInstantiated = std::move(onInstantiated), .PauseWorldDrawing = true};
}

void PlayerStartupReadiness::Begin()
{
    m_Pending = true;
    m_Frames = 0;
    m_Seconds = 0.0;
}

bool PlayerStartupReadiness::Update(bool revealed, bool recipesSettled, float64 deltaSeconds)
{
    if (!m_Pending)
        return false;
    ++m_Frames;
    m_Seconds += deltaSeconds;
    if (!revealed || !recipesSettled)
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
