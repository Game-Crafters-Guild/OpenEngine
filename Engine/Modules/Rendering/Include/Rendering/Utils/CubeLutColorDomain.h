#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace GameEngine::Rendering
{

struct CubeLutParseResult;

/// The encoding a .cube LUT's input is authored in. The shaders convert linear Rec.709 into it
/// before the lookup and back after it (Includes/cube_lut.glsl reads the same values).
enum class CubeLutInputEncoding : int32_t
{
    Linear = 0,
    Rec709SRGB = 1,
    ArriLogC3 = 2,
    DaVinciIntermediate = 3,
    ACEScct = 4,
    Cineon = 5,
};

inline constexpr int32_t kCubeLutInputEncodingCount = 6;

/// The value the asset database stores for an encoding ("linear", "srgb", "logc3", "davinci",
/// "acescct", "cineon") and the label a user reads.
std::string_view CubeLutInputEncodingKey(CubeLutInputEncoding encoding);
std::string_view CubeLutInputEncodingLabel(CubeLutInputEncoding encoding);
std::optional<CubeLutInputEncoding> ParseCubeLutInputEncodingKey(std::string_view key);

/// Whether a LUT authored in this encoding over this input range expects display values: Rec.709 /
/// sRGB, or Linear over a range that ends at or below 1. Such a table belongs at the Finish stage,
/// after the look; at the Grade stage it receives scene values and clips those above its range. A log
/// curve, or Linear over a wider range, reads scene values and belongs at Grade.
bool CubeLutExpectsDisplayInput(CubeLutInputEncoding encoding, float inputMax);

/// What the importer proposes for a .cube's input encoding from what the file says, with the
/// reason in words. The .cube format has no field for it, so the proposal reads the TITLE and the
/// input range:
///
/// | the file                                                     | input encoding             |
/// |--------------------------------------------------------------|----------------------------|
/// | the title names LogC (LogC3), ACEScct, DaVinci Intermediate  | that encoding              |
/// | (or DWG), Cineon, Rec.709 (709), sRGB or linear              |                            |
/// | the title names an encoding the engine does not have         | Rec.709 / sRGB, the reason |
/// | (S-Log3, ACEScc, LogC4, V-Log, Log3G10, F-Log)               | names it as not supported  |
/// | otherwise, the input range reaches above 1                   | Linear                     |
/// | otherwise (a 0 to 1 range, nothing named)                    | Rec.709 / sRGB             |
///
/// The title is read as whole words, without regard to case, so "Warrior" names no curve and
/// "Nonlinear" is not "linear". A title of the form "X to Y" describes a conversion from X: only the
/// words before the first "to" are read. Where they name two encodings, the first named wins. A
/// user's choice on the asset replaces the proposal.
struct CubeLutColorProposal
{
    CubeLutInputEncoding InputEncoding = CubeLutInputEncoding::Rec709SRGB;
    std::string Reason; ///< e.g. "the title names ACEScct"
};

CubeLutColorProposal ProposeCubeLutColor(const CubeLutParseResult& parsed);

} // namespace GameEngine::Rendering
