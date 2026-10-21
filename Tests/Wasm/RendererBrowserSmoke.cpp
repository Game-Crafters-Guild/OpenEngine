// Browser rendering smoke for the wasm target (web platform plan, Phase 4).
// The engine's WebGPU backend brings up the real browser device: async
// adapter/device request, canvas surface + swapchain through the surface
// bridge, then a rAF-driven loop that clears to an animated color and draws
// one green triangle through the backend's pipeline path (WGSL — browser WebGPU
// has no SPIR-V ingestion). The triangle's position and colour streams sit at
// engine vertex bindings UV7 and Weights1, past the first six, so a backend that
// binds only the low bindings fails the draw's validation. A nine-stream pipeline,
// one past the eight vertex buffers Chrome binds, must be refused at creation
// with its streams named. One log line per milestone; a heartbeat every
// kHeartbeatFrames keeps the console proving liveness. A manual check, not a
// gate:
//
//   python Tools/Web/browser_gate.py --root build/wasm-release-singlethread/bin
//       --page RendererBrowserSmoke.html
//       --expect "SMOKE: nine-stream pipeline refused:"
//       --expect "graphics pipeline 'BrowserSmoke.NineStreams' is refused: its mesh has 9 vertex streams"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <emscripten/emscripten.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace
{

using namespace GameEngine::Rendering;

constexpr int kHeartbeatFrames = 60;

// Clip-space position (location 0) and colour (location 4) from the vertex
// streams. Entry points are "main" to match the backend's shared entry-point
// convention.
constexpr const char kTriangleVertWgsl[] = R"(
struct Out { @builtin(position) position : vec4f, @location(0) color : vec4f };
@vertex
fn main(@location(0) position : vec3f, @location(4) color : vec4f) -> Out {
    var out : Out;
    out.position = vec4f(position, 1.0);
    out.color = color;
    return out;
}
)";

constexpr const char kTriangleFragWgsl[] = R"(
@fragment
fn main(@location(0) color : vec4f) -> @location(0) vec4f {
    return color;
}
)";

std::shared_ptr<const std::vector<uint8_t>> WgslBlob(const char* text)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    // Exclude the trailing NUL: the blob is sized UTF-8 text.
    return std::make_shared<const std::vector<uint8_t>>(bytes, bytes + std::char_traits<char>::length(text));
}

struct SmokeState
{
    GLFWwindow* Window = nullptr;
    std::unique_ptr<IDevice> Device;
    WindowTargetHandle WindowTarget;
    PipelineHandle Pipeline;
    BufferHandle Positions; // bound at engine binding UV7
    BufferHandle Colors;    // bound at engine binding Weights1
    uint32_t Width = 0;
    uint32_t Height = 0;
    int Frame = 0;
    bool FirstFramePresented = false;
};

SmokeState g_State;

[[noreturn]] void Fail(const char* what)
{
    std::printf("SMOKE FAIL: %s\n", what);
    std::exit(1);
}

void Frame()
{
    SmokeState& s = g_State;

    // The canvas can be resized by CSS at any time: re-poll each frame and
    // recreate the swapchain on change.
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(s.Window, &width, &height);
    if (width > 0 && height > 0 &&
        (static_cast<uint32_t>(width) != s.Width || static_cast<uint32_t>(height) != s.Height))
    {
        s.Width = static_cast<uint32_t>(width);
        s.Height = static_cast<uint32_t>(height);
        if (s.Frame > 0)
        {
            s.Device->RecreateWindowTargetSwapchain(s.WindowTarget, s.Width, s.Height);
        }
    }

    if (!s.Device->BeginFrame())
    {
        return;
    }

    const TextureHandle backbuffer = s.Device->GetCurrentSwapchainImageHandle();
    if (!backbuffer.IsValid())
    {
        return;
    }

    auto commandList = s.Device->CreateCommandList(IDevice::QueueType::Graphics);
    commandList->Begin();

    RenderPassDesc pass{};
    pass.colorTargets[0] = backbuffer;
    pass.colorTargetCount = 1;
    pass.clearColor[0] = true;
    const float phase = static_cast<float>(s.Frame) * 0.02f;
    pass.clearColorValue[0][0] = 0.1f;
    pass.clearColorValue[0][1] = 0.2f + 0.15f * std::sin(phase);
    pass.clearColorValue[0][2] = 0.45f;
    pass.clearColorValue[0][3] = 1.0f;
    pass.depthTarget = INVALID_TEXTURE_HANDLE;
    pass.clearDepth = false;
    commandList->BeginRenderPass(pass);
    commandList->SetPipeline(s.Pipeline);
    commandList->SetViewport(0.0f, 0.0f, static_cast<float>(s.Width), static_cast<float>(s.Height));
    commandList->SetScissor(0, 0, s.Width, s.Height);
    commandList->SetVertexBuffer(s.Positions, VertexBinding::UV7);
    commandList->SetVertexBuffer(s.Colors, VertexBinding::Weights1);
    commandList->Draw(3);
    commandList->EndRenderPass();
    commandList->End();

    std::vector<CommandList*> lists{commandList.get()};
    s.Device->ExecuteCommandLists(lists);
    s.Device->Present();

    if (!s.FirstFramePresented)
    {
        s.FirstFramePresented = true;
        std::printf("SMOKE: first-frame-presented (%ux%u)\n", s.Width, s.Height);
    }
    ++s.Frame;
    if (s.Frame % kHeartbeatFrames == 0)
    {
        std::printf("SMOKE: heartbeat frame=%d\n", s.Frame);
    }
}

