#pragma once

#include "ApiModel.h"
#include "FunctionParser.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// A declaration marked in some <Module>ScriptApi.h that a type text can name: a record, an
// enum or a handle, with its description id ("Input.ActionState") and its fully qualified C++
// name.
struct ApiNamedType
{
    ApiKind Kind = ApiKind::Record;
    std::string Id;
    std::string CppName;
};

// Where a type name is looked up: the types marked in every module, and the namespaces and
// class around the declaration that writes it ("GameEngine::Input::Script").
struct ApiLookup
{
    const std::vector<ApiNamedType>& Named;
    std::string_view Scope;
};

// How a parameter is passed: by value or `const T&` (in), or `T&` (out, written by the
// function). A pointer is refused.
struct ApiParamShape
{
    ApiType Type;
    bool Out = false;
};

// Resolves the C++ spelling of a type to the binding layer's kind table (ApiModel.h). A name
// matches a kind when its written qualification is the tail of the kind's full name:
// `Vector2`, `Mathematics::Vector2` and `GameEngine::Mathematics::Vector2` are Vec2, and
// `glm::vec2` or `Physics::Vector2` are not. Anything outside the table is refused with the
// fix.
namespace ApiKindResolver
{

// A value type with no cv-qualifier, reference or pointer ("Mathematics::Vector2",
// "std::span<const float>", "ActionState").
std::optional<ApiType> ResolveValue(std::string_view typeText, const ApiLookup& lookup,
                                    DeclarationProblem& outProblem);

// A parameter's whole type text ("const PlayOptions&", "Mathematics::Vector2&").
std::optional<ApiParamShape> ResolveParameter(std::string_view typeText, const ApiLookup& lookup,
                                              DeclarationProblem& outProblem);

// True when the written name `spelled` ("Mathematics::Vector2", "::GameEngine::StringId")
// names `qualified` ("GameEngine::Mathematics::Vector2"): its segments are the tail of the
// qualified name's.
bool NamesQualified(std::string_view spelled, std::string_view qualified);

// The size and alignment of a value kind in bytes, as the C++ type lays it out; Record, Enum
// and Handle take theirs from the declaration, so the caller handles them.
std::uint32_t SizeOf(ApiKind kind);
std::uint32_t AlignOf(ApiKind kind);

// True for the integer kinds (an enum's underlying type).
bool IsInteger(ApiKind kind);

} // namespace ApiKindResolver

} // namespace GameEngine::ComponentScanner
