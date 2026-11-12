// Opt-in read-cost benchmark: every test is DISABLED_ and no gate runs one. Per file size, a test prints the
// cost of a read into a ByteBuffer beside the same read into a zero-filled std::vector. Quote only a Release
// number. Run: AssetPerformanceBenchmarks --gtest_also_run_disabled_tests --gtest_filter='AssetReadBenchmark.*'
#include "AssetCore/SharedFileRead.h"
#include "TestTempDir.h"
#include "Types/ByteBuffer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;

namespace
{

// Each arm reads about this many bytes per file size, from the page cache.
constexpr size_t kBytesPerArm = size_t{512} << 20;
constexpr size_t kMinReads = 8;
// A material, a mesh chunk, a terrain page, a small texture, a cooked texture, a large model.
constexpr size_t kFileSizes[] = {size_t{4} << 10,  size_t{64} << 10, size_t{256} << 10,
                                 size_t{1} << 20, size_t{16} << 20, size_t{64} << 20};

std::filesystem::path WriteBenchmarkFile(const std::filesystem::path& dir, size_t bytes)
{
    const auto path = dir / ("file_" + std::to_string(bytes) + ".bin");
    std::string content(bytes, '\0');
    for (size_t i = 0; i < bytes; ++i)
        content[i] = static_cast<char>(i * 31 + 7);
    std::ofstream(path, std::ios::binary).write(content.data(), static_cast<std::streamsize>(bytes));
    return path;
}

size_t ReadCount(size_t bytes)
{
    return std::max(kBytesPerArm / bytes, kMinReads);
}

// Nanoseconds per call of `read`, or -1 when a read fails.
template <typename Read>
double NanosecondsPerRead(size_t reads, Read&& read)
{
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < reads; ++i)
    {
        if (!read())
            return -1.0;
    }
    const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
    return elapsed.count() / static_cast<double>(reads);
}

bool ReadWholeFile(const std::filesystem::path& path, size_t bytes)
{
    ByteBuffer buffer;
    return ReadFileBytesShared(path, buffer) && buffer.Size() == bytes;
}

bool ReadWholeFileIntoZeroFilledVector(const std::filesystem::path& path, size_t bytes)
{
    SharedFileReader reader(path);
    std::vector<uint8> buffer(bytes);
    return reader.Read(buffer.data(), bytes) == static_cast<int64>(bytes);
}

bool ReadRange(SharedFileReader& reader, size_t bytes)
{
    ByteBuffer buffer(bytes);
    return reader.SeekTo(0) && reader.Read(buffer.Data(), bytes) == static_cast<int64>(bytes);
}

bool ReadRangeIntoZeroFilledVector(SharedFileReader& reader, size_t bytes)
{
    std::vector<uint8> buffer(bytes);
    return reader.SeekTo(0) && reader.Read(buffer.data(), bytes) == static_cast<int64>(bytes);
}

void PrintHeader()
{
    std::printf("%10s %8s %16s %18s\n", "bytes", "reads", "ByteBuffer ns", "zero-filled ns");
}

void PrintRow(size_t bytes, size_t reads, double bufferNs, double zeroFilledNs)
{
    std::printf("%10zu %8zu %16.0f %18.0f\n", bytes, reads, bufferNs, zeroFilledNs);
}

} // namespace

// The whole-file read (ReadFileBytesShared) beside the same read into a zero-filled vector.
TEST(AssetReadBenchmark, DISABLED_WholeFileReadBesideAZeroFilledVector)
{
    const TestUtils::ScopedTempDir dir(TestUtils::MakeUniqueTempDirectory("ge_asset_read_whole"));
    PrintHeader();
    for (const size_t size : kFileSizes)
    {
        const auto path = WriteBenchmarkFile(dir.Path(), size);
        ASSERT_TRUE(ReadWholeFile(path, size)) << path.string();
        const size_t reads = ReadCount(size);
        const double buffer = NanosecondsPerRead(reads, [&path, size] { return ReadWholeFile(path, size); });
        const double zeroFilled =
            NanosecondsPerRead(reads, [&path, size] { return ReadWholeFileIntoZeroFilledVector(path, size); });
        ASSERT_GE(buffer, 0.0) << path.string();
        ASSERT_GE(zeroFilled, 0.0) << path.string();
        PrintRow(size, reads, buffer, zeroFilled);
    }
}

// A seek and a read of the whole file, the pattern of ReadFileRange and SourceRangeReader.
TEST(AssetReadBenchmark, DISABLED_RangeReadBesideAZeroFilledVector)
{
    const TestUtils::ScopedTempDir dir(TestUtils::MakeUniqueTempDirectory("ge_asset_read_range"));
    PrintHeader();
    for (const size_t size : kFileSizes)
    {
        const auto path = WriteBenchmarkFile(dir.Path(), size);
        SharedFileReader reader(path);
        ASSERT_TRUE(ReadRange(reader, size)) << path.string();
        const size_t reads = ReadCount(size);
        const double buffer = NanosecondsPerRead(reads, [&reader, size] { return ReadRange(reader, size); });
        const double zeroFilled =
            NanosecondsPerRead(reads, [&reader, size] { return ReadRangeIntoZeroFilledVector(reader, size); });
        ASSERT_GE(buffer, 0.0) << path.string();
        ASSERT_GE(zeroFilled, 0.0) << path.string();
        PrintRow(size, reads, buffer, zeroFilled);
    }
}
