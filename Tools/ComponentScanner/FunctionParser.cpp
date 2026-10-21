#include "FunctionParser.h"

#include "Lexical.h"

#include <algorithm>

namespace GameEngine::ComponentScanner::FunctionParser
{
namespace
{

using namespace Lexical;

constexpr std::size_t kNoMatch = std::string_view::npos;

// A brace block opened before the offset ScopeAt reads.
struct OpenBlock
{
    enum class Kind
    {
        Namespace,
        Class,
        Transparent, // extern "C" { ... }: its contents sit in the enclosing scope
        Other,
    };
    Kind BlockKind = Kind::Other;
    std::string Name;
};

bool IsWord(std::string_view text, std::size_t pos, std::string_view word)
{
    if (text.substr(pos, word.size()) != word)
        return false;
    if (pos > 0 && IsIdentifierChar(text[pos - 1]))
        return false;
    const std::size_t end = pos + word.size();
    return end >= text.size() || !IsIdentifierChar(text[end]);
}

// The position of `word` as a whole word in `text`, or npos.
std::size_t FindWord(std::string_view text, std::string_view word)
{
    for (std::size_t pos = text.find(word); pos != kNoMatch; pos = text.find(word, pos + 1))
        if (IsWord(text, pos, word))
            return pos;
    return kNoMatch;
}

// What the text before a '{' opens: "namespace A::B", "struct Name : Base", extern "C" (its
// string blanked by the stripper), or anything else.
OpenBlock ClassifyBlockHead(std::string_view head)
{
    const std::string_view t = TrimView(head);
    const std::vector<std::string> words = SplitWhitespace(t);
    std::size_t first = 0;
    if (!words.empty() && words[0] == "inline")
        first = 1;
    if (first < words.size() && words[first] == "namespace")
    {
        if (words.size() == first + 1)
            return OpenBlock{OpenBlock::Kind::Namespace, ""};
        if (words.size() == first + 2)
            return OpenBlock{OpenBlock::Kind::Namespace, words[first + 1]};
    }
    if (words.size() == 1 && words[0] == "extern")
        return OpenBlock{OpenBlock::Kind::Transparent, ""};
    if (t.find('(') == kNoMatch && t.find('=') == kNoMatch && FindWord(t, "enum") == kNoMatch)
    {
        for (std::string_view key : {std::string_view("struct"), std::string_view("class"), std::string_view("union")})
        {
            const std::size_t pos = FindWord(t, key);
            if (pos == kNoMatch)
                continue;
            const std::string name = FirstToken(t.substr(pos + key.size()));
            if (!name.empty())
                return OpenBlock{OpenBlock::Kind::Class, name};
        }
    }
    return OpenBlock{OpenBlock::Kind::Other, ""};
}

// The index of the bracket closing the one at `open`, counting (), <>, {} and [] together, or
// npos.
std::size_t MatchBracket(std::string_view text, std::size_t open)
{
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '(' || c == '{' || c == '[' || c == '<')
            ++depth;
        else if (c == ')' || c == '}' || c == ']' || c == '>')
        {
            if (--depth == 0)
                return i;
        }
    }
    return kNoMatch;
}

// Split at `delim` where no (), <>, {} or [] is open.
std::vector<std::string> SplitTopLevel(std::string_view text, char delim)
{
    std::vector<std::string> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '(' || c == '{' || c == '[' || c == '<')
            ++depth;
        else if (c == ')' || c == '}' || c == ']' || c == '>')
            --depth;
        else if (c == delim && depth == 0)
        {
            parts.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.emplace_back(text.substr(start));
    return parts;
}

