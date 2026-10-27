#include "SplineRecipes/FenceSpanOverrides.h"

#include "SplineRecipes/FenceRecipeLayout.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace GameEngine::SplineRecipes
{
namespace
{

using Components::kSplineFenceMaxParts;
using Components::kSplineFenceMaxSpanOverrides;
using Components::SplineFence;
using Components::SplineFencePart;
using Components::SplineFencePartKind;
using Components::SplineSpanOverride;
using Components::SplineSpanOverrideKind;
using SpanAddress = std::pair<uint32, uint32>;

bool Names(const SplineSpanOverride& entry, uint32 point, uint32 ordinal)
{
    return entry.Kind != SplineSpanOverrideKind::None && entry.PointIndex == point &&
           entry.SpanOrdinal == ordinal;
}

// True when `part` is in the pool of `kind`: that kind, with a mesh.
bool InPool(const SplineFencePart& part, SplineFencePartKind kind)
{
    return kind != SplineFencePartKind::None && part.Kind == kind && !part.Mesh.IsNull();
}

// The row of the part at `slot` of the pool of `kind`, or kNoPartRow.
uint32 RowOfPoolSlot(const SplineFence& recipe, SplineFencePartKind kind, uint32 slot)
{
    uint32 seen = 0;
    for (uint32 row = 0; row < kSplineFenceMaxParts; ++row)
    {
        if (InPool(recipe.Parts[row], kind) && seen++ == slot)
            return row;
    }
    return kNoPartRow;
}

// Of the standing spans with no override, the first at or after `after`, else the first.
std::optional<SpanAddress> FreeStandingSpan(const SplineFence& recipe, std::span<const SpanAddress> standingSpans,
                                            SpanAddress after)
{
    std::optional<SpanAddress> firstAfter;
    std::optional<SpanAddress> first;
    for (const SpanAddress& span : standingSpans)
    {
        if (span.second > std::numeric_limits<uint16>::max() || FindSpanOverride(recipe, span.first, span.second))
            continue;
        if (!first || span < *first)
            first = span;
        if (span >= after && (!firstAfter || span < *firstAfter))
            firstAfter = span;
    }
    return firstAfter ? firstAfter : first;
}

} // namespace

const SplineSpanOverride* FindSpanOverride(const Components::SplineFence& recipe, uint32 point,
                                           uint32 ordinal)
{
    for (const SplineSpanOverride& entry : recipe.Overrides)
    {
        if (Names(entry, point, ordinal))
            return &entry;
    }
    return nullptr;
}

uint32 CountSpanOverrides(const Components::SplineFence& recipe)
{
    uint32 count = 0;
    for (const SplineSpanOverride& entry : recipe.Overrides)
        count += entry.Kind != SplineSpanOverrideKind::None ? 1u : 0u;
    return count;
}

bool SetSpanOverride(Components::SplineFence& recipe, uint32 point, uint32 ordinal,
                     SplineSpanOverrideKind kind, uint8 slot)
{
    // The table stores the ordinal in 16 bits; a span past that is not one a
    // fence of 4096 pieces can have.
    if (ordinal > std::numeric_limits<uint16>::max())
        return false;

    if (kind == SplineSpanOverrideKind::None)
    {
        for (SplineSpanOverride& entry : recipe.Overrides)
        {
            if (Names(entry, point, ordinal))
                entry = SplineSpanOverride{};
        }
        return true;
    }

    SplineSpanOverride* target = nullptr;
    for (SplineSpanOverride& entry : recipe.Overrides)
    {
        if (Names(entry, point, ordinal))
        {
            target = &entry;
            break;
        }
    }
    if (!target)
    {
        for (SplineSpanOverride& entry : recipe.Overrides)
        {
            if (entry.Kind == SplineSpanOverrideKind::None)
            {
                target = &entry;
                break;
            }
        }
    }
    if (!target)
        return false;
    target->PointIndex = point;
    target->SpanOrdinal = static_cast<uint16>(ordinal);
    target->Kind = kind;
    target->PoolSlot = slot;
    return true;
}

SplineFencePartKind SpanOverridePartKind(SplineSpanOverrideKind kind)
{
    switch (kind)
    {
    case SplineSpanOverrideKind::Gate:
        return SplineFencePartKind::Gate;
    case SplineSpanOverrideKind::ExplicitPart:
        return SplineFencePartKind::Span;
    case SplineSpanOverrideKind::None:
        break;
    }
    return SplineFencePartKind::None;
}

void CompactSpanOverrides(SplineFence& recipe)
{
    uint32 count = 0;
    for (const SplineSpanOverride& entry : recipe.Overrides)
    {
        if (entry.Kind != SplineSpanOverrideKind::None)
            recipe.Overrides[count++] = entry;
    }
    for (uint32 i = count; i < kSplineFenceMaxSpanOverrides; ++i)
        recipe.Overrides[i] = SplineSpanOverride{};
}

bool AddSpanOverride(SplineFence& recipe, std::span<const SpanAddress> standingSpans)
{
    CompactSpanOverrides(recipe);
    const uint32 count = CountSpanOverrides(recipe);
    if (count >= kSplineFenceMaxSpanOverrides)
        return false;
    SplineSpanOverride added{0u, 0u, SplineSpanOverrideKind::ExplicitPart, 0u};
    SpanAddress after{0u, 0u};
    if (count > 0u)
    {
        added = recipe.Overrides[count - 1u];
        after = {added.PointIndex, added.SpanOrdinal + 1u};
    }
    else if (FencePoolOf(recipe, SplineFencePartKind::Gate).Count > 0u)
    {
        added.Kind = SplineSpanOverrideKind::Gate;
    }

    if (const std::optional<SpanAddress> standing = FreeStandingSpan(recipe, standingSpans, after))
    {
        added.PointIndex = standing->first;
        added.SpanOrdinal = static_cast<uint16>(standing->second);
    }
    else
    {
        added.PointIndex = after.first;
        uint32 ordinal = std::min<uint32>(after.second, std::numeric_limits<uint16>::max());
        while (FindSpanOverride(recipe, added.PointIndex, ordinal) && ordinal < std::numeric_limits<uint16>::max())
            ++ordinal;
        added.SpanOrdinal = static_cast<uint16>(ordinal);
    }
    recipe.Overrides[count] = added;
    return true;
}

void RemoveSpanOverride(SplineFence& recipe, uint32 row)
{
    CompactSpanOverrides(recipe);
    if (row >= kSplineFenceMaxSpanOverrides)
        return;
    recipe.Overrides[row] = SplineSpanOverride{};
    CompactSpanOverrides(recipe);
}

void SwapSpanOverrides(SplineFence& recipe, uint32 row, uint32 otherRow)
{
    CompactSpanOverrides(recipe);
    const uint32 count = CountSpanOverrides(recipe);
    if (row < count && otherRow < count)
        std::swap(recipe.Overrides[row], recipe.Overrides[otherRow]);
}

void SetSpanOverrideKind(SplineFence& recipe, uint32 row, SplineSpanOverrideKind kind)
{
    CompactSpanOverrides(recipe);
    if (row >= CountSpanOverrides(recipe) || recipe.Overrides[row].Kind == kind)
        return;
    recipe.Overrides[row].Kind = kind;
    recipe.Overrides[row].PoolSlot = 0u;
}

SpanOverridePartRows PartRowsOfSpanOverrides(const SplineFence& recipe)
{
    SpanOverridePartRows rows{};
    for (uint32 slot = 0; slot < kSplineFenceMaxSpanOverrides; ++slot)
    {
        const SplineSpanOverride& entry = recipe.Overrides[slot];
        rows[slot] = RowOfPoolSlot(recipe, SpanOverridePartKind(entry.Kind), entry.PoolSlot);
    }
    return rows;
}

void KeepSpanOverridesOnTheirParts(SplineFence& recipe, const SpanOverridePartRows& drawn, const PartRowMoves& moved)
{
    for (uint32 slot = 0; slot < kSplineFenceMaxSpanOverrides; ++slot)
    {
        if (drawn[slot] == kNoPartRow)
            continue;
        SplineSpanOverride& entry = recipe.Overrides[slot];
        const SplineFencePartKind kind = SpanOverridePartKind(entry.Kind);
        const uint32 row = moved[drawn[slot]];
        if (row == kNoPartRow || !InPool(recipe.Parts[row], kind))
        {
            entry.PoolSlot = kRemovedPartSlot;
            continue;
        }
        entry.PoolSlot = static_cast<uint8>(std::count_if(recipe.Parts, recipe.Parts + row,
                                                          [kind](const SplineFencePart& part)
                                                          { return InPool(part, kind); }));
    }
}

} // namespace GameEngine::SplineRecipes
