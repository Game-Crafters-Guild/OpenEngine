// DescriptorSetCache alone, then IDevice::GetOrCreateDescriptorSet on each backend in the build.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"

#include "Memory/AllocationCountScope.h"

#include "../Source/Core/DescriptorSetCache.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;

namespace
{

constexpr uint64_t kCachedSetBufferSize = 256;

DescriptorSetLayoutDesc StorageBufferLayout()
{
    DescriptorSetLayoutDesc layout;
    layout.bindings.push_back({.binding = 0, .type = DescriptorType::StorageBuffer, .shaderStages = kShaderStageCompute});
    return layout;
}

DescriptorWrite StorageBufferWrite(BufferHandle buffer, uint64_t range = kCachedSetBufferSize)
{
    return {.Binding = 0, .Type = DescriptorType::StorageBuffer, .Buffer = buffer, .Range = range};
}

} // namespace

TEST(DescriptorSetCache, ContentsThatShareAKeyKeepTheirOwnSets)
{
    DescriptorSetCache cache;
    const DescriptorSetLayoutDesc layout = StorageBufferLayout();
    const DescriptorWrite first[] = {StorageBufferWrite(BufferHandle(1))};
    const DescriptorWrite second[] = {StorageBufferWrite(BufferHandle(2))};
    const uint64_t sharedKey = DescriptorSetCache::HashContent(layout, first);
    const DescriptorSetHandle firstSet(11);
    const DescriptorSetHandle secondSet(12);

    ASSERT_EQ(cache.Insert(sharedKey, layout, first, firstSet), firstSet);
    EXPECT_FALSE(cache.Find(sharedKey, layout, second).IsValid()) << "a different content got the set of the key";
    EXPECT_EQ(cache.Insert(sharedKey, layout, second, secondSet), secondSet);
    EXPECT_EQ(cache.Find(sharedKey, layout, second), secondSet);
    EXPECT_FALSE(cache.Find(sharedKey, layout, first).IsValid()) << "the replaced content still has a set";
}

TEST(DescriptorSetCache, SetThatLosesItsKeyExpiresOnTheAgeRule)
{
    DescriptorSetCache cache;
    const DescriptorSetLayoutDesc layout = StorageBufferLayout();
    const DescriptorWrite first[] = {StorageBufferWrite(BufferHandle(1))};
    const DescriptorWrite second[] = {StorageBufferWrite(BufferHandle(2))};
    const uint64_t sharedKey = DescriptorSetCache::HashContent(layout, first);
    const DescriptorSetHandle firstSet(11);
    const DescriptorSetHandle secondSet(12);
    cache.Insert(sharedKey, layout, first, firstSet);
    cache.Insert(sharedKey, layout, second, secondSet);

    // The second set is used in each frame, so only the replaced set expires.
    std::vector<DescriptorSetHandle> expired;
    for (uint64_t frame = 0; frame < DescriptorSetCache::kFramesKeptAfterLastUse; ++frame)
    {
        cache.Age(expired);
        ASSERT_EQ(cache.Find(sharedKey, layout, second), secondSet);
    }
    EXPECT_TRUE(expired.empty()) << "the replaced set expired before the kept frames";
    cache.Age(expired);
    EXPECT_EQ(expired, std::vector<DescriptorSetHandle>{firstSet}) << "the replaced set did not expire";
}

namespace
{

std::vector<GraphicsAPI> BackendsInThisBuild()
{
    std::vector<GraphicsAPI> apis;
#if defined(RENDERING_HAS_VULKAN)
    apis.push_back(GraphicsAPI::Vulkan);
#endif
#if defined(RENDERING_HAS_METAL)
    apis.push_back(GraphicsAPI::Metal);
#endif
    return apis;
}

std::string BackendName(const ::testing::TestParamInfo<GraphicsAPI>& info)
{
    return info.param == GraphicsAPI::Vulkan ? "Vulkan" : "Metal";
}

std::unique_ptr<IDevice> CreateHeadlessDevice(GraphicsAPI api)
{
    DeviceDesc desc{};
    desc.preferredAPI = api;
    desc.enableDynamicRendering = true;
    desc.enableSwapchain = false;
    auto device = DeviceFactory::CreateDevice(desc);
    if (!device || !device->Initialize(desc) || device->GetAPI() != api)
        return nullptr;
    return device;
}

// Sets from GetOrCreateDescriptorSet over one storage buffer. A call that allocates nothing on
// this thread returned a cached set: a build allocates for the backend set and the cache entry.
class DescriptorSetContentCacheTest : public ::testing::TestWithParam<GraphicsAPI>
{
  protected:
    void SetUp() override
    {
        m_Device = CreateHeadlessDevice(GetParam());
        if (!m_Device)
            GTEST_SKIP() << "no headless device for this backend";
        m_Layout = StorageBufferLayout();
        for (BufferHandle& buffer : m_Buffers)
        {
            BufferDesc desc{};
            desc.size = kCachedSetBufferSize;
            desc.usage = static_cast<uint32_t>(BufferUsage::Storage);
            buffer = m_Device->CreateBuffer(desc);
            ASSERT_TRUE(buffer.IsValid());
        }
        ASSERT_TRUE(m_Device->BeginFrame());
    }

    void TearDown() override
    {
        if (!m_Device)
            return;
        m_Device->FinalizeFrame();
        m_Device->WaitForIdle();
        for (BufferHandle buffer : m_Buffers)
            if (buffer.IsValid())
                m_Device->DestroyBuffer(buffer);
        m_Device->Shutdown();
    }

