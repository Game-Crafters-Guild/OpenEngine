// The gamepad fly camera: the first pad's sticks move and turn the camera entity.
#include "Components/Rendering/Camera.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECSModules/Rendering/Systems/GamepadCameraControllerSystem.h"
#include "Input/GamepadCodes.h"
#include "Input/InputScriptApi.h"
#include "Input/InputSystem.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <gtest/gtest.h>

using namespace GameEngine;
using GameEngine::Engine::Renderer::GamepadCameraControllerSystem;
using GameEngine::Input::GamepadAxis;
using GameEngine::Mathematics::Quaternion;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr int kFirstPad = 0;
constexpr float kFrameSeconds = 0.1f;

// A camera at the origin that looks along +Z, and the first pad connected with one axis held.
class GamepadCameraControllerTests : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Camera = m_World.CreateEntity();
        const Components::Transform transform =
            Components::Transform::FromTRS(Mathematics::WorldPosition{}, Quaternion{}, Vector3{1.0f, 1.0f, 1.0f});
        m_World.AddComponentImmediate(m_Camera, transform);
        m_World.AddComponentImmediate(m_Camera, Components::WorldTransform{});
        m_World.AddComponentImmediate(m_Camera, Components::Camera{});
    }

    void HoldAxisForOneFrame(GamepadAxis axis, float value)
    {
        float axes[Input::kGamepadAxisCount] = {};
        axes[static_cast<int>(axis)] = value;
        m_Input.OnGamepadState(kFirstPad, axes, Input::kGamepadAxisCount, /*connected=*/true);
        m_System.Update(m_World, kFrameSeconds);
    }

    const Components::Transform& CameraTransform() { return *m_World.GetComponent<Components::Transform>(m_Camera); }

    Input::InputSystem m_Input;
    GamepadCameraControllerSystem m_System{&m_Input};
    ECS::World m_World;
    ECS::EntityHandle m_Camera;
};

TEST_F(GamepadCameraControllerTests, LeftStickPushedUpMovesTheCameraForward)
{
    HoldAxisForOneFrame(GamepadAxis::LeftY, 1.0f);

    const Mathematics::WorldPosition position = CameraTransform().GetPosition();
    EXPECT_GT(position.Z, 0.0);
    EXPECT_DOUBLE_EQ(position.X, 0.0);
    EXPECT_DOUBLE_EQ(position.Y, 0.0);
}

// The fly camera's look is direct: a right stick pushed up tilts the view up.
TEST_F(GamepadCameraControllerTests, RightStickPushedUpTiltsTheViewUp)
{
    HoldAxisForOneFrame(GamepadAxis::RightY, 1.0f);

    const Vector3 forward = CameraTransform().GetRotation().Rotate(Vector3{0.0f, 0.0f, 1.0f});
    EXPECT_GT(forward.y, 0.0f);
    EXPECT_NEAR(forward.x, 0.0f, 1e-6f);
}

} // namespace
