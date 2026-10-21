#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace GameEngine::Rendering
{

// One pipeline creation at a time per shader-stage set; any number at once across sets.
//
// vkCreate*Pipelines runs concurrently on the device's VkPipelineCache (see
// VulkanPipelineCache::GetVkPipelineCache). Many pipelines share their stages and differ in
// fixed-function state: a material's front-face twins, or materials whose composed shaders
// are byte-identical. Created at the same time, each compiles the stages cold; created one
// after the other, the later ones find the compiled stages in the pipeline cache and cost
// about a millisecond (measured on NVIDIA: the front-face twin of a 1.7 s pipeline reports
// VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT in 0.1 ms). So a creation
// waits while another creation with the same stage key runs, and creations with different
// keys never wait on each other. A compute pipeline has no fixed-function state, so its stage
// key is nearly its whole pipeline key and the gate rarely makes a compute creation wait.
class VulkanPipelineCompileGate
{
  public:
    // Held for the duration of one vkCreate*Pipelines call; releases its key on destruction.
    class Hold
    {
      public:
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        ~Hold();

      private:
        friend class VulkanPipelineCompileGate;
        Hold(VulkanPipelineCompileGate* gate, uint64_t key) noexcept;

        VulkanPipelineCompileGate* m_Gate;
        uint64_t m_Key;
    };

    // Blocks while another Hold of `stageKey` is alive.
    [[nodiscard]] Hold Enter(uint64_t stageKey);

    // Identifies a pipeline's stages: the SPIR-V of every stage, in stage order, and the
    // specialization constants they are created with.
    static uint64_t StageKey(std::span<const std::span<const uint8_t>> stages,
                             const VkSpecializationInfo* specialization);

  private:
    void Leave(uint64_t stageKey);

    std::mutex m_Mutex;
    std::condition_variable m_Left;
    std::vector<uint64_t> m_Running; // Keys with a live Hold: at most one per creating thread
};

} // namespace GameEngine::Rendering
