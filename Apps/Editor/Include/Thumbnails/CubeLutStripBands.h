#pragma once

#include "Thumbnails/CubeLutStripRequest.h"

#include "Rendering/Core/Device.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{

/// The Cube LUT preview textures one window shows, graph-free: which keys are live, their textures
/// and whether they rendered. Each owner (an inspector's field) holds one live key per slot (its
/// content, source and size): when the UI opens a new key for the same slot (an intensity, stage
/// or look edit), the previous texture is retired, so tuning a LUT never grows the window's
/// textures. Asset thumbnails (owner 0) are keyed by asset and size and are never retired.
class CubeLutStripBands
{
  public:
    struct Band
    {
        CubeLutStripRequest Request{};
        Rendering::TextureHandle Texture{};
        bool Rendered = false;
        bool UiBound = false;
    };
    using Retired = std::vector<std::pair<std::string, Band>>;

    /// Opens the texture a key names (nothing when it is already open). Returns false for a key that
    /// names no strip.
    bool Open(const std::string& key);

    std::unordered_map<std::string, Band>& Live() { return m_Live; }
    const std::unordered_map<std::string, Band>& Live() const { return m_Live; }

    /// The textures retired since the last call, for the caller to unbind from the UI and destroy.
    Retired TakeRetired();

    /// Every texture, live and retired, emptying the window (project switch, shutdown).
    Retired TakeAll();

    /// Marks every live texture of a LUT for rendering again, in place (its file or its asset's
    /// settings changed). Returns whether any was live.
    bool MarkLutChanged(const GUID& lut);

  private:
    std::unordered_map<std::string, Band> m_Live;
    std::unordered_map<std::string, std::string> m_KeyBySlot; ///< owner, content, source and size -> live key
    Retired m_Retired;
};

} // namespace GameEngine
