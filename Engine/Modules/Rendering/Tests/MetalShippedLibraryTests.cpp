// Shipped Metal libraries: a pipeline stage whose translation inputs an export compiled offline loads
// the shipped library instead of translating and compiling MSL; other inputs or other options miss;
// a development run records each set of inputs once.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/Pipeline/PipelineTypes.h"
#include "Rendering/Core/SpecializationConstants.h"
#include "Rendering/ShaderCache/MetalLibraryArchive.h"
#include "Rendering/ShaderCache/MetalLibraryBake.h"
#include "Rendering/ShaderCache/ShaderPackageContainer.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/ShaderCache/ShaderStageRecord.h"

#include "../Source/Metal/MetalDevice.h"
#include "../Source/Metal/MetalShaderTranslator.h"
#include "../Source/Metal/MetalShippedLibraries.h"
#include "TestGlslCompile.h"

#include <mach/mach.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

#if RENDERING_HAS_SHADERC
namespace
{

using Tests::CompileGlsl;

constexpr const char* kKernel = R"(#version 450
layout(local_size_x = 8, local_size_y = 1, local_size_z = 1) in;
layout(constant_id = 0) const uint kValue = 0u;
layout(set = 0, binding = 0) buffer Out { uint values[]; };
void main() { values[gl_GlobalInvocationID.x] = kValue + 7u; }
)";

std::filesystem::path TestDirectory(const char* name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ge_metal_shipped_libraries" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// Resets the files the device reads, so one test's archive or record never reaches another.
struct ScopedPrecompiledShaderFiles
{
    explicit ScopedPrecompiledShaderFiles(PrecompiledShaderFiles files) { SetPrecompiledShaderFiles(std::move(files)); }
    ~ScopedPrecompiledShaderFiles() { SetPrecompiledShaderFiles({}); }
};

struct Kernel
{
    std::shared_ptr<const std::vector<uint8_t>> Spirv;
    DescriptorSetLayoutDesc Layout;
};

Kernel MakeKernel()
{
    Kernel kernel;
    kernel.Spirv = std::make_shared<const std::vector<uint8_t>>(CompileGlsl(kKernel, shaderc_compute_shader, "k.comp"));
    DescriptorBinding storage{};
    storage.binding = 0;
    storage.type = DescriptorType::StorageBuffer;
    storage.shaderStages = kShaderStageCompute;
    kernel.Layout.bindings.push_back(storage);
    return kernel;
}

SpecializationConstants ValueConstant(uint32_t value)
{
    SpecializationConstants constants;
    constants.AddConstant(0u, value, "kValue");
    return constants;
}

// Compiles one job with the Metal Toolchain and @p flags, as an export does. False when the toolchain is
// not installed or the compile fails.
bool CompileWithToolchain(const MetalLibraryCompileJob& job, std::span<const std::string> flags)
{
    std::string flagList;
    for (const std::string& flag : flags)
        flagList += flag + " ";
    const std::filesystem::path air = std::filesystem::path(job.Library).replace_extension(".air");
    const std::string command = "xcrun -sdk macosx metal -c " + flagList + "\"" + job.Source.string() + "\" -o \"" +
                                air.string() + "\" 2>/dev/null && xcrun -sdk macosx metallib \"" + air.string() +
                                "\" -o \"" + job.Library.string() + "\" 2>/dev/null";
    return std::system(command.c_str()) == 0;
}

// The offline library for the kernel under @p value, built as the export builds it: the runtime
// translator, then the Metal Toolchain. Empty when the toolchain is not installed.
std::vector<uint8_t> CompileOfflineLibrary(const Kernel& kernel, uint32_t value, const std::filesystem::path& dir,
                                           MetalShaderTranslation& outTranslation)
{
    std::filesystem::create_directories(dir);
    const SpecializationConstants constants = ValueConstant(value);
    outTranslation = TranslateSpirvToMsl(*kernel.Spirv, MetalShaderStage::Compute, {&kernel.Layout}, &constants);
    if (!outTranslation.Success)
        return {};
    const std::filesystem::path source = dir / "stage.metal";
    std::ofstream(source) << outTranslation.Msl;
    if (!CompileWithToolchain({source, dir / "stage.metallib"}, MetalLibraryCompileFlags(MetalFastMathEnabled())))
        return {};
    std::ifstream library(dir / "stage.metallib", std::ios::binary);
    return {std::istreambuf_iterator<char>(library), std::istreambuf_iterator<char>()};
}

// An archive with the kernel's library for @p value, written under @p optionsHash.
std::filesystem::path WriteArchive(const Kernel& kernel, uint32_t value, uint64_t optionsHash,
                                   const std::filesystem::path& dir)
{
    MetalShaderTranslation translation;
    const std::vector<uint8_t> library = CompileOfflineLibrary(kernel, value, dir, translation);
    if (library.empty())
        return {};
    const SpecializationConstants constants = ValueConstant(value);
    const MetalLibraryArchiveEntry entry{
        ComputeTranslationHash(*kernel.Spirv, MetalShaderStage::Compute, {&kernel.Layout}, &constants),
        translation.LocalSizeX, translation.LocalSizeY, translation.LocalSizeZ, library};
    const std::vector<uint8_t> bytes = WriteMetalLibraryArchive(optionsHash, std::span(&entry, 1));
    const std::filesystem::path archive = dir / std::string(kMetalLibraryArchiveFileName);
    std::ofstream(archive, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                   static_cast<std::streamsize>(bytes.size()));
    return archive;
}

std::unique_ptr<IDevice> CreateMetalDevice()
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Metal;
    desc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(desc);
    if (device && device->Initialize(desc) && device->GetAPI() == GraphicsAPI::Metal)
        return device;
    return nullptr;
}

