#include "Inspectors/BuiltInInspectorRegistration.h"

#include "DayCycle/DayCycleProfile/DayCycleProfileInspector.h"
#include "Inspectors/ShipRules/ShipRuleRow.h"
#include "Editor/Registries/EditorPluginRegistry.h"
#include "Inspectors/AmbientLightInspector.h"
#include "Inspectors/PostProcessEffects/AmbientOcclusionEffectInspector.h"
#include "Inspectors/AnimatorInspector.h"
#include "Inspectors/AtmosphericCloudLayerInspector.h"
#include "Inspectors/VolumetricCloudsInspector.h"
#include "Inspectors/AudioEmitterInspector.h"
#include "Inspectors/AudioInspector.h"
#include "Inspectors/PostProcessEffects/BloomEffectInspector.h"
#include "Inspectors/CameraInspector.h"
#include "Inspectors/PostProcessEffects/ChromaticAberrationEffectInspector.h"
#include "Inspectors/PostProcessEffects/ColorFilterEffectInspector.h"
#include "Inspectors/PostProcessEffects/ColorGradeEffectInspector.h"
#include "Inspectors/PostProcessEffects/ContrastAdaptiveSharpenEffectInspector.h"
#include "Inspectors/PostProcessEffects/CrtEffectInspector.h"
#include "Inspectors/CubeLut/CubeLutAssetInspector.h"
#include "Inspectors/CubeLut/CubeLutEffectInspector.h"
#include "Inspectors/PostProcessEffects/DebandEffectInspector.h"
#include "Inspectors/PostProcessEffects/DepthOfFieldEffectInspector.h"
#include "Inspectors/PostProcessEffects/ExposureAdjustmentEffectInspector.h"
#include "Inspectors/PostProcessEffects/FastBlurEffectInspector.h"
#include "Inspectors/PostProcessEffects/FilmSimulationEffectInspector.h"
#include "Inspectors/PostProcessEffects/HeatDistortionEffectInspector.h"
#include "Inspectors/PostProcessEffects/HeightFogEffectInspector.h"
#include "Inspectors/HumanoidRigInspector.h"
#include "Inspectors/LensFlareSourceInspector.h"
#include "Inspectors/LightInspector.h"
#include "Inspectors/MaterialInspector.h"
#include "Inspectors/MeasureInspector.h"
#include "Inspectors/Navigation/NavigationAgentInspector.h"
#include "Inspectors/Navigation/NavigationSurfaceInspector.h"
#include "Inspectors/MeshRendererInspector.h"
#include "Inspectors/ModelInspector.h"
#include "Inspectors/NativeSourceInspector.h"
#include "Inspectors/OceanDepthCacheSourceInspector.h"
#include "Inspectors/OceanWaveSpectrumInspector.h"
#include "Inspectors/Particles/ParticleInspector.h"
#include "Inspectors/Physics/PhysicsBodyInspector.h"
#include "Inspectors/CharacterControllerInspector.h"
#include "Inspectors/Physics/PhysicsColliderInspector.h"
#include "Inspectors/PostProcessVolumeInspector.h"
#include "Inspectors/RenderPipelineInspector.h"
#include "Inspectors/RetargetMapInspector.h"
#include "Inspectors/PostProcessEffects/ScreenSpaceReflectionsEffectInspector.h"
#include "Inspectors/ScriptInspector.h"
#include "Inspectors/ShaderInspector.h"
#include "Inspectors/PostProcessEffects/ShadowSettingsEffectInspector.h"
#include "Inspectors/SkeletonProfileInspector.h"
#include "Inspectors/SkinnedMeshRendererInspector.h"
#include "Sky/SkyEnvironment/SkyEnvironmentInspector.h"
#include "Inspectors/SkyboxInspector.h"
#include "Inspectors/SplineFence/SplineFenceInspector.h"
#include "Inspectors/SplineFence/SplineFenceSpanPieceInspector.h"
#include "Inspectors/Spline/SplineInspector.h"
#include "Inspectors/Spline/SplinePlacementInspector.h"
#include "Inspectors/SplineRecipe/Paving/SplinePavingInspector.h"
#include "Inspectors/Texture/TextureInspector.h"
#include "Inspectors/TransformInspector.h"
#include "Inspectors/ValueCurveInspector.h"
#include "Inspectors/PostProcessEffects/VhsEffectInspector.h"
#include "Inspectors/PostProcessEffects/VignetteEffectInspector.h"
#include "Inspectors/PostProcessEffects/VolumetricFogEffectInspector.h"
#include "Inspectors/WindVolumeInspector.h"
#include "SceneView/SceneViewEye.h"
#include "Terrain/TerrainHeightmapInspector.h"
#include "Terrain/TerrainInspector.h"
#include "Terrain/TerrainColorZoneInspector.h"
#include "Terrain/TerrainMaterialLibraryInspector.h"
#include "Terrain/TerrainModifier/TerrainModifierInspectors.h"

