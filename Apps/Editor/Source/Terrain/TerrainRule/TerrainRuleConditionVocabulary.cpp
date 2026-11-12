#include "Terrain/TerrainRule/TerrainRuleConditionVocabulary.h"

#include "Mathematics/Interpolation.h"
#include "Components/Terrain/Terrain.h"
#include "TerrainECS/TerrainDefaultSurfaceRules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace GameEngine::Editor::TerrainRuleVocabulary
{
namespace
{

// Slope in degrees spans the full quarter turn; a vertical face is 90.
constexpr float32 kSlopeDegreesMax = 90.0f;
// Latitude spans the equator (0) to either pole (90).
constexpr float32 kLatitudeDegreesMax = 90.0f;
// A Latitude band from at least this angle to a pole heads its rule "Polar".
constexpr float32 kPolarLatitudeMinDegrees = 60.0f;

// The Measures entries, in list order. The kind numbers are not contiguous: 3 is not a kind.
constexpr Kind kPlanarKinds[] = {Kind::SlopeDegrees, Kind::SlopeNormalized, Kind::HeightMeters, Kind::Noise};
constexpr Kind kPlanetKinds[] = {Kind::SlopeDegrees, Kind::SlopeNormalized, Kind::HeightMeters, Kind::Noise,
                                 Kind::LatitudeDegrees};

// Meters domain when the terrain's range is empty (no terrain bound to the
// inspector yet): the component's default Height Scale.
constexpr float32 kFallbackHeightMeters = 256.0f;

// Ceiling on the digits added to keep a small non-zero value from rendering as
// zero. Three covers a millimetre of terrain; past that the number is noise.
constexpr int kMaxExtraDecimals = 3;

std::string FormatFixed(float32 value, int decimals)
{
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(value));
    return std::string(buffer);
}

// True when a rendered number carries no significant digit — "0", "0.00", "-0".
bool ReadsAsZero(const std::string& text)
{
    return text.find_first_not_of("-+0.") == std::string::npos;
}

// A height band whose high edge is at the domain sentinel has no upper edge.
bool HasNoUpperLimit(const Components::TerrainRuleCondition& condition)
{
    return condition.Kind == Kind::HeightMeters && condition.Max >= TerrainECS::kDefaultHeightDomainMaxMeters;
}

bool ReachesThePole(const Components::TerrainRuleCondition& condition)
{
    return condition.Kind == Kind::LatitudeDegrees && condition.Max >= kLatitudeDegreesMax;
}

// "an" before a number spoken with a vowel sound (8, 11, 18, 80, 11,000), else "a".
const char* ArticleBeforeNumber(const std::string& number)
{
    const size_t integerDigits = number.find_first_not_of("0123456789");
    const size_t digitCount = integerDigits == std::string::npos ? number.size() : integerDigits;
    if (number.starts_with('8'))
        return "an";
    const bool elevenOrEighteen = number.starts_with("11") || number.starts_with("18");
    return elevenOrEighteen && digitCount % 3 == 2 ? "an" : "a";
}

} // namespace

int ConditionDecimals(Kind kind)
{
    // Degrees and meters carry one decimal; a [0, 1] band needs two before its
    // handles read as having moved at all. NOT magnitude-dependent: a threshold
    // made 4.8 print "5" and 3.2 print "3", and it made a caption say "34" next
    // to a field saying "34.4". Trailing zeros come off afterwards, so a whole
    // value still reads whole.
    switch (kind)
    {
    case Kind::SlopeDegrees:
    case Kind::HeightMeters:
    case Kind::LatitudeDegrees:
        return 1;
    case Kind::SlopeNormalized:
    case Kind::Noise:
        break;
    }
    return 2;
}

std::string_view ConditionKindLabel(Kind kind)
{
    switch (kind)
    {
    case Kind::SlopeDegrees:
        return "Slope (degrees)";
    case Kind::SlopeNormalized:
        return "Slope (0-1 raw)";
    case Kind::HeightMeters:
        return "Height (meters)";
    case Kind::Noise:
        return "Noise";
    case Kind::LatitudeDegrees:
        return "Latitude (degrees)";
    }
    return "Slope (degrees)";
}

