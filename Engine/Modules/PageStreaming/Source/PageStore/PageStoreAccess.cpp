#include "PageStreaming/PageStore/PageStoreAccess.h"

#include "PageCodec.h"

#include "PageStreaming/PageLoader.h"
#include "PageStreaming/PageStore/PageStoreReader.h"

#include <mutex>

namespace GameEngine::PageStreaming
{

PageStoreAccess::PageStoreAccess(PageFieldKind kind, const PageStoreHeader& shape)
    : m_Kind(kind)
    , m_Shape(shape)
    , m_Levels(BuildPageStoreLevels(shape.SamplesX, shape.SamplesZ))
{
}

PageStoreAccess::~PageStoreAccess() = default;

bool PageStoreAccess::MatchesShape(const PageStoreLayout& layout) const
{
    return layout.Header.Format == m_Shape.Format && layout.Header.FaceCount == m_Shape.FaceCount &&
           layout.Header.SamplesX == m_Shape.SamplesX && layout.Header.SamplesZ == m_Shape.SamplesZ;
}

PageFetchOutcome PageStoreAccess::Fetch(const PageAddress& address) const
{
    PageFetchOutcome outcome;
    outcome.Address = address;
    std::vector<uint8> stored;
    auto payload = std::make_shared<std::vector<uint8>>();
    std::shared_lock lock(m_Mutex);
    if (m_Writing)
    {
        PageIndexEntry entry;
        if (m_Writer.ReadPage(address, stored, entry))
        {
            DecodePagePayload(*m_WriterCodec, m_Shape.Format, address.Level, entry, stored, *payload);
            outcome.Bytes = std::move(payload);
        }
        else if (entry.Offset == 0)
            outcome.Absent = true;
        else
            outcome.Error = "the page store being baked could not be read";
        return outcome;
    }
    if (!m_Reader || !m_Reader->Layout().FindPresent(address))
    {
        outcome.Absent = true;
        return outcome;
    }
    if (m_Reader->ReadPageBytes(address, stored) && m_Reader->DecodePayload(address, stored, *payload))
        outcome.Bytes = std::move(payload);
    else
        outcome.Error = "the page store could not be read";
    return outcome;
}

std::string PageStoreAccess::PublishLocked(const std::filesystem::path& file)
{
    m_Reader.reset();
    auto reader = std::make_unique<PageStoreReader>();
    if (std::string error = reader->Open(file, m_Kind); !error.empty())
        return error;
    if (!MatchesShape(reader->Layout()))
        return "the page store " + file.string() + " is for another lattice";
    m_Reader = std::move(reader);
    m_Version.fetch_add(1, std::memory_order_acq_rel);
    return {};
}

std::string PageStoreAccess::Publish(const std::filesystem::path& file)
{
    std::unique_lock lock(m_Mutex);
    if (m_Writing)
        return "the page store " + m_File.string() + " is being written: finish it first";
    return PublishLocked(file);
}

std::string PageStoreAccess::Create(const std::filesystem::path& target, PageStoreLayout layout)
{
    if (!MatchesShape(layout))
        return "the page store " + target.string() + " is for another lattice";
    std::unique_lock lock(m_Mutex);
    m_Reader.reset();
    m_WriterCodec = MakePageCodec(layout.Header);
    m_File = target;
    std::string error = m_Writer.Create(target, std::move(layout));
    m_Writing = error.empty();
    return error;
}

std::string PageStoreAccess::OpenForPatch(const std::filesystem::path& file, PageStoreLayout layout)
{
    if (!MatchesShape(layout))
        return "the page store " + file.string() + " is for another lattice";
    std::unique_lock lock(m_Mutex);
    // The patch moves the pages of the file that the reader holds open.
    m_Reader.reset();
    m_WriterCodec = MakePageCodec(layout.Header);
    m_File = file;
    std::string error = m_Writer.OpenForPatch(file, file, std::move(layout));
    m_Writing = error.empty();
    if (!m_Writing)
    {
        m_Writer.Abandon();
        PublishLocked(file);
    }
    return error;
}

std::string PageStoreAccess::AddPresentPages(std::span<const uint8> present)
{
    std::unique_lock lock(m_Mutex);
    std::string error = m_Writer.AddPresentPages(present);
    m_Writing = m_Writing && error.empty();
    return error;
}

void PageStoreAccess::Commit()
{
    m_Version.fetch_add(1, std::memory_order_acq_rel);
}

std::string PageStoreAccess::Finish()
{
    std::unique_lock lock(m_Mutex);
    m_Writing = false;
    if (std::string error = m_Writer.Finish(); !error.empty())
        return error;
    return PublishLocked(m_File);
}

void PageStoreAccess::Close()
{
    std::unique_lock lock(m_Mutex);
    m_Writer.Abandon();
    m_Writing = false;
    m_Reader.reset();
}

} // namespace GameEngine::PageStreaming
