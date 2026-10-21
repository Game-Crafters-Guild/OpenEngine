#pragma once
namespace Game::Combat {
struct Weapon : public ECS::ComponentBase { float Damage = 1.0f; int Ammo; };
struct Helper { int X; };
struct Mover : ECS::EntitySystem<Mover> { void OnUpdate(float dt); };
}
struct TickSystem : public ECS::SystemBase { int Count; };
