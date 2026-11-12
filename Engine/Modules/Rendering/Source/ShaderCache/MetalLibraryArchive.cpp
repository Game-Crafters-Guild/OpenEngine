#include "Rendering/ShaderCache/MetalLibraryArchive.h"

#include <cstring>

namespace GameEngine::Rendering
{

namespace
{

// Layout: magic, version, options hash, entry count, then per entry (key, x, y, z, offset, size),
// then the libraries. Offsets count from the start of the file.
constexpr char kMagic[8] = {'G', 'E', 'M', 'T', 'L', 'L', 'I', 'B'};
constexpr uint32_t kVersion = 1;

struct Header
{
    char Magic[8];
    uint32_t Version;
    uint32_t EntryCount;
    uint64_t OptionsHash;
};

struct IndexEntry
{
    uint64_t Key;
    uint32_t LocalSize[3];
    uint32_t Reserved;
    uint64_t Offset;
    uint64_t Size;
};

} // namespace

std::vector<uint8_t> WriteMetalLibraryArchive(uint64_t optionsHash, std::span<const MetalLibraryArchiveEntry> entries)
{
    Header header{};
    std::memcpy(header.Magic, kMagic, sizeof(kMagic));
    header.Version = kVersion;
    header.EntryCount = static_cast<uint32_t>(entries.size());
    header.OptionsHash = optionsHash;
    std::vector<IndexEntry> index(entries.size());
    uint64_t offset = sizeof(Header) + sizeof(IndexEntry) * entries.size();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const MetalLibraryArchiveEntry& entry = entries[i];
        index[i] = {entry.Key, {entry.LocalSizeX, entry.LocalSizeY, entry.LocalSizeZ}, 0, offset, entry.Library.size()};
        offset += entry.Library.size();
    }
    std::vector<uint8_t> out(offset);
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), index.data(), sizeof(IndexEntry) * index.size());
    for (size_t i = 0; i < entries.size(); ++i)
        std::memcpy(out.data() + index[i].Offset, entries[i].Library.data(), entries[i].Library.size());
    return out;
}

bool ReadMetalLibraryArchive(std::span<const uint8_t> bytes,
                             uint64_t& outOptionsHash,
                             std::vector<MetalLibraryArchiveEntry>& outEntries,
                             std::string& outError)
{
    outEntries.clear();
    Header header{};
    if (bytes.size() < sizeof(header))
        return outError = "file is shorter than its header", false;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (std::memcmp(header.Magic, kMagic, sizeof(kMagic)) != 0)
        return outError = "not a Metal library archive", false;
    if (header.Version != kVersion)
        return outError = "archive format version " + std::to_string(header.Version) + " is not " +
                          std::to_string(kVersion), false;
    const uint64_t indexEnd = sizeof(header) + uint64_t{sizeof(IndexEntry)} * header.EntryCount;
    if (indexEnd > bytes.size())
        return outError = "index runs past the end of the file", false;
    outEntries.reserve(header.EntryCount);
    for (uint32_t i = 0; i < header.EntryCount; ++i)
    {
        IndexEntry entry{};
        std::memcpy(&entry, bytes.data() + sizeof(header) + sizeof(IndexEntry) * i, sizeof(entry));
        if (entry.Offset < indexEnd || entry.Size > bytes.size() || entry.Offset > bytes.size() - entry.Size)
        {
            outEntries.clear();
            return outError = "library " + std::to_string(i) + " lies outside the file", false;
        }
        outEntries.push_back({entry.Key, entry.LocalSize[0], entry.LocalSize[1], entry.LocalSize[2],
                              bytes.subspan(entry.Offset, entry.Size)});
    }
    outOptionsHash = header.OptionsHash;
    return true;
}

} // namespace GameEngine::Rendering
