#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Editor
{

/// What the unit that owns an asset kind registers to open it: from an Assets browser double-click, a
/// context-menu Open or an inspector Open button. The first opener in registration order that handles the
/// extension opens the file, and a later opener for that extension is never reached. To replace a built-in
/// opener, a feature registers the built-in's OpenerId.
struct AssetOpenerDescriptor
{
    /// Stable id and replace key, e.g. "scene", "graph".
    std::string OpenerId;
    /// Whether this opener takes the file. `lowercaseExtension` includes the leading dot.
    std::function<bool(std::string_view lowercaseExtension)> HandlesExtension;
    /// Opens the file. False when the open failed; the editor then hands the file to the operating system.
    std::function<bool(const std::filesystem::path& path)> Open;
};

/// Registration and opens are main-thread only. Registering an OpenerId again replaces the earlier
/// descriptor in place and keeps its position.
class AssetOpenerRegistry
{
  public:
    static AssetOpenerRegistry& Get();

    void Register(AssetOpenerDescriptor descriptor);

    /// Opens `path` with the first opener that handles its extension.
    /// @return false when no opener handles the file or its open failed; the caller then opens the file
    ///         with the operating system's default application.
    bool Open(const std::filesystem::path& path) const;

  private:
    std::vector<AssetOpenerDescriptor> m_Openers; // registration order
};

} // namespace GameEngine::Editor
