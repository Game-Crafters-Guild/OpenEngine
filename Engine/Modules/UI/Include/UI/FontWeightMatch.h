#pragma once

namespace GameEngine::UI
{

/// Cost of drawing a request for CSS `font-weight: requestedWeight` with a face of weight
/// `candidateWeight`, in the matching order of CSS Fonts Level 4 §5.2. Of the faces a family
/// has, the one with the lowest cost is the one to draw with:
/// - above 500, the nearest heavier face first, then the nearest lighter one;
/// - from 400 to 500, the nearest face up to 500 first, then lighter, then heavier;
/// - below 400, the nearest lighter face first, then heavier.
/// A face in a preferred direction always costs less than one in a fallback direction, and
/// distance orders the faces inside a direction. Every cost is below 2000, so a caller can add
/// a larger penalty for another property (a style mismatch) and keep it dominant.
/// The requested weight is clamped to the CSS range 1..1000.
constexpr int FontWeightMatchCost(int requestedWeight, int candidateWeight)
{
    const int requested = requestedWeight < 1 ? 1 : (requestedWeight > 1000 ? 1000 : requestedWeight);
    const int diff = candidateWeight - requested; // positive: the face is heavier
    if (requested > 500)
        return diff >= 0 ? diff : 1000 - diff;
    if (requested >= 400)
    {
        if (diff >= 0 && candidateWeight <= 500)
            return diff;
        if (diff < 0)
            return 500 - diff;
        return 1000 + diff;
    }
    return diff <= 0 ? -diff : 1000 + diff;
}

} // namespace GameEngine::UI
