#pragma once

#include "ApiModel.h"
#include "EnumScanner.h"
#include "FunctionParser.h"
#include "StructParser.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// One header's text and what the structural passes read from it.
struct HeaderSource
{
    std::string Module;
    std::string File;    // the path as the report prints it
    std::string Include; // "Input/InputScriptApi.h"
    std::vector<std::string> Lines;
    std::string Stripped;
    std::vector<ParsedStruct> Structs;
    std::vector<EnumDecl> Enums;
    std::vector<ParsedConstant> Constants;
};

enum class DraftKind
{
    Function,
    Record,
    Enum,
    Handle,
    Subscribe,
    Unsubscribe,
};

// A marked declaration read but not yet resolved against the kind table. HeaderIndex is the
// header of its module it sits in; Scope the namespaces and class around it
// ("GameEngine::Input::Script"), where a type name it writes is looked up first.
struct Draft
{
    DraftKind Kind = DraftKind::Function;
    std::size_t HeaderIndex = 0;
    std::string Scope;
    int Line = 0;
    std::string Doc;
    std::string Name;
    std::string CppName;
    ApiThread Thread = ApiThread::Main;
    bool Try = false;
    bool Context = false;
    bool Flags = false;
    std::optional<ApiDelivery> Delivery;
    ParsedFunction Function;
    std::size_t StructIndex = 0;
    std::size_t EnumIndex = 0;
    ParsedAlias Alias;
};

// One script module: its header (Include/<Module>/<Name>ScriptApi.h is the <Name> module) and
// the drafts read from it.
struct ModuleDraft
{
    std::string Name;
    std::vector<HeaderSource> Headers;
    std::vector<Draft> Drafts;
};

// The namespaces and class of `scope`, joined ("GameEngine::Input::Script").
inline std::string ScopeName(const DeclarationScope& scope)
{
    if (scope.Class.empty())
        return scope.Namespace;
    return scope.Namespace.empty() ? scope.Class : scope.Namespace + "::" + scope.Class;
}

// The fully qualified name of `name` declared in `scope`.
inline std::string Qualify(const DeclarationScope& scope, std::string_view name)
{
    std::string result = scope.Namespace;
    if (!scope.Class.empty())
        result += (result.empty() ? "" : "::") + scope.Class;
    return result + (result.empty() ? "" : "::") + std::string(name);
}

} // namespace GameEngine::ComponentScanner
