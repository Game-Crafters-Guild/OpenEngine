#include "Engine/Rendering/Pipeline/ViewExposureHistory.h"

#include "Engine/Rendering/ExposureReadbackFeature.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline
{
static_assert(kViewExposureHistoryBytes == ExposureReadbackFeature::kStateBytes,
              "the ExposureHistory buffer holds exactly the CPU mirror's state");

namespace
{
// Persistent (never transient: pool buffers alias across frames and the state is read across
// frames), one per view.
constexpr const char* kImportedHistoryPrefix = "Pipeline.AutoExposure.History.View";
} // namespace

PipelineBufferBindingRG ResolveViewExposureHistory(ViewDeclare& d)
{
    const PipelineBufferBindingRG resolved = d.ResolveBuffer(Names::Res::ExposureHistory);
    if (resolved.IsValid() && resolved.Graph.IsValid())
        return resolved;

    const std::string name =
        std::string(kImportedHistoryPrefix) + std::to_string(static_cast<uint32_t>(d.View.id));
    Rendering::BufferDesc desc{};
    desc.size = kViewExposureHistoryBytes;
    desc.usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage) |
                 static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
    desc.memoryUsage = Rendering::BufferMemoryUsage::DeviceLocal;
    desc.debugName = name.c_str();
    bool needsZeroInit = false;
    const Rendering::RenderGraph::RGBuffer buffer =
        d.Frame.ImportPersistentBuffer(name.c_str(), desc, &needsZeroInit);
    if (!buffer.IsValid())
        return {};
    if (needsZeroInit)
        d.Frame.AddBufferZeroInit(buffer, ("ZeroInit." + name).c_str());

    PipelineBufferBindingRG binding{};
    binding.Buffer = d.Frame.PhysicalBuffer(buffer);
    binding.Offset = 0;
    binding.Size = kViewExposureHistoryBytes;
    binding.Graph = buffer;
    d.PublishBuffer(Names::Res::ExposureHistory, binding);
    return binding;
}
} // namespace GameEngine::Engine::Renderer::Pipeline
