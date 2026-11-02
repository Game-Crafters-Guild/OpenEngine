// The IBL EnvData UBO is an IDevice::GetFrameRingDepth()-deep ring written once per device
// frame. The ring is deeper than the device paces frames, and the surplus elements ARE the
// write-after-fence margin: the CPU write at frame F is the next touch of an element last
// read at F - ringDepth, so with 3 frames in flight the element's last reader retired two
// full frames before the write.
//
// That margin is only real if the rotation actually reaches every element.
// IDevice::GetFrameIndex() reports a slot in [0, GetFramesInFlight()) — a domain too
// narrow to name the last element — so reducing its VALUE collapses the rotation onto
// the device's pacing and strands the surplus element unwritten and unbound. These tests
// drive the feature over real device frames (BeginFrame/Present, the loop's own pacing),
// which is the domain the production callers see.

#include <gtest/gtest.h>

#include "Engine/Rendering/IEnvironmentSource.h" // complete type for the feature's unique_ptr member
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFrameCounter.h"
#include "TestDeviceHelper.h"

#include <cstdint>
#include <memory>
#include <set>
#include <vector>

using GameEngine::Engine::Renderer::ImageBasedLightingFeature;
using GameEngine::Rendering::BufferHandle;
using GameEngine::Rendering::DeviceFrameCounter;
using GameEngine::Rendering::IDevice;


class IblEnvDataRingTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
        m_Feature = std::make_unique<ImageBasedLightingFeature>();
        ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));
    }

    void TearDown() override
    {
        m_Feature.reset(); // owns device resources; must die before the device
        if (m_Device)
            m_Device->Shutdown();
    }

    // One device frame exactly as the render loop runs it: BeginFrame, the feature's
    // per-frame EnvData refresh, then Present — which arms this frame's fence and
    // rotates the device's wrapped index. Returns the element bound for that frame.
    BufferHandle RunDeviceFrame(float iblIntensity)
    {
        m_Device->BeginFrame();
        m_Feature->SetIblIntensity(iblIntensity);
        const BufferHandle bound = m_Feature->UploadEnvData(m_Device.get());
        m_Device->Present();
        return bound;
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<ImageBasedLightingFeature> m_Feature;
};

// The premise the ring depth is chosen against. If a backend ever paces as many frames
// as the ring has elements the margin is gone and the ring depth must grow with it —
// this is where that shows up.
TEST_F(IblEnvDataRingTest, RingDepthExceedsDevicePacing)
{
    const uint32_t ringDepth = m_Device->GetFrameRingDepth();
    const uint32_t pacing = m_Device->GetFramesInFlight();
    EXPECT_GT(pacing, 1u) << "a paced device is the premise of every test here";
    EXPECT_LT(pacing, ringDepth)
        << "the EnvData ring (" << ringDepth << ") must stay deeper than the device's pacing ("
        << pacing << ") or a ring write lands while the element's last reader is in flight";
}

// The device's own index domain, sampled rather than assumed: over ringDepth frames it
// must repeat, which is exactly why its value cannot select a ring element. This is the
// control for the rotation tests below — without it, "all elements reached" could be
// passing because the device happens to pace as deep as the ring.
TEST_F(IblEnvDataRingTest, DeviceFrameIndexDomainIsNarrowerThanTheRing)
{
    const uint32_t ringDepth = m_Device->GetFrameRingDepth();
    std::set<uint32_t> tokens;
    for (uint32_t f = 0; f < ringDepth; ++f)
    {
        m_Device->BeginFrame();
        tokens.insert(m_Device->GetFrameIndex());
        m_Device->Present();
    }
    EXPECT_EQ(tokens.size(), m_Device->GetFramesInFlight())
        << "the device index cycles over its pacing, not over the ring";
    EXPECT_LT(tokens.size(), static_cast<size_t>(ringDepth))
        << "the device index domain reaches " << tokens.size() << " of " << ringDepth
        << " elements: reducing its VALUE can never write the rest";
}

