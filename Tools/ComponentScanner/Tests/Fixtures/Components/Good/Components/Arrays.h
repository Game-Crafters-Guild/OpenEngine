#pragma once
// The component mirror fixture: a fixed array of each element the array rule allows, with every
// form of extent the rule reads.

#include "Components/Kinds.h"

#include <cstdint>

namespace GameEngine::Components
{

inline constexpr std::uint32_t kFixtureLaneCount = 3;
inline constexpr std::uint32_t kFixtureSlotCount = kFixtureLaneCount + 2u;

/// One rule of a fixture surface: an element struct, held in an array.
struct FixtureRule
{
    float Height = 0.0f;
    std::uint8_t Layer = 0;
};

/// A probe that holds arrays.
struct FixtureArrays
{
    static constexpr std::uint32_t kPoolCapacity = 2;

    float Weights[4] = {};
    std::int32_t Lanes[kFixtureLaneCount] = {};
    std::uint8_t Slots[kFixtureSlotCount] = {};
    bool Flags[kFixtureSlotCount - 1] = {};
    Mathematics::Vector3 Points[2];
    ColorLinear Colors[2];
    FixtureModelRef Models[kPoolCapacity];
    FixtureRule Rules[kPoolCapacity + 1];
    // @ge-hidden
    std::uint32_t Scratch[2] = {};
    // @ge-readonly
    float History[2] = {};
};

} // namespace GameEngine::Components
