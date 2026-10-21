#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// Why a declaration cannot be read as the script API vocabulary allows: what is wrong and
// the fix, without the location (the caller adds it).
struct DeclarationProblem
{
    std::string Problem;
    std::string Fix;
};

// Where a declaration sits: the namespaces around it ("GameEngine::Input::Script"), the class
// it is a member of (empty at namespace scope), and whether it is inside a function body or
// another brace block that is neither a namespace nor a class.
struct DeclarationScope
{
    std::string Namespace;
    std::string Class;
    bool InOtherBlock = false;
};

// One parameter as written: its type text ("const PlayOptions&"), its name, and its default
// value text when it has one ("{}", "1.0f", "PointerLockState::Locked").
struct ParsedParam
{
    std::string Type;
    std::string Name;
    std::optional<std::string> Default;
};

// A function declaration as written, up to its ';' or its body's '{'.
struct ParsedFunction
{
    std::string Name;
    DeclarationScope Scope;
    bool Static = false;
    std::string ReturnType;
    std::vector<ParsedParam> Params;
};

// `using Name = Target;`
struct ParsedAlias
{
    std::string Name;
    DeclarationScope Scope;
    std::string Target;
};

// `constexpr Type Name = Literal;` (or `{Literal}`), with the 1-based line it starts on.
struct ParsedConstant
{
    std::string Name;
    DeclarationScope Scope;
    std::string Type;
    std::string Literal;
    int Line = 0;
};

// Reads the declarations a script API header marks, over COMMENT-STRIPPED source
// (CommentStripper::Strip keeps every newline, so offsets map to the raw lines). The parse is
// lexical: it reads the C-shaped declarations the vocabulary allows and names, with the fix,
// anything else it meets at a marked line (a template, an instance method, a trailing return
// type, an unnamed parameter).
namespace FunctionParser
{

// The offset of the first character of 1-based `line`, or the text's size past its end.
std::size_t LineOffset(std::string_view stripped, int line);

// The offset of the first non-whitespace character at or after `offset`.
std::size_t SkipSpace(std::string_view stripped, std::size_t offset);

// The scope at `offset`, read from the namespace, class and other brace blocks opened and not
// closed before it.
DeclarationScope ScopeAt(std::string_view stripped, std::size_t offset);

// True when the statement holding `offset` began before it: text other than whitespace sits
// between the previous ';', '{' or '}' and `offset`. A marker then sits inside a declaration
// rather than above its first line.
bool StartsInsideStatement(std::string_view stripped, std::size_t offset);

// The declaration's first word ("struct", "enum", "using", "template", a type name).
std::string FirstWord(std::string_view stripped, std::size_t offset);

// A function declaration starting at `offset`, or why it is not one the vocabulary allows.
std::optional<ParsedFunction> ParseFunction(std::string_view stripped, std::size_t offset, DeclarationProblem& outProblem);

// A `using Name = Target;` alias starting at `offset`, or why it is not one.
std::optional<ParsedAlias> ParseAlias(std::string_view stripped, std::size_t offset, DeclarationProblem& outProblem);

// Every `constexpr` variable with a single initializer outside a function body.
std::vector<ParsedConstant> ScanConstants(std::string_view stripped);

} // namespace FunctionParser

} // namespace GameEngine::ComponentScanner