namespace GameEngine
{

void RegisterBuiltInInspectors()
{
    RegisterTransformInspector();
    RegisterMeshRendererInspector();
    RegisterSkinnedMeshRendererInspector();
    RegisterAnimatorInspector();
    RegisterValueCurveInspector();
    RegisterAudioEmitterInspector();
    RegisterCameraInspector();
    RegisterLightInspector();
    RegisterLensFlareSourceInspector();
    RegisterMeasureInspector();
    RegisterSkyEnvironmentInspector();
    RegisterAmbientLightInspector();
    RegisterSkyboxInspector();
    RegisterOceanDepthCacheSourceInspector();
    RegisterOceanWaveSpectrumInspector();
    RegisterParticleInspector();
    Editor::RegisterSceneViewEyeTraits();
    RegisterPostProcessVolumeInspector();
    RegisterWindVolumeInspector();
    RegisterNavigationAgentInspector();
    RegisterNavigationSurfaceInspector();
    RegisterSplineWallInspector();
    RegisterSplineExtrudeInspector();
    RegisterSplinePavingInspector();
    RegisterBuiltInComponentTraits();
    RegisterBloomEffectInspector();
    RegisterChromaticAberrationEffectInspector();
    RegisterDepthOfFieldEffectInspector();
    RegisterFilmSimulationEffectInspector();
    RegisterAmbientOcclusionEffectInspector();
    RegisterScreenSpaceReflectionsEffectInspector();
    RegisterColorGradeEffectInspector();
    RegisterContrastAdaptiveSharpenEffectInspector();
    RegisterExposureAdjustmentEffectInspector();
    RegisterCrtEffectInspector();
    RegisterFastBlurEffectInspector();
    RegisterHeatDistortionEffectInspector();
    RegisterVhsEffectInspector();
    RegisterHeightFogEffectInspector();
    RegisterColorFilterEffectInspector();
    RegisterCubeLutEffectInspector();
    RegisterDebandEffectInspector();
    RegisterVignetteEffectInspector();
    RegisterShadowSettingsEffectInspector();
    RegisterAtmosphericCloudLayerInspector();
    RegisterVolumetricCloudsInspector();
    RegisterVolumetricFogEffectInspector();
    RegisterPhysicsBodyInspector();
    RegisterCharacterControllerInspector();
    RegisterPhysicsColliderInspectors();
    RegisterTerrainInspector();
    RegisterTerrainModifierInspectors();
    RegisterTerrainColorZoneInspector();
    RegisterSplineInspectors();
    RegisterSplinePlacementInspector();
    RegisterSplineFenceInspector();
    RegisterSplineFenceSpanPieceInspector();

    RegisterMaterialInspector();
    RegisterModelInspector();
    RegisterTextureInspector();
    RegisterAudioInspector();
    Editor::ShipRuleRow::Register();
    RegisterCubeLutAssetInspector();
    Editor::RegisterDayCycleProfileInspector();
    RegisterScriptInspector();
    RegisterNativeSourceInspector();
    RegisterShaderInspector();
    RegisterRenderPipelineInspector();
    RegisterTerrainMaterialLibraryInspector();
    RegisterTerrainHeightmapInspector();
    RegisterSkeletonProfileInspector();
    RegisterHumanoidRigInspector();
    RegisterRetargetMapInspector();

    Editor::EditorPluginRegistry::Get().RegisterInspectors();
}

} // namespace GameEngine
