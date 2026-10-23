// MaterialDeformationMarginsTests: the culling margin a material resolves from
// its vertex modifier's @deformationEnvelope and its own property values, and
// the per-materialIndex array the culling kernels bind.
//
// The GPU culling suites hand the kernels margins directly; these pin the CPU
// side that produces them: the scale's magnitude, every zero-margin case and the
// message that states its fix, and when the array is rebuilt and logged.

#include "Engine/Rendering/MaterialDeformationMargins.h"

#include "Engine/Rendering/Material.h"
#include "EngineLogCapture.h"
#include "Logger/Logger.h"
#include "Rendering/Materials/ShaderDeformationEnvelope.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Types/StringId.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <vector>

using GameEngine::Engine::Renderer::DeformationMargin;
using GameEngine::Engine::Renderer::Material;
using GameEngine::Engine::Renderer::MaterialDeformationMargins;
using GameEngine::Engine::Renderer::MaterialRegistryStamp;
using GameEngine::Engine::Renderer::ResolveDeformationMargin;
namespace Rendering = GameEngine::Rendering;

namespace
{

// A material whose modifier has the given form and declares `meters`, scaled
// by `scaledBy` when it is not empty. It carries a float `amount` at offset 0
// and a vec4 `windStrength` at offset 16.
Material MakeDeformer(Rendering::MaterialKeyword form, const char* scaledBy, float meters = 1.9f)
{
    Material mat = Material::TestFactory::Create(GameEngine::GUID::Generate(), "Wind", 32u);
    Material::TestFactory::AddPropertyLayout(mat, GameEngine::HashStringId("amount"), 0u, 4u);
    Material::TestFactory::AddPropertyLayout(mat, GameEngine::HashStringId("windStrength"), 16u, 16u);
    Rendering::ShaderVariantKey key{};
    key.materialKeywords = form;
    Material::TestFactory::SetVariantKey(mat, key);
    Rendering::DeformationEnvelopeDeclaration envelope{};
    envelope.ModifierFile = "wind.glsl";
    envelope.Declared = true;
    envelope.Meters = meters;
    envelope.ScaledBy = scaledBy;
    mat.SetDeformationEnvelope(envelope);
    return mat;
}

constexpr Rendering::MaterialKeyword kPositionOnly = Rendering::MaterialKeyword::HasVertexMod;
constexpr Rendering::MaterialKeyword kFullOutput = Rendering::MaterialKeyword::HasVertexOutputMod;

std::vector<float> Copy(std::span<const float> margins)
{
    return std::vector<float>(margins.begin(), margins.end());
}

size_t CountContaining(const std::vector<std::string>& lines, const std::string& needle)
{
    return static_cast<size_t>(
        std::count_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; }));
}

} // namespace

// A scalar scales the envelope by its absolute value, a vector by the length of
// its first three components: ez_tree's windStrength carries its enable flag in w.
TEST(MaterialDeformationMargins, TheScaleIsTheMagnitudeOfTheProperty)
{
    Material scalar = MakeDeformer(kPositionOnly, "amount");
    scalar.SetFloat(GameEngine::HashStringId("amount"), -0.25f);
    const DeformationMargin fromScalar = ResolveDeformationMargin(scalar);
    EXPECT_TRUE(fromScalar.Problem.empty()) << fromScalar.Problem;
    EXPECT_FLOAT_EQ(fromScalar.Meters, 1.9f * 0.25f);

    Material vector = MakeDeformer(kPositionOnly, "windStrength", 1.21f);
    const float wind[4] = {3.0f, 4.0f, 0.0f, 1.0f};
    vector.SetVector(GameEngine::HashStringId("windStrength"), wind, 4u);
    const DeformationMargin fromVector = ResolveDeformationMargin(vector);
    EXPECT_TRUE(fromVector.Problem.empty()) << fromVector.Problem;
    EXPECT_FLOAT_EQ(fromVector.Meters, 1.21f * 5.0f);

    const DeformationMargin unscaled = ResolveDeformationMargin(MakeDeformer(kPositionOnly, "", 0.3f));
    EXPECT_FLOAT_EQ(unscaled.Meters, 0.3f);
}

