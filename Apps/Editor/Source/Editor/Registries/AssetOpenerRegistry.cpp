#include "Editor/Registries/AssetOpenerRegistry.h"

#include "Logger/Logger.h"
#include "Types/PathUtils.h"

#include <algorithm>
#include <string>
#include <utility>

namespace GameEngine::Editor
{

AssetOpenerRegistry& AssetOpenerRegistry::Get()
{
    static AssetOpenerRegistry s_Instance;
    return s_Instance;
}

void AssetOpenerRegistry::Register(AssetOpenerDescriptor descriptor)
{
    if (descriptor.OpenerId.empty() || !descriptor.HandlesExtension || !descriptor.Open)
    {
        Logger::Log::Error("AssetOpenerRegistry: the opener '{}' is not added. Give it an OpenerId, a "
                           "HandlesExtension and an Open function.",
                           descriptor.OpenerId);
        return;
    }
    const auto existing = std::ranges::find(m_Openers, descriptor.OpenerId, &AssetOpenerDescriptor::OpenerId);
    if (existing != m_Openers.end())
        *existing = std::move(descriptor);
    else
        m_Openers.push_back(std::move(descriptor));
}

bool AssetOpenerRegistry::Open(const std::filesystem::path& path) const
{
    if (path.empty())
        return false;
    const std::string extension = LowerCaseExtension(path);
    for (const AssetOpenerDescriptor& opener : m_Openers)
    {
        if (opener.HandlesExtension(extension))
            return opener.Open(path);
    }
    return false;
}

} // namespace GameEngine::Editor
