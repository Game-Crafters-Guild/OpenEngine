#include "ShaderSourceCache.h"

#include "Rendering/Materials/Shader/ShaderComposer.h"
#include "ShaderIncludeClosure.h"
#include "Types/StringUtils.h"

#include <ranges>
#include <utility>

namespace GameEngine { namespace Rendering {

namespace {

// Files this thread has read from disk through the cache, a failed read included.
thread_local uint64_t t_FileReadsOnThisThread = 0;

// `dir / operand` when it exists.
ShaderSourceCache::ResolutionPtr TryResolveIn(const std::filesystem::path& dir, const std::filesystem::path& operand)
{
    std::error_code ec;
    std::filesystem::path candidate = dir / operand;
    if (!std::filesystem::exists(candidate, ec))
        return nullptr;
    auto resolution = std::make_shared<ShaderSourceCache::Resolution>();
    resolution->Key = ShaderSourceCache::NormalizedKey(candidate);
    resolution->CandidateDir = candidate.parent_path();
    resolution->Candidate = std::move(candidate);
    return resolution;
}

} // namespace

ShaderSourceCache& ShaderSourceCache::Get()
{
    static ShaderSourceCache instance;
    return instance;
}

std::string ShaderSourceCache::NormalizedKey(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
    if (ec)
        absolutePath = path;
    return absolutePath.lexically_normal().string();
}

std::string ShaderSourceCache::MakeRootsSignature(const std::vector<std::filesystem::path>& roots)
{
    return Join(roots | std::views::transform([](const std::filesystem::path& root) { return root.string(); }), "\n");
}

ShaderSourceCache::EntryPtr ShaderSourceCache::Read(const std::filesystem::path& path,
                                                    const std::string& normalizedKey)
{
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const auto it = m_Entries.find(normalizedKey);
        if (it != m_Entries.end())
            return it->second;
        generation = m_Generation;
    }

    // Read, hash and scan outside the lock: on a cold cache a parallel prewarm
    // must not serialize every worker behind one file read.
    std::string bytes;
    ++t_FileReadsOnThisThread;
    if (!ReadShaderFileText(path, bytes, nullptr))
        return nullptr;
    auto entry = std::make_shared<Entry>();
    entry->Hash = HashShaderBytes(bytes.data(), bytes.size());
    entry->IncludeOperands = ScanShaderIncludeDirectives(bytes);
    entry->Bytes = std::move(bytes);

    std::lock_guard<std::mutex> lock(m_Mutex);
    // Read before an announcement: the bytes may predate the edit it announced,
    // so they serve this caller only.
    if (generation != m_Generation)
        return entry;
    // Two workers racing a cold file read the same bytes; the first stored wins
    // so every later reader shares one snapshot.
    return m_Entries.try_emplace(normalizedKey, std::move(entry)).first->second;
}

bool ShaderSourceCache::ReadText(const std::filesystem::path& path, std::string& out)
{
    const EntryPtr entry = Read(path, NormalizedKey(path));
    if (!entry)
        return false;
    out = entry->Bytes;
    return true;
}

ShaderSourceCache::TextureSlotsPtr ShaderSourceCache::TextureSlots(const std::filesystem::path& path,
                                                                   const std::string& normalizedKey)
{
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const auto it = m_TextureSlots.find(normalizedKey);
        if (it != m_TextureSlots.end())
            return it->second;
        generation = m_Generation;
    }

    // Parse outside the lock, as Read reads: an owner-thread lookup never waits for a worker's parse.
    const EntryPtr entry = Read(path, normalizedKey);
    if (!entry)
        return nullptr;
    TextureSlotsPtr slots = std::make_shared<const TextureSlotResolution>(ShaderComposer::ResolveTextureSlots(entry->Bytes));

    std::lock_guard<std::mutex> lock(m_Mutex);
    // Parsed from bytes read before an announcement: they serve this caller only.
    if (generation != m_Generation)
        return slots;
    return m_TextureSlots.try_emplace(normalizedKey, std::move(slots)).first->second;
}

bool ShaderSourceCache::HoldsTextureSlots(const std::string& normalizedKey)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_TextureSlots.contains(normalizedKey);
}

uint64_t ShaderSourceCache::FileReadsOnThisThreadForTest()
{
    return t_FileReadsOnThisThread;
}

ShaderSourceCache::ResolutionPtr ShaderSourceCache::Resolve(const std::filesystem::path& requestingDir,
                                                            const std::vector<std::filesystem::path>& roots,
                                                            const std::string& rootsSignature,
                                                            const std::string& operand)
{
    // Reused per thread: the walk resolves every directive of every program, so the
    // memo key's buffer is kept rather than rebuilt from nothing per directive.
    thread_local std::string memoKey;
    memoKey.assign(requestingDir.string());
    memoKey += '\n';
    memoKey += operand;

    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const auto rootsIt = m_Resolutions.find(rootsSignature);
        if (rootsIt != m_Resolutions.end())
        {
            const auto it = rootsIt->second.find(memoKey);
            if (it != rootsIt->second.end())
                return it->second;
        }
        generation = m_Generation;
    }

    const std::filesystem::path operandPath(operand);
    ResolutionPtr resolution = requestingDir.empty() ? nullptr : TryResolveIn(requestingDir, operandPath);
    for (size_t i = 0; !resolution && i < roots.size(); ++i)
        resolution = TryResolveIn(roots[i], operandPath);

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (generation != m_Generation)
        return resolution;
    return m_Resolutions[rootsSignature].try_emplace(memoKey, std::move(resolution)).first->second;
}

void ShaderSourceCache::InvalidateAll()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    ++m_Generation;
    m_Entries.clear();
    m_Resolutions.clear();
    m_TextureSlots.clear();
}

uint64_t ShaderSourceCache::Generation()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Generation;
}

}} // namespace GameEngine::Rendering
