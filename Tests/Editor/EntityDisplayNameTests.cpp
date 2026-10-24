#include "Editor/Entities/EntityDisplayName.h"

#include <gtest/gtest.h>

namespace GameEngine::Editor
{

// The Add Component menu and the inspector's component headers name a type with no label by
// its registered name read as words: the namespace goes, and a dimension keeps its letter.
TEST(ComponentTypeNameAsWords, ReadsTheUnqualifiedTypeNameAsWords)
{
    EXPECT_EQ(ComponentTypeNameAsWords("GameEngine::Components::AnimatedSprite2D"), "Animated Sprite 2D");
    EXPECT_EQ(ComponentTypeNameAsWords("GameEngine::Components::ParticleEmitter3D"), "Particle Emitter 3D");
    EXPECT_EQ(ComponentTypeNameAsWords("GameEngine::Components::DDGIVolume"), "DDGI Volume");
    EXPECT_EQ(ComponentTypeNameAsWords("MyGame.Combat.HealthPool"), "Health Pool");
    EXPECT_EQ(ComponentTypeNameAsWords("Turret"), "Turret");
}

} // namespace GameEngine::Editor
