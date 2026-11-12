// cbt_terrain_params.glsl: the terrain params row as both graphics stages read it (mirror
// Terrain/TerrainTypes.h TerrainGPUParams), std430 on set 2 binding 1. The extraction system fills
// it; the vertex modifier reads the flags, the surface reads the material config.
#ifndef CBT_TERRAIN_PARAMS_GLSL
#define CBT_TERRAIN_PARAMS_GLSL

struct TerrainParamsEntry
{
    float WorldOriginX;
    float WorldOriginZ;
    float WorldSizeX;
    float WorldSizeZ;
    float HeightScale;
    float InvHeightmapWidth;
    float InvHeightmapHeight;
    float TexelSize;
    float WorldOriginY;
    uint SplatmapBindless;
    float MaterialTiling;
    uint LayerCount;
    uint LayerRole[4];
    float GridDimHalf;
    uint Flags;
    uint NormalmapBindless;
    uint HeightmapBindless;
    uint GrassEnabled;
    float GrassDensity;
    float GrassBladeHeight;
    float GrassBladeWidth;
    float GrassMaskThreshold;
    float GrassDensityFalloff;
    float GrassRange;
    float GrassWindStrength;
    uint GrassLayerIndex;
    float GrassBrightness;
    float GrassGroundingStrength;
    float GrassRandomScale;
    float GrassRandomBrightness;
    float GrassTranslucency;
    uint GrassRootColor;
    uint GrassTipColor;
    uint GrassBacklightColor;
    float GrassWindDirection;
    float GrassWindGustSpeed;
    float GrassWindGustScale;
    float GrassWindRestingLean;
    float GrassWindFlutterAmount;
    float GrassWindFlutterSpeed;
    float GrassWindSeed;
    uint GrassBladeSegments;
    uint GrassAlbedoBindless;
    uint GrassAlphaBindless;
    uint GrassNormalBindless;
    uint GrassAtlasColumns;
    uint GrassAtlasRows;
    uint GrassAtlasTileCount;
    float GrassAlphaCutoff;
    float GrassNormalStrength;
    float GrassTextureCardsPerSquareMeter;
    float GrassTextureSize;
    float GrassPlacementSeed;
    float GrassBladeTilt;
    float GrassBladeBend;
    float GrassBladeTwist;
    float GrassBladeTaperStart;
    float GrassTuftFan;
    float GrassClumpSize;
    float GrassClumpHeightVariance;
    float GrassClumpAlignment;
    float GrassClumpTaper;
    float GrassHueVariation;
    float GrassCanopyOcclusion;
    float GrassRootFadeStart;
    float GrassRootFadeEnd;
    uint GrassControlBindless;
    uint ColorMapBindless;
    float GrassGroundTintHeight;
    float LatticeOverhangX;
    float LatticeOverhangZ;
};

layout(std430, set = 2, binding = 1) readonly buffer TerrainParamsBuffer
{
    TerrainParamsEntry terrains[];
};

#endif // CBT_TERRAIN_PARAMS_GLSL
