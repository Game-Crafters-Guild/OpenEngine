#include "Rendering/ShaderCache/ShaderProgramIndex.h"

#include "FileSystem/FileSystem.h"
#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Materials/ShaderProfileDefines.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>

#include "Types/Fnv1a.h"

namespace GameEngine::Rendering
{
namespace
{
// File layout, little-endian: the magic, the version, the compile inputs (length-prefixed), the
// source count and each source's length-prefixed identity and content hash, the entry count, then
// each entry's input key and length-prefixed package path, sorted by key.
constexpr std::string_view kMagic = "GEPRGIDX";
// A package path is a key directory and a file name, and the compile inputs are one short line;
// anything longer than this is not an index this runtime wrote.
constexpr uint32_t kMaxStringLength = 1024;

void AppendU32(std::string& out, uint32_t value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void AppendU64(std::string& out, uint64_t value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

void AppendString(std::string& out, const std::string& value)
{
    AppendU32(out, static_cast<uint32_t>(value.size()));
    out += value;
}

// Reads fixed-width values and length-prefixed strings off a byte buffer. A read past the end
// fails, and every read after a failed one fails too.
class IndexReader
{
public:
    explicit IndexReader(std::string_view bytes) : m_Bytes(bytes) {}

    bool U32(uint32_t& out) { return Raw(&out, sizeof(out)); }
    bool U64(uint64_t& out) { return Raw(&out, sizeof(out)); }
    bool String(std::string& out)
    {
        uint32_t length = 0;
        if (!U32(length) || length > kMaxStringLength || m_Bytes.size() - m_Offset < length)
            return m_Ok = false;
        out.assign(m_Bytes.data() + m_Offset, length);
        m_Offset += length;
        return true;
    }
    bool AtEnd() const { return m_Ok && m_Offset == m_Bytes.size(); }

private:
    bool Raw(void* out, size_t size)
    {
        if (!m_Ok || m_Bytes.size() - m_Offset < size)
            return m_Ok = false;
        std::memcpy(out, m_Bytes.data() + m_Offset, size);
        m_Offset += size;
        return true;
    }

    std::string_view m_Bytes;
    size_t m_Offset = 0;
    bool m_Ok = true;
};

// A package path an index may name: relative, inside the cache root, not empty.
bool IsCachePackagePath(const std::string& packagePath)
{
    const std::filesystem::path path(packagePath);
    if (packagePath.empty() || packagePath.size() > kMaxStringLength || path.has_root_name() ||
        path.has_root_directory())
        return false;
    return std::none_of(path.begin(), path.end(), [](const std::filesystem::path& part) { return part == ".."; });
}

bool ByInputKey(const ShaderProgramIndex::Entry& a, const ShaderProgramIndex::Entry& b)
{
    return a.InputKey < b.InputKey;
}

// The content hash of a source file, or false when it cannot be read.
bool HashSourceFile(const std::filesystem::path& file, uint64_t& outHash)
{
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open())
        return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    outHash = Hashing::Fnv1a64(bytes.data(), bytes.size());
    return true;
}
} // namespace

std::string ShaderProgramIndex::CurrentCompileInputs()
{
    std::string inputs(ShaderCompileService::CacheKeyTarget());
    inputs += IsCompatShaderProfile() ? ";profile=compat" : ";profile=full";
    inputs += AreInterpolationFunctionsAvailable() ? ";interpolation=on" : ";interpolation=off";
    return inputs;
}

bool ShaderProgramIndex::Write(const std::filesystem::path& path, std::vector<Entry> entries,
                               const std::vector<std::filesystem::path>& sources, const MaterialBuildContext& context,
                               std::string* outError)
{
    auto fail = [&](std::string message) {
        if (outError)
            *outError = std::move(message);
        return false;
    };
    std::sort(entries.begin(), entries.end(), ByInputKey);
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (!IsCachePackagePath(entries[i].PackagePath))
            return fail("the program index entry '" + entries[i].PackagePath +
                        "' is not a package path inside the cache root");
        if (i > 0 && entries[i].InputKey == entries[i - 1].InputKey &&
            entries[i].PackagePath != entries[i - 1].PackagePath)
            return fail("one program index input key names two packages: '" + entries[i - 1].PackagePath +
                        "' and '" + entries[i].PackagePath + "'");
    }
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const Entry& a, const Entry& b) { return a.InputKey == b.InputKey; }),
                  entries.end());

    // Each source once, in identity order, so equal source sets write equal bytes.
    std::set<std::string> identities;
    std::vector<std::pair<std::string, uint64_t>> hashedSources;
    for (const std::filesystem::path& source : sources)
    {
        std::string identity = ShaderSourceIdentity(source, context);
        if (identity.empty() || identity.size() > kMaxStringLength || !identities.insert(identity).second)
            continue;
        uint64_t hash = 0;
        if (!HashSourceFile(source, hash))
            return fail("could not read the shader source " + source.string() + " to record it in the program index");
        hashedSources.emplace_back(std::move(identity), hash);
    }
    std::sort(hashedSources.begin(), hashedSources.end());

    std::string bytes(kMagic);
    AppendU32(bytes, kShaderProgramIndexVersion);
    AppendString(bytes, CurrentCompileInputs());
    AppendU64(bytes, hashedSources.size());
    for (const auto& [identity, hash] : hashedSources)
    {
        AppendString(bytes, identity);
        AppendU64(bytes, hash);
    }
    AppendU64(bytes, entries.size());
    for (const Entry& entry : entries)
    {
        AppendU64(bytes, entry.InputKey);
        AppendString(bytes, entry.PackagePath);
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = FileSystem::MakeTemporarySiblingPath(path);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            return fail("could not write the program index " + temp.string());
    }
    if (!FileSystem::PublishFile(temp, path))
        return fail("could not publish the program index " + path.string());
    return true;
}

