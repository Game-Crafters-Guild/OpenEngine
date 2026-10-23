#pragma once

// ShaderDeformationEnvelope: the all-times deformation envelope a vertex
// modifier declares with
//
//   // @deformationEnvelope <meters> [scaledBy=<property>]
//
// <meters> is the largest distance the modifier can move a vertex, over every
// clock value and every instance payload, in the mesh's local space before the
// instance transform. scaledBy names a material property the displacement is
// proportional to: a scalar scales the envelope by its absolute value, a vector
// by the length of its first three components. The GPU culling kernels widen a
// deforming instance's sphere to R + envelope x largest axis scale.
//
// A declaration is a promise culling trusts, so a broken one is refused rather
// than guessed at: the refusal states the fix and the margin is zero, so the
// material culls on its mesh bounds alone. A refusal never rejects the shader.

#include <cstdint>
#include <string>

namespace GameEngine::Rendering
{

struct DeformationEnvelopeDeclaration
{
    // The vertex modifier the declaration was read from; empty when the program
    // composes no vertex modifier.
    std::string ModifierFile;
    // The modifier carries an @deformationEnvelope line, usable or not.
    bool Declared = false;
    float Meters = 0.0f;
    std::string ScaledBy; // empty: the envelope is not scaled
    // Why the declaration cannot be used, as the message that states the fix.
    // Empty when the declaration is usable or absent.
    std::string Refusal;
    uint32_t SourceLine = 0;

    bool Usable() const { return Declared && Refusal.empty(); }
    bool operator==(const DeformationEnvelopeDeclaration&) const = default;
};

} // namespace GameEngine::Rendering
