// The shader inspector's stage handling for a Slang module. A .slang file carries
// no stage in its extension: the inspector takes the stage from main's
// [shader("...")] attribute (the Slang lane compiles only main), offers the stage
// picker, and names the entry point. A module with no staged main gets no stage and
// no compile, and with the Slang lane off no module is compiled.

#include <gtest/gtest.h>

#include "InspectorRegistry.h"
#include "Inspectors/ShaderInspector.h"

#include "Assets/ShaderSourceAsset.h"
#include "Core/Engine.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextArea.h"
#include "UI/UIElement.h"

#include "TestEnvVar.h"
#include "TestTempDir.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

struct InspectorTexts
{
    std::vector<std::string> Labels;
    std::vector<std::string> Buttons;
    int TextAreas = 0;
};

void CollectTexts(const UIElement& element, InspectorTexts& out)
{
    if (const auto* label = dynamic_cast<const Label*>(&element))
        out.Labels.push_back(label->GetText());
    if (const auto* button = dynamic_cast<const Button*>(&element))
        out.Buttons.push_back(button->GetText());
    if (dynamic_cast<const TextArea*>(&element))
        ++out.TextAreas;
    for (const auto& child : element.GetChildren())
        CollectTexts(*child, out);
}

bool Contains(const std::vector<std::string>& texts, const std::string& text)
{
    return std::find(texts.begin(), texts.end(), text) != texts.end();
}

bool AnyStartsWith(const std::vector<std::string>& texts, const std::string& prefix)
{
    return std::any_of(texts.begin(), texts.end(), [&prefix](const std::string& t) { return t.starts_with(prefix); });
}

InspectorTexts InspectSlangModule(const std::string& fileName, const std::string& source)
{
    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        EXPECT_TRUE(engine.Initialize(config));
    }

    RegisterShaderInspector();
    InspectorFn* fn = InspectorRegistry::Get().TryGetAssetInspector(AssetType::Shader);
    EXPECT_NE(fn, nullptr);
    if (!fn)
        return {};

    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_shader_inspector")};
    const std::filesystem::path path = tmpDir.Path() / fileName;
    {
        std::ofstream out(path, std::ios::binary);
        out << source;
    }

    ShaderSourceAsset asset(GUID::Generate(), path);
    UIElement root("div");
    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.Object = &asset;
    (*fn)(ctx);

    InspectorTexts texts;
    CollectTexts(root, texts);
    return texts;
}

} // namespace

TEST(ShaderInspectorSlang, AComputeModuleShowsTheComputeStageThePickerAndTheEntryPoint)
{
    const InspectorTexts texts = InspectSlangModule("hzb_like.slang",
                                                    "[shader(\"compute\")]\n"
                                                    "[numthreads(8, 8, 1)]\n"
                                                    "void main(uint3 tid : SV_DispatchThreadID) {}\n");

    EXPECT_TRUE(Contains(texts.Labels, "Stage: Compute (cs)")) << "the stage comes from the [shader] attribute";
    EXPECT_TRUE(Contains(texts.Labels, "Entry point: main"));
    for (const char* stage : {"Vertex", "Fragment", "Compute", "Geometry"})
        EXPECT_TRUE(Contains(texts.Buttons, stage)) << "the stage picker lacks " << stage;
}

TEST(ShaderInspectorSlang, AModuleWithNoEntryPointHasNoStageAndIsNotCompiled)
{
    const InspectorTexts texts = InspectSlangModule("types_only.slang",
                                                    "module types_only;\n"
                                                    "public struct Params { float4 Value; }\n");

    EXPECT_TRUE(AnyStartsWith(texts.Labels, "Stage: none.")) << "a module without an entry point has no stage";
    EXPECT_EQ(texts.TextAreas, 0) << "a module without an entry point is not compiled, so it shows no failure";
}

TEST(ShaderInspectorSlang, TheStageComesFromMainsAttributeNotTheFirstAttribute)
{
    const InspectorTexts texts = InspectSlangModule("two_entries.slang",
                                                    "[shader(\"vertex\")]\n"
                                                    "float4 helperVertex() : SV_Position { return float4(0, 0, 0, 1); }\n"
                                                    "[shader(\"compute\")]\n"
                                                    "[numthreads(8, 8, 1)]\n"
                                                    "void main(uint3 tid : SV_DispatchThreadID) {}\n");

    EXPECT_TRUE(Contains(texts.Labels, "Stage: Compute (cs)")) << "the Slang lane compiles main, so main's stage is shown";
}

TEST(ShaderInspectorSlang, AStageThePickerCannotShowIsNamed)
{
    const InspectorTexts texts = InspectSlangModule("hull_main.slang",
                                                    "[shader(\"hull\")]\n"
                                                    "void main() {}\n");

    EXPECT_TRUE(Contains(texts.Labels, "Stage: none. main is a hull entry point, a stage the inspector does not compile."));
    EXPECT_EQ(texts.TextAreas, 0);
}

TEST(ShaderInspectorSlang, WithTheSlangLaneOffAModuleIsNotCompiledAndTheLineSaysHowToTurnItOn)
{
    const char* prior = std::getenv("GE_SHADER_SLANG_LANE");
    const std::string saved = prior ? prior : "";
    Testing::SetEnvVar("GE_SHADER_SLANG_LANE", "");

    const InspectorTexts texts = InspectSlangModule("lane_off.slang",
                                                    "[shader(\"compute\")]\n"
                                                    "[numthreads(8, 8, 1)]\n"
                                                    "void main(uint3 tid : SV_DispatchThreadID) {}\n");
    Testing::SetEnvVar("GE_SHADER_SLANG_LANE", saved.c_str());

    EXPECT_TRUE(AnyStartsWith(texts.Labels, "Not compiled: the Slang lane is off. Start the editor with GE_SHADER_SLANG_LANE=1"));
    EXPECT_EQ(texts.TextAreas, 0) << "a lane that is off is not a compile failure";
}
