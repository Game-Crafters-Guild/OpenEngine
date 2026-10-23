#include "ShaderDeformationEnvelopeParse.h"

#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/Materials/ShaderPropertyTable.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <vector>

namespace GameEngine::Rendering
{

namespace
{

constexpr std::string_view kTag = "@deformationEnvelope";
constexpr std::string_view kScaledByKey = "scaledBy=";
constexpr std::string_view kNotWidened =
    "Culling bounds are not widened for materials using this modifier, so deformed geometry may be culled "
    "while still on screen.";

// The text after the tag when `line` is a declaration: the tag is the first
// token after `//`, and `@deformationEnvelopeX` is not the tag.
bool FindDeclaration(const std::string& line, std::string& rest)
{
    const size_t s = line.find_first_not_of(" \t\r");
    if (s == std::string::npos || line.compare(s, 2, "//") != 0)
        return false;
    size_t at = line.find_first_not_of(" \t", s + 2);
    if (at == std::string::npos || line.compare(at, kTag.size(), kTag) != 0)
        return false;
    at += kTag.size();
    if (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r')
        return false;
    rest = line.substr(at);
    return true;
}

bool ParseMeters(const std::string& token, float& out)
{
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(token.c_str(), &end);
    if (end != token.c_str() + token.size() || errno == ERANGE || !std::isfinite(value) || value < 0.0f)
        return false;
    out = value;
    return true;
}

bool IsIdentifier(std::string_view s)
{
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s.front())) || s.front() == '_'))
        return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    return true;
}

std::string Trim(const std::string& v)
{
    const size_t b = v.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return {};
    return v.substr(b, v.find_last_not_of(" \t\r") - b + 1);
}

std::string GrammarRefusal(const std::string& file, const std::string& text)
{
    return "Vertex modifier '" + file + "': '" + std::string(kTag) + (text.empty() ? "" : " ") + text +
           "' is not a distance in meters, optionally followed by 'scaledBy=<propertyName>'. " +
           std::string(kNotWidened);
}

// Fill `out` from one declaration's text; false when the grammar refuses it.
bool ParseDeclarationText(const std::string& text, DeformationEnvelopeDeclaration& out)
{
    std::istringstream tokens(text);
    std::vector<std::string> words;
    for (std::string word; tokens >> word;)
        words.push_back(word);
    if (words.empty() || words.size() > 2 || !ParseMeters(words[0], out.Meters))
        return false;
    if (words.size() == 1)
        return true;
    const std::string_view clause = words[1];
    if (clause.substr(0, kScaledByKey.size()) != kScaledByKey || !IsIdentifier(clause.substr(kScaledByKey.size())))
        return false;
    out.ScaledBy = std::string(clause.substr(kScaledByKey.size()));
    return true;
}

} // namespace

DeformationEnvelopeDeclaration ParseDeformationEnvelope(const ShaderPropertySource& modifier)
{
    DeformationEnvelopeDeclaration declaration{};
    declaration.ModifierFile = modifier.File;
    std::istringstream lines(modifier.Text);
    std::string line;
    uint32_t lineNo = 0;
    while (std::getline(lines, line))
    {
        ++lineNo;
        std::string rest;
        if (!FindDeclaration(line, rest))
            continue;
        if (declaration.Declared)
        {
            declaration.Refusal = "Vertex modifier '" + modifier.File + "' declares " + std::string(kTag) +
                                  " on lines " + std::to_string(declaration.SourceLine) + " and " +
                                  std::to_string(lineNo) + "; keep one. " + std::string(kNotWidened);
            declaration.Meters = 0.0f;
            declaration.ScaledBy.clear();
            return declaration;
        }
        declaration.Declared = true;
        declaration.SourceLine = lineNo;
        const std::string text = Trim(rest);
        if (!ParseDeclarationText(text, declaration))
        {
            declaration.Refusal = GrammarRefusal(modifier.File, text);
            declaration.Meters = 0.0f;
            declaration.ScaledBy.clear();
        }
    }
    return declaration;
}

void ValidateDeformationEnvelopeScale(DeformationEnvelopeDeclaration& declaration,
                                      const ShaderPropertyTable& table)
{
    if (!declaration.Usable() || declaration.ScaledBy.empty())
        return;
    if (const ShaderProperty* property = table.Find(declaration.ScaledBy))
    {
        const ShaderPropertyType type = property->Type;
        if (type != ShaderPropertyType::Bool && type != ShaderPropertyType::Int && type != ShaderPropertyType::Enum)
            return;
        declaration.Refusal = "Vertex modifier '" + declaration.ModifierFile + "': '" + std::string(kTag) +
                              " scaledBy=" + declaration.ScaledBy + "' names a " + ShaderPropertyTypeName(type) +
                              " property; scaledBy takes a float or vector property. " + std::string(kNotWidened);
        return;
    }
    if (!table.HasSurfaceDeclarations && FindLegacyMaterialLane(declaration.ScaledBy))
        return;
    declaration.Refusal = "Vertex modifier '" + declaration.ModifierFile + "': '" + std::string(kTag) +
                          " scaledBy=" + declaration.ScaledBy +
                          "' names a property this shader does not declare. Add '// @property float " +
                          declaration.ScaledBy +
                          "' or remove the scaledBy clause. Until then the margin is zero and culling bounds "
                          "are not widened for this deformation, so deformed geometry may be culled while "
                          "still on screen.";
}

} // namespace GameEngine::Rendering