std::string_view ConditionUnitSuffix(Kind kind)
{
    switch (kind)
    {
    case Kind::SlopeDegrees:
    case Kind::LatitudeDegrees:
        return "\xC2\xB0"; // U+00B0 DEGREE SIGN
    case Kind::HeightMeters:
        return " m";
    case Kind::SlopeNormalized:
    case Kind::Noise:
        break;
    }
    return "";
}

std::span<const Kind> ConditionKinds(Components::TerrainDomain domain)
{
    if (domain == Components::TerrainDomain::Spherical)
        return kPlanetKinds;
    return kPlanarKinds;
}

std::string_view ConditionKindHelp(Kind kind, Components::TerrainDomain domain)
{
    const bool planet = domain == Components::TerrainDomain::Spherical;
    switch (kind)
    {
    case Kind::SlopeDegrees:
        return "Surface angle from horizontal, 0 flat to 90 vertical - the unit to author slope in.";
    case Kind::SlopeNormalized:
        if (planet)
            return "0 on flat ground and 1 on a vertical face, rising faster as the ground steepens - "
                   "NOT an angle. Author slope in degrees instead.";
        return "The raw 1 - normal.Y, flattened by the terrain's Height Scale - NOT an angle, so "
               "the same number means a different steepness on every terrain. Author slope in "
               "degrees instead.";
    case Kind::HeightMeters:
        if (planet)
            return "Meters above the planet's radius: below 0 in the lowlands, above 0 on high ground. "
                   "Moving the planet does not move the band.";
        return "Meters above the terrain's base, not world Y - so the band holds still when the "
               "terrain entity moves, and a snow line at 400 m stays at 400 m when the "
               "mountain grows under it.";
    case Kind::Noise:
        if (planet)
            return "Value noise over the planet's surface from 0 to 1, with its own frequency and seed, "
                   "to break up the banding the slope and height kinds produce on their own.";
        return "Value noise over world XZ from 0 to 1, with its own frequency and seed, to break "
               "up the banding the slope and height kinds produce on their own.";
    case Kind::LatitudeDegrees:
        if (planet)
            return "Degrees from the equator: 0 at the equator, 90 at either pole.";
        return "Degrees from the equator: 0 at the equator, 90 at either pole. A flat terrain has no "
               "latitude and reads 0.";
    }
    return "";
}

ConditionDomain ConditionAuthoringDomain(Kind kind, ConditionDomain heightRangeMeters)
{
    switch (kind)
    {
    case Kind::SlopeDegrees:
        return {0.0f, kSlopeDegreesMax};
    case Kind::LatitudeDegrees:
        return {0.0f, kLatitudeDegreesMax};
    case Kind::HeightMeters:
        if (heightRangeMeters.Max > heightRangeMeters.Min)
            return heightRangeMeters;
        return {0.0f, kFallbackHeightMeters};
    case Kind::SlopeNormalized:
    case Kind::Noise:
        break;
    }
    return {0.0f, 1.0f};
}

namespace
{

// A refused condition's band has no unit; it reads at the precision of the [0, 1] kinds.
constexpr int kRefusedBandDecimals = 2;

std::string FormatTrimmed(float32 value, int decimals)
{
    std::string text = FormatFixed(value, decimals);

    // Trailing zeros come off: a caption reading "34.0" beside a field reading
    // "34.4" is the same disagreement the whole-number rounding produced, and a
    // caption reading "34.40" is noise. Whole values keep no decimal point.
    if (text.find('.') != std::string::npos)
    {
        text.erase(text.find_last_not_of('0') + 1);
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }

    // A non-zero value must never render as zero. Metres and degrees round to
    // whole numbers, so a 0.4 m feather would otherwise print "0 m" while the
    // sentence around it calls the band feathered — the reader is then told a
    // hard edge and a soft one in the same breath. Add precision until a
    // significant digit appears rather than pinning one precision per unit.
    if (value != 0.0f)
    {
        for (int extra = 1; extra <= kMaxExtraDecimals && ReadsAsZero(text); ++extra)
        {
            text = FormatFixed(value, decimals + extra);
            if (text.find('.') != std::string::npos)
            {
                text.erase(text.find_last_not_of('0') + 1);
                if (!text.empty() && text.back() == '.')
                    text.pop_back();
            }
        }
    }
    return text;
}

} // namespace

