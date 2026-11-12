#pragma once

#include "Components/Spline/SplinePaving.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector4.h"
#include "SplineGeometry/AreaBoolean.h"
#include "Types/Types.h"

#include <optional>
#include <span>
#include <vector>

namespace GameEngine
{
enum class MaterialPatternSymmetry : uint8_t;
}

namespace GameEngine::SplineRecipes
{

struct LidPieces;
struct PavingRoadUV;

// What a seam reads of the paving material: the symmetry of its pattern and its albedo tiling, in
// texture tiles per UV unit on U and on V.
struct PavingSeamMaterial
{
    MaterialPatternSymmetry Symmetry{};
    Mathematics::Vector2 TilesPerUV{1.0f, 1.0f};

    bool operator==(const PavingSeamMaterial&) const = default;
};

// How one road meets the fields it yields to: from its own paving, else from the paving that paves
// the junction.
struct PavingRoadSeam
{
    Components::SplinePavingSeam Look = Components::SplinePavingSeam::Blend;
    float32 BandTiles = 2.0f;
    // Meters; 0 takes the look's own width.
    float32 CourseWidth = 0.0f;
    // A course drawn with its own material is not tinted and tiles once per square of its width. A
    // course over the paving's material takes the paving's tile size.
    bool HasCourseMaterial = false;
};

// The lid's own mapping: U and V in world meters along the owner's ground axes, plus the pattern's phase.
struct PavingOwnMapping
{
    Mathematics::Vector2 UAxis{1.0f, 0.0f};
    Mathematics::Vector2 VAxis{0.0f, 1.0f};
    Mathematics::Vector2 Phase{};
};

// A UV field's rise per meter along the ground's x and z, in texture tiles, for U and for V.
struct SeamFieldFrame
{
    Mathematics::Vector2 GradientU{};
    Mathematics::Vector2 GradientV{};
};

// A move a symmetric pattern draws the same lines under: Swap exchanges U and V, then Sign flips each.
struct SeamAxisMove
{
    bool Swap = false;
    Mathematics::Vector2 Sign{1.0f, 1.0f};

    Mathematics::Vector2 Apply(const Mathematics::Vector2& tiles) const;
};

// The move `symmetry` allows (Square: quarter turns and mirrors; Rectangular: half turns and mirrors)
// that brings the axes of `other` within 30 degrees of `yielding`'s, each pitch equal within 1 percent.
// None: no such move, and the seam takes the crossfade.
std::optional<SeamAxisMove> SolveSeamAxisMove(MaterialPatternSymmetry symmetry, const SeamFieldFrame& yielding,
                                              const SeamFieldFrame& other);

// The validity rule's last term: along the whole seam, the moved field stays less than this many tiles
// from the yielding field on each axis.
inline constexpr float32 kSeamMaxOffsetTiles = 0.5f;

// A vertex's attributes under its mapping. The tint's alpha is the weight of SecondUV in a crossfade band.
struct PavingSeamVertex
{
    Mathematics::Vector2 UV{};
    Mathematics::Vector2 SecondUV{};
    Mathematics::Vector4 Tint{1.0f, 1.0f, 1.0f, 1.0f};
    // The direction U rises on the ground, unit length: the tangent's direction.
    Mathematics::Vector2 UDirection{1.0f, 0.0f};
};

// The seams of one lid: each road yields to the roads before it in claim order and to the lid's own
// mapping across an Area outline. A band or a course lies inside the yielding road's lid. Mappings for
// R roads: road r is r, the own mapping kOwnMapping, road r's course R + r, its crossfade band 2R + r.
class PavingSeams
{
public:
    PavingSeams(std::span<const PavingRoadUV> roads, std::span<const PavingRoadSeam> seams,
                std::span<const SplineGeometry::AreaRing> outlines, const PavingOwnMapping& own,
                const PavingSeamMaterial& material);

    // Clips each road's lid on its seam lines, then cuts its band or its course. The part of a road's
    // lid past a seam takes the field across it.
    void Cut(LidPieces& pieces);

    PavingSeamVertex VertexOf(uint32 mapping, const Mathematics::Vector2& xz) const;

