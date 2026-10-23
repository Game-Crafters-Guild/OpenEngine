#pragma once
// A module enum a fixture component uses: the mirror's enum table reads the Include tree of every
// module a component header includes.

#include <cstdint>

namespace GameEngine::FixtureShapes
{

/// The outline of a probe.
enum class Shape : std::uint8_t
{
    Box,
    Sphere,
    Capsule,
};

} // namespace GameEngine::FixtureShapes