    void RunFrames(uint64_t count)
    {
        for (uint64_t frame = 0; frame < count; ++frame)
        {
            m_Device->FinalizeFrame();
            ASSERT_TRUE(m_Device->BeginFrame());
        }
    }

    DescriptorSetHandle GetSet(BufferHandle buffer, uint64_t& allocations, uint64_t range = kCachedSetBufferSize)
    {
        const DescriptorWrite writes[] = {StorageBufferWrite(buffer, range)};
        const GameEngine::Memory::AllocationCountScope scope(GameEngine::Memory::CountWindow::ThisThread);
        const DescriptorSetHandle set = m_Device->GetOrCreateDescriptorSet(m_Layout, writes, "CachedSetTest");
        allocations = scope.Count();
        return set;
    }

    std::unique_ptr<IDevice> m_Device;
    DescriptorSetLayoutDesc m_Layout;
    std::array<BufferHandle, 2> m_Buffers{};
};

} // namespace

TEST_P(DescriptorSetContentCacheTest, UnchangedWritesReturnTheCachedSetAndAllocateNothing)
{
    uint64_t buildAllocations = 0;
    const DescriptorSetHandle built = GetSet(m_Buffers[0], buildAllocations);
    ASSERT_TRUE(built.IsValid());
    EXPECT_GT(buildAllocations, 0u);

    RunFrames(1);
    uint64_t hitAllocations = 0;
    EXPECT_EQ(GetSet(m_Buffers[0], hitAllocations), built);
    EXPECT_EQ(hitAllocations, 0u) << "the second call with the same writes built a new set";
}

TEST_P(DescriptorSetContentCacheTest, ChangedWriteBuildsANewSet)
{
    uint64_t allocations = 0;
    const DescriptorSetHandle first = GetSet(m_Buffers[0], allocations);
    const DescriptorSetHandle second = GetSet(m_Buffers[1], allocations);
    ASSERT_TRUE(first.IsValid() && second.IsValid());
    EXPECT_NE(second, first);
    EXPECT_GT(allocations, 0u);

    EXPECT_EQ(GetSet(m_Buffers[0], allocations), first);
    EXPECT_EQ(allocations, 0u);
}

TEST_P(DescriptorSetContentCacheTest, ChangedRangeBuildsANewSet)
{
    uint64_t allocations = 0;
    const DescriptorSetHandle whole = GetSet(m_Buffers[0], allocations);
    const DescriptorSetHandle half = GetSet(m_Buffers[0], allocations, kCachedSetBufferSize / 2);
    ASSERT_TRUE(whole.IsValid() && half.IsValid());
    EXPECT_NE(half, whole);
    EXPECT_GT(allocations, 0u);
}

TEST_P(DescriptorSetContentCacheTest, DestroyedResourceEvictsItsSets)
{
    uint64_t allocations = 0;
    const DescriptorSetHandle cached = GetSet(m_Buffers[0], allocations);
    ASSERT_TRUE(cached.IsValid());
    const BufferHandle destroyed = m_Buffers[0];
    m_Device->DestroyBuffer(destroyed);
    m_Buffers[0] = {};

    // The evicted set lives until the age rule destroys it, so a new set has a new handle.
    EXPECT_NE(GetSet(destroyed, allocations), cached);
    EXPECT_GT(allocations, 0u);
}

TEST_P(DescriptorSetContentCacheTest, DestroyedSamplerEvictsItsSets)
{
    DescriptorSetLayoutDesc layout;
    layout.bindings.push_back({.binding = 0, .type = DescriptorType::Sampler, .shaderStages = kShaderStageCompute});
    const SamplerHandle sampler = m_Device->CreateSampler(SamplerDesc::PointClamp());
    ASSERT_TRUE(sampler.IsValid());
    const DescriptorWrite writes[] = {{.Binding = 0, .Type = DescriptorType::Sampler, .Sampler = sampler}};
    const DescriptorSetHandle cached = m_Device->GetOrCreateDescriptorSet(layout, writes, "CachedSetTest");
    ASSERT_TRUE(cached.IsValid());
    m_Device->DestroySampler(sampler);
    EXPECT_NE(m_Device->GetOrCreateDescriptorSet(layout, writes, "CachedSetTest"), cached);
}

TEST_P(DescriptorSetContentCacheTest, UnusedSetLivesForTheKeptFramesThenIsRebuilt)
{
    uint64_t allocations = 0;
    const DescriptorSetHandle built = GetSet(m_Buffers[0], allocations);
    ASSERT_TRUE(built.IsValid());

    RunFrames(DescriptorSetCache::kFramesKeptAfterLastUse);
    EXPECT_EQ(GetSet(m_Buffers[0], allocations), built);
    EXPECT_EQ(allocations, 0u) << "evicted after " << DescriptorSetCache::kFramesKeptAfterLastUse << " frames";

    RunFrames(DescriptorSetCache::kFramesKeptAfterLastUse + 1);
    GetSet(m_Buffers[0], allocations);
    EXPECT_GT(allocations, 0u) << "still cached after " << DescriptorSetCache::kFramesKeptAfterLastUse + 1
                               << " frames without use";
}

INSTANTIATE_TEST_SUITE_P(Backends, DescriptorSetContentCacheTest, ::testing::ValuesIn(BackendsInThisBuild()),
                         BackendName);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(DescriptorSetContentCacheTest);
