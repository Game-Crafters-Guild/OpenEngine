#include "Rendering/Materials/Shader/ShaderPropertyTableCache.h"

#include "ShaderSourceCache.h"

namespace GameEngine::Rendering
{

namespace
{

std::string CacheKey(const ShaderPropertyTableInputs& inputs)
{
    return inputs.AdapterPath.generic_string() + "|" + inputs.SurfacePath.generic_string() + "|" +
           inputs.VertexModifierPath.generic_string();
}

} // namespace

ShaderPropertyTableCache& ShaderPropertyTableCache::Instance()
{
    static ShaderPropertyTableCache cache;
    return cache;
}

std::shared_ptr<const ShaderPropertyTable> ShaderPropertyTableCache::Build(
    const ShaderPropertyTableInputs& inputs, bool& readFailed)
{
    std::vector<ShaderPropertySource> sources;
    auto add = [&](const std::filesystem::path& path, ShaderPropertyOrigin origin, bool required,
                   ShaderPropertyTable& errors)
    {
        if (path.empty())
            return;
        ShaderPropertySource source{};
        source.File = path.generic_string();
        source.Origin = origin;
        if (!ShaderSourceCache::Get().ReadText(path, source.Text))
        {
            if (required)
                errors.Errors.push_back({source.File, 0, "could not read the shader to collect its @property declarations"});
            return;
        }
        sources.push_back(std::move(source));
    };

    ShaderPropertyTable readErrors{};
    add(inputs.AdapterPath, ShaderPropertyOrigin::Adapter, true, readErrors);
    add(inputs.SurfacePath, ShaderPropertyOrigin::Surface, true, readErrors);
    add(inputs.VertexModifierPath, ShaderPropertyOrigin::VertexModifier, false, readErrors);
    readFailed = readErrors.Rejected();
    if (readFailed)
        return std::make_shared<const ShaderPropertyTable>(std::move(readErrors));
    return std::make_shared<const ShaderPropertyTable>(BuildShaderPropertyTable(sources));
}

std::shared_ptr<const ShaderPropertyTable> ShaderPropertyTableCache::Resolve(
    const ShaderPropertyTableInputs& inputs)
{
    const std::string key = CacheKey(inputs);
    const uint64_t generation = ShaderSourceCache::Get().Generation();
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        const auto it = m_Entries.find(key);
        if (it != m_Entries.end() && it->second.SourceGeneration == generation)
            return it->second.Table;
    }

    // Build outside the lock, as ShaderSourceCache::TextureSlots parses: a lookup never waits for a build.
    bool readFailed = false;
    std::shared_ptr<const ShaderPropertyTable> table = Build(inputs, readFailed);
    // Built from bytes read before an announcement: it serves this caller only.
    if (generation != ShaderSourceCache::Get().Generation())
        return table;

    std::lock_guard<std::mutex> lock(m_Mutex);
    // A build of a newer generation stored its table while this build ran: keep that table.
    const auto held = m_Entries.find(key);
    if (held != m_Entries.end() && held->second.SourceGeneration > generation)
        return table;
    // A table with a read error is not stored, as in the source cache: an adapter or
    // a surface that appears later is read on the next Resolve without an announcement.
    if (readFailed)
        m_Entries.erase(key);
    else
        m_Entries[key] = Entry{generation, table};
    return table;
}

bool ShaderPropertyTableCache::Holds(const ShaderPropertyTableInputs& inputs)
{
    const std::string key = CacheKey(inputs);
    const uint64_t generation = ShaderSourceCache::Get().Generation();
    std::lock_guard<std::mutex> lock(m_Mutex);
    const auto it = m_Entries.find(key);
    return it != m_Entries.end() && it->second.SourceGeneration == generation;
}

} // namespace GameEngine::Rendering