// Every ring element must be reached over one cycle of device frames. A wrapped selector
// leaves the surplus element on the uninitialized upload memory CreateBuffer handed out,
// never written and never bound, and the reuse distance becomes the pacing.
TEST_F(IblEnvDataRingTest, EnvDataRingRotatesThroughEveryElement)
{
    const uint32_t ringDepth = m_Device->GetFrameRingDepth();
    const uint32_t pacing = m_Device->GetFramesInFlight();
    ASSERT_GT(pacing, 1u);
    ASSERT_LT(pacing, ringDepth);

    std::vector<BufferHandle> seen;
    for (uint32_t f = 0; f < ringDepth; ++f)
        seen.push_back(RunDeviceFrame(1.0f + static_cast<float>(f)));

    for (uint32_t f = 0; f < ringDepth; ++f)
        EXPECT_TRUE(seen[f].IsValid()) << "frame " << f << " bound no EnvData element";

    for (uint32_t a = 0; a < ringDepth; ++a)
        for (uint32_t b = a + 1; b < ringDepth; ++b)
            EXPECT_NE(seen[a].id, seen[b].id)
                << "frames " << a << " and " << b << " bound the same EnvData element: the "
                   "rotation collapsed onto the device's pacing ("
                << pacing << "), so the element beyond it is never written and the reuse "
                             "distance is the pacing, not the ring depth ("
                << ringDepth << ")";
}

// The reuse distance stated as data rather than handle identity: after a full cycle each
// element still holds ITS OWN frame's iblIntensity. Two frames landing on one element
// would leave the earlier frame's buffer carrying the later frame's value.
TEST_F(IblEnvDataRingTest, EachRingElementHoldsItsOwnFrame)
{
    const uint32_t ringDepth = m_Device->GetFrameRingDepth();
    std::vector<BufferHandle> seen;
    for (uint32_t f = 0; f < ringDepth; ++f)
        seen.push_back(RunDeviceFrame(1.0f + static_cast<float>(f)));

    for (uint32_t f = 0; f < ringDepth; ++f)
    {
        ASSERT_TRUE(seen[f].IsValid());
        const void* mapped = m_Device->MapBuffer(seen[f]);
        ASSERT_NE(mapped, nullptr) << "EnvData is host-visible and persistently mapped";
        // iblIntensity is the first float of the std140 EnvData block.
        const float intensity = *static_cast<const float*>(mapped);
        m_Device->UnmapBuffer(seen[f]);
        EXPECT_FLOAT_EQ(intensity, 1.0f + static_cast<float>(f))
            << "the element bound for frame " << f << " holds another frame's EnvData: two "
               "frames of one cycle aliased onto the same element";
    }
}

// Several entry points refresh EnvData inside one frame (the IBL gen node, each view's
// world pass, the ocean and grass contributors). A repeated device token is the same
// frame, not the next one: rotating inside a frame would spend the reuse distance the
// ring depth buys, and would leave the frame's readers on different elements.
TEST_F(IblEnvDataRingTest, RepeatedObserversOfOneFrameShareTheElement)
{
    m_Device->BeginFrame();

    m_Feature->SetIblIntensity(2.0f);
    const BufferHandle first = m_Feature->UploadEnvData(m_Device.get());
    ASSERT_TRUE(first.IsValid());

    for (int observer = 0; observer < 3; ++observer)
        EXPECT_EQ(m_Feature->UploadEnvData(m_Device.get()).id, first.id)
            << "observer " << observer << " of the same device frame selected another element";

    m_Device->Present();
}

// Device-free pin on the domain claim itself, so the rule survives a machine with no
// Vulkan device (where every test above skips). For any pacing shallower than the ring,
// reducing the device's wrapped index reaches exactly `pacing` elements while the
// unwrapped counter reaches all of them.
TEST(IblEnvDataRingDomain, OnlyTheUnwrappedCounterReachesEveryElement)
{
    for (uint32_t pacing = 1; pacing <= IDevice::kMaxSupportedFramesInFlight; ++pacing)
    {
        const uint32_t ringDepth = IDevice::FrameRingDepth(pacing);
        std::set<uint32_t> wrapped;
        std::set<uint32_t> unwrapped;
        DeviceFrameCounter counter;
        for (uint32_t f = 0; f < ringDepth * 4; ++f)
        {
            const uint32_t deviceFrameIndex = f % pacing;
            wrapped.insert(deviceFrameIndex % ringDepth);
            unwrapped.insert(static_cast<uint32_t>(counter.Tick(deviceFrameIndex) % ringDepth));
        }
        EXPECT_EQ(wrapped.size(), pacing)
            << "pacing " << pacing << ": the device index can only ever name " << pacing
            << " of the ring's " << ringDepth << " elements";
        EXPECT_EQ(unwrapped.size(), (pacing > 1 ? ringDepth : 1u))
            << "pacing " << pacing << ": the unwrapped counter must rotate the whole ring";
    }
}
