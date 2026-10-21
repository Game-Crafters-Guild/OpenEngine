#include "StructParser.h"

#include "Lexical.h"

#include <algorithm>
#include <string>

namespace GameEngine::ComponentScanner::StructParser
{
namespace
{

using namespace Lexical;

// 1-based line number of the character at `index` (count of '\n' before it, + 1). The
// comment stripper preserves newlines, so a position in the stripped text maps to the
// same line number in the raw source.
int LineNumberAt(std::string_view src, std::size_t index)
{
    const std::size_t limit = std::min(index, src.size());
    return 1 + static_cast<int>(std::count(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(limit), '\n'));
}

std::size_t FirstNonSpace(std::string_view src, std::size_t from, std::size_t end)
{
    std::size_t i = from;
    while (i < end && IsWhiteSpace(src[i]))
        ++i;
    return i;
}

constexpr std::size_t kNoMatch = std::string_view::npos;

// A byte of a struct name: [A-Za-z0-9_] or a UTF-8 byte past ASCII (MSVC, Clang and GCC
// accept UTF-8 letters in identifiers).
bool IsStructNameChar(char c)
{
    return IsIdentifierChar(c) || static_cast<unsigned char>(c) >= 0x80;
}

// Index of the matching '}' for the '{' at openIndex, or npos.
std::size_t MatchBrace(std::string_view src, std::size_t openIndex, std::size_t end)
{
    int depth = 0;
    for (std::size_t i = openIndex; i < end; ++i)
    {
        const char c = src[i];
        if (c == '{')
            ++depth;
        else if (c == '}')
        {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return kNoMatch;
}

std::size_t MatchParen(std::string_view src, std::size_t openIndex, std::size_t end)
{
    int depth = 0;
    for (std::size_t i = openIndex; i < end; ++i)
    {
        const char c = src[i];
        if (c == '(')
            ++depth;
        else if (c == ')')
        {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return kNoMatch;
}

// A keyword at `pos` with an identifier boundary before it and `keyword.size()` bytes
// of exact text; the caller checks what follows.
bool KeywordAt(std::string_view src, std::size_t pos, std::string_view keyword)
{
    if (pos > 0 && IsIdentifierChar(src[pos - 1]))
        return false;
    return src.substr(pos, keyword.size()) == keyword;
}

std::size_t SkipWhitespace(std::string_view src, std::size_t pos)
{
    while (pos < src.size() && IsWhiteSpace(src[pos]))
        ++pos;
    return pos;
}

// "namespace Name {" at `pos` (Name may be qualified, "A::B"). Returns the index of the
// '{' and the name, or npos.
std::size_t MatchNamespaceHead(std::string_view src, std::size_t pos, std::string& outName)
{
    constexpr std::string_view kNamespace = "namespace";
    if (!KeywordAt(src, pos, kNamespace))
        return kNoMatch;
    std::size_t i = pos + kNamespace.size();
    if (i >= src.size() || !IsWhiteSpace(src[i]))
        return kNoMatch;
    i = SkipWhitespace(src, i);
    if (i >= src.size() || !IsIdentifierStart(src[i]))
        return kNoMatch;
    const std::size_t nameStart = i;
    while (i < src.size() && (IsIdentifierChar(src[i]) || src[i] == ':'))
        ++i;
    outName.assign(src.substr(nameStart, i - nameStart));
    i = SkipWhitespace(src, i);
    if (i >= src.size() || src[i] != '{')
        return kNoMatch;
    return i;
}

// "struct Name {" / "struct Name : bases {" / "class Name ... {" at `pos`. Returns the
// index of the '{'; the keyword, the name and the base clause (text between ':' and '{',
// untrimmed; empty without a base clause) come back through the out parameters.
std::size_t MatchStructHead(std::string_view src, std::size_t pos, std::string& outKind, std::string& outName,
                            std::string& outBases)
{
    constexpr std::string_view kStruct = "struct";
    constexpr std::string_view kClass = "class";
    std::string_view kind;
    if (KeywordAt(src, pos, kStruct))
        kind = kStruct;
    else if (KeywordAt(src, pos, kClass))
        kind = kClass;
    else
        return kNoMatch;

    std::size_t i = pos + kind.size();
    if (i >= src.size() || !IsWhiteSpace(src[i]))
        return kNoMatch;
    i = SkipWhitespace(src, i);
    if (i >= src.size() || IsAsciiDigit(src[i]) || !IsStructNameChar(src[i]))
        return kNoMatch;
    const std::size_t nameStart = i;
    while (i < src.size() && IsStructNameChar(src[i]))
        ++i;
    const std::string_view name = src.substr(nameStart, i - nameStart);
    i = SkipWhitespace(src, i);
    if (i >= src.size())
        return kNoMatch;

    std::string_view bases;
    if (src[i] == ':')
    {
        const std::size_t basesStart = i + 1;
        std::size_t j = basesStart;
        while (j < src.size() && src[j] != '{' && src[j] != '}' && src[j] != ';')
            ++j;
        if (j >= src.size() || src[j] != '{')
            return kNoMatch;
        bases = src.substr(basesStart, j - basesStart);
        i = j;
    }
    else if (src[i] != '{')
    {
        return kNoMatch;
    }

    outKind.assign(kind);
    outName.assign(name);
    outBases.assign(bases);
    return i;
}

// True if the struct/class keyword at `structIndex` is immediately preceded by a template
// parameter clause ("template< ... >"), i.e. this is a template definition rather than a
// concrete type. Walks back over whitespace, a balanced <...>, and checks for the
// `template` keyword. `lowerBound` clamps the backward walk to the enclosing scope.
bool IsTemplatePrefixed(std::string_view src, std::size_t structIndex, std::size_t lowerBound)
{
    std::ptrdiff_t j = static_cast<std::ptrdiff_t>(structIndex) - 1;
    const std::ptrdiff_t bound = static_cast<std::ptrdiff_t>(lowerBound);
    while (j >= bound && IsWhiteSpace(src[static_cast<std::size_t>(j)]))
        --j;
    if (j < bound || src[static_cast<std::size_t>(j)] != '>')
        return false;

    int depth = 0;
    while (j >= bound)
    {
        const char ch = src[static_cast<std::size_t>(j)];
        if (ch == '>')
            ++depth;
        else if (ch == '<')
        {
            --depth;
            if (depth == 0)
            {
                --j;
                break;
            }
        }
        --j;
    }
    if (depth != 0)
        return false;

    while (j >= bound && IsWhiteSpace(src[static_cast<std::size_t>(j)]))
        --j;
    const std::ptrdiff_t wordEnd = j + 1;
    while (j >= bound && IsIdentifierChar(src[static_cast<std::size_t>(j)]))
        --j;
    const std::size_t wordStart = static_cast<std::size_t>(j + 1);
    return src.substr(wordStart, static_cast<std::size_t>(wordEnd) - wordStart) == "template";
}

// Split on a delimiter that is at brace/paren/bracket depth 0.
std::vector<std::string> SplitTopLevel(std::string_view s, char delim)
{
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (char c : s)
    {
        if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if (c == ')' || c == '}' || c == ']')
            --depth;

        if (c == delim && depth == 0)
        {
            parts.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty() || !parts.empty())
        parts.push_back(current);
    return parts;
}

std::size_t IndexOfTopLevel(std::string_view s, char target)
{
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if (c == ')' || c == '}' || c == ']')
            --depth;
        else if (c == target && depth == 0)
            return i;
    }
    return kNoMatch;
}

// True when `text` ends in "operator", optional whitespace, then a run of non-whitespace:
// the declarator before the '=' of operator=, operator+=, operator() and the like.
bool EndsWithOperatorToken(std::string_view text)
{
    constexpr std::string_view kOperator = "operator";
    std::size_t pos = text.find(kOperator);
    while (pos != std::string_view::npos)
    {
        if (pos == 0 || !IsIdentifierChar(text[pos - 1]))
        {
            std::string_view rest = text.substr(pos + kOperator.size());
            std::size_t k = 0;
            while (k < rest.size() && IsWhiteSpace(rest[k]))
                ++k;
            rest = rest.substr(k);
            if (rest.find_first_of(" \t\n\r\v\f") == std::string_view::npos)
                return true;
        }
        pos = text.find(kOperator, pos + 1);
    }
    return false;
}

// True if `s` (the declarator text accumulated before a '(') contains a top-level
// copy-initializer '=' — i.e. the '(' begins an initializer expression, not a function
// parameter list. Excludes comparison operators (==, <=, >=, !=) and operator-overload
// functions (operator=, operator+=, operator(), ...), which are not data members. The
// accumulated text never contains '(' or '{' (both are consumed by their own handlers),
// so only '[' ']' affect depth here.
bool HasTopLevelAssign(std::string_view s)
{
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '(' || c == '{' || c == '[')
            ++depth;
        else if (c == ')' || c == '}' || c == ']')
            --depth;
        else if (c == '=' && depth == 0)
        {
            const char prev = i > 0 ? s[i - 1] : '\0';
            const char next = i + 1 < s.size() ? s[i + 1] : '\0';
            if (prev == '=' || prev == '<' || prev == '>' || prev == '!' || next == '=')
                continue;
            if (EndsWithOperatorToken(s.substr(0, i)))
                continue;
            return true;
        }
    }
    return false;
}

// True if `head` (text accumulated before a '{') ends in a declarator that would make the
// following braces an initializer rather than a nested type body: at least a type and a
// name ("float32 Threshold", "float32 Color[3]") and no type-introducing keyword first. A
// lone identifier ("Foo") before '{' with no preceding type is a nested type.
bool LooksLikeMemberInitializer(std::string_view head)
{
    const std::string_view t = TrimView(head);
    if (t.empty())
        return false;

    const std::string first = FirstToken(t);
    if (first == "struct" || first == "class" || first == "enum" || first == "union" || first == "namespace")
        return false;

    return TokenizeDeclarator(t).size() >= 2;
}

// Remove leading "public:" / "private:" / "protected:", possibly repeated.
std::string StripAccessSpecifiers(std::string_view s)
{
    std::string_view t = s;
    while (true)
    {
        const std::string_view trimmed = TrimView(t);
        std::size_t len = 0;
        for (std::string_view specifier : {std::string_view("public"), std::string_view("private"), std::string_view("protected")})
        {
            if (StartsWith(trimmed, specifier))
            {
                std::size_t k = specifier.size();
                while (k < trimmed.size() && IsWhiteSpace(trimmed[k]))
                    ++k;
                if (k < trimmed.size() && trimmed[k] == ':')
                    len = k + 1;
                break;
            }
        }
        if (len == 0)
            break;
        t = trimmed.substr(len);
    }
    return Trim(t);
}

// ALL_CAPS with underscores/digits, length >= 4, at least one underscore, starting with a
// letter: a field-injection macro name, never a fixed-width-int style alias.
bool IsMacroLikeToken(std::string_view t)
{
    if (t.size() < 4)
        return false;
    if (!IsUpper(t[0]))
        return false;
    for (char c : t)
        if (!(IsUpper(c) || IsAsciiDigit(c) || c == '_'))
            return false;
    return t.find('_') != std::string_view::npos;
}

// The first bare macro-like token that appears as a leading token of a declarator
// statement (all but the last token, the field name), or a statement that is just one.
std::optional<std::string> FindBareMacroToken(std::string_view statement)
{
    const std::vector<std::string> tokens = TokenizeDeclarator(statement);
    const std::size_t upTo = tokens.empty() ? 0 : tokens.size() - 1;
    for (std::size_t i = 0; i < upTo; ++i)
        if (IsMacroLikeToken(tokens[i]))
            return tokens[i];
    if (tokens.size() == 1 && IsMacroLikeToken(tokens[0]))
        return tokens[0];
    return std::nullopt;
}

std::string ExtractPointerRefMarkers(std::string_view text)
{
    std::string markers;
    for (char ch : text)
        if (ch == '*' || ch == '&')
            markers.push_back(ch);
    return markers;
}

// Parse a single declarator chunk. Without knownType, the type prefix is derived from the
// chunk (everything before the final name); otherwise knownType is reused (comma-separated
// declarators share the leading type).
std::optional<FieldDecl> ParseDeclarator(std::string_view chunk, const std::string* knownType, int declLine,
                                         int endLine, std::string& outTypePrefix)
{
    outTypePrefix = knownType ? *knownType : "";

    const std::string_view c = TrimView(chunk);
    if (c.empty())
        return std::nullopt;

    // Cut off the initializer at a top-level '='; an array '[' stays, it is part of the name.
    const std::size_t eq = IndexOfTopLevel(c, '=');
    const std::string declPart = eq != kNoMatch ? Trim(c.substr(0, eq)) : std::string(c);

    const std::size_t bracket = declPart.find('[');
    const std::string beforeBracket = bracket != std::string::npos ? Trim(std::string_view(declPart).substr(0, bracket)) : declPart;

    // The name is the last identifier in beforeBracket; the type is everything before it.
    const std::vector<std::string> tokens = TokenizeDeclarator(beforeBracket);
    if (tokens.empty())
        return std::nullopt;

    const std::string& name = tokens.back();
    if (!IsIdentifier(name))
        return std::nullopt;

    if (!knownType)
    {
        const std::size_t nameIdx = beforeBracket.rfind(name);
        outTypePrefix = (nameIdx != std::string::npos && nameIdx > 0)
                            ? Trim(std::string_view(beforeBracket).substr(0, nameIdx))
                            : "";
        // A bare single token with no type ("Enabled" alone) is not a field.
        if (outTypePrefix.empty() && tokens.size() < 2)
            return std::nullopt;
    }

    // Type string used for skip checks: the type prefix plus any pointer/ref markers that
    // attached to the declarator ("Foo* p" -> '*' lands in beforeBracket).
    std::string effectiveType;
    if (knownType)
    {
        std::string declMarkers;
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
            declMarkers += tokens[i] + " ";
        effectiveType = *knownType + " " + declMarkers + ExtractPointerRefMarkers(beforeBracket);
    }
    else
    {
        effectiveType = outTypePrefix + " " + ExtractPointerRefMarkers(beforeBracket);
    }

    std::optional<std::string> arrayExtent;
    if (bracket != std::string::npos)
    {
        const std::size_t close = declPart.rfind(']');
        if (close != std::string::npos && close > bracket)
            arrayExtent = Trim(std::string_view(declPart).substr(bracket + 1, close - bracket - 1));
    }

    return FieldDecl{Trim(effectiveType), name, declLine, endLine, arrayExtent};
}

// The initializer of one declarator as written: the text after a top-level '=', or a brace
// initializer from its '{'; nullopt when the declarator has none.
std::optional<std::string> InitializerText(std::string_view rawDeclarator)
{
    if (const std::size_t eq = IndexOfTopLevel(rawDeclarator, '='); eq != kNoMatch)
        return Trim(rawDeclarator.substr(eq + 1));
    if (const std::size_t brace = rawDeclarator.find('{'); brace != std::string_view::npos)
        return Trim(rawDeclarator.substr(brace));
    return std::nullopt;
}

// Parse "type declarator [, declarator]..." into FieldDecls. The first chunk carries the
// type; subsequent chunks are bare declarators sharing that type. `raw` is the same
// statement with its initializers' braces and parentheses kept, which `s` has lost.
void ExtractDeclarators(std::string_view s, std::string_view raw, ParsedStruct& ps, int startLine, int endLine)
{
    const std::vector<std::string> chunks = SplitTopLevel(s, ',');
    if (chunks.empty())
        return;
    const std::vector<std::string> rawChunks = SplitTopLevel(raw, ',');
    const auto initializerOf = [&rawChunks](std::size_t k) {
        return k < rawChunks.size() ? InitializerText(rawChunks[k]) : std::nullopt;
    };

    std::string typePrefix;
    std::optional<FieldDecl> firstField = ParseDeclarator(chunks[0], nullptr, startLine, endLine, typePrefix);
    if (!firstField)
        return;

    // No type prefix means there was no real type.
    if (TrimView(typePrefix).empty())
        return;

    firstField->Initializer = initializerOf(0);
    ps.Fields.push_back(std::move(*firstField));

    for (std::size_t k = 1; k < chunks.size(); ++k)
    {
        std::string unused;
        if (std::optional<FieldDecl> f = ParseDeclarator(chunks[k], &typePrefix, startLine, endLine, unused))
        {
            f->Initializer = initializerOf(k);
            ps.Fields.push_back(std::move(*f));
        }
    }
}

// Handle a single ';'-terminated statement: decide skip vs. extract field name(s). `raw` is
// the statement's source text, initializers included.
void ProcessStatement(std::string_view statement, std::string_view raw, ParsedStruct& ps, int startLine, int endLine)
{
    std::string s = Trim(statement);
    if (s.empty())
        return;

    // A virtual member anywhere makes the whole struct non-POD.
    if (ContainsWord(s, "virtual"))
    {
        ps.HasVirtual = true;
        return;
    }

    // Access specifiers arrive with a trailing colon and no ';', possibly as a leftover
    // before the next ';'.
    s = StripAccessSpecifiers(s);
    if (s.empty())
        return;

    const std::string first = FirstToken(s);
    if (first == "static" || first == "using" || first == "typedef" || first == "friend" || first == "enum" ||
        first == "struct" || first == "class" || first == "union" || first == "constexpr" ||
        first == "template" || first == "static_assert")
        return;

    // Function-like (anything still containing '(') — defensive; the paren handler in
    // ExtractFields normally swallows these.
    if (s.find('(') != std::string::npos)
        return;

    // A bare ALL-CAPS token with no type + name pair following it is an unexpanded macro
    // pulling in fields the scanner cannot see; the struct is skipped by the caller.
    if (std::optional<std::string> macro = FindBareMacroToken(s))
        ps.UnexpandedMacro = std::move(macro);

    // A complex field type is still recorded; the caller's skip evaluation decides whether
    // to drop the whole struct. The right field NAME(s) and a representative TYPE string
    // must still be extracted.
    ExtractDeclarators(s, StripAccessSpecifiers(raw), ps, startLine, endLine);
}

// Extracts top-level member field declarations from a struct body [start, end). Splits on
// top-level ';' (depth 0 wrt () {} []), skips nested braces, and ignores anything that
// isn't a plain data member.
void ExtractFields(std::string_view src, std::size_t start, std::size_t end, ParsedStruct& ps)
{
    std::size_t i = start;
    std::string stmt;
    // Source index of the first non-whitespace char of the statement being accumulated, or
    // npos between statements; it gives the field's first line.
    std::size_t stmtStartIndex = kNoMatch;

    while (i < end)
    {
        const char c = src[i];

        if (c == '{')
        {
            // A nested type body OR a brace-init "type name{...};": a statement that already
            // holds a plausible declarator makes it an initializer, else a nested definition.
            if (LooksLikeMemberInitializer(stmt))
            {
                // Consume the balanced braces; the field name was before the '{'.
                std::size_t close = MatchBrace(src, i, end);
                if (close == kNoMatch)
                    close = end - 1;
                i = close + 1;
                continue;
            }

            // Nested struct/enum/class/union/anonymous block: skip it and any trailing
            // declarator up to the next ';'.
            std::size_t close = MatchBrace(src, i, end);
            if (close == kNoMatch)
                close = end - 1;
            const std::size_t after = close + 1;
            std::size_t semi = src.find(';', after);
            if (semi == std::string_view::npos || semi >= end)
                semi = end - 1;
            i = semi + 1;
            stmt.clear();
            stmtStartIndex = kNoMatch;
            continue;
        }

        if (c == '(')
        {
            // A '(' after a top-level '=' is part of an initializer expression
            // ("= static_cast<uint32>(Val)", "= MakeThing()"), not a parameter list: consume
            // the balanced parens and keep accumulating toward the ';' so the field survives
            // (its name and type come from the part before '=').
            if (HasTopLevelAssign(stmt))
            {
                const std::size_t initClose = MatchParen(src, i, end);
                i = initClose == kNoMatch ? end : initClose + 1;
                continue;
            }

            // A virtual function is consumed here as a function and never reaches
            // ProcessStatement, so flag it now.
            if (ContainsWord(stmt, "virtual"))
                ps.HasVirtual = true;

            // Method / constructor / function-like: consume balanced parens, then skip to the
            // terminating ';' or '{' (function body), past any trailing qualifiers (const,
            // noexcept, override, final, = default, ref-qualifiers, a trailing return).
            std::size_t close = MatchParen(src, i, end);
            if (close == kNoMatch)
                close = end - 1;
            const std::size_t after = close + 1;

            std::size_t j = after;
            while (j < end && src[j] != '{' && src[j] != ';')
                ++j;

            if (j < end && src[j] == '{')
            {
                // Function body: skip the balanced braces. A member function body is not
                // followed by a required ';', so resume right after the '}' and only swallow
                // a ';' that sits adjacent (as after a lambda-assigned member).
                std::size_t bclose = MatchBrace(src, j, end);
                if (bclose == kNoMatch)
                    bclose = end - 1;
                const std::size_t afterBody = bclose + 1;
                std::size_t k = afterBody;
                while (k < end && IsWhiteSpace(src[k]))
                    ++k;
                i = (k < end && src[k] == ';') ? k + 1 : afterBody;
            }
            else if (j < end && src[j] == ';')
            {
                i = j + 1;
            }
            else
            {
                i = after;
            }
            stmt.clear();
            stmtStartIndex = kNoMatch;
            continue;
        }

        if (c == ';')
        {
            const int startLine = LineNumberAt(src, stmtStartIndex != kNoMatch ? stmtStartIndex : i);
            const int endLine = LineNumberAt(src, i);
            const std::string_view raw =
                stmtStartIndex != kNoMatch ? src.substr(stmtStartIndex, i - stmtStartIndex) : std::string_view();
            ProcessStatement(stmt, raw, ps, startLine, endLine);
            stmt.clear();
            stmtStartIndex = kNoMatch;
            ++i;
            continue;
        }

        if (stmtStartIndex == kNoMatch && !IsWhiteSpace(c))
            stmtStartIndex = i;
        stmt.push_back(c);
        ++i;
    }
    // Trailing content without a ';' is ignored.
}

// Walks [start, end) recording the structs declared at this scope and recursing into
// namespaces. Struct bodies are parsed for fields only; a struct nested in a struct is not
// a component candidate. `depth` counts the namespace bodies around this scope; one that would nest
// past kMaxNamespaceDepth is skipped with a warning, so the recursion stays bounded.
void ParseScope(std::string_view src, std::size_t start, std::size_t end, int depth, std::vector<std::string>& nsStack,
                std::vector<ParsedStruct>& results, std::vector<SourceWarning>& warnings)
{
    std::size_t i = start;
    while (i < end)
    {
        const char c = src[i];

        if (c == '}')
        {
            // Stray closing brace inside this scope (defensive): skip it so a malformed region
            // cannot truncate the rest of the namespace.
            ++i;
            continue;
        }

        const std::size_t p = FirstNonSpace(src, i, end);

        std::string nsName;
        if (const std::size_t bodyOpen = MatchNamespaceHead(src, p, nsName); bodyOpen != kNoMatch)
        {
            std::size_t bodyClose = MatchBrace(src, bodyOpen, end);
            if (bodyClose == kNoMatch)
                bodyClose = end;

            if (depth == kMaxNamespaceDepth)
            {
                const std::string limit = std::to_string(kMaxNamespaceDepth);
                warnings.push_back(SourceWarning{LineNumberAt(src, p),
                                                 "namespaces nest deeper than " + limit +
                                                     "; the declarations inside this one are not scanned. Move the "
                                                     "component to a namespace nested " + limit + " deep or less"});
                i = bodyClose + 1;
                continue;
            }

            // A namespace head can itself stack names: "namespace A::B { ... }"
            std::size_t pushed = 0;
            std::size_t pos = 0;
            while (pos <= nsName.size())
            {
                std::size_t next = nsName.find("::", pos);
                if (next == std::string::npos)
                    next = nsName.size();
                if (next > pos)
                {
                    nsStack.push_back(nsName.substr(pos, next - pos));
                    ++pushed;
                }
                pos = next + 2;
            }

            ParseScope(src, bodyOpen + 1, bodyClose, depth + 1, nsStack, results, warnings);

            nsStack.resize(nsStack.size() - pushed);
            i = bodyClose + 1;
            continue;
        }

        std::string kind;
        std::string name;
        std::string bases;
        if (const std::size_t bodyOpen = MatchStructHead(src, p, kind, name, bases); bodyOpen != kNoMatch)
        {
            std::size_t bodyClose = MatchBrace(src, bodyOpen, end);
            if (bodyClose == kNoMatch)
                bodyClose = end;

            // Only a non-template "struct" declared directly in a namespace is a component
            // candidate: a `template<...> struct` is a template definition (AssetRef<T>), and
            // registering it would emit GE_REGISTER_COMPONENT on an unspecialized template.
            if (kind == "struct" && !IsTemplatePrefixed(src, p, start))
            {
                ParsedStruct ps;
                ps.Name = name;
                std::string qualified;
                for (std::size_t k = 0; k < nsStack.size(); ++k)
                {
                    if (k > 0)
                        qualified += "::";
                    qualified += nsStack[k];
                }
                ps.Namespace = qualified;
                ps.DeclLine = LineNumberAt(src, p);
                ps.Bases = Trim(bases);
                ExtractFields(src, bodyOpen + 1, bodyClose, ps);
                results.push_back(std::move(ps));
            }

            // Skip the definition and a trailing declarator ("} foo;") up to the next ';'.
            const std::size_t after = bodyClose + 1;
            const std::size_t semi = src.find(';', after);
            i = (semi != std::string_view::npos && semi < end) ? semi + 1 : after;
            continue;
        }

        if (c == '{')
        {
            // An unrecognized brace block at namespace scope (a free function body, an
            // extern "C" group, a free initializer): skip the balanced block so its contents
            // cannot be mistaken for nested structs and its '}' cannot truncate the scope.
            const std::size_t close = MatchBrace(src, i, end);
            i = close == kNoMatch ? end : close + 1;
            continue;
        }

        ++i;
    }
}

} // namespace

std::vector<ParsedStruct> Parse(std::string_view source, std::vector<SourceWarning>& outWarnings)
{
    std::vector<ParsedStruct> results;
    std::vector<std::string> nsStack;
    ParseScope(source, 0, source.size(), 0, nsStack, results, outWarnings);
    return results;
}

} // namespace GameEngine::ComponentScanner::StructParser
