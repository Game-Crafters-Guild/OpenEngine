#pragma once

#include "PageStreaming/PageStore/PageStoreFormat.h"

#include <array>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{

/// The finest step of an absolute store's level: 2^-20 of the source's units (under a micrometer in
/// meters), finer than any elevation source measures.
inline constexpr int32 kHeightStepExponentMin = -20;
/// The share of a level's pages the step rule lets store 32-bit integers: 1 percent, and at least
/// one page, so a level under 100 pages also lets its steepest page escape. The step is the finest
/// that keeps the rest 16-bit, so a few steep pages cost bytes, not every page's precision. A level
/// of one page has no other page to keep: that page sets the step.
inline constexpr float64 kHeightWidePageShare = 0.01;

/// The exponent of the finest power-of-two step at which values spanning `range` fit a page's 16-bit
/// words, the rounding of the base and the top included; at least kHeightStepExponentMin.
int32 SixteenBitStepExponent(float32 range);

/// Chooses each level's step of an absolute store from its level-0 samples, pushed in row-major
/// order in chunks of any size. A level's step is the finest power of two at which at most
/// kHeightWidePageShare of its pages, and at least one, never every page, need 32-bit integers, judged by each page's range of level-0
/// values under its footprint (the bound PagePyramidBuilder gives the page's base), never finer
/// than kHeightStepExponentMin, and never so fine that the largest height passes kHeightIntegerMax
/// steps. The steps are a function of the samples and the grid alone, so every cook of one source
/// chooses them alike. A store of several faces pushes them one after another, each in row-major
/// order, and a level's pages are every face's. Memory: two floats per level-0 page.
class HeightStepPlanner
{
public:
    HeightStepPlanner(uint32 samplesX, uint32 samplesZ, uint32 faces);

    void Push(std::span<const float32> samples);

    /// Each level's step exponent once every sample is pushed; levels past the store's are 0.
    std::array<int8, kPageStoreMaxLevels> StepExponents() const;

private:
    void PushRowSpan(std::span<const float32> samples, uint32 firstX);

    std::vector<PageStoreLevel> m_Levels;
    std::vector<float32> m_PageMin; // per level-0 page: the lowest sample under its footprint
    std::vector<float32> m_PageMax;
    float32 m_Magnitude = 0.0f; // the largest |sample|
    uint32 m_Faces = 0;
    std::size_t m_FacePages = 0; // level-0 pages of one face
    uint32 m_X = 0;              // where the next pushed sample lands
    uint32 m_Z = 0;
    uint32 m_Face = 0;
};

} // namespace GameEngine::PageStreaming