PipelineHandle BuildKernel(IDevice& device, const Kernel& kernel, uint32_t value, const std::string& name)
{
    ComputePipelineDesc desc{};
    desc.DebugName = name;
    desc.ComputeShader = kernel.Spirv;
    desc.Specialization = ValueConstant(value);
    desc.DescriptorSetLayouts = {device.InternDescriptorSetLayout(kernel.Layout)};
    return device.GetOrCreateComputePipeline(device.InternComputePipeline(desc));
}

std::vector<std::string> ReadLines(const std::filesystem::path& file)
{
    std::vector<std::string> lines;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

std::unique_ptr<IDevice> CreateVulkanDevice()
{
    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::Vulkan;
    desc.enableSwapchain = false;
    desc.enableDynamicRendering = true;
    auto device = DeviceFactory::CreateDevice(desc);
    if (device && device->Initialize(desc) && device->GetAPI() == GraphicsAPI::Vulkan)
        return device;
    return nullptr;
}

// The values after one workgroup of the kernel ran on 64 zeroed values.
std::vector<uint32_t> DispatchOneWorkgroup(IDevice& device, PipelineHandle pipeline, const Kernel& kernel)
{
    constexpr uint32_t kCount = 64;
    BufferDesc bufferDesc{};
    bufferDesc.size = kCount * sizeof(uint32_t);
    bufferDesc.usage = static_cast<uint32_t>(BufferUsage::Storage);
    const BufferHandle buffer = device.CreateBuffer(bufferDesc);
    auto* mapped = static_cast<uint32_t*>(device.MapBuffer(buffer));
    if (mapped == nullptr)
        return {};
    std::fill(mapped, mapped + kCount, 0u);
    DescriptorSetDesc setDesc{};
    setDesc.layout = kernel.Layout;
    const DescriptorSetHandle set = device.CreateDescriptorSet(setDesc);
    device.UpdateStorageBufferBinding(set, 0, buffer, 0, bufferDesc.size);

    auto commands = device.CreateCommandList(IDevice::QueueType::Graphics);
    commands->Begin();
    commands->SetPipeline(pipeline);
    commands->BindDescriptorSet(0, set, pipeline);
    commands->Dispatch(1, 1, 1);
    commands->End();
    device.ExecuteCommandLists({commands.get()});
    device.WaitForIdle();

    std::vector<uint32_t> values(mapped, mapped + kCount);
    device.DestroyDescriptorSet(set);
    device.DestroyBuffer(buffer);
    return values;
}

} // namespace

