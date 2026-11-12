#pragma once

#include "Types/Types.h"

#include <filesystem>
#include <functional>

namespace GameEngine
{

class SceneLoadInstantiated;
struct SceneLoadRequest;

/// The request for the Player's load of the game's startup scene. The load pauses world drawing until
/// the scene is ready to show, so the world first draws when the startup scene is ready.
SceneLoadRequest MakeStartupSceneLoadRequest(std::filesystem::path scene,
                                             std::function<void(SceneLoadInstantiated&)> onInstantiated);

/// Whether the Player's startup scene is ready for its startup line and its --screenshot
/// capture: the scene loader revealed it and its spline recipes are settled. The wait has no
/// clock: each recipe's own hold budget bounds it.
class PlayerStartupReadiness
{
public:
    /// Starts the wait for the startup scene that just opened.
    void Begin();
    /// Advances the wait by one frame. Returns true on the frame the scene becomes ready.
    bool Update(bool revealed, bool recipesSettled, float64 deltaSeconds);

    [[nodiscard]] bool IsPending() const;
    [[nodiscard]] uint32 Frames() const;
    [[nodiscard]] float64 Seconds() const;

private:
    bool m_Pending = false;
    uint32 m_Frames = 0;
    float64 m_Seconds = 0.0;
};

} // namespace GameEngine
