#include "Engine/Rendering/MaterialDeformationMargins.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialVertexAnimation.h"
#include "Logger/Logger.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Types/StringId.h"

#include <cmath>

namespace GameEngine::Engine::Renderer
{

namespace
{

// The position-only form, `vec3 ModifyVertex(vec3, InstanceData)`, moves mesh
// vertices these kernels cull. The full-output form writes the vertex output
// itself; every shipped producer of it (terrain, ocean, grass, particles) owns
// its own culling, so its silence is not reported.
bool IsPositionOnlyModifier(const Material& mat)
{
    namespace R = ::GameEngine::Rendering;
    const R::MaterialKeyword keywords = mat.GetVariantKey().materialKeywords;
    return R::HasKeyword(keywords, R::MaterialKeyword::HasVertexMod)
        && !R::HasKeyword(keywords, R::MaterialKeyword::HasVertexOutputMod);
}

std::string ModifierName(const Material& mat)
{
    const std::string& declared = mat.GetDeformationEnvelope().ModifierFile;
    return declared.empty() ? mat.GetCompileSpec().vertexModifierPath : declared;
}

// The scaledBy property's magnitude: the absolute value of a scalar, the length
// of a vector's first three components. False when the material has no such
// property.
bool ReadScaleMagnitude(const Material& mat, const std::string& name, float& magnitude)
{
    const StringId id = HashStringId(name);
    for (uint32_t count = 3; count >= 1; --count)
    {
        float v[3]{};
        if (!mat.GetVector(id, v, count))
            continue;
        magnitude = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return true;
    }
    return false;
}

} // namespace

DeformationMargin ResolveDeformationMargin(const Material& mat)
{
    DeformationMargin out{};
    if (!MaterialDeformsVertices(mat))
        return out;
    const ::GameEngine::Rendering::DeformationEnvelopeDeclaration& envelope = mat.GetDeformationEnvelope();
    if (!envelope.Declared)
    {
        if (IsPositionOnlyModifier(mat))
            out.Problem = "Vertex modifier '" + ModifierName(mat) +
                          "' declares no '@deformationEnvelope', so culling bounds are not widened for its "
                          "deformation and deformed geometry may be culled while still on screen. Add "
                          "'// @deformationEnvelope <meters> [scaledBy=<property>]' with the largest distance "
                          "the modifier moves a vertex.";
        return out;
    }
    if (!envelope.Refusal.empty())
    {
        out.Problem = envelope.Refusal;
        return out;
    }
    float scale = 1.0f;
    if (!envelope.ScaledBy.empty() && !ReadScaleMagnitude(mat, envelope.ScaledBy, scale))
    {
        out.Problem = "Vertex modifier '" + ModifierName(mat) + "': '@deformationEnvelope scaledBy=" +
                      envelope.ScaledBy +
                      "' names a property this material does not have, so the margin is zero and deformed "
                      "geometry may be culled while still on screen. Recompile the material after adding "
                      "the property to its shader.";
        return out;
    }
    const float meters = envelope.Meters * scale;
    if (!std::isfinite(meters))
    {
        out.Problem = "The deformation envelope's scale property '" + envelope.ScaledBy +
                      "' is not a finite number, so the culling margin is zero and deformed geometry may be "
                      "culled while still on screen. Set '" + envelope.ScaledBy + "' to a finite value.";
        return out;
    }
    out.Meters = meters;
    return out;
}

std::span<const float> MaterialDeformationMargins::Refresh(std::span<const Material* const> materialsByIndex,
                                                           MaterialRegistryStamp stamp)
{
    if (m_Built && stamp == m_Stamp && m_Margins.size() == materialsByIndex.size())
        return m_Margins;
    m_Built = true;
    m_Stamp = stamp;
    m_Margins.assign(materialsByIndex.size(), 0.0f);
    for (size_t index = 0; index < materialsByIndex.size(); ++index)
    {
        const Material* mat = materialsByIndex[index];
        if (!mat)
            continue;
        const DeformationMargin margin = ResolveDeformationMargin(*mat);
        m_Margins[index] = margin.Meters;
        if (margin.Problem.empty())
        {
            m_ReportedProblems.erase(mat->GetGuid());
            continue;
        }
        std::string& reported = m_ReportedProblems[mat->GetGuid()];
        if (reported == margin.Problem)
            continue;
        reported = margin.Problem;
        Logger::Log::Warning("Material '{}': {}", mat->GetName(), margin.Problem);
    }
    return m_Margins;
}

} // namespace GameEngine::Engine::Renderer