TEST(MetalShippedLibraries, StageWithShippedLibraryIsNotTranslatedOrCompiled)
{
    const Kernel kernel = MakeKernel();
    ASSERT_FALSE(kernel.Spirv->empty());
    const std::filesystem::path dir = TestDirectory("hit");
    const std::filesystem::path archive =
        WriteArchive(kernel, 11, ComputeMetalLibraryOptionsHash(MetalFastMathEnabled()), dir);
    if (archive.empty())
        GTEST_SKIP() << "the Metal Toolchain is not installed (xcodebuild -downloadComponent MetalToolchain)";
    ScopedPrecompiledShaderFiles files({archive, {}});
    auto device = CreateMetalDevice();
    ASSERT_NE(device, nullptr);
    auto& metal = static_cast<MetalDevice&>(*device);

    const PipelineHandle pipeline = BuildKernel(*device, kernel, 11, "ShippedKernel");
    ASSERT_TRUE(pipeline.IsValid());

    EXPECT_EQ(metal.GetStageLibraryCounts().Shipped, 1u);
    EXPECT_EQ(metal.GetStageLibraryCounts().Compiled, 0u);
    // The shipped kernel runs with its own workgroup size (8) and the constant it was built with (11 + 7).
    const std::vector<uint32_t> values = DispatchOneWorkgroup(*device, pipeline, kernel);
    ASSERT_EQ(values.size(), 64u);
    for (uint32_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(values[i], i < 8 ? 18u : 0u) << "value " << i;
}

// A shipped stage is not translated, but a debug build still checks the stage's writes against the bound set.
TEST(MetalShippedLibrariesDeathTest, BindingASetThatDeclaresAWrittenBufferReadOnlyToAShippedStageAsserts)
{
#ifdef NDEBUG
    GTEST_SKIP() << "the check runs in debug builds only";
#else
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Kernel kernel = MakeKernel();
    kernel.Layout.bindings[0].flags |= kDescriptorBindingReadOnlyStorage;
    const std::filesystem::path archive =
        WriteArchive(kernel, 11, ComputeMetalLibraryOptionsHash(MetalFastMathEnabled()), TestDirectory("read-only-write"));
    if (archive.empty())
        GTEST_SKIP() << "the Metal Toolchain is not installed (xcodebuild -downloadComponent MetalToolchain)";
    ScopedPrecompiledShaderFiles files({archive, {}});
    auto device = CreateMetalDevice();
    ASSERT_NE(device, nullptr);
    const PipelineHandle pipeline = BuildKernel(*device, kernel, 11, "ShippedReadOnlyKernel");
    ASSERT_TRUE(pipeline.IsValid());
    ASSERT_EQ(static_cast<MetalDevice&>(*device).GetStageLibraryCounts().Shipped, 1u);

    EXPECT_DEATH(DispatchOneWorkgroup(*device, pipeline, kernel), "declares read-only a storage binding");
#endif
}

TEST(MetalShippedLibraries, OtherInputsOrOtherOptionsCompileAtRunTime)
{
    const Kernel kernel = MakeKernel();
    const std::filesystem::path dir = TestDirectory("stale");
    // The archive holds the kernel for value 11 only, built with the other fast-math setting.
    const std::filesystem::path staleOptions =
        WriteArchive(kernel, 11, ComputeMetalLibraryOptionsHash(!MetalFastMathEnabled()), dir / "options");
    const std::filesystem::path current =
        WriteArchive(kernel, 11, ComputeMetalLibraryOptionsHash(MetalFastMathEnabled()), dir / "inputs");
    if (staleOptions.empty() || current.empty())
        GTEST_SKIP() << "the Metal Toolchain is not installed (xcodebuild -downloadComponent MetalToolchain)";

    {
        ScopedPrecompiledShaderFiles files({staleOptions, {}});
        auto device = CreateMetalDevice();
        ASSERT_NE(device, nullptr);
        EXPECT_TRUE(BuildKernel(*device, kernel, 11, "StaleOptions").IsValid());
        EXPECT_EQ(static_cast<MetalDevice&>(*device).GetStageLibraryCounts().Shipped, 0u);
        EXPECT_EQ(static_cast<MetalDevice&>(*device).GetStageLibraryCounts().Compiled, 1u);
    }
    {
        ScopedPrecompiledShaderFiles files({current, {}});
        auto device = CreateMetalDevice();
        ASSERT_NE(device, nullptr);
        EXPECT_TRUE(BuildKernel(*device, kernel, 12, "OtherConstant").IsValid());
        EXPECT_EQ(static_cast<MetalDevice&>(*device).GetStageLibraryCounts().Shipped, 0u);
        EXPECT_EQ(static_cast<MetalDevice&>(*device).GetStageLibraryCounts().Compiled, 1u);
    }
}

TEST(MetalShippedLibraries, DevelopmentRunRecordsEachStageInputSetOnce)
{
    const Kernel kernel = MakeKernel();
    const std::filesystem::path record = TestDirectory("record") / "Pipelines" / "ShaderStages.txt";
    ScopedPrecompiledShaderFiles files({{}, record});
    {
        auto device = CreateMetalDevice();
        ASSERT_NE(device, nullptr);
        EXPECT_TRUE(BuildKernel(*device, kernel, 2, "RecordB").IsValid());
        EXPECT_TRUE(BuildKernel(*device, kernel, 1, "RecordA").IsValid());
    }
    {
        // A later run with the same inputs adds nothing.
        auto device = CreateMetalDevice();
        ASSERT_NE(device, nullptr);
        EXPECT_TRUE(BuildKernel(*device, kernel, 1, "RecordAgain").IsValid());
    }

    const std::vector<std::string> lines = ReadLines(record);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0], lines[1]);
    ShaderStageRecord parsed;
    ASSERT_TRUE(ParseShaderStageRecord(lines[0], parsed));
    EXPECT_EQ(parsed.Stage, "comp");
    EXPECT_EQ(parsed.BytesHash, HashShaderStageBytes(*kernel.Spirv));
    ASSERT_EQ(parsed.SetLayouts.size(), 1u);
    ASSERT_TRUE(parsed.SetLayouts[0].has_value());
    ASSERT_EQ(parsed.SetLayouts[0]->size(), 1u);
    EXPECT_EQ((*parsed.SetLayouts[0])[0].Type, static_cast<uint32_t>(DescriptorType::StorageBuffer));
    ASSERT_EQ(parsed.Constants.size(), 1u);
    EXPECT_EQ(parsed.Constants[0].Id, 0u);
}