std::string FormatConditionValue(Kind kind, float32 value)
{
    return FormatTrimmed(value, ConditionDecimals(kind)) + std::string(ConditionUnitSuffix(kind));
}

namespace
{

// What the rule does with the weight the band just described — one sentence, one
// slot, three mutually exclusive answers, all in the same "The rule then ..."
// shape so the caption reads as one voice however the row is set up.
//
// Below full strength the row scales whatever its mode would have given, and the
// percentage is the whole story; naming the mode there as well would have to
// promise "fully replaces" for a row that lerps halfway.
//
// At full strength the two modes reach genuinely different ground, which is why
// "Full weight" alone over-promised. CompositeSplatTexel (TerrainSplatComposite.h)
// is the authority for both: BLEND adds this rule's weight to the texel and
// renormalizes, so a full-weight row against ground holding none of its material
// ends up level with it — the ground keeps half, it is not displaced. REPLACE
// lerps every channel toward the pure material, so at full weight nothing of the
// ground survives.
std::string RuleOutcomeSentence(const Components::TerrainSurfaceRule& rule)
{
    if (rule.Strength < 1.0f)
    {
        const int percent =
            static_cast<int>(std::lround(Math::Clamp01(rule.Strength) * 100.0f));
        return " The rule then scales this to " + std::to_string(percent) + "%.";
    }
    if (rule.Replace)
        return " The rule then fully replaces the ground where it applies.";
    return " The rule then blends this up to an equal share with the ground beneath.";
}

} // namespace

std::string ConditionBandSummary(const Components::TerrainRuleCondition& condition,
                                 const Components::TerrainSurfaceRule& owningRule)
{
    // A refused row keeps the default kind as a placeholder; its numbers are not in that unit.
    if (condition.RefusedKind != 0)
        return "Refused kind: this band (" + FormatTrimmed(condition.Min, kRefusedBandDecimals) + " to "
             + FormatTrimmed(condition.Max, kRefusedBandDecimals) + ") has no unit and applies as "
             "slope in degrees until a kind is picked under Measures.";

    const Kind kind = condition.Kind;
    const std::string low = FormatConditionValue(kind, condition.Min);
    const std::string high = FormatConditionValue(kind, condition.Max);
    const bool feathered = condition.Feather > 0.0f;
    const std::string reach = FormatConditionValue(kind, condition.Feather);
    const char* shape = condition.FalloffCurve == Curve::Smoothstep ? "smoothstep" : "linear";
    const std::string outcome = RuleOutcomeSentence(owningRule);

    // No plateau: the two ramps are the whole of the weight, and with no feather
    // there is no weight at all. Said first because it changes what the rest means.
    if (condition.Min > condition.Max)
    {
        if (!feathered)
            return "Empty band - nothing is between " + low + " and " + high + ", and with no "
                   "feather nothing ramps into it either. Drag the low handle below the high "
                   "one, or give the edges a feather." + outcome;
        return "Empty band - only the " + reach + " " + shape + " feather outside " + high
             + " and " + low + " carries any weight." + outcome;
    }

    // Min == Max is a peak, not a band: the plateau has no width, so describing
    // it as a range would read as a hard-edged nothing.
    if (condition.Min == condition.Max)
    {
        if (!feathered)
            return "Peaks at " + low + " with a hard edge, so only an exact match counts. "
                   "Widen the band or add a feather." + outcome;
        return "Peaks at " + low + ", falling to nothing over " + ArticleBeforeNumber(reach) + " " + reach
             + " " + shape + " feather on each side." + outcome;
    }

    if (HasNoUpperLimit(condition))
    {
        if (!feathered)
            return "Full weight from " + low + " up, with a hard edge below it." + outcome;
        return "Full weight from " + low + " up, falling to nothing over " + ArticleBeforeNumber(reach) + " "
             + reach + " " + shape + " feather below it." + outcome;
    }

    if (ReachesThePole(condition))
    {
        if (!feathered)
            return "Full weight from " + low + " to the pole, with a hard edge below it." + outcome;
        return "Full weight from " + low + " to the pole, falling to nothing over " + ArticleBeforeNumber(reach)
             + " " + reach + " " + shape + " feather below it." + outcome;
    }

    if (!feathered)
        return "Full weight from " + low + " to " + high + ", hard edges - nothing outside."
             + outcome;
    return "Full weight from " + low + " to " + high + ", falling to nothing over " + ArticleBeforeNumber(reach)
         + " " + reach + " " + shape + " feather past each edge." + outcome;
}

