#pragma once

#include "ReflectedModel.h"

#include <optional>
#include <string>
#include <vector>

namespace GameEngine::ComponentScanner
{

// The generated C++ translation units: the component registration TU (engine and user
// mode) and the user-system registration TU.
namespace ReflectionEmitter
{

// The C++ FieldFlags expression to emit via GE_REFLECT_FIELD_FLAGS, or nullopt when the
// field carries no policy marker.
std::optional<std::string> FieldFlagExpr(const ReflectedField& field);

// One cap-free GE_REGISTER_COMPONENT_BEGIN / GE_REGISTER_COMPONENT_FIELD /
// GE_REGISTER_COMPONENT_END block per reflected struct (one FIELD line per member, so a
// component may have any number of reflected fields), preceded by one constexpr name<->value
// table per enum a reflected field uses, with the GE_REFLECT_* metadata lines after each
// block. `includes` is the ordered header list; user mode includes the type-erased
// registration header so a user .cpp never pulls the World template surface.
std::string EmitGeneratedCpp(const std::vector<ReflectedStruct>& reflected, const std::vector<std::string>& includes,
                             bool userMode, const EnumTable& enums);

// The user-system registration TU: GameSDK/System.h plus each header declaring a system,
// then a file-scope static per system that wraps it in a UserSystemAdapter<T> and registers
// it at module load. Registration is non-owning (the adapter lives as long as the
// never-unloaded user DLL); the engine's bridge ticks the registry during play mode.
std::string EmitSystemRegistrationCpp(const std::vector<ReflectedSystem>& systems,
                                      const std::vector<std::string>& includes);

} // namespace ReflectionEmitter

} // namespace GameEngine::ComponentScanner