// An editor and a development Player record to one workspace at the same time: neither loses the
// other's lines, and neither writes again a line that the other wrote.
TEST(MetalShippedLibraries, TwoRunsOnOneWorkspaceKeepEachOthersLines)
{
    const Kernel kernel = MakeKernel();
    const std::filesystem::path record = TestDirectory("two-runs") / "Pipelines" / "ShaderStages.txt";
    ScopedPrecompiledShaderFiles files({{}, record});
    auto editor = CreateMetalDevice();
    auto player = CreateMetalDevice();
    ASSERT_NE(editor, nullptr);
    ASSERT_NE(player, nullptr);
    EXPECT_TRUE(BuildKernel(*editor, kernel, 1, "Editor1").IsValid());
    EXPECT_TRUE(BuildKernel(*player, kernel, 2, "Player2").IsValid());
    EXPECT_TRUE(BuildKernel(*editor, kernel, 3, "Editor3").IsValid());
    EXPECT_TRUE(BuildKernel(*player, kernel, 3, "Player3").IsValid());
    EXPECT_EQ(ReadLines(record).size(), 3u);

    player.reset();
    editor.reset();
    std::vector<std::string> lines = ReadLines(record);
    std::sort(lines.begin(), lines.end());
    EXPECT_EQ(lines.size(), 3u);
    EXPECT_EQ(std::unique(lines.begin(), lines.end()), lines.end());
}

// A development run on another backend records the same line for the same stage inputs, so a
// Windows or Linux run also records the stages that the next Mac export compiles.
TEST(MetalShippedLibraries, VulkanDevelopmentRunRecordsTheLineMetalRecords)
{
    const Kernel kernel = MakeKernel();
    const std::filesystem::path dir = TestDirectory("backends");
    {
        ScopedPrecompiledShaderFiles files({{}, dir / "Metal.txt"});
        auto metal = CreateMetalDevice();
        ASSERT_NE(metal, nullptr);
        EXPECT_TRUE(BuildKernel(*metal, kernel, 5, "MetalRecord").IsValid());
    }
    {
        ScopedPrecompiledShaderFiles files({{}, dir / "Vulkan.txt"});
        auto vulkan = CreateVulkanDevice();
        if (vulkan == nullptr)
            GTEST_SKIP() << "no Vulkan device on this machine";
        EXPECT_TRUE(BuildKernel(*vulkan, kernel, 5, "VulkanRecord").IsValid());
        vulkan->Shutdown();
    }
    const std::vector<std::string> metalLines = ReadLines(dir / "Metal.txt");
    ASSERT_EQ(metalLines.size(), 1u);
    EXPECT_EQ(ReadLines(dir / "Vulkan.txt"), metalLines);
}