std::string RuleInertReason(const Components::TerrainSurfaceRule& rule)
{
    if (rule.Strength <= 0.0f)
        return "This rule is ignored: its Strength is 0, so every texel it matches gets no "
               "weight. Raise Strength above 0.";

    const uint32 conditionCount =
        std::min<uint32>(rule.ConditionCount, Components::kMaxTerrainRuleConditions);
    for (uint32 i = 0; i < conditionCount; ++i)
    {
        const Components::TerrainRuleCondition& condition = rule.Conditions[i];
        if (condition.Min > condition.Max && condition.Feather <= 0.0f)
        {
            return "This rule is ignored: condition " + std::to_string(i + 1) + " has its low "
                   "handle above its high one and no feather, so no value can satisfy it and "
                   "the whole rule multiplies to zero. Drag the handles back into order, or "
                   "give the condition a feather.";
        }
    }

    return {};
}

std::string ConditionBandRangeText(const Components::TerrainRuleCondition& condition)
{
    if (condition.RefusedKind != 0)
        return FormatTrimmed(condition.Min, kRefusedBandDecimals) + " - "
             + FormatTrimmed(condition.Max, kRefusedBandDecimals);
    const std::string high = HasNoUpperLimit(condition) ? std::string(kNoUpperLimitText)
                                                        : FormatConditionValue(condition.Kind, condition.Max);
    return FormatConditionValue(condition.Kind, condition.Min) + " - " + high;
}

std::string RefusedKindLabel(uint8 refusedKind)
{
    return "Refused (kind " + std::to_string(refusedKind) + ")";
}

std::string FormatHeightRange(ConditionDomain heightRangeMeters)
{
    return FormatConditionValue(Kind::HeightMeters, heightRangeMeters.Min) + " to "
         + FormatConditionValue(Kind::HeightMeters, heightRangeMeters.Max);
}

std::string HeightRangeNote(ConditionDomain heightRangeMeters, Components::TerrainDomain domain)
{
    if (heightRangeMeters.Max <= heightRangeMeters.Min)
        return {};
    if (domain == Components::TerrainDomain::Spherical)
        return "Height conditions are meters above the planet's radius: " + FormatHeightRange(heightRangeMeters)
             + ".";
    return "Height conditions are meters above this terrain's base: " + FormatHeightRange(heightRangeMeters) + ".";
}

std::string_view RuleHeadingQualifier(const Components::TerrainSurfaceRule& rule)
{
    const uint32 conditionCount = std::min<uint32>(rule.ConditionCount, Components::kMaxTerrainRuleConditions);
    for (uint32 i = 0; i < conditionCount; ++i)
    {
        const Components::TerrainRuleCondition& condition = rule.Conditions[i];
        if (ReachesThePole(condition) && condition.Min >= kPolarLatitudeMinDegrees)
            return "Polar ";
    }
    return "";
}

Components::TerrainRuleCondition MakeDefaultCondition(Kind kind, ConditionDomain heightRangeMeters)
{
    const ConditionDomain domain = ConditionAuthoringDomain(kind, heightRangeMeters);
    Components::TerrainRuleCondition condition{};
    condition.Kind = kind;
    condition.Min = domain.Min;
    condition.Max = domain.Max;
    return condition;
}

std::string_view UnconditionalRuleNote()
{
    return "No conditions, so this rule covers the whole volume at full strength. "
           "Add a condition to restrict where it applies.";
}

std::string_view SurfaceRulesSectionHelp()
{
    return "Rules apply top to bottom. Within a rule, conditions AND together and their feathers "
           "multiply. Rules composite into the same splat as the effects earlier in this volume's "
           "stack, so at full strength a rule takes ground a paint layer put down - outright in "
           "Replace, down to an equal share in Blend.";
}

} // namespace GameEngine::Editor::TerrainRuleVocabulary
