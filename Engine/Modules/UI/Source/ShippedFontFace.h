#pragma once

#include "UI/FontWeightMatch.h"

#include <span>
#include <string_view>

namespace GameEngine::UI
{

/// A face the editor ships under Assets/Fonts, by its CSS weight.
struct ShippedFontFace
{
    int Weight;
    const char* Upright;
    const char* Italic;
};

inline constexpr ShippedFontFace kShippedRobotoFaces[] = {
    {400, "Roboto-Regular.ttf", "Roboto-Italic.ttf"},
    {500, "Roboto-Medium.ttf", "Roboto-MediumItalic.ttf"},
    {700, "Roboto-Bold.ttf", "Roboto-BoldItalic.ttf"},
};

inline constexpr ShippedFontFace kShippedRobotoMonoFaces[] = {
    {400, "RobotoMono-Regular.ttf", "RobotoMono-Italic.ttf"},
    {700, "RobotoMono-Bold.ttf", "RobotoMono-BoldItalic.ttf"},
};

/// The file name of the shipped face that draws `family` at CSS `weight`, chosen by
/// FontWeightMatchCost (the first face wins a tie), or nullptr when the editor ships no face of
/// that family. `familyKey` is the lowercased family name. Roboto Mono is checked first because
/// "roboto mono" also contains "roboto".
constexpr const char* PickShippedFontFile(std::string_view familyKey, int weight, bool italic)
{
    std::span<const ShippedFontFace> faces;
    if (familyKey.find("roboto mono") != std::string_view::npos || familyKey.find("robotomono") != std::string_view::npos)
        faces = kShippedRobotoMonoFaces;
    else if (familyKey.find("roboto") != std::string_view::npos)
        faces = kShippedRobotoFaces;
    else
        return nullptr;

    const ShippedFontFace* best = &faces.front();
    for (const ShippedFontFace& face : faces)
    {
        if (FontWeightMatchCost(weight, face.Weight) < FontWeightMatchCost(weight, best->Weight))
            best = &face;
    }
    return italic ? best->Italic : best->Upright;
}

} // namespace GameEngine::UI
