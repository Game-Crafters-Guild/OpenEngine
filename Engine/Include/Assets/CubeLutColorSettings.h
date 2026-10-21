#pragma once

#include "Rendering/Utils/CubeLutColorDomain.h"

#include <filesystem>

namespace GameEngine
{

class AssetRegistry;

/// The input encoding a .cube asset renders with: the user's choice stored on the asset (the asset
/// database key kCubeLutInputEncodingMetaKey) where there is one, otherwise the importer's proposal
/// from the file.
struct CubeLutColorSettings
{
    Rendering::CubeLutInputEncoding InputEncoding = Rendering::CubeLutInputEncoding::Rec709SRGB;
    bool InputEncodingChosen = false; ///< the user chose the encoding; false = the proposal
};

CubeLutColorSettings ResolveCubeLutColorSettings(const AssetRegistry& registry, const std::filesystem::path& assetPath,
                                                 const Rendering::CubeLutColorProposal& proposal);

} // namespace GameEngine
