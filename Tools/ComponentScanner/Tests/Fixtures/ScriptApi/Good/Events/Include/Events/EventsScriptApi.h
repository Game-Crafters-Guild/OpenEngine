#pragma once

// An event pair. Never compiled: the scanner reads it.

namespace GameEngine::Events
{

class EventsSystem;

/// Two bodies touched.
// @ge-script
struct ContactEvent
{
    ECS::EntityHandle self;
    ECS::EntityHandle other;
    Mathematics::Vector3 point{0.0f, 0.0f, 0.0f};
    float impulse = 0.0f;
};

/// Calls the handler when a body of the entity starts to touch another.
// @ge-script-event PhysicsStep
// @ge-context
SubscriptionId SubscribeContactBegin(EventsSystem& events, ECS::EntityHandle scope, ScriptCallback<ContactEvent> callback);

/// Stops the calls of SubscribeContactBegin.
// @ge-script-event @ge-context
void UnsubscribeContactBegin(EventsSystem& events, SubscriptionId subscription);

} // namespace GameEngine::Events