// The export path: a development run records the kernel, the bake compiles the recorded stage from
// the shader package that holds its SPIR-V, and an exported run loads that library and runs it.
TEST(MetalShippedLibraries, RecordedStageBakesToTheLibraryAnExportedRunLoads)
{
    const Kernel kernel = MakeKernel();
    const std::filesystem::path dir = TestDirectory("bake");
    {
        ScopedPrecompiledShaderFiles files({{}, dir / "ShaderStages.txt"});
        auto device = CreateMetalDevice();
        ASSERT_NE(device, nullptr);
        EXPECT_TRUE(BuildKernel(*device, kernel, 11, "Recorded").IsValid());
    }
    std::vector<uint8_t> package;
    const ShaderPackageChunkSource chunk{"cs-spv", *kernel.Spirv};
    ASSERT_TRUE(WriteShaderPackageContainer(std::span(&chunk, 1), package));
    std::filesystem::create_directories(dir / "Packages" / "0a");
    std::ofstream(dir / "Packages" / "0a" / "kernel.shaderpkg", std::ios::binary)
        .write(reinterpret_cast<const char*>(package.data()), static_cast<std::streamsize>(package.size()));

    // Without the toolchain a stand-in library still shows which entry the bake writes.
    bool toolchain = true;
    const std::filesystem::path roots[] = {dir / "Packages"};
    const MetalLibraryBake bake =
        BakeMetalLibraries(dir / "ShaderStages.txt", roots, dir / "Work",
                           [&](std::span<const MetalLibraryCompileJob> jobs, std::span<const std::string> flags) {
            for (const MetalLibraryCompileJob& job : jobs)
            {
                if (!CompileWithToolchain(job, flags))
                {
                    toolchain = false;
                    std::ofstream(job.Library, std::ios::binary) << "stand-in";
                }
            }
        });
    EXPECT_EQ(bake.RecordedStages, 1u);
    EXPECT_EQ(bake.ResolvedStages, 1u);
    ASSERT_EQ(bake.CompiledStages, 1u);
    uint64_t optionsHash = 0;
    std::vector<MetalLibraryArchiveEntry> entries;
    std::string error;
    ASSERT_TRUE(ReadMetalLibraryArchive(bake.Archive, optionsHash, entries, error)) << error;
    EXPECT_EQ(optionsHash, ComputeMetalLibraryOptionsHash(kShippedFastMath));
    ASSERT_EQ(entries.size(), 1u);
    const SpecializationConstants constants = ValueConstant(11);
    EXPECT_EQ(entries[0].Key,
              ComputeTranslationHash(*kernel.Spirv, MetalShaderStage::Compute, {&kernel.Layout}, &constants));
    EXPECT_EQ(entries[0].LocalSizeX, 8u);
    if (!toolchain)
        GTEST_SKIP() << "the Metal Toolchain is not installed (xcodebuild -downloadComponent MetalToolchain)";
    if (MetalFastMathEnabled() != kShippedFastMath)
        GTEST_SKIP() << "GE_METAL_PRECISE_MATH=1: a run with other compile options does not use a shipped library";

    const std::filesystem::path archive = dir / std::string(kMetalLibraryArchiveFileName);
    std::ofstream(archive, std::ios::binary)
        .write(reinterpret_cast<const char*>(bake.Archive.data()), static_cast<std::streamsize>(bake.Archive.size()));
    ScopedPrecompiledShaderFiles files({archive, {}});
    auto device = CreateMetalDevice();
    ASSERT_NE(device, nullptr);
    const PipelineHandle pipeline = BuildKernel(*device, kernel, 11, "Exported");
    ASSERT_TRUE(pipeline.IsValid());
    EXPECT_EQ(static_cast<MetalDevice&>(*device).GetStageLibraryCounts().Shipped, 1u);
    EXPECT_EQ(static_cast<MetalDevice&>(*device).GetStageLibraryCounts().Compiled, 0u);
    const std::vector<uint32_t> values = DispatchOneWorkgroup(*device, pipeline, kernel);
    ASSERT_EQ(values.size(), 64u);
    EXPECT_EQ(values[0], 18u);
    EXPECT_EQ(values[8], 0u);
}

