#include "PageStreaming/PageStoreReader.h"

#include "PageCodec.h"
#include "PageStoreSerialization.h"

#include "AssetCore/SharedFileRead.h"

namespace GameEngine::PageStreaming
{

PageStoreReader::PageStoreReader() = default;
PageStoreReader::~PageStoreReader() = default;

std::string PageStoreReader::Open(const std::filesystem::path& file, PageFieldKind kind, uint64 baseOffset,
                                  uint64 storeBytes)
{
    m_IsOpen = false;
    m_Layout = {};
    auto reader = std::make_unique<SharedFileReader>();
    if (!reader->Open(file))
        return "the page store " + file.string() + " could not be opened";
    const int64 fileBytes = reader->Size();
    if (fileBytes < 0 || baseOffset > static_cast<uint64>(fileBytes))
        return "the page store " + file.string() + " is shorter than its offset";
    uint64 available = static_cast<uint64>(fileBytes) - baseOffset;
    if (storeBytes != 0)
    {
        if (storeBytes > available)
            return "the page store in " + file.string() + " is truncated";
        available = storeBytes;
    }
    PageStoreLayout layout;
    if (std::string reason = ReadPageStoreHead(*reader, kind, baseOffset, available, layout); !reason.empty())
        return reason + " (" + file.string() + ")";

    m_File = file;
    m_BaseOffset = baseOffset;
    m_Codec = MakePageCodec(layout.Header);
    m_Layout = std::move(layout);
    m_Reader = std::move(reader);
    m_IsOpen = true;
    return {};
}

bool PageStoreReader::ReadPageBytes(const PageAddress& address, std::vector<uint8>& outBytes) const
{
    const PageIndexEntry* entry = m_IsOpen ? m_Layout.FindPresent(address) : nullptr;
    if (!entry)
        return false;
    outBytes.resize(m_Layout.StoredBytes(*entry));
    std::lock_guard lock(m_ReadMutex);
    return m_Reader->SeekTo(m_BaseOffset + entry->Offset) &&
           m_Reader->Read(outBytes.data(), outBytes.size()) == static_cast<int64>(outBytes.size());
}

bool PageStoreReader::ReadPageSamples(const PageAddress& address, std::span<uint8> outSamples) const
{
    const uint32 stride = PageStrideTexels(m_Layout.Header.Format);
    std::vector<uint8> bytes;
    if (outSamples.size() != static_cast<std::size_t>(stride) * stride * PageTexelBytes(m_Layout.Header.Format) ||
        !ReadPageBytes(address, bytes))
        return false;
    m_Codec->Decode(address.Level, *m_Layout.FindPresent(address), bytes, outSamples);
    return true;
}

bool PageStoreReader::DecodePayload(const PageAddress& address, std::span<const uint8> stored,
                                    std::vector<uint8>& outPayload) const
{
    const PageIndexEntry* entry = m_IsOpen ? m_Layout.FindPresent(address) : nullptr;
    if (!entry || stored.size() != m_Layout.StoredBytes(*entry))
        return false;
    if (IsBlockFormat(m_Layout.Header.Format))
    {
        outPayload.assign(stored.begin(), stored.end());
        return true;
    }
    outPayload.resize(PagePayloadBytes(m_Layout.Header.Format));
    m_Codec->Decode(address.Level, *entry, stored, outPayload);
    return true;
}

} // namespace GameEngine::PageStreaming
