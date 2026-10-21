#pragma once

#include <cstdint>

namespace GameEngine
{

/// The id of an event subscription a script API hands out; 0 is none.
using SubscriptionId = std::uint64_t;

/**
 * @brief The handler a script hands an event of a script API (`SubscribeContactBegin`): two C function
 * pointers and a user word, so it crosses the C boundary as it is.
 *
 * `Invoke` runs once per event, on the event's delivery thread, with the payload and `User`. `Ended` runs
 * once, with `User`, when the subscription ends for any reason (an unsubscribe, the system shutting
 * down); after it neither pointer runs again, so the owner of `User` may release it. `Ended` may be null.
 */
template <class Payload>
struct ScriptCallback
{
    void (*Invoke)(const Payload* payload, void* user) = nullptr;
    void (*Ended)(void* user) = nullptr;
    void* User = nullptr;
};

} // namespace GameEngine
