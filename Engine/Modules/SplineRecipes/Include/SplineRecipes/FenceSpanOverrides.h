#pragma once

#include "Components/Spline/SplineFence.h"
#include "Types/Types.h"

#include <array>
#include <span>
#include <utility>

namespace GameEngine::SplineRecipes
{

// The override a fence recipe carries for one span, or null: the first filled
// slot naming it, which is the one the layout applies.
const Components::SplineSpanOverride* FindSpanOverride(const Components::SplineFence& recipe,
                                                       uint32 point, uint32 ordinal);

// How many slots of the recipe's override table are filled.
uint32 CountSpanOverrides(const Components::SplineFence& recipe);

// Sets what one span draws: a gate or a pinned piece from `slot` of its pool,
// or, for None, the seeded wall again. The span's existing override is
// rewritten in place — every override naming it is cleared for None — and a
// new one takes the first empty slot. Returns false, with the recipe
// untouched, when a new override is asked for and every slot is filled.
bool SetSpanOverride(Components::SplineFence& recipe, uint32 point, uint32 ordinal,
                     Components::SplineSpanOverrideKind kind, uint8 slot);

// The kind of part an override draws: Gate parts for a gate, Span parts for an explicit part.
Components::SplineFencePartKind SpanOverridePartKind(Components::SplineSpanOverrideKind kind);

// The edits of the override list. Each first moves the filled overrides to the front of the table,
// in order, so that row n of the list is slot n. The layout uses the first of two overrides on one
// span, so the order is kept.
void CompactSpanOverrides(Components::SplineFence& recipe);

// Adds a copy of the last override (an empty list: a gate if a Gate part has a mesh) on the first free
// span of `standingSpans`, the (point, span) pairs the last build placed, after the last override,
// else before it, else on the next free span. False, with the recipe untouched, when the table is full.
bool AddSpanOverride(Components::SplineFence& recipe,
                     std::span<const std::pair<uint32, uint32>> standingSpans);
void RemoveSpanOverride(Components::SplineFence& recipe, uint32 row);
void SwapSpanOverrides(Components::SplineFence& recipe, uint32 row, uint32 otherRow);
// A change of kind draws from the other pool, so the override takes that pool's first part.
void SetSpanOverrideKind(Components::SplineFence& recipe, uint32 row, Components::SplineSpanOverrideKind kind);

// A row of the Parts list that holds no part.
inline constexpr uint32 kNoPartRow = Components::kSplineFenceMaxParts;
// The PoolSlot of an override whose part an edit took away. It is past every pool, so the
// override shows a missing part and no other part takes its place.
inline constexpr uint8 kRemovedPartSlot = static_cast<uint8>(Components::kSplineFenceMaxParts);

// The row of the Parts list that each override slot draws, or kNoPartRow.
using SpanOverridePartRows = std::array<uint32, Components::kSplineFenceMaxSpanOverrides>;
// The new row of each part after one edit of the Parts list, by old row; kNoPartRow for a removed part.
using PartRowMoves = std::array<uint32, Components::kSplineFenceMaxParts>;

SpanOverridePartRows PartRowsOfSpanOverrides(const Components::SplineFence& recipe);
// Points each override at the part it drew before an edit of the Parts list, at the new row
// that `moved` gives. A removed part, or one that left its pool, leaves its override with no part.
void KeepSpanOverridesOnTheirParts(Components::SplineFence& recipe, const SpanOverridePartRows& drawn,
                                   const PartRowMoves& moved);

} // namespace GameEngine::SplineRecipes
