#pragma once

// Real binaries for engine-ABI digest fixtures: the digest folds the link stamp of the
// SDK's Engine and EditorSDK binaries (ReadBinaryLinkStamp), so a fixture needs files of
// the host's binary format that carry one.

#include "NativeScripting/EngineBuildIdentity.h"
#include "Platform/Shell.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace GameEngine::NativeScripting::Testing
{

// Writes a copy of this test executable to `path`. A nonzero `variant` is XORed into the
// first byte of the copy's link stamp, so copies with different variants differ only in
// their link identity: same size, same exports, every other byte equal.
inline void WriteStampedBinary(const std::filesystem::path& path, unsigned char variant)
{
    const std::filesystem::path self = Platform::GetExecutablePath();
    const std::string stamp = ReadBinaryLinkStamp(self);
    ASSERT_FALSE(stamp.empty()) << "the test executable carries no link stamp: " << self.generic_string();

    std::string bytes;
    {
        std::ifstream in(self, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::size_t at = bytes.find(stamp);
    ASSERT_NE(at, std::string::npos) << "link stamp not found in " << self.generic_string();
    bytes[at] = static_cast<char>(static_cast<unsigned char>(bytes[at]) ^ variant);

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(static_cast<bool>(out)) << "could not write " << path.generic_string();
}

} // namespace GameEngine::NativeScripting::Testing
