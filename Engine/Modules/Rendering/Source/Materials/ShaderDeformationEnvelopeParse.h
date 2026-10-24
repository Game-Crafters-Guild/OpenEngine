#pragma once

// The parse and the check behind ShaderPropertyTable::DeformationEnvelope
// (Rendering/Materials/ShaderDeformationEnvelope.h). Private to the module:
// callers read the declaration from the built table.

#include "Rendering/Materials/ShaderDeformationEnvelope.h"

namespace GameEngine::Rendering
{

struct ShaderPropertySource;
struct ShaderPropertyTable;

// Read the vertex modifier's @deformationEnvelope line. Grammar refusals land in
// Refusal; scaledBy is checked against the composed program by
// ValidateDeformationEnvelopeScale.
DeformationEnvelopeDeclaration ParseDeformationEnvelope(const ShaderPropertySource& modifier);

// Refuse a scaledBy that names a property the composed program does not have:
// a declared property of the merged table, or, for a program whose surface
// declares nothing, a legacy lane name.
void ValidateDeformationEnvelopeScale(DeformationEnvelopeDeclaration& declaration,
                                      const ShaderPropertyTable& table);

} // namespace GameEngine::Rendering
