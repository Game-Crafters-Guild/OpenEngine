#include "Assets/Polyhaven/PolyhavenService.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

constexpr std::string_view kSlug = "gameengine_thumbnail_cache_test";
const std::string kPngBody("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16);
const std::string kWebpBody("RIFF\x8a\x13\0\0WEBPVP8X", 16);

// Counts the warning lines that name kSlug. The logger owns the sink; only the callback is unregistered.
class SlugWarningCounter
{
  public:
    SlugWarningCounter()
    {
        Logger::Log::Initialize({Logger::LogLevel::Debug, false});
        auto sink = std::make_unique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback([this](const Logger::LogMessage& message) { Count(message); });
    }
    ~SlugWarningCounter() { m_Sink->UnregisterCallback(m_CallbackId); }

    int Lines() const
    {
        Logger::Log::Flush();
        return m_Count.load();
    }

  private:
    void Count(const Logger::LogMessage& message)
    {
        if (message.Level == Logger::LogLevel::Warning && message.Message.find(kSlug) != std::string::npos)
            ++m_Count;
    }

    Logger::CallbackSink* m_Sink = nullptr;
    std::uint64_t m_CallbackId = 0;
    std::atomic<int> m_Count{0};
};

std::string TestSlug(int index)
{
    return std::string(kSlug) + "_" + std::to_string(index);
}

PolyhavenManifestEntry Entry(int index)
{
    return {TestSlug(index), "Test " + std::to_string(index),
            "https://cdn.polyhaven.com/asset_img/thumbs/" + TestSlug(index) + ".png?format=webp"};
}

PolyhavenService::ThumbnailFetch Answering(std::string contentType, std::string body)
{
    return [contentType, body](const std::string&) { return std::optional(PolyhavenThumbnailResponse{contentType, body}); };
}

PolyhavenLoadResult Materialize(const std::vector<PolyhavenManifestEntry>& manifest,
                                const PolyhavenService::ThumbnailFetch& fetch)
{
    return PolyhavenService::MaterializeBatch(manifest, 0, manifest.size(), "Polyhaven Model", fetch);
}

class PolyhavenThumbnailCacheTests : public ::testing::Test
{
  protected:
    static constexpr int kSlugCount = 3;

    void SetUp() override { RemoveThumbnails(); }
    void TearDown() override { RemoveThumbnails(); }

    static void RemoveThumbnails()
    {
        std::error_code ec;
        for (int index = 0; index < kSlugCount; ++index)
            std::filesystem::remove_all(PolyhavenService::GetThumbnailPath(TestSlug(index)), ec);
    }
};

// Each entry of directory whose name starts with the file name of path.
std::vector<std::filesystem::path> FilesNamedAfter(const std::filesystem::path& path)
{
    std::vector<std::filesystem::path> files;
    const std::string prefix = path.filename().string();
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
        if (entry.path().filename().string().starts_with(prefix))
            files.push_back(entry.path());
    return files;
}

TEST_F(PolyhavenThumbnailCacheTests, PngThumbnailIsCachedAndListed)
{
    const std::filesystem::path path = PolyhavenService::GetThumbnailPath(TestSlug(0));
    ASSERT_FALSE(path.empty());

    const PolyhavenLoadResult result = Materialize({Entry(0)}, Answering("image/png", kPngBody));

    ASSERT_EQ(result.pathAndLabels.size(), 1u);
    EXPECT_EQ(result.pathAndLabels[0].first, path);
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), kPngBody);
}

TEST_F(PolyhavenThumbnailCacheTests, AssetWithoutPngThumbnailIsListedWithoutPicture)
{
    const PolyhavenService::ThumbnailFetch webpOrFailure = [](const std::string& url)
    {
        if (url.find(TestSlug(0)) != std::string::npos)
            return std::optional(PolyhavenThumbnailResponse{"image/webp", kWebpBody});
        return std::optional<PolyhavenThumbnailResponse>();
    };

    const PolyhavenLoadResult result = Materialize({Entry(0), Entry(1)}, webpOrFailure);

    ASSERT_EQ(result.pathAndLabels.size(), 2u);
    for (int index = 0; index < 2; ++index)
    {
        EXPECT_EQ(result.pathAndLabels[index].first, PolyhavenService::GetThumbnailPath(TestSlug(index)));
        EXPECT_FALSE(std::filesystem::exists(result.pathAndLabels[index].first));
    }
}

