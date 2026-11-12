#pragma once

#include "Components/Terrain/TerrainModifierEffects.h"
#include "Mathematics/Vector3.h"
#include "PageStreaming/Height/HeightPageLayer.h"
#include "PageStreaming/PageAddress.h"
#include "Types/Types.h"

#include <memory>
#include <span>
#include <vector>

namespace GameEngine::PageStreaming
{
class GeneratedHeightPageProvider;
struct GeneratedPagePlanes;
} // namespace GameEngine::PageStreaming

namespace GameEngine::TerrainECS
{

/// The composed height window of one planet page: the generated heights (FillPlanetWindow) plus each
/// layer (ApplyToWindow), with each sample's unit direction. kPageSurfaceWindowMarginSamples past each
/// side of the owned samples, row-major.
struct PlanetSurfaceWindow
{
    uint32 Stride = 0;
    std::vector<float32> Heights; ///< meters above the reference radius
    std::vector<Mathematics::Vector3> Directions;
};

/// The unit surface normal at window sample (column, row), 1 to Stride - 2, from central differences
/// (the CPU twin of cbt_planet_page.glsl CBT_PlanetPageSurfaceAt). The sample's direction where a
/// tangent is zero (past a cube corner).
Mathematics::Vector3 PlanetWindowNormal(const PlanetSurfaceWindow& window, uint32 column, uint32 row,
                                        float64 referenceRadius);

/// The splat and normal pages of a planet's surface, made from the page's height window and the rule
/// rows. Thread-safe: a page is a pure function of its address. The provider holds a copy of each layer,
/// so the owner may write a layer while a fill runs.
class PlanetSurfacePageProvider
{
public:
    /// Copies `layers`, added in order (the modifiers, then the overlay); a null layer is skipped. The
    /// copy costs HeightPageLayer::Bytes of each layer on the calling thread.
    PlanetSurfacePageProvider(std::shared_ptr<const PageStreaming::GeneratedHeightPageProvider> heights,
                              std::span<const PageStreaming::HeightPageLayer* const> layers,
                              const Components::TerrainSurfaceRulesEffect& rules, float64 referenceRadius);

    /// The page source planes of `provider` (PageSource::GeneratedPlanes): the splat (Rgba8), then the
    /// normal (Bc5), on six faces of the heights' lattice. Each fill holds the provider.
    static std::shared_ptr<const PageStreaming::GeneratedPagePlanes> MakePagePlanes(
        std::shared_ptr<const PlanetSurfacePageProvider> provider);

    /// Fills the composed height window of page `address`. False outside the pyramid.
    bool FillWindow(const PageStreaming::PageAddress& address, PlanetSurfaceWindow& out) const;

    /// Fills page `address`'s splat (Rgba8, the rows' Replace composite) and normal (Rg8 at the Bc5
    /// stride, in the face's tangent frame: x along the face's X axis, z = x cross the direction).
    /// False outside the pyramid or when a span is not its plane's size.
    bool FillPage(const PageStreaming::PageAddress& address, std::span<uint8> splat, std::span<uint8> normal) const;

private:
    std::shared_ptr<const PageStreaming::GeneratedHeightPageProvider> m_Heights;
    std::vector<PageStreaming::HeightPageLayer> m_Layers;
    Components::TerrainSurfaceRulesEffect m_Rules;
    float64 m_ReferenceRadius = 0.0;
};

} // namespace GameEngine::TerrainECS