// The first '=' where no bracket is open, that is not part of ==, <=, >= or !=; npos when none.
std::size_t TopLevelAssign(std::string_view text)
{
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '(' || c == '{' || c == '[' || c == '<')
            ++depth;
        else if (c == ')' || c == '}' || c == ']' || c == '>')
            --depth;
        else if (c == '=' && depth == 0)
        {
            const char prev = i > 0 ? text[i - 1] : '\0';
            const char next = i + 1 < text.size() ? text[i + 1] : '\0';
            if (prev != '=' && prev != '<' && prev != '>' && prev != '!' && next != '=')
                return i;
        }
    }
    return kNoMatch;
}

// The end of the statement starting at `offset`: the first ';' or '{' with no parenthesis
// open, or the text's size.
std::size_t StatementEnd(std::string_view text, std::size_t offset)
{
    int parens = 0;
    for (std::size_t i = offset; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '(')
            ++parens;
        else if (c == ')')
            --parens;
        else if ((c == ';' || c == '{') && parens == 0)
            return i;
    }
    return text.size();
}

// Removes [[attribute]] groups.
std::string WithoutAttributes(std::string_view text)
{
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text.substr(i, 2) == "[[")
        {
            const std::size_t close = text.find("]]", i + 2);
            if (close != kNoMatch)
            {
                result.push_back(' ');
                i = close + 1;
                continue;
            }
        }
        result.push_back(text[i]);
    }
    return result;
}

// "const PlayOptions& options" -> type "const PlayOptions&", name "options"; false when the
// declarator holds no name after its type.
bool SplitTypeAndName(std::string_view declarator, std::string& outType, std::string& outName)
{
    const std::string_view t = TrimView(declarator);
    std::size_t end = t.size();
    std::size_t start = end;
    while (start > 0 && IsIdentifierChar(t[start - 1]))
        --start;
    if (start == end || !IsIdentifierStart(t[start]))
        return false;
    const std::string type = Trim(t.substr(0, start));
    if (type.empty())
        return false;
    // A name that is only a type word ("const Vector2&" has none) is the type's end.
    const std::string_view name = t.substr(start, end - start);
    if (name == "const" || name == "volatile")
        return false;
    outType = type;
    outName = std::string(name);
    return true;
}

DeclarationProblem Problem(std::string problem, std::string fix)
{
    return DeclarationProblem{std::move(problem), std::move(fix)};
}

} // namespace

std::size_t LineOffset(std::string_view stripped, int line)
{
    std::size_t offset = 0;
    for (int current = 1; current < line; ++current)
    {
        const std::size_t newline = stripped.find('\n', offset);
        if (newline == kNoMatch)
            return stripped.size();
        offset = newline + 1;
    }
    return offset;
}

std::size_t SkipSpace(std::string_view stripped, std::size_t offset)
{
    while (offset < stripped.size() && IsWhiteSpace(stripped[offset]))
        ++offset;
    return offset;
}

DeclarationScope ScopeAt(std::string_view stripped, std::size_t offset)
{
    std::vector<OpenBlock> open;
    std::size_t headStart = 0;
    const std::size_t end = std::min(offset, stripped.size());
    for (std::size_t i = 0; i < end; ++i)
    {
        const char c = stripped[i];
        if (c == '{')
        {
            const bool inOther = std::any_of(open.begin(), open.end(),
                                             [](const OpenBlock& b) { return b.BlockKind == OpenBlock::Kind::Other; });
            open.push_back(inOther ? OpenBlock{} : ClassifyBlockHead(stripped.substr(headStart, i - headStart)));
            headStart = i + 1;
        }
        else if (c == '}')
        {
            if (!open.empty())
                open.pop_back();
            headStart = i + 1;
        }
        else if (c == ';')
        {
            headStart = i + 1;
        }
    }

    DeclarationScope scope;
    for (const OpenBlock& block : open)
    {
        switch (block.BlockKind)
        {
        case OpenBlock::Kind::Namespace:
            if (!block.Name.empty())
                scope.Namespace += (scope.Namespace.empty() ? "" : "::") + block.Name;
            break;
        case OpenBlock::Kind::Class:
            scope.Class += (scope.Class.empty() ? "" : "::") + block.Name;
            break;
        case OpenBlock::Kind::Transparent:
            break;
        case OpenBlock::Kind::Other:
            scope.InOtherBlock = true;
            break;
        }
    }
    return scope;
}

