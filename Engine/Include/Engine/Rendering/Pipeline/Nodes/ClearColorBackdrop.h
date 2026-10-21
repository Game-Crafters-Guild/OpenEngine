#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
}

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// The backdrop of a view whose sky draws nothing (SkyRenderNode): its clear colour, drawn into the
// view's colour target at the frame's pre-exposure P. A clear value is a constant set on the CPU,
// which cannot know P (it lives on the GPU, written by the PreExposureSnapshot pass), so a non-black
// clear would store an absolute colour where every other writer stores colour times P. The view's
// world pass then loads the target instead of clearing it. A black clear needs no pass: 0 x P is 0.
class ClearColorBackdrop
{
  public:
    // Declares the fill for `d`'s view when its clear colour is not black. Returns whether it did.
    bool Declare(ViewDeclare& d);

  private:
    bool EnsurePipeline(Rendering::IDevice& device);

    Rendering::DescriptorSetLayoutDesc m_Layout{};
    Rendering::PipelineDesc m_Pipeline{};
    Rendering::GraphicsPipelineId m_PipelineId{};
    bool m_LoadAttempted = false;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