TEST_F(PolyhavenThumbnailCacheTests, FailedThumbnailWriteLeavesNoFile)
{
    const std::filesystem::path path = PolyhavenService::GetThumbnailPath(TestSlug(0));
    const PolyhavenService::ThumbnailFetch blockFinalPath = [&path](const std::string&)
    {
        std::filesystem::create_directories(path);
        return std::optional(PolyhavenThumbnailResponse{"image/png", kPngBody});
    };

    const PolyhavenLoadResult result = Materialize({Entry(0)}, blockFinalPath);

    ASSERT_EQ(result.pathAndLabels.size(), 1u);
    EXPECT_FALSE(std::filesystem::is_regular_file(path));
    EXPECT_EQ(FilesNamedAfter(path), std::vector<std::filesystem::path>{path});
}

TEST_F(PolyhavenThumbnailCacheTests, ThumbnailRequestAsksForPng)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"https://h/a.png", "https://h/a.png?format=png"},
        {"https://h/a.png?format=webp&width=256", "https://h/a.png?format=png&width=256"},
        {"https://h/a.png?width=256&format=webp", "https://h/a.png?width=256&format=png"},
        {"https://h/a.png?formatX=1", "https://h/a.png?formatX=1&format=png"},
        {"https://h/a.png?Format=WEBP#top", "https://h/a.png?format=png#top"},
        {"https://h/a.png#top", "https://h/a.png?format=png#top"},
        {"https://h/a.png?&format=webp", "https://h/a.png?&format=png"},
        {"", "?format=png"},
    };
    std::vector<PolyhavenManifestEntry> manifest;
    for (const auto& [thumbUrl, expected] : cases)
        manifest.push_back({TestSlug(0), "Test", thumbUrl});
    std::vector<std::string> requested;
    const PolyhavenService::ThumbnailFetch recordUrl = [&requested](const std::string& url)
    {
        requested.push_back(url);
        return std::optional<PolyhavenThumbnailResponse>();
    };

    Materialize(manifest, recordUrl);

    ASSERT_EQ(requested.size(), cases.size());
    for (size_t index = 0; index < cases.size(); ++index)
        EXPECT_EQ(requested[index], cases[index].second) << cases[index].first;
}

TEST_F(PolyhavenThumbnailCacheTests, SlugThatIsNotPlainHasNoPathAndIsSkippedWithOneWarning)
{
    const std::string parentSlug = "../../" + std::string(kSlug);
    const std::string absoluteSlug = "C:/" + std::string(kSlug);
    EXPECT_TRUE(PolyhavenService::GetThumbnailPath(parentSlug).empty());
    EXPECT_TRUE(PolyhavenService::GetThumbnailPath(absoluteSlug).empty());
    SlugWarningCounter warnings;
    int fetches = 0;
    const PolyhavenService::ThumbnailFetch countFetch = [&fetches](const std::string&)
    {
        ++fetches;
        return std::optional<PolyhavenThumbnailResponse>();
    };

    const PolyhavenLoadResult result =
        Materialize({{parentSlug, "Parent", "https://h/a.png"}, {absoluteSlug, "Absolute", "https://h/b.png"}, Entry(0)},
                    countFetch);

    ASSERT_EQ(result.pathAndLabels.size(), 1u);
    EXPECT_EQ(result.pathAndLabels[0].first, PolyhavenService::GetThumbnailPath(TestSlug(0)));
    EXPECT_EQ(fetches, 1);
    EXPECT_EQ(warnings.Lines(), 1);
}

TEST_F(PolyhavenThumbnailCacheTests, ThumbnailsThatAreNotPngWarnOncePerBatch)
{
    SlugWarningCounter warnings;

    Materialize({Entry(0), Entry(1), Entry(2)}, Answering("image/webp", kWebpBody));

    EXPECT_EQ(warnings.Lines(), 1);
}

} // namespace
} // namespace GameEngine
