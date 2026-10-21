#include "ReflectedModel.h"

#include "Lexical.h"

namespace GameEngine::ComponentScanner
{

const EnumDecl* EnumTable::Match(std::string_view fieldType) const
{
    if (Lexical::TrimView(fieldType).empty())
        return nullptr;
    const std::vector<std::string> tokens = Lexical::SplitWhitespace(fieldType);
    const std::string last = tokens.empty() ? Lexical::Trim(fieldType) : tokens.back();
    const std::string simple(Lexical::LastScopeSegment(last));
    if (Ambiguous.count(simple) != 0)
        return nullptr;
    const auto it = ByName.find(simple);
    return it == ByName.end() ? nullptr : &it->second;
}

} // namespace GameEngine::ComponentScanner
