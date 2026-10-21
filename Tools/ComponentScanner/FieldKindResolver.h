#pragma once

#include "FieldKind.h"
#include "ReflectedModel.h"

#include <map>
#include <string>

namespace GameEngine::ComponentScanner
{

// The field's kind; for an enum field with a scanned enumerator table, that enum; and the
// type name the resolution ended on (aliases followed, namespace dropped).
struct ResolvedKind
{
    FieldKind Kind = FieldKind::Unknown;
    const EnumDecl* Enum = nullptr;
    std::string TypeName;
};

// Resolves a reflected field's C++ type text to the FieldKind its registration gets from
// FieldTypeIdOf<decltype(T::field)>() (ECS/Reflection.h), without a compiler: the same
// ladder over type names instead of types.
//   - enums       -> the kind of the declared underlying type (int when none is written); the
//                    enumerator table is matched on the name as written, before any alias, as
//                    the C++ emitter binds it; an alias that leads to an enum gets the enum's
//                    underlying kind and no table, as the registry does
//   - aliases     -> followed through the `using` declarations of the scanned headers, then
//                    the engine's scalar aliases (Types/Types.h, Types/StringId.h)
//   - arrays      -> the element's kind; a plain char array is String
//   - composites  -> the types that carry a FieldTypeTraits specialization (ColorLinear,
//                    EntityHandle, the Mathematics vectors and quaternion, AssetRef)
//   - otherwise   -> Unknown
namespace FieldKindResolver
{
ResolvedKind Resolve(const ReflectedField& field, const std::map<std::string, std::string>& aliases,
                     const EnumTable& enums);
}

} // namespace GameEngine::ComponentScanner
