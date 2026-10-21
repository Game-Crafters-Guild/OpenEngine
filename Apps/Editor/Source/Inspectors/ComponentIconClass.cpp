#include "Inspectors/ComponentIconClass.h"

#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/ValueCurve.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Audio/AudioListener.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Rendering/WindVolume.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "Components/Video/VideoTextureComponent.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EditorInspectorPseudoComponents.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "UI/Controls/SearchDialog.h"

namespace GameEngine::Editor
{

using namespace Components;

namespace
{

constexpr const char* kDefaultIconClass = "inspector-section-icon-default";

// The glyph of a picker category, for an entry none of whose components has an icon of its own.
const char* CategoryIconClass(const std::string& category)
{
    if (category == "Animation")
        return "inspector-section-icon-animator";
    if (category == "Atmosphere")
        return "inspector-section-icon-post-process";
    if (category == "Audio")
        return "inspector-section-icon-audio-emitter";
    if (category == "Physics")
        return "inspector-section-icon-physics";
    if (category == "Post FX")
        return "inspector-section-icon-post-process";
    if (category == "Rendering")
        return "inspector-section-icon-mesh-renderer";
    if (category == "Spline")
        return "inspector-section-icon-spline";
    if (category == "Terrain")
        return "inspector-section-icon-terrain";
    return kDefaultIconClass;
}

} // namespace

std::string ComponentIconClass(ECS::World* world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId)
{
    // Plugin-registered traits (editor package modules) — live lookup, before
    // the built-in chains.
    if (Editor::EditorComponentTraits traits;
        Editor::EditorComponentTraitsRegistry::Get().TryGet(typeId, traits) && !traits.InspectorIconClass.empty())
        return traits.InspectorIconClass;

    static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();
    static const ECS::ComponentTypeId meshRendererId = ECS::GetComponentTypeId<MeshRenderer>();
    static const ECS::ComponentTypeId skinnedMeshRendererId = ECS::GetComponentTypeId<SkinnedMeshRenderer>();
    static const ECS::ComponentTypeId materialSectionId = ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
    static const ECS::ComponentTypeId cameraId = ECS::GetComponentTypeId<Camera>();
    static const ECS::ComponentTypeId lightId = ECS::GetComponentTypeId<Light>();
    static const ECS::ComponentTypeId measureId = ECS::GetComponentTypeId<MeasureComponent>();
    static const ECS::ComponentTypeId skyEnvId = ECS::GetComponentTypeId<SkyEnvironment>();
    static const ECS::ComponentTypeId skyboxId = ECS::GetComponentTypeId<Skybox>();
    static const ECS::ComponentTypeId reflectionProbeId = ECS::GetComponentTypeId<Components::ReflectionProbe>();
    static const ECS::ComponentTypeId postProcessId = ECS::GetComponentTypeId<PostProcessVolume>();
    static const ECS::ComponentTypeId windVolumeId = ECS::GetComponentTypeId<Components::WindVolume>();
    static const ECS::ComponentTypeId lensFlareSourceId = ECS::GetComponentTypeId<Components::LensFlareSource>();
    static const ECS::ComponentTypeId terrainId = ECS::GetComponentTypeId<Components::Terrain>();
    static const ECS::ComponentTypeId terrainGrassId = ECS::GetComponentTypeId<Components::TerrainGrass>();
    static const ECS::ComponentTypeId terrainModifierVolumeId =
        ECS::GetComponentTypeId<Components::TerrainModifierVolume>();
    static const ECS::ComponentTypeId videoTextureId = ECS::GetComponentTypeId<Components::VideoTextureComponent>();
    static const ECS::ComponentTypeId oceanSurfaceId = ECS::GetComponentTypeId<Components::OceanSurface>();
    static const ECS::ComponentTypeId oceanSpectrumId = ECS::GetComponentTypeId<Components::OceanWaveSpectrum>();
    static const ECS::ComponentTypeId oceanBuoyancyId = ECS::GetComponentTypeId<Components::OceanBuoyancy>();
    static const ECS::ComponentTypeId splineId = ECS::GetComponentTypeId<Components::SplineComponent>();
    static const ECS::ComponentTypeId physicsBodyId = ECS::GetComponentTypeId<PhysicsBody>();
    static const ECS::ComponentTypeId physicsColliderId = ECS::GetComponentTypeId<PhysicsCollider>();
    static const ECS::ComponentTypeId physicsColliderOwnerId = ECS::GetComponentTypeId<PhysicsColliderOwner>();
    static const ECS::ComponentTypeId physicsWorldSettingsId = ECS::GetComponentTypeId<PhysicsWorldSettingsComponent>();
    static const ECS::ComponentTypeId boxColliderId = ECS::GetComponentTypeId<BoxColliderShape>();
    static const ECS::ComponentTypeId sphereColliderId = ECS::GetComponentTypeId<SphereColliderShape>();
    static const ECS::ComponentTypeId planeColliderId = ECS::GetComponentTypeId<PlaneColliderShape>();
    static const ECS::ComponentTypeId capsuleColliderId = ECS::GetComponentTypeId<CapsuleColliderShape>();
    static const ECS::ComponentTypeId audioEmitterId = ECS::GetComponentTypeId<AudioEmitter>();
    static const ECS::ComponentTypeId audioListenerId = ECS::GetComponentTypeId<AudioListener>();
    static const ECS::ComponentTypeId animatorId = ECS::GetComponentTypeId<Animator>();
    static const ECS::ComponentTypeId valueCurveId = ECS::GetComponentTypeId<ValueCurve>();
    static const ECS::ComponentTypeId animatedNodeRefId = ECS::GetComponentTypeId<AnimatedNodeRef>();
    static const ECS::ComponentTypeId lodGroupId = ECS::GetComponentTypeId<LODGroup>();

    if (typeId == transformId)
        return "inspector-section-icon-transform";
    if (typeId == meshRendererId)
        return "inspector-section-icon-mesh-renderer";
    if (typeId == skinnedMeshRendererId)
        return "inspector-section-icon-skinned-mesh";
    if (typeId == materialSectionId)
        return "inspector-section-icon-material";
    if (typeId == cameraId)
        return "inspector-section-icon-camera";
    if (typeId == lightId)
    {
        if (world && world->IsValid(entity))
        {
            if (const auto* light = world->GetComponent<Components::Light>(entity))
            {
                using LT = Components::LightType;
                switch (light->Type)
                {
                case LT::Ambient:
                    return "inspector-section-icon-light-ambient";
                case LT::Directional:
                    return "inspector-section-icon-light-directional";
                case LT::Spot:
                    return "inspector-section-icon-light-spot";
                case LT::Point:
                    return "inspector-section-icon-light-point";
                case LT::Area:
                    return "inspector-section-icon-light-area";
                case LT::Volume:
                    return "inspector-section-icon-light-volume";
                default:
                    break;
                }
            }
        }
        return "inspector-section-icon-light-directional";
    }
    if (typeId == skyEnvId)
        return "inspector-section-icon-sky";
    if (typeId == measureId)
        return "inspector-section-icon-measure";
    if (typeId == skyboxId)
        return "inspector-section-icon-sky";
    if (typeId == reflectionProbeId)
        return "inspector-section-icon-reflection-probe";
    if (typeId == postProcessId)
        return "inspector-section-icon-post-process-volume";
    if (typeId == windVolumeId)
        return "inspector-section-icon-wind-volume";
    if (typeId == lensFlareSourceId)
        return "inspector-section-icon-lens-flare";
    if (typeId == terrainId || typeId == terrainModifierVolumeId)
        return "inspector-section-icon-terrain";
    if (typeId == terrainGrassId)
        return "inspector-section-icon-grass";
    if (typeId == videoTextureId)
        return "inspector-section-icon-video-texture";
    if (typeId == oceanSurfaceId || typeId == oceanSpectrumId || typeId == oceanBuoyancyId)
        return "inspector-section-icon-ocean";
    if (typeId == splineId)
        return "inspector-section-icon-spline";
    if (typeId == physicsBodyId ||
        typeId == physicsColliderId ||
        typeId == physicsColliderOwnerId)
        return "inspector-section-icon-physics";
    if (typeId == physicsWorldSettingsId)
        return "inspector-section-icon-physics-settings";
    if (typeId == boxColliderId)
        return "inspector-section-icon-shape-cube";
    if (typeId == sphereColliderId)
        return "inspector-section-icon-shape-sphere";
    if (typeId == planeColliderId)
        return "inspector-section-icon-shape-plane";
    if (typeId == capsuleColliderId)
        return "inspector-section-icon-shape-capsule";
    if (typeId == audioEmitterId)
        return "inspector-section-icon-audio-emitter";
    if (typeId == audioListenerId)
        return "inspector-section-icon-audio-listener";
    if (typeId == animatorId)
        return "inspector-section-icon-animator";
    if (typeId == valueCurveId)
        return "inspector-section-icon-animator";
    if (typeId == animatedNodeRefId)
        return "inspector-section-icon-animated-node";
    if (typeId == lodGroupId)
        return "inspector-section-icon-lod-group";
    return kDefaultIconClass;
}

SearchIcon ComponentPickerIcon(std::span<const ECS::ComponentTypeId> typeIds, const std::string& category)
{
    for (auto it = typeIds.rbegin(); it != typeIds.rend(); ++it)
    {
        std::string iconClass = ComponentIconClass(nullptr, ECS::EntityHandle{}, *it);
        if (iconClass != kDefaultIconClass)
            return SearchIcon::FromClass(iconClass);
    }
    return SearchIcon::FromClass(CategoryIconClass(category));
}

} // namespace GameEngine::Editor
