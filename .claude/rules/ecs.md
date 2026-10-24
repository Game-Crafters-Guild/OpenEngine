---
paths:
  - "Engine/Modules/ECS/**"
  - "Engine/ECSModules/**"
  - "Engine/Modules/*ECS/**"
  - "**/Systems/**"
---

# ECS

Archetype-based and cache-coherent: chunk-based SoA storage, generational ids, parallel iteration, deferred commands. Key API:

```cpp
world.Create<Position, Velocity>(pos, vel);
// Queries declare access intent: Read<T> parameters arrive as const T&;
// an unqualified generic (auto&) parameter is a compile error.
world.Query<ECS::Read<Position>, ECS::Write<Velocity>>()
    .Each([](const Position& pos, Velocity& vel) { ... });
auto q = world.Query<ECS::Read<Transform>, ECS::Write<WorldTransform>>();
q.Changed<Transform>(m_Gate);  // ChangeGate member: skip chunks whose Transform was not written since last run.
q.Each([](const Transform& t, WorldTransform& wt) { ... });
entity.Set<T>(value); entity.Get<T>() /* const read */; entity.GetForWrite<T>(); entity.Has<T>();
```

- Change tracking is per chunk and column: writes stamp versions, reads never do. Use `GetComponentForWrite` and `GetForWrite` only when actually mutating.
- Gate on a column you read, never one you write: stamp-at-visit marks the chunks you touch, so `Changed<T>` over your own `Write<T>` re-triggers itself forever.
- Count entities through an archetype query, never by scanning. Cache debug data on a version or dirty flag so idle paths converge; no per-frame scans.
- `EntityHandle`, never a raw integer, for identity across systems; a feature flag does not go on a shared component such as `MeshRenderer`: derive it from the material or add a component.
