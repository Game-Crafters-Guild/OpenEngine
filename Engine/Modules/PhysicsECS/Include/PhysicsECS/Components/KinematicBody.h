#pragma once

namespace GameEngine::Components
{
/// Derived state: present on an entity whose PhysicsBody has a kinematic backend body. Never authored
/// and never saved.
///
/// Writer: PhysicsInitSystem, from the body's motion type. It adds the tag when it builds a kinematic
/// body and removes it when it builds the body with another motion type.
///
/// Readers: the physics step moves only these bodies, and the physics writeback skips them, so a
/// kinematic body's chunk is stamped only by whoever moves its Transform. A game system that wants the
/// kinematic bodies of a world queries `With<KinematicBody>`.
struct KinematicBody {};
} // namespace GameEngine::Components