    uint32 CourseMapping(uint32 road) const { return RoadCount() + road; }
    uint32 BandMapping(uint32 road) const { return 2u * RoadCount() + road; }
    // Whether Cut gave the band of `road`'s seam with `partner` (a road, or kOwnMapping) the projected form.
    bool IsProjected(uint32 road, uint32 partner) const;
    // Whether `road`'s Blend asks for a band at its seam with `partner` (a road, or kOwnMapping) but draws
    // as a hard cut: its strip ends near that seam, so the form the seam takes has no depth. Call after Cut.
    bool DrawsHardCut(uint32 road, uint32 partner) const;

private:
    // A field is a road's index, or RoadCount() for the lid's own mapping.
    struct NearestSeam
    {
        uint32 Partner = 0;
        float32 Meters = 0.0f;
    };
    struct Projection
    {
        SeamAxisMove Move;
        Mathematics::Vector2 ShiftTiles{};
    };
    struct SeamAlong
    {
        float32 Meters = 0.0f;
        Mathematics::Vector2 Direction{1.0f, 0.0f};
    };

    uint32 RoadCount() const { return static_cast<uint32>(m_Roads.size()); }
    uint32 OwnField() const { return RoadCount(); }
    uint32 MappingOfField(uint32 field) const;
    Mathematics::Vector2 FieldUV(uint32 field, const Mathematics::Vector2& xz) const;
    Mathematics::Vector2 FieldTiles(uint32 field, const Mathematics::Vector2& xz) const;
    SeamFieldFrame FrameOf(uint32 field, const Mathematics::Vector2& xz) const;
    // Meters from `xz` to the seam of `partner`, positive on the yielding road's side.
    float32 SeamMeters(uint32 partner, const Mathematics::Vector2& xz) const;
    NearestSeam NearestSeamOf(uint32 road, const Mathematics::Vector2& xz) const;
    std::vector<float32> NearestSeamMeters(const LidPieces& pieces, uint32 road) const;
    // The depth of `road`'s band at its seam with the field `partner`: for the projected form, and the
    // crossfade's shorter depth.
    float32 BandMeters(uint32 road, uint32 partner) const { return m_BandMeters[SlotOf(road, partner)]; }
    float32 CrossfadeMeters(uint32 road, uint32 partner) const { return m_CrossfadeMeters[SlotOf(road, partner)]; }
    // Per partner field: meters from its seam to the nearest of `road`'s strip caps whose nearest seam
    // it is, on the yielding side. A partner no cap is nearest to reads the float's maximum.
    std::vector<float32> CapMeters(uint32 road) const;
    float32 CourseMeters(uint32 road, uint32 partner) const;
    // The course's UV per meter along and across the seam.
    Mathematics::Vector2 CourseUVPerMeter(uint32 road, uint32 partner, float32 width) const;
    SeamAlong AlongSeam(uint32 road, uint32 partner, const Mathematics::Vector2& xz) const;
    // Whether `road`'s course draws the paving's own stones turned across the seam: a Stone Course with
    // no Course Material.
    bool TurnsStones(uint32 road) const;
    // The unit direction across `partner`'s seam at `xz`, toward the yielding side, square to `along`.
    Mathematics::Vector2 AcrossSeam(uint32 partner, const Mathematics::Vector2& xz,
                                    const Mathematics::Vector2& along) const;
    Mathematics::Vector2 BlendUV(uint32 road, const Mathematics::Vector2& xz) const;
    std::optional<Mathematics::Vector2> CrossingOf(uint32 road, uint32 partner) const;
    size_t SlotOf(uint32 road, uint32 partner) const;
    bool HasPartner(uint32 road, uint32 partner) const;

    void ClipOnSeams(LidPieces& pieces, uint32 road) const;
    void SolveProjections(const LidPieces& pieces, uint32 road);
    void CutBand(LidPieces& pieces, uint32 road) const;
    void CutCourse(LidPieces& pieces, uint32 road) const;

    std::span<const PavingRoadUV> m_Roads;
    std::span<const PavingRoadSeam> m_Seams;
    std::span<const SplineGeometry::AreaRing> m_Outlines;
    PavingOwnMapping m_Own;
    PavingSeamMaterial m_Material;
    // Per road and partner field (SlotOf): the band's depth, which ends at the road's strip caps nearest
    // that seam, so the lid meets each strip in the road's own field.
    std::vector<float32> m_BandMeters;
    std::vector<float32> m_CrossfadeMeters;
    // Per road and partner field: the projection where the rule holds.
    std::vector<std::optional<Projection>> m_Projections;
};

} // namespace GameEngine::SplineRecipes
