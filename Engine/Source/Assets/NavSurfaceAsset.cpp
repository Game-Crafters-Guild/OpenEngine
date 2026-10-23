#include "Assets/NavSurfaceAsset.h"

#include "AssetCore/SharedFileRead.h"
#include "Logger/Logger.h"
#include "Pathfinding/NavSurfaceFile.h"

namespace GameEngine
{

NavSurfaceAsset::NavSurfaceAsset(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::NavigationSurfaceData, path)
{
}

NavSurfaceAsset::~NavSurfaceAsset()
{
    Unload();
}

bool NavSurfaceAsset::Load()
{
    if (GetState() == AssetState::Loaded)
        return true;
    std::vector<uint8> bytes;
    if (!Exists() || !ReadFileBytesShared(GetPath(), bytes))
    {
        Logger::Log::Error("Navigation surface data '{}' could not be read", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }
    return LoadFromData(bytes);
}

bool NavSurfaceAsset::LoadFromData(const Vector<uint8>& data)
{
    if (GetState() == AssetState::Loaded)
        return true;
    SetState(AssetState::Loading);
    auto parsed = std::make_shared<Pathfinding::NavSurfaceData>();
    std::string error;
    if (!Pathfinding::ReadNavSurface(data, *parsed, error))
    {
        Logger::Log::Error("Navigation surface data '{}' was not loaded: {}", GetPath().string(), error);
        SetState(AssetState::Failed);
        return false;
    }
    m_Data = std::move(parsed);
    m_Bytes = data.size();
    SetState(AssetState::Loaded);
    return true;
}

void NavSurfaceAsset::Unload()
{
    m_Data.reset();
    m_Bytes = 0;
    SetState(AssetState::Unloaded);
}

size_t NavSurfaceAsset::GetMemoryUsage() const
{
    return m_Bytes;
}

} // namespace GameEngine