// The triangle's streams: its positions and a green colour per vertex.
void CreateTriangleStreams(SmokeState& s)
{
    const float positions[] = {-0.8f, -0.8f, 0.0f, 0.8f, -0.8f, 0.0f, 0.0f, 0.8f, 0.0f};
    const float colors[] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    BufferDesc desc{};
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.usage = static_cast<uint32_t>(BufferUsage::Vertex);
    desc.size = sizeof(positions);
    s.Positions = s.Device->CreateBuffer(desc);
    desc.size = sizeof(colors);
    s.Colors = s.Device->CreateBuffer(desc);
    if (!s.Positions.IsValid() || !s.Colors.IsValid())
    {
        Fail("triangle vertex buffers");
    }
    s.Device->UpdateBuffer(s.Positions, 0, sizeof(positions), positions);
    s.Device->UpdateBuffer(s.Colors, 0, sizeof(colors), colors);
}

// Creates `triangle`'s program over nine vertex streams (bindings Core to UV2), one
// past the eight vertex buffers Chrome binds, and logs whether the device refused it
// and why.
void ReportNineStreamPipeline(IDevice& device, const GraphicsPipelineDesc& triangle,
                              const PipelineFormatKey& formatKey)
{
    GraphicsPipelineDesc wide = triangle;
    wide.DebugName = "BrowserSmoke.NineStreams";
    wide.VertexBindings.clear();
    for (uint32_t binding = VertexBinding::Core; binding <= VertexBinding::UV2; ++binding)
    {
        wide.VertexBindings.push_back({binding, 4 * sizeof(float), /*inputRate=*/0});
    }
    wide.VertexAttributes = {{0, VertexBinding::Core, Format::R32G32B32_FLOAT, 0},
                             {4, VertexBinding::Weights1, Format::R32G32B32A32_FLOAT, 0}};
    const PipelineHandle pipeline =
        device.GetOrCreateGraphicsPipeline(device.InternGraphicsPipeline(wide), formatKey);
    if (pipeline.IsValid())
    {
        std::printf("SMOKE: nine-stream pipeline created\n");
        return;
    }
    std::printf("SMOKE: nine-stream pipeline refused: %s\n", device.GetLastGraphicsPipelineFailure().c_str());
}

} // namespace

int main()
{
    // Deliberately no Log::Initialize: the uninitialized logger's synchronous
    // console fallback prints on the calling thread, so no drain-thread write
    // races the runtime's unflushed-stdio check at the first ASYNCIFY unwind.
    std::printf("SMOKE: boot (threads=%d)\n",
#if defined(GE_WASM_SINGLE_THREAD)
                0
#else
                1
#endif
    );

    if (glfwInit() != GLFW_TRUE)
    {
        Fail("glfwInit");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Initial canvas backing size; the per-frame poll follows CSS resizes.
    g_State.Window = glfwCreateWindow(1280, 720, "RendererBrowserSmoke", nullptr, nullptr);
    if (g_State.Window == nullptr)
    {
        Fail("glfwCreateWindow (canvas)");
    }

    DeviceDesc desc{};
    desc.preferredAPI = GraphicsAPI::WebGPU;
    desc.enableSwapchain = true;
    desc.vsync = true;
    desc.applicationName = "RendererBrowserSmoke";
    g_State.Device = DeviceFactory::CreateDevice(desc);
    if (g_State.Device == nullptr || !g_State.Device->Initialize(desc))
    {
        Fail("WebGPU device init (adapter/device request)");
    }
    std::printf("SMOKE: adapter+device ready\n");

    g_State.WindowTarget = g_State.Device->CreateWindowTarget(g_State.Window, 1280, 720);
    if (!g_State.WindowTarget.IsValid() || !g_State.Device->SetActiveWindowTarget(g_State.WindowTarget))
    {
        Fail("canvas surface / swapchain creation");
    }
    std::printf("SMOKE: surface+swapchain ready\n");

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.VertexShader = WgslBlob(kTriangleVertWgsl);
    pipelineDesc.PixelShader = WgslBlob(kTriangleFragWgsl);
    pipelineDesc.Rasterization.cullMode = CullModeFlagBits::None;
    pipelineDesc.ColorBlend.attachments.emplace_back();
    pipelineDesc.DebugName = "BrowserSmoke.Triangle";
    pipelineDesc.VertexBindings = {{VertexBinding::UV7, 3 * sizeof(float), /*inputRate=*/0},
                                   {VertexBinding::Weights1, 4 * sizeof(float), /*inputRate=*/0}};
    pipelineDesc.VertexAttributes = {{0, VertexBinding::UV7, Format::R32G32B32_FLOAT, 0},
                                     {4, VertexBinding::Weights1, Format::R32G32B32A32_FLOAT, 0}};
    CreateTriangleStreams(g_State);
    const GraphicsPipelineId pipelineId = g_State.Device->InternGraphicsPipeline(pipelineDesc);

    PipelineFormatKey formatKey{};
    formatKey.ColorFormats[0] = g_State.Device->GetSwapchainTextureFormat();
    formatKey.ColorCount = 1;
    g_State.Pipeline = g_State.Device->GetOrCreateGraphicsPipeline(pipelineId, formatKey);
    if (!g_State.Pipeline.IsValid())
    {
        Fail("triangle pipeline creation (WGSL)");
    }
    std::printf("SMOKE: pipeline ready\n");
    ReportNineStreamPipeline(*g_State.Device, pipelineDesc, formatKey);

    // rAF drive: the browser presents when each callback returns (the same
    // seam Application::RunPlatformLoop targets).
    emscripten_set_main_loop(&Frame, 0, /*simulate_infinite_loop*/ 0);
    return 0;
}
