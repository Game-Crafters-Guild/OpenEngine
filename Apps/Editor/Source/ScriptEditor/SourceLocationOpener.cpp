#include "ScriptEditor/SourceLocationOpener.h"

#include "Editor/Settings/ScriptEditorSettings.h"
#include "EditorPanelManager.h"
#include "ExternalScriptEditorLauncher.h"
#include "Panels/ScriptEditorPanel.h"
#include "ScriptEditor/SourceLocation.h"

#include <algorithm>
#include <optional>

namespace GameEngine::Editor
{

SourceLocationOpener::SourceLocationOpener(EditorPanelManager& panelManager, EditorWindowContext* window,
                                           ScriptEditorPanel* scriptEditor)
    : m_PanelManager(&panelManager)
    , m_Window(window)
    , m_ScriptEditor(scriptEditor)
{
}

void SourceLocationOpener::Open(const SourceLocation& location) const
{
    if (ScriptEditorSettings::Get().GetOpenInScriptInspector() && OpenInScriptEditor(location))
        return;
    OpenExternally(location.Path);
}

bool SourceLocationOpener::OpenInScriptEditor(const SourceLocation& location) const
{
    if (!m_ScriptEditor)
        return false;

    m_PanelManager->ShowOrActivateScriptEditorPanel(m_Window);
    m_ScriptEditor->OpenScript(location.Path);
    if (location.Line > 0)
        m_ScriptEditor->ScrollToLine(location.Line, {}, location.Column);
    return true;
}

void SourceLocationOpener::OpenLogSourceFile(const std::string& file, int line) const
{
    if (file.empty())
        return;

    SourceLocation location;
    location.Path = std::filesystem::path(file);
    location.Line = static_cast<size_t>(std::max(line, 0));

    if (IsCSharpSource(location.Path))
        Open(location);
    else
        OpenExternally(location.Path);
}

void SourceLocationOpener::OpenExternally(const std::filesystem::path& path) const
{
    // The launcher falls back to the OS handler itself; a failed launch has no
    // better destination here.
    (void)ExternalScriptEditorLauncher::OpenScript(path);
}

} // namespace GameEngine::Editor
