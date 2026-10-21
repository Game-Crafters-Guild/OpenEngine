#pragma once

// Required components: a component type declares the components an entity must
// also hold whenever it holds that type.
//
//   template <> struct GameEngine::ECS::Detail::RequiredComponents<MyComponent>
//   { using type = TypeList<Transform, Parent>; };
//
// Declare the specialization in the component's own header, after the types it
// names: every image that registers the component must see the same list.
//
// The registry records each type's direct requirements when the type registers
// (ComponentRegistry::RegisterComponent). The World resolves the closure at
// every structural add and every creation: an entity lands in the archetype
// that already holds every required column, in one placement, and each column
// it did not supply holds the required type's default-constructed value. A
// required component the entity already holds keeps its value.

namespace GameEngine::ECS::Detail
{

template <typename... Ts>
struct TypeList
{
};

template <typename T>
struct RequiredComponents
{
    using type = TypeList<>;
};

} // namespace GameEngine::ECS::Detail
