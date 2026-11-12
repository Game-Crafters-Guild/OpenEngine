// Deterministic camera-relative grass placement arithmetic.
//
// MIRROR OBLIGATION: byte-for-byte counterpart of
// Engine/Modules/TerrainGrass/Include/TerrainGrass/GrassPlacementModel.h. The CPU sizes the
// dispatch and the instance budget from these same slot counts, so a drift mis-sizes the buffer
// this shader writes into. Change one, change the other, and re-run GrassPlacementModelTests.

#ifndef GRASS_PLACEMENT_GLSL
#define GRASS_PLACEMENT_GLSL

const float kGrassCellSize = 8.0;
const float kGrassCellArea = kGrassCellSize * kGrassCellSize;
const uint kGrassMaxSlotsPerCell = 16384u;
const float kGrassMinGrowth = 0.02;
const float kGrassGrowthSlotFraction = 0.15;
const uint kGrassLodCount = 2u;

// Largest cell window the dispatch can cover, and the cell-plan array both kernels index.
const uint kGrassMaxCellHalfSpan = 127u;
const uint kGrassMaxCells = (kGrassMaxCellHalfSpan * 2u + 1u) * (kGrassMaxCellHalfSpan * 2u + 1u);

// One cell's share of the instance pool, decided by the plan kernel and executed by the emit
// kernel: slot `s` of this cell always lands at `Base + s`. Nothing else assigns an index, which
// is what makes the placed set AND its order a pure function of the inputs.
struct GrassCellPlan
{
    uint Slots;           // slots this cell reserves; 0 means the cell places nothing
    uint SlotsWantedBits; // classify: floatBitsToUint of the cell center's ground Y, which the plan reads;
                          // plan: floatBitsToUint of the unrounded count, which the growth ramp reads
    uint LodAndTerrain;   // valid bit | LOD bit | terrain row
    uint Base;            // first reserved slot, counted from this LOD's end of the pool
};

// Distance rings the budget fit histograms over. A cell is culled past Bounds.w, which the CPU
// caps at the largest fitted radius plus one cell, so ring 128 is the last one reachable.
const uint kGrassMaxRings = kGrassMaxCellHalfSpan + 2u;
// The histogram sits ahead of the cell array in the same buffer, padded so the struct array that
// follows keeps its 16-byte alignment.
const uint kGrassRingHistogramWords = 132u;
// The first padding word holds floatBitsToUint of the ground Y under the camera (classify writes it).
const uint kGrassCameraGroundWord = kGrassMaxRings;

const uint kGrassCellValidBit = 0x80000000u;
const uint kGrassCellLodShift = 30u;
const uint kGrassCellTerrainMask = 0x3FFFFFFFu;

// GE_SHARED_GRASS_CELL_BOX_BEGIN
// Every point of a cell lies within this distance of one of its four corners or its center.
const float kGrassCellSampleCover = 4.0;
// The steepest ground the box assumes between its samples is at least 45 degrees.
const float kGrassCellMinPadSlope = 1.0;
// The largest per-blade jitter factors the place kernel applies. Mirror: Terrain::TerrainGrassBladeReach
// (TerrainTypes.h), which sizes the reach every cell box grows by from them.
const float kGrassBladeHeightJitterMax = 1.35f;
const float kGrassBladeWidthJitterMax = 1.25f;
const float kGrassCardSizeJitterMax = 1.15f;

// The ground's vertical range over an 8 m cell from its corner heights and its center height.
// Bound: the range holds every ground point whose slope to its nearest sample is at most
// max(45 degrees, the steepest slope between the samples).
vec2 GrassCellGroundRange(float h00, float h10, float h01, float h11, float hc)
{
    float lo = min(min(min(h00, h10), min(h01, h11)), hc);
    float hi = max(max(max(h00, h10), max(h01, h11)), hc);
    float edgeRise = max(max(abs(h10 - h00), abs(h11 - h01)), max(abs(h01 - h00), abs(h11 - h10)));
    float centerRise = max(max(abs(hc - h00), abs(hc - h10)), max(abs(hc - h01), abs(hc - h11)));
    float centerToCorner = length(vec2(kGrassCellSampleCover, kGrassCellSampleCover));
    float slope = max(edgeRise / kGrassCellSize, centerRise / centerToCorner);
    float pad = max(slope, kGrassCellMinPadSlope) * kGrassCellSampleCover;
    return vec2(lo - pad, hi + pad);
}

struct GrassCellBox
{
    vec3 Min;
    vec3 Max;
};

// The box that holds every blade rooted in the cell. The roots lie in the cell's square over
// groundRange; every vertex lies within `reach` of its root, so the box grows by it on every side.
GrassCellBox GrassCellBladeBox(vec2 cellMin, vec2 cellMax, vec2 groundRange, float reach)
{
    GrassCellBox box;
    box.Min = vec3(cellMin.x - reach, groundRange.x - reach, cellMin.y - reach);
    box.Max = vec3(cellMax.x + reach, groundRange.y + reach, cellMax.y + reach);
    return box;
}
// GE_SHARED_GRASS_CELL_BOX_END

