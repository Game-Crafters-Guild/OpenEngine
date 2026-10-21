#pragma once
#include <GameSDK/Component.h>

namespace Game {
enum class Team : uint8 { Red = 0, Blue = 1 };
// @ge-no-add
struct Health : ECS::ComponentBase
{
    float Current = 100.0f;  // @ge-range 0 100
    float Max = 100.0f;      // @ge-tooltip "Upper \"bound\"."
    uint32 LiveHandle = 0;   // [DoNotSerialize]
    Team Side = Team::Red;
    int32 Lives[3] = {};
};
}