bool StartsInsideStatement(std::string_view stripped, std::size_t offset)
{
    std::size_t i = std::min(offset, stripped.size());
    while (i > 0)
    {
        const char c = stripped[i - 1];
        if (c == ';' || c == '{' || c == '}')
            return false;
        if (!IsWhiteSpace(c))
            return true;
        --i;
    }
    return false;
}

std::string FirstWord(std::string_view stripped, std::size_t offset)
{
    return FirstToken(stripped.substr(std::min(offset, stripped.size())));
}

std::optional<ParsedFunction> ParseFunction(std::string_view stripped, std::size_t offset, DeclarationProblem& outProblem)
{
    const std::size_t end = StatementEnd(stripped, offset);
    const std::string text = WithoutAttributes(stripped.substr(offset, end - offset));

    if (FirstToken(text) == "template")
    {
        outProblem = Problem("a template cannot be bound", "declare one function per type it serves, each with its own name");
        return std::nullopt;
    }

    // The parameter list: the first '(' outside any template argument list.
    std::size_t open = kNoMatch;
    int angles = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '<')
            ++angles;
        else if (text[i] == '>')
            --angles;
        else if (text[i] == '(' && angles == 0)
        {
            open = i;
            break;
        }
    }
    if (open == kNoMatch)
    {
        outProblem = Problem("the marked declaration is not a function, a struct, an enum class or a handle alias",
                             "mark a free or static function, a struct, an enum class, or a `using` handle alias");
        return std::nullopt;
    }
    const std::size_t close = MatchBracket(text, open);
    if (close == kNoMatch)
    {
        outProblem = Problem("the parameter list has no closing ')'", "close the parameter list");
        return std::nullopt;
    }

    ParsedFunction result;
    result.Scope = ScopeAt(stripped, offset);

    std::vector<std::string> headWords = SplitWhitespace(std::string_view(text).substr(0, open));
    std::vector<std::string> typeWords;
    for (const std::string& word : headWords)
    {
        if (word == "static")
            result.Static = true;
        else if (word == "virtual")
        {
            outProblem = Problem("an instance method cannot be bound",
                                 "declare a free function whose first parameter is the object, marked @ge-context");
            return std::nullopt;
        }
        else if (word == "friend" || word == "operator" || word.rfind("operator", 0) == 0)
        {
            outProblem = Problem("an operator or a friend declaration cannot be bound", "declare a named free function");
            return std::nullopt;
        }
        else if (word != "inline" && word != "constexpr" && word != "extern" && word != "explicit")
            typeWords.push_back(word);
    }
    std::string head;
    for (const std::string& word : typeWords)
        head += (head.empty() ? "" : " ") + word;

    std::string returnType;
    std::string name;
    if (!SplitTypeAndName(head, returnType, name))
    {
        outProblem = Problem("the function has no return type", "write the return type in front of the name (`void` for none)");
        return std::nullopt;
    }
    if (returnType == "auto")
    {
        outProblem = Problem("a deduced or trailing return type cannot be bound", "write the return type in front of the name");
        return std::nullopt;
    }
    if (returnType.back() == ':')
    {
        outProblem = Problem("a qualified name declares a function defined elsewhere",
                             "mark the declaration in its namespace, not an out-of-line definition");
        return std::nullopt;
    }
    result.Name = name;
    result.ReturnType = returnType;

    const std::string tail = Trim(std::string_view(text).substr(close + 1));
    if (!tail.empty() && tail != "noexcept")
    {
        if (tail.rfind("->", 0) == 0)
            outProblem = Problem("a trailing return type cannot be bound", "write the return type in front of the name");
        else
            outProblem = Problem("`" + tail + "` after the parameter list makes this a member function the binding cannot call",
                                 "declare a free function whose first parameter is the object, marked @ge-context");
        return std::nullopt;
    }
    if (!result.Scope.Class.empty() && !result.Static)
    {
        outProblem = Problem("an instance method cannot be bound",
                             "make it static, or declare a free function whose first parameter is the object, marked @ge-context");
        return std::nullopt;
    }

    const std::string_view list = TrimView(std::string_view(text).substr(open + 1, close - open - 1));
    if (!list.empty() && list != "void")
    {
        for (const std::string& raw : SplitTopLevel(list, ','))
        {
            ParsedParam param;
            std::string_view declarator = raw;
            if (const std::size_t eq = TopLevelAssign(raw); eq != kNoMatch)
            {
                param.Default = Trim(std::string_view(raw).substr(eq + 1));
                declarator = std::string_view(raw).substr(0, eq);
            }
            if (!SplitTypeAndName(declarator, param.Type, param.Name))
            {
                outProblem = Problem("parameter `" + Trim(raw) + "` has no name",
                                     "name every parameter: the name is the parameter's name in every language");
                return std::nullopt;
            }
            result.Params.push_back(std::move(param));
        }
    }
    return result;
}