// Blades per square metre at `distanceMeters` from the camera. nearDensity is the authored spec
// unit (blades/m² AT the camera) and is independent of terrain size.
float GrassDensityAtDistance(float nearDensity, float farRadius, float falloff, float distanceMeters)
{
    if (farRadius <= 0.0)
        return 0.0;
    float t = clamp(1.0 - distanceMeters / farRadius, 0.0, 1.0);
    return max(nearDensity, 0.0) * pow(t, max(falloff, 0.0));
}

// Unrounded slot count for a cell at this distance. The fraction IS the growth ramp.
float GrassSlotsWantedForCell(float nearDensity, float farRadius, float falloff, float distanceMeters)
{
    float wanted = GrassDensityAtDistance(nearDensity, farRadius, falloff, distanceMeters) * kGrassCellArea;
    return min(wanted, float(kGrassMaxSlotsPerCell));
}

// GE_SHARED_GRASS_FLOOR_THINNING_BEGIN
// Width, in pixels, of the whole leaf that a blade never projects below. Under about a pixel a blade
// shows in one screen column and not the next, which is the grazing-angle hatch.
const float kGrassMinPixelWidth = 1.5f;
// Ceiling on how far that floor widens a blade, and the top of the range the floor is quantized over
// (GrassPackFlags). An edge-on blade past it is a sliver among slivers.
const float kGrassMaxWidthExpansion = 6.0f;
// Optical depth at which a field is closed: exp(-3.912) = 0.02, so the ground shows through 2 percent
// of a pixel.
const float kGrassClosedFieldOpticalDepth = 3.912f;
// Lower limit of the view depth the floor reads, as a share of the eye distance (60 degrees off the
// view axis). With it, the budget fit can bound the floor over a ring.
const float kGrassFloorMinDepthShare = 0.5f;

// The floor's leaf width in meters at a view depth. pixelsPerMeterAtUnitDistance is H * P11 / 2.
float GrassFloorWidth(float viewDepth, float eyeDistance, float pixelsPerMeterAtUnitDistance)
{
    return kGrassMinPixelWidth * max(viewDepth, eyeDistance * kGrassFloorMinDepthShare)
           / pixelsPerMeterAtUnitDistance;
}

// kClosed * h against tau * h, with tau = n * kw * hb * (d / h): the kept share is min(1, kClosed / tau).
// h is clamped to 0, so the share falls to 0 as the camera falls to the ground and stays 0 under it.
float GrassFloorKeptShare(float opticalDepthTimesHeight, float cameraHeight)
{
    float closedTimesHeight = kGrassClosedFieldOpticalDepth * max(cameraHeight, 0.0f);
    return opticalDepthTimesHeight > closedTimesHeight ? closedTimesHeight / opticalDepthTimesHeight : 1.0f;
}

// Share of a cell's slots that the floor band keeps, in [0, 1]. A blade wider than the floor keeps
// every slot. In the floor band, slots past the closed optical depth cover pixels other blades cover.
// cameraHeight is the camera above the ground the field stands on.
float GrassFloorThinningFactor(float bladesPerSquareMeter, float bladeWidth, float bladeHeight,
                               float horizontalDistance, float viewDepth, float cameraHeight,
                               float pixelsPerMeterAtUnitDistance)
{
    // Pixel size unknown (no world pass yet, or an orthographic view), or a card terrain.
    if (pixelsPerMeterAtUnitDistance <= 0.0f || bladeWidth <= 0.0f)
        return 1.0f;
    float height = max(cameraHeight, 0.0f);
    float eyeDistance = sqrt(horizontalDistance * horizontalDistance + height * height);
    float floorWidth = GrassFloorWidth(viewDepth, eyeDistance, pixelsPerMeterAtUnitDistance);
    if (floorWidth <= bladeWidth)
        return 1.0f;
    float drawnWidth = min(floorWidth, bladeWidth * kGrassMaxWidthExpansion);
    return GrassFloorKeptShare(bladesPerSquareMeter * drawnWidth * bladeHeight * horizontalDistance, height);
}

// The factor of one cell. h is the camera's height above the lower of the cell's ground and the ground
// under the camera, so ground above the eye reads as flat ground. depthAtEyeHeight is the view depth of
// the cell center at the camera's height; forwardY is the view direction's y.
float GrassFloorThinningCellFactor(float bladesPerSquareMeter, float bladeWidth, float bladeHeight,
                                   float horizontalDistance, float depthAtEyeHeight, float forwardY,
                                   float cameraY, float cellGroundY, float groundUnderCameraY,
                                   float pixelsPerMeterAtUnitDistance)
{
    float height = cameraY - min(cellGroundY, groundUnderCameraY);
    return GrassFloorThinningFactor(bladesPerSquareMeter, bladeWidth, bladeHeight, horizontalDistance,
                                    depthAtEyeHeight - forwardY * height, height, pixelsPerMeterAtUnitDistance);
}