// Every zero margin of a deforming material carries the message that states its
// fix, except a full-output modifier that declares nothing: its draw path culls
// its own geometry. A material that does not deform has no margin and nothing
// to report.
TEST(MaterialDeformationMargins, AZeroMarginStatesWhy)
{
    const DeformationMargin missing = ResolveDeformationMargin(MakeDeformer(kPositionOnly, "gust"));
    EXPECT_EQ(missing.Meters, 0.0f);
    EXPECT_NE(missing.Problem.find("scaledBy=gust' names a property this material does not have"), std::string::npos)
        << missing.Problem;

    Material infinite = MakeDeformer(kPositionOnly, "amount");
    infinite.SetFloat(GameEngine::HashStringId("amount"), std::numeric_limits<float>::infinity());
    const DeformationMargin notFinite = ResolveDeformationMargin(infinite);
    EXPECT_EQ(notFinite.Meters, 0.0f);
    EXPECT_NE(notFinite.Problem.find("Set 'amount' to a finite value"), std::string::npos) << notFinite.Problem;

    Material refused = MakeDeformer(kPositionOnly, "");
    Rendering::DeformationEnvelopeDeclaration refusal = refused.GetDeformationEnvelope();
    refusal.Refusal = "the refusal's own fix";
    refused.SetDeformationEnvelope(refusal);
    const DeformationMargin fromRefusal = ResolveDeformationMargin(refused);
    EXPECT_EQ(fromRefusal.Meters, 0.0f);
    EXPECT_EQ(fromRefusal.Problem, "the refusal's own fix");

    Material silent = MakeDeformer(kPositionOnly, "");
    silent.SetDeformationEnvelope({});
    const DeformationMargin undeclared = ResolveDeformationMargin(silent);
    EXPECT_EQ(undeclared.Meters, 0.0f);
    EXPECT_NE(undeclared.Problem.find("declares no '@deformationEnvelope'"), std::string::npos) << undeclared.Problem;

    Material fullOutput = MakeDeformer(kFullOutput, "");
    fullOutput.SetDeformationEnvelope({});
    const DeformationMargin ownCulling = ResolveDeformationMargin(fullOutput);
    EXPECT_EQ(ownCulling.Meters, 0.0f);
    EXPECT_TRUE(ownCulling.Problem.empty()) << ownCulling.Problem;

    const DeformationMargin rigid =
        ResolveDeformationMargin(MakeDeformer(Rendering::MaterialKeyword::None, "amount"));
    EXPECT_EQ(rigid.Meters, 0.0f);
    EXPECT_TRUE(rigid.Problem.empty()) << rigid.Problem;
}

// The array is rebuilt only when the stamp moves, so an idle frame reuses it; a
// scaledBy edit moves the content epoch and the margin follows in the same call.
// A material's problem is logged once, and again only when it changes.
TEST(MaterialDeformationMargins, RefreshFollowsTheStampAndLogsAProblemOnce)
{
    const GameEngine::StringId amount = GameEngine::HashStringId("amount");
    Material wind = MakeDeformer(kPositionOnly, "amount");
    wind.SetFloat(amount, 0.25f);
    Material broken = MakeDeformer(kPositionOnly, "gust");
    const Material* byIndex[] = {&wind, nullptr, &broken};
    auto stampNow = [] { return MaterialRegistryStamp{1u, Material::GetGlobalContentEpoch()}; };

    std::vector<std::string> lines;
    GameEngine::TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
    MaterialDeformationMargins margins;
    const std::vector<float> first = Copy(margins.Refresh(byIndex, stampNow()));
    EXPECT_EQ(first, (std::vector<float>{1.9f * 0.25f, 0.0f, 0.0f}));

    // An edit the stamp does not see is not picked up: the array is reused.
    const MaterialRegistryStamp held = stampNow();
    margins.Refresh(byIndex, held);
    wind.SetFloat(amount, 0.1f);
    EXPECT_EQ(margins.Refresh(byIndex, held)[0], 1.9f * 0.25f);

    const std::vector<float> edited = Copy(margins.Refresh(byIndex, stampNow()));
    EXPECT_FLOAT_EQ(edited[0], 1.9f * 0.1f);

    wind.SetFloat(amount, 0.2f);
    margins.Refresh(byIndex, stampNow());
    Logger::Log::Flush();
    EXPECT_EQ(CountContaining(lines, "scaledBy=gust"), 1u) << "an unchanged problem is logged once";

    Material::TestFactory::AddPropertyLayout(broken, GameEngine::HashStringId("gust"), 4u, 4u);
    broken.SetFloat(GameEngine::HashStringId("gust"), std::numeric_limits<float>::infinity());
    const std::vector<float> changed = Copy(margins.Refresh(byIndex, stampNow()));
    EXPECT_EQ(changed[2], 0.0f);
    Logger::Log::Flush();
    EXPECT_EQ(CountContaining(lines, "Set 'gust' to a finite value"), 1u) << "a changed problem is logged again";
}