std::optional<ParsedAlias> ParseAlias(std::string_view stripped, std::size_t offset, DeclarationProblem& outProblem)
{
    const std::size_t end = StatementEnd(stripped, offset);
    const std::string_view text = TrimView(stripped.substr(offset, end - offset));
    const std::vector<std::string> words = SplitWhitespace(text);
    if (!words.empty() && words[0] == "typedef")
    {
        outProblem = Problem("a handle is declared with `typedef`", "write it as `using Name = StrongHandle<NameTag>;`");
        return std::nullopt;
    }
    const std::size_t eq = text.find('=');
    if (words.empty() || words[0] != "using" || eq == kNoMatch)
    {
        outProblem = Problem("@ge-script-handle marks a `using` alias", "write `using Name = StrongHandle<NameTag>;`");
        return std::nullopt;
    }
    ParsedAlias alias;
    alias.Name = Trim(text.substr(5, eq - 5));
    alias.Target = Trim(text.substr(eq + 1));
    alias.Scope = ScopeAt(stripped, offset);
    if (!IsIdentifier(alias.Name))
    {
        outProblem = Problem("the alias has no plain name", "write `using Name = StrongHandle<NameTag>;`");
        return std::nullopt;
    }
    return alias;
}

std::vector<ParsedConstant> ScanConstants(std::string_view stripped)
{
    std::vector<ParsedConstant> constants;
    constexpr std::string_view kConstexpr = "constexpr";
    for (std::size_t pos = stripped.find(kConstexpr); pos != kNoMatch; pos = stripped.find(kConstexpr, pos + 1))
    {
        if (!IsWord(stripped, pos, kConstexpr))
            continue;
        const std::size_t end = StatementEnd(stripped, pos);
        if (end >= stripped.size() || stripped[end] != ';')
            continue; // a function with a body, or a brace initializer read below
        std::string_view statement = stripped.substr(pos + kConstexpr.size(), end - pos - kConstexpr.size());
        if (statement.find('(') != kNoMatch)
            continue;
        std::string declarator;
        std::string literal;
        if (const std::size_t eq = TopLevelAssign(statement); eq != kNoMatch)
        {
            declarator = std::string(statement.substr(0, eq));
            literal = Trim(statement.substr(eq + 1));
        }
        else
            continue;
        ParsedConstant constant;
        if (!SplitTypeAndName(declarator, constant.Type, constant.Name))
            continue;
        constant.Literal = literal;
        constant.Scope = ScopeAt(stripped, pos);
        if (constant.Scope.InOtherBlock)
            continue;
        constant.Line = 1 + static_cast<int>(std::count(stripped.begin(), stripped.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
        constants.push_back(std::move(constant));
    }
    return constants;
}

} // namespace GameEngine::ComponentScanner::FunctionParser
