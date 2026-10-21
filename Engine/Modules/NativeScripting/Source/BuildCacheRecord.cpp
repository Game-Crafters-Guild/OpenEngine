#include "NativeScripting/BuildCacheRecord.h"

#include "NativeScripting/EngineBuildIdentity.h"
#include "NativeScripting/NativeBuildConfig.h"

#include <cstdio>
#include <fstream>
#include <system_error>

namespace GameEngine
{
namespace NativeScripting
{
namespace
{

// Continues `seed` over a binary's link stamp (ReadBinaryLinkStamp), never its path or
// timestamps: the engine_abi marker written against the build tree's staged SDK must
// match the same SDK in any copy of the tree (a release zip extract, an isolated editor
// copy). Empty when the binary has no readable stamp.
std::optional<std::uint64_t> HashBinaryLinkStamp(const std::filesystem::path& binary, std::uint64_t seed)
{
    const std::string stamp = ReadBinaryLinkStamp(binary);
    if (stamp.empty())
        return std::nullopt;
    return Fnv1aHash(stamp, seed);
}

} // namespace

std::optional<BuildCacheRecord> ReadBuildCacheRecord(const std::filesystem::path& dir)
{
    std::ifstream in(dir / kBuildCacheFileName);
    if (!in)
        return std::nullopt;
    BuildCacheRecord record;
    std::getline(in, record.Digest);
    std::getline(in, record.DllPath);
    std::getline(in, record.AbiDigest);     // absent on older 2-line caches -> empty
    std::getline(in, record.EngineBuildId); // absent on older 3-line caches -> empty
    if (record.DllPath.empty())
        return std::nullopt;
    return record;
}

bool WriteBuildCacheRecord(const std::filesystem::path& dir, const BuildCacheRecord& record)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / kBuildCacheFileName, std::ios::trunc);
    if (!out)
        return false;
    out << record.Digest << '\n'
        << record.DllPath << '\n'
        << record.AbiDigest << '\n'
        << record.EngineBuildId << '\n';
    out.flush();
    return static_cast<bool>(out);
}

std::string ReadEngineAbiMarker(const std::filesystem::path& dir)
{
    std::ifstream in(dir / kEngineAbiMarkerFileName);
    if (!in)
        return {};
    std::string digest;
    std::getline(in, digest);
    return digest;
}

bool WriteEngineAbiMarker(const std::filesystem::path& dir, const std::string& abiDigest)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / kEngineAbiMarkerFileName, std::ios::trunc);
    if (!out)
        return false;
    out << abiDigest << '\n';
    out.flush();
    return static_cast<bool>(out);
}

std::uint64_t Fnv1aHash(std::string_view data, std::uint64_t seed)
{
    for (unsigned char c : data)
    {
        seed ^= c;
        seed *= 0x00000100000001b3ULL;
    }
    return seed;
}

std::string ToHexDigest(std::uint64_t hash)
{
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(hex);
}

std::optional<std::uint64_t> HashEngineAbiInputs(const NativeBuildConfig& config)
{
    std::optional<std::uint64_t> hash = HashBinaryLinkStamp(config.EngineBinary, Fnv1aHash(config.Config));
    if (!hash)
        return std::nullopt;
    for (const std::string& def : config.CompileDefinitions)
        hash = Fnv1aHash(def, *hash);
    // Editor-kind modules (EditorImportLib set) additionally fold the EditorSDK binary;
    // a Runtime module hashes no editor input at all.
    if (!config.EditorImportLib.empty())
        hash = HashBinaryLinkStamp(config.EditorBinary, *hash);
    return hash;
}

std::string ComputeEngineAbiDigest(const NativeBuildConfig& config)
{
    const std::optional<std::uint64_t> hash = HashEngineAbiInputs(config);
    return hash ? ToHexDigest(*hash) : std::string{};
}

std::string ComputeRuntimeEngineAbiDigest(NativeBuildConfig config)
{
    config.EditorImportLib.clear();
    config.EditorBinary.clear();
    config.EditorIncludeDirs.clear();
    return ComputeEngineAbiDigest(config);
}

} // namespace NativeScripting
} // namespace GameEngine