std::shared_ptr<const ShaderProgramIndex> ShaderProgramIndex::Load(const std::filesystem::path& path,
                                                                   const MaterialBuildContext& context,
                                                                   std::string* outReason)
{
    auto refuse = [&](std::string reason) -> std::shared_ptr<const ShaderProgramIndex> {
        if (outReason)
            *outReason = std::move(reason);
        return nullptr;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return refuse("no program index at " + path.string());
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!std::string_view(bytes).starts_with(kMagic))
        return refuse(path.string() + " is not a program index");

    IndexReader reader(std::string_view(bytes).substr(kMagic.size()));
    uint32_t version = 0;
    if (!reader.U32(version))
        return refuse(path.string() + " is truncated");
    if (version != kShaderProgramIndexVersion)
        return refuse(path.string() + " was written under program index version " + std::to_string(version) +
                      " and this runtime reads version " + std::to_string(kShaderProgramIndexVersion) +
                      "; cook the materials again");
    std::string inputs;
    uint64_t sourceCount = 0;
    if (!reader.String(inputs) || !reader.U64(sourceCount))
        return refuse(path.string() + " is truncated");
    const std::string currentInputs = CurrentCompileInputs();
    if (inputs != currentInputs)
        return refuse(path.string() + " was cooked for '" + inputs + "' and this runtime compiles for '" +
                      currentInputs + "'; cook the materials for this device class");
    if (sourceCount > bytes.size() / (sizeof(uint64_t) + sizeof(uint32_t)))
        return refuse(path.string() + " is corrupt: it lists more sources than it holds");
    for (uint64_t i = 0; i < sourceCount; ++i)
    {
        std::string identity;
        uint64_t recordedHash = 0;
        if (!reader.String(identity) || !reader.U64(recordedHash))
            return refuse(path.string() + " is truncated");
        const std::filesystem::path file = ResolveShaderSourceIdentity(identity, context);
        uint64_t currentHash = 0;
        if (file.empty() || !HashSourceFile(file, currentHash))
            return refuse("the shader source '" + identity + "' its programs were cooked from is missing here; "
                          "cook the materials again");
        if (currentHash != recordedHash)
            return refuse("the shader source '" + identity + "' changed after its programs were cooked (" +
                          file.string() + "); cook the materials again");
    }
    uint64_t count = 0;
    if (!reader.U64(count))
        return refuse(path.string() + " is truncated");
    // Each entry takes at least its key and its length, so a larger count is a corrupt file, not
    // an allocation to attempt.
    if (count > bytes.size() / (sizeof(uint64_t) + sizeof(uint32_t)))
        return refuse(path.string() + " is corrupt: it lists more entries than it holds");

    auto index = std::make_shared<ShaderProgramIndex>();
    index->m_CacheRoot = path.parent_path();
    index->m_Entries.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i)
    {
        Entry entry;
        if (!reader.U64(entry.InputKey) || !reader.String(entry.PackagePath))
            return refuse(path.string() + " is truncated");
        if (!IsCachePackagePath(entry.PackagePath))
            return refuse(path.string() + " is corrupt: an entry names '" + entry.PackagePath + "'");
        if (!index->m_Entries.empty() && index->m_Entries.back().InputKey >= entry.InputKey)
            return refuse(path.string() + " is corrupt: its entries are not sorted by input key");
        index->m_Entries.push_back(std::move(entry));
    }
    if (!reader.AtEnd())
        return refuse(path.string() + " is corrupt: bytes follow its last entry");
    return index;
}

std::filesystem::path ShaderProgramIndex::Find(uint64_t inputKey) const
{
    const auto it = std::lower_bound(m_Entries.begin(), m_Entries.end(), inputKey,
                                     [](const Entry& entry, uint64_t key) { return entry.InputKey < key; });
    if (it == m_Entries.end() || it->InputKey != inputKey)
        return {};
    return m_CacheRoot / it->PackagePath;
}

void ShaderProgramIndex::CountLookup(bool served) const
{
    (served ? m_Hits : m_Misses).fetch_add(1, std::memory_order_relaxed);
}

ShaderProgramIndex::Counts ShaderProgramIndex::GetCounts() const
{
    return {m_Hits.load(std::memory_order_relaxed), m_Misses.load(std::memory_order_relaxed)};
}
} // namespace GameEngine::Rendering
