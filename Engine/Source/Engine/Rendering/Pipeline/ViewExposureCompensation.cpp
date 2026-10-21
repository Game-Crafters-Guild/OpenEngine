#include "Engine/Rendering/Pipeline/ViewExposureCompensation.h"

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline
{
namespace
{
// Persistent (read across frames: the star pass draws before this frame's resolve), one per view.
constexpr const char* kCompensationPrefix = "Pipeline.AutoExposure.Compensation.View";
} // namespace

PipelineBufferBindingRG ResolveViewExposureCompensation(ViewDeclare& d)
{
    const std::string name = std::string(kCompensationPrefix) + std::to_string(static_cast<uint32_t>(d.View.id));
    Rendering::BufferDesc desc{};
    desc.size = kViewExposureCompensationBytes;
    desc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage) |
                 static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
    desc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    desc.debugName = name.c_str();
    bool needsZeroInit = false;
    const Rendering::RenderGraph::RGBuffer buffer = d.Frame.ImportPersistentBuffer(name.c_str(), desc, &needsZeroInit);
    if (!buffer.IsValid())
        return {};
    if (needsZeroInit)
        d.Frame.AddBufferZeroInit(buffer, ("ZeroInit." + name).c_str());

    PipelineBufferBindingRG binding{};
    binding.Buffer = d.Frame.PhysicalBuffer(buffer);
    binding.Offset = 0;
    binding.Size = kViewExposureCompensationBytes;
    binding.Graph = buffer;
    return binding;
}
} // namespace GameEngine::Engine::Renderer::Pipeline
