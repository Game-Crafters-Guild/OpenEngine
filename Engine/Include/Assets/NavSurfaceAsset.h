#pragma once

#include "AssetCore/Asset.h"

#include <memory>

namespace GameEngine
{

namespace Pathfinding
{
struct NavSurfaceData;
} // namespace Pathfinding

// A navigation surface's baked tiles (.navsurface, Pathfinding/NavSurfaceFile.h), written beside
// the scene when it is saved and referenced from the surface's BakedData field, so the surface
// loads its tiles instead of baking them.
class NavSurfaceAsset : public Asset
{
  public:
    NavSurfaceAsset(const GUID& guid, const std::filesystem::path& path);
    ~NavSurfaceAsset() override;

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;
    size_t GetMemoryUsage() const override;

    // The file's contents; null until loaded. Shared, so a surface keeps reading the tiles it
    // started loading after a reload replaces them.
    std::shared_ptr<const Pathfinding::NavSurfaceData> GetData() const { return m_Data; }

  private:
    std::shared_ptr<const Pathfinding::NavSurfaceData> m_Data;
    size_t m_Bytes = 0;

    DISALLOW_COPY_AND_ASSIGN(NavSurfaceAsset);
};

} // namespace GameEngine
