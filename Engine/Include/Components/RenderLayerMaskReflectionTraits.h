#pragma once

// Teaches GE_REFLECT about Rendering::RenderLayerMask: a reflected member of that type registers as
// FieldTypeId::RenderLayerMask, so the scene serializer writes it as layer numbers in brackets (or
// all, or none) and the inspector and the web pages know it is a layer mask, not a bare integer.
//
// A bridge header in the Components layer, like MathReflectionTraits.h: ECS links Logger, JobSystem,
// Memory and Types only, and Rendering does not link ECS. ComponentRegistrationCommon.h includes it,
// so the specialization is visible at every GE_REGISTER_COMPONENT site before FieldTypeIdOf runs.

#include "ECS/Reflection.h"
#include "Rendering/RenderLayerMask.h"

namespace GameEngine::ECS
{

template <>
struct FieldTypeTraits<Rendering::RenderLayerMask>
{
    static constexpr FieldTypeId kType = FieldTypeId::RenderLayerMask;
};

static_assert(FieldElementSize(FieldTypeId::RenderLayerMask) == sizeof(Rendering::RenderLayerMask),
              "FieldElementSize(RenderLayerMask) must equal sizeof(Rendering::RenderLayerMask)");

} // namespace GameEngine::ECS
