/**
 * @file MaterialDeformationMargins.h
 * @brief The culling margin per material: how far its vertex modifier can move a vertex.
 *
 * The GPU culling kernels (frustum_culling.comp, hzb_culling.comp) read one
 * float per materialIndex and test a deforming instance's sphere with radius
 * R + margin x largest axis scale before the per-view cull margin. The margin is
 * in local meters: the modifier's @deformationEnvelope (ShaderDeformationEnvelope.h)
 * times the magnitude of its scaledBy property. It is zero when the material does
 * not deform, when the modifier declares nothing, when the declaration was
 * refused and when the scaled value is not finite; zero culls on the mesh
 * bounds alone. The scatter's level selection never reads it.
 */
#pragma once

#include "AssetCore/GUID.h"
#include "Engine/Rendering/MaterialRegistryStamp.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Engine::Renderer
{

class Material;

struct DeformationMargin
{
    float Meters = 0.0f;
    // Why the margin is zero for a material that deforms, as the message that
    // states the fix. Empty when the margin is the declared one.
    std::string Problem;
};

/// The margin `mat` culls with right now, from its installed declaration and its
/// current property values.
DeformationMargin ResolveDeformationMargin(const Material& mat);

/// The per-materialIndex margin array the culling kernels bind. Rebuilt when the
/// registry stamp moves (an index assignment, or any material's content
/// revision, which a declaration change and a scaledBy property edit both move),
/// so an idle frame reuses the last array. A material's problem is logged once,
/// and again only when it changes.
class MaterialDeformationMargins
{
  public:
    std::span<const float> Refresh(std::span<const Material* const> materialsByIndex,
                                   MaterialRegistryStamp stamp);

  private:
    std::vector<float> m_Margins;
    MaterialRegistryStamp m_Stamp{};
    bool m_Built = false;
    std::unordered_map<GUID, std::string> m_ReportedProblems;
};

} // namespace GameEngine::Engine::Renderer
