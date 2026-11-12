#pragma once

#include "PageStreaming/PageAddress.h"
#include "TerrainECS/Planet/PlanetChart.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::TerrainECS
{

/// A level-0 page a planet page body reads: its kPageSampleCount heights (meters above the reference
/// radius, apron included) with the planet's page layer applied.
struct PlanetPageBodyPage
{
    PageStreaming::PageAddress Address;
    std::vector<float32> Heights;
};

/// What one planet page body's fill reads. Owned by the fill job: nothing in it points at main-thread
/// state.
struct PlanetPageBodyInput
{
    PlanetChartLayout Layout;
    float64 ReferenceRadius = 0.0;
    PageStreaming::PageAddress Page; ///< the body's level-0 page
    std::vector<PlanetPageBodyPage> Pages; ///< AppendPlanetPageBodyReads of Page, each once
};

/// Fills `samples` (kFinestPageGridSamples squared, row-major, X along the chart's TangentX) with the
/// body's heights: the meters along the chart's Up from its plane to the drawn surface, over the body's
/// plane rectangle (PlanetPageBodyRectOf). The drawn surface is the page heights, bilinear on the page
/// lattice. Pure; safe on any thread. False when a sample lands on a page `input` lacks.
bool FillPlanetPageBody(const PlanetPageBodyInput& input, std::span<float32> samples);

} // namespace GameEngine::TerrainECS
