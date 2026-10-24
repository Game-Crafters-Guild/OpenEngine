#pragma once
// The component mirror fixture: every field kind a C# mirror holds, once. ComponentScannerTests
// compares the scanner's mirrors of this tree with Expected/, ScriptBindingTests compiles the
// layout pins against these headers, and BindingTests compiles the C# mirrors.

#include "AssetCore/GUID.h"
#include "Components/AssetRef.h"
#include "ECS/ECS.h"
#include "FixtureShapes/Shape.h"
#include "Mathematics/Curve.h"
#include "Mathematics/Geometry.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Rendering/RenderLayerMask.h"
#include "Types/Color.h"
#include "Types/StringId.h"

#include <cstdint>

namespace GameEngine::Components
{

using FixtureModelRef = AssetRef<AssetType::Model>;

/// How brightly a probe glows.
enum class FixtureGlow : std::uint8_t
{
    Off,
    Soft,
    Bright,
};

/// How fast a probe pulses. The values have gaps.
enum class FixturePulse : std::int16_t
{
    Slow = -2,
    Even = 0,
    Fast = 5,
};

/// A probe that holds one field of every kind a mirror holds.
struct FixtureKinds
{
    bool Enabled = true;
    std::int8_t Tilt = -1;
    std::uint16_t Steps = 2;
    std::int32_t Score = 3;
    std::uint64_t Seed = 4;
    float speed = 1.5f;
    double Time = 0.25;
    Mathematics::Vector2 Offset;
    Mathematics::Vector3 Position;
    Mathematics::Vector4 Plane;
    Mathematics::Quaternion Rotation;
    Mathematics::Matrix4x4 Pose;
    ColorLinear Tint;
    ECS::EntityHandle Target;
    StringId Tag;
    Rendering::RenderLayerMask Layers;
    FixtureModelRef Model;
    GUID Source;
    Math::Curve Falloff;
    Mathematics::BoundingBox Bounds;
    FixtureGlow Glow = FixtureGlow::Soft;
    FixturePulse Pulse = FixturePulse::Even;
    FixtureShapes::Shape Outline = FixtureShapes::Shape::Sphere;
    char Label[16] = {};
    // @ge-hidden
    std::uint64_t GpuHandle = 0;
    // @ge-readonly
    float Weight = 0.0f;
    // @ge-readonly
    FixtureGlow LastGlow = FixtureGlow::Off;
    // @ge-readonly
    char Owner[8] = {};
};

} // namespace GameEngine::Components