// Upper limit of the factor over a ring's cells, for the budget fit. cameraHeight is the camera above the
// world's lowest ground, at least every cell's h. The floor is read at the ring's inner distance (h = 0)
// against the widest blade; in the band, the thinnest and shortest blade bound kw * hb.
float GrassFloorThinningRingFactor(float bladesPerSquareMeter, float widestBladeWidth, float thinnestBladeWidth,
                                   float shortestBladeHeight, float ringInnerDistance, float cameraHeight,
                                   float pixelsPerMeterAtUnitDistance)
{
    if (pixelsPerMeterAtUnitDistance <= 0.0f || thinnestBladeWidth <= 0.0f)
        return 1.0f;
    float height = max(cameraHeight, 0.0f);
    float floorWidth = GrassFloorWidth(0.0f, ringInnerDistance, pixelsPerMeterAtUnitDistance);
    if (floorWidth <= widestBladeWidth)
        return 1.0f;
    float drawnWidth = min(floorWidth, thinnestBladeWidth * kGrassMaxWidthExpansion);
    return GrassFloorKeptShare(bladesPerSquareMeter * drawnWidth * shortestBladeHeight * ringInnerDistance, height);
}

// The share the far-thinning dial keeps, from the floor band's kept share: share + (1 - farThinning)(1 - share).
// This form returns the share exactly at farThinning 1; 0 keeps every slot.
float GrassFarThinningKeptShare(float floorKeptShare, float farThinning)
{
    return floorKeptShare + (1.0f - farThinning) * (1.0f - floorKeptShare);
}
// GE_SHARED_GRASS_FLOOR_THINNING_END

// Scale in [0,1] applied to slot `slot`'s height and width, so the topmost slots of a cell scale in
// as the camera approaches instead of popping.
float GrassGrowthForSlot(float slotsWanted, uint slot)
{
    float growWidth = max(1.0, slotsWanted * kGrassGrowthSlotFraction);
    return clamp((slotsWanted - float(slot)) / growWidth, 0.0, 1.0);
}

uint GrassSlotsToIterate(float slotsWanted)
{
    if (slotsWanted <= 0.0)
        return 0u;
    return min(uint(ceil(slotsWanted)), kGrassMaxSlotsPerCell);
}

uint GrassHashSite(int cellX, int cellZ, uint slot, uint seed)
{
    uint h = uint(cellX) * 0x9E3779B1u;
    h ^= uint(cellZ) * 0x85EBCA77u;
    h ^= slot * 0xC2B2AE3Du;
    h ^= seed * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

uint GrassHashNext(uint h)
{
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

float GrassHashToUnit(uint h)
{
    return float(h >> 8) * (1.0 / 16777216.0);
}

// Halton(2,3) inside the cell. Prefix-uniform for EVERY slot count, which is what lets a cell grow
// from 25 slots to 512 without moving a single existing blade.
float GrassRadicalInverse2(uint index)
{
    uint bits = index;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return float(bits) * 2.3283064365386963e-10;
}

float GrassRadicalInverse3(uint index)
{
    float result = 0.0;
    float denominator = 1.0 / 3.0;
    uint n = index;
    while (n > 0u)
    {
        result += float(n % 3u) * denominator;
        n /= 3u;
        denominator *= 1.0 / 3.0;
    }
    return result;
}

struct GrassSlotSample
{
    float CellU;
    float CellV;
    float Yaw01;
    float ScaleRand;
    float WidthRand;
    float MaskRand;
    float TiltRand;
    float TwistRand;
};

GrassSlotSample GrassSampleSlot(int cellX, int cellZ, uint slot, uint seed)
{
    // Cranley-Patterson rotation per cell: decorrelates neighbours without disturbing the
    // low-discrepancy property inside a cell.
    uint cellHash = GrassHashSite(cellX, cellZ, 0u, seed);
    uint cellHash2 = GrassHashNext(cellHash);
    float rotU = GrassHashToUnit(cellHash);
    float rotV = GrassHashToUnit(cellHash2);

    GrassSlotSample s;
    s.CellU = fract(GrassRadicalInverse2(slot) + rotU);
    s.CellV = fract(GrassRadicalInverse3(slot) + rotV);

    uint h = GrassHashSite(cellX, cellZ, slot + 1u, seed);
    s.Yaw01 = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.ScaleRand = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.WidthRand = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.MaskRand = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.TiltRand = GrassHashToUnit(h);
    h = GrassHashNext(h);
    s.TwistRand = GrassHashToUnit(h);
    return s;
}

#endif // GRASS_PLACEMENT_GLSL
