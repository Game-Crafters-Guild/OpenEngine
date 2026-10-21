#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "InspectorRegistry.h"
#include "Inspectors/RenderPipelineInspector.h"

#include "Assets/RenderPipelineAsset.h"
#include "UI/UIElement.h"

#include "TestTempDir.h"

using namespace GameEngine;

namespace
{
static void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}
} // namespace

TEST(RenderPipelineInspector, BuildsUiForComputeAndFullscreenPasses)
{
    RegisterRenderPipelineInspector();
    InspectorFn* fn = InspectorRegistry::Get().TryGetAssetInspector(AssetType::RenderPipeline);
    ASSERT_NE(fn, nullptr);

    const TestUtils::ScopedTempDir tmpDir{TestUtils::MakeUniqueTempDirectory("ge_editor_tests")};
    const std::filesystem::path pipePath = tmpDir.Path() / "TestPipe.rendergraph";

    const std::string json = R"JSON(
{
  "schemaVersion": 2,
  "pipelineName": "TestPipe",
  "resources": {
    "ClusterParams": {
      "kind": "Buffer",
      "scope": "PerView",
      "memoryUsage": "Upload",
      "usage": ["ConstantBuffer"],
      "flags": ["PersistentlyMapped"],
      "size": {
        "bytes": 16,
        "expression": "clustersX * depthSlices * 4",
        "variables": {
          "clustersX": "ceil(renderWidth / 16)",
          "depthSlices": 24
        }
      }
    },
    "ClusterDebug": {
      "kind": "Texture",
      "scope": "PerView",
      "format": "RGBA8_UNORM",
      "usage": ["RenderTarget", "ShaderResource"],
      "extent": { "scale": [0.5, 0.5] }
    }
  },
  "passes": [
    {
      "id": "Cull",
      "type": "ComputeShader",
      "enabled": true,
      "shaderPkg": "",
      "dispatch": { "x": "ceil(renderWidth / 16)", "y": "ceil(renderHeight / 16)", "z": 1 },
      "inputs": { "DepthTex": "View.Depth" },
      "inputUsages": { "DepthTex": "DepthRead" },
      "buffers": { "ClusterParams": "ClusterParams" },
      "bufferUsages": { "ClusterParams": "Sampled" }
    },
    {
      "id": "Overlay",
      "type": "FullscreenShader",
      "enabled": true,
      "shaderPkg": "",
      "alphaBlend": true,
      "output": "View.Resolve",
      "inputs": { "ColorIn": "View.Color" },
      "buffers": { "ClusterParams": "ClusterParams" },
      "bufferUsages": { "ClusterParams": "StorageRead" }
    }
  ],
  "outputs": { "FinalColor": "View.Resolve" }
}
)JSON";

    WriteTextFile(pipePath, json);

    RenderPipelineAsset pipe(GUID::Generate(), pipePath);
    ASSERT_TRUE(pipe.Load());
    ASSERT_TRUE(pipe.GetErrors().empty());

    UIElement root("div");
    InspectorContext ctx{};
    ctx.Parent = &root;
    ctx.Object = &pipe;

    // Must not crash.
    (*fn)(ctx);

    // Basic sanity: inspector added content.
    EXPECT_GT(root.GetChildren().size(), 0u);

    // Verify a few key controls exist (IDs are set in the inspector implementation).
    EXPECT_NE(root.FindById("rp-pass-0-type"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-0-dispatch-x"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-0-inputs-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-1-type"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-1-alphaBlend"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-pass-1-output"_sid), nullptr);

    // Verify resource editor controls exist.
    EXPECT_NE(root.FindById("rp-res-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-kind"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-scope"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-bytes"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-memoryUsage"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-usage-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-flags-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-vars-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-var-clustersX-key"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-var-clustersX-type"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-var-clustersX-string"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterParams-size-var-depthSlices-number"_sid), nullptr);

    EXPECT_NE(root.FindById("rp-res-ClusterDebug-kind"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-format"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-usage-add"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-extent-mode"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-extent-scale-x"_sid), nullptr);
    EXPECT_NE(root.FindById("rp-res-ClusterDebug-extent-scale-y"_sid), nullptr);
}