// The device and an export's offline compile build MSL under one language version, so a stage that
// ships in one pipeline and compiles at run time in another gives the same code in both.
TEST(MetalShippedLibraries, DeviceAndExportCompileUnderOneLanguageVersion)
{
    const std::string probe = std::format("#if __METAL_VERSION__ != {}{}0\n#error other language version\n#endif\n"
                                          "kernel void probe() {{}}\n",
                                          kMetalLanguageMajor, kMetalLanguageMinor);
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    ASSERT_NE(device, nullptr);
    MTL::CompileOptions* options = MakeMetalCompileOptions();
    NS::Error* error = nullptr;
    MTL::Library* library = device->newLibrary(NS::String::string(probe.c_str(), NS::UTF8StringEncoding), options, &error);
    EXPECT_NE(library, nullptr) << (error != nullptr ? error->localizedDescription()->utf8String() : "");
    if (library != nullptr)
        library->release();
    options->release();
    device->release();

    if (std::system("xcrun -sdk macosx metal --version > /dev/null 2>&1") != 0)
        GTEST_SKIP() << "the Metal Toolchain is not installed (xcodebuild -downloadComponent MetalToolchain)";
    const std::filesystem::path dir = TestDirectory("language");
    std::ofstream(dir / "probe.metal") << probe;
    EXPECT_TRUE(CompileWithToolchain({dir / "probe.metal", dir / "probe.metallib"},
                                     MetalLibraryCompileFlags(kShippedFastMath)));
}

namespace
{

constexpr const char* kVertex = R"(#version 450
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)";

constexpr const char* kFragment = R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0); }
)";

constexpr const char* kTask = R"(#version 450
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
void main() { EmitMeshTasksEXT(1, 1, 1); }
)";

constexpr const char* kMesh = R"(#version 450
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
layout(triangles, max_vertices = 3, max_primitives = 1) out;
void main()
{
    SetMeshOutputsEXT(3, 1);
    gl_MeshVerticesEXT[0].gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
    gl_MeshVerticesEXT[1].gl_Position = vec4(1.0, 0.0, 0.0, 1.0);
    gl_MeshVerticesEXT[2].gl_Position = vec4(0.0, 1.0, 0.0, 1.0);
    gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
)";

std::shared_ptr<const std::vector<uint8_t>> CompileStage(const char* source, shaderc_shader_kind kind, const char* name)
{
    return std::make_shared<const std::vector<uint8_t>>(CompileGlsl(source, kind, name));
}

} // namespace

// A development run records every stage of a graphics pipeline: vertex and fragment, and mesh and task.
TEST(MetalShippedLibraries, DevelopmentRunRecordsEveryGraphicsStage)
{
    const std::filesystem::path record = TestDirectory("graphics") / "ShaderStages.txt";
    ScopedPrecompiledShaderFiles files({{}, record});
    auto device = CreateMetalDevice();
    ASSERT_NE(device, nullptr);
    GraphicsPipelineDesc vertexFragment{};
    vertexFragment.Kind = GraphicsPipelineKind::VertexFragment;
    vertexFragment.VertexShader = CompileStage(kVertex, shaderc_vertex_shader, "r.vert");
    vertexFragment.PixelShader = CompileStage(kFragment, shaderc_fragment_shader, "r.frag");
    GraphicsPipelineDesc meshFragment = vertexFragment;
    meshFragment.Kind = GraphicsPipelineKind::MeshFragment;
    meshFragment.VertexShader = nullptr;
    meshFragment.MeshShader = CompileStage(kMesh, shaderc_mesh_shader, "r.mesh");
    meshFragment.AmplificationShader = CompileStage(kTask, shaderc_task_shader, "r.task");
    // The device records the stages before the backend builds the pipeline, so the build result does not matter.
    device->GetOrCreateGraphicsPipeline(device->InternGraphicsPipeline(vertexFragment), {});
    device->GetOrCreateGraphicsPipeline(device->InternGraphicsPipeline(meshFragment), {});

    std::vector<std::string> stages;
    ShaderStageRecord parsed;
    for (const std::string& line : ReadLines(record))
    {
        ASSERT_TRUE(ParseShaderStageRecord(line, parsed)) << line;
        stages.push_back(parsed.Stage);
    }
    std::sort(stages.begin(), stages.end());
    EXPECT_EQ(stages, (std::vector<std::string>{"frag", "mesh", "task", "vert"}));
}

