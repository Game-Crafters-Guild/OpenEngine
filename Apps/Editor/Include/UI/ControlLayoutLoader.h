#pragma once

#include "Types/FlatMap.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace GameEngine
{
class UIElement;

namespace Editor
{

/// Loads the layout files of editor controls and keeps the text of each file until the file changes.
class ControlLayoutLoader
{
public:
    /// The root of an editor control's layout file ("UI/controls/<Control>/<Control>.uxml" under the install assets),
    /// or null with one error that names the file when it does not read, does not parse or lacks one of `requiredIds`.
    /// A UILayout Modified, Created or Destroyed event of the file makes the next call read it. UI thread only.
    static std::unique_ptr<UIElement> Load(const char* layoutAssetPath, std::span<const std::string_view> requiredIds);

private:
    struct ChangedFiles;

    ControlLayoutLoader() = default;
    /// The text of the layout file, or empty with an error that names the file when it does not read.
    static std::string ReadText(const char* layoutAssetPath);
    static ControlLayoutLoader& Instance();
    void ListenToLayoutEdits();
    void DropChangedTexts();

    // A load holds its text through its own pointer, so a nested load can add or drop entries while the
    // parser reads the text.
    FlatMap<std::string, std::shared_ptr<const std::string>> m_Texts;
    // The asset event callback owns the list, so the list expires when the dispatcher drops the callback.
    std::weak_ptr<ChangedFiles> m_ChangedFiles;
};

} // namespace Editor
} // namespace GameEngine
