#pragma once

#include "Rendering/Core/Device.h"

#include <functional>
#include <vector>

namespace GameEngine::Testing
{
// Stands in for the engine's worker dispatcher: keeps every pipeline build the
// device queues until the test runs it, so the window in which a build is
// pending is observable. Removes itself from the device before its jobs go.
class HeldPipelineBuilds
{
  public:
    explicit HeldPipelineBuilds(Rendering::IDevice& device) : m_Device(device)
    {
        m_Device.SetPipelineBuildDispatcher([this](std::function<void()> job) { Jobs.push_back(std::move(job)); });
    }
    ~HeldPipelineBuilds() { m_Device.SetPipelineBuildDispatcher({}); }
    HeldPipelineBuilds(const HeldPipelineBuilds&) = delete;
    HeldPipelineBuilds& operator=(const HeldPipelineBuilds&) = delete;

    void RunAll()
    {
        std::vector<std::function<void()>> jobs = std::move(Jobs);
        Jobs.clear();
        for (std::function<void()>& job : jobs)
            job();
    }

    std::vector<std::function<void()>> Jobs;

  private:
    Rendering::IDevice& m_Device;
};
} // namespace GameEngine::Testing