// A run that stopped inside a line leaves it without a line break; the next line starts a line of its own.
TEST(MetalShippedLibraries, LineAfterAnUnendedLineStaysWhole)
{
    const Kernel kernel = MakeKernel();
    const std::filesystem::path record = TestDirectory("unended") / "ShaderStages.txt";
    std::ofstream(record, std::ios::binary) << "comp 0123";
    ScopedPrecompiledShaderFiles files({{}, record});
    auto device = CreateMetalDevice();
    ASSERT_NE(device, nullptr);
    EXPECT_TRUE(BuildKernel(*device, kernel, 3, "AfterUnended").IsValid());
    const std::vector<std::string> lines = ReadLines(record);
    ASSERT_EQ(lines.size(), 2u);
    ShaderStageRecord parsed;
    EXPECT_FALSE(ParseShaderStageRecord(lines[0], parsed));
    EXPECT_TRUE(ParseShaderStageRecord(lines[1], parsed));
}

// The engine loads some stages from loose .spv files, not from shader packages. The bake finds those too.
TEST(MetalShippedLibraries, RecordedStageInALooseSpirvFileBakes)
{
    const Kernel kernel = MakeKernel();
    const std::filesystem::path dir = TestDirectory("loose");
    {
        ScopedPrecompiledShaderFiles files({{}, dir / "ShaderStages.txt"});
        auto device = CreateMetalDevice();
        ASSERT_NE(device, nullptr);
        EXPECT_TRUE(BuildKernel(*device, kernel, 5, "Loose").IsValid());
    }
    std::filesystem::create_directories(dir / "Shaders" / "Module");
    std::ofstream(dir / "Shaders" / "Module" / "kernel.comp.spv", std::ios::binary)
        .write(reinterpret_cast<const char*>(kernel.Spirv->data()), static_cast<std::streamsize>(kernel.Spirv->size()));

    const std::filesystem::path roots[] = {dir / "Shaders"};
    const MetalLibraryBake bake = BakeMetalLibraries(
        dir / "ShaderStages.txt", roots, dir / "Work",
        [](std::span<const MetalLibraryCompileJob> jobs, std::span<const std::string>) {
            for (const MetalLibraryCompileJob& job : jobs)
                std::ofstream(job.Library, std::ios::binary) << "stand-in";
        });
    EXPECT_EQ(bake.RecordedStages, 1u);
    EXPECT_EQ(bake.ResolvedStages, 1u);
    EXPECT_EQ(bake.CompiledStages, 1u);
}

// The resident size of this process: the pages it wrote and the pages of a file mapping it read.
uint64_t ResidentSize()
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.resident_size;
}

// A lookup reads the archive's index, not its libraries: the first stage a frame asks for does not wait
// for the whole archive.
TEST(MetalShippedLibraries, LookupDoesNotReadTheWholeArchive)
{
    constexpr size_t kLibraryBytes = 64u << 20;
    const std::filesystem::path archive = TestDirectory("lookup") / std::string(kMetalLibraryArchiveFileName);
    const uint64_t optionsHash = ComputeMetalLibraryOptionsHash(MetalFastMathEnabled());
    {
        const std::vector<uint8_t> library(kLibraryBytes, 0xA5);
        const MetalLibraryArchiveEntry entry{1, 1, 1, 1, library};
        const std::vector<uint8_t> bytes = WriteMetalLibraryArchive(optionsHash, std::span(&entry, 1));
        std::ofstream(archive, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    auto device = CreateMetalDevice();
    ASSERT_NE(device, nullptr);
    MetalShippedLibraries libraries;
    MetalShaderTranslation translation;

    const uint64_t before = ResidentSize();
    ASSERT_NE(before, 0u);
    EXPECT_EQ(libraries.NewFunction(static_cast<MetalDevice&>(*device).GetMTLDevice(), archive, optionsHash, 2,
                                    translation),
              nullptr);
    EXPECT_LT(ResidentSize(), before + kLibraryBytes / 4);
}
#endif // RENDERING_HAS_SHADERC
