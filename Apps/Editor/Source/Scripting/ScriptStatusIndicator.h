#pragma once

#include "Scripting/ScriptAssemblyStatus.h"

#include <cstdint>
#include <functional>
#include <string>

namespace GameEngine
{
class Label;
}

namespace GameEngine::Editor
{
/// Where the project's native C++ modules stand, as the native build callbacks report it.
enum class NativeScriptState : std::uint8_t
{
    None,     ///< No native build or load has reported yet.
    Building, ///< A native module build is running.
    Loaded,   ///< The native modules built and loaded.
    Failed,   ///< The last native build failed; the last good build stays loaded.
};

/// What the bottom bar shows for scripts: the text and its state-color class (`is-building`,
/// `is-ok`, `is-fail`, or empty for the neutral color).
struct ScriptStatusDisplay
{
    std::string Text;
    std::string StateClass;

    bool operator==(const ScriptStatusDisplay&) const = default;
};

/// The bottom bar's one scripts status: states the C# side (the project's scripts assembly) and the
/// C++ side (native modules) together, and brings the Log forward when either side fails.
class ScriptStatusIndicator
{
  public:
    using LabelLookup = std::function<Label*()>;
    using ShowLog = std::function<void()>;

    /// @param findLabel Returns the status label, or null while the UI has none; looked up on each
    ///                  change so a layout rebuild never leaves a dangling pointer.
    /// @param showLog   Brings the Log panel forward.
    ScriptStatusIndicator(LabelLookup findLabel, ShowLog showLog);

    /// Records a native build edge; a failure brings the Log forward.
    void SetNativeState(NativeScriptState state);

    /// Takes the managed side's current status (polled once per frame); the transition into a
    /// failure brings the Log forward. Writes the label only when the display changes.
    void Update(const ScriptAssemblyStatus& managed);

    /// The display for one pair of states.
    static ScriptStatusDisplay Describe(NativeScriptState native, const ScriptAssemblyStatus& managed);

  private:
    void Refresh();

    LabelLookup m_FindLabel;
    ShowLog m_ShowLog;
    NativeScriptState m_Native = NativeScriptState::None;
    ScriptAssemblyStatus m_Managed;
    ScriptStatusDisplay m_Shown;
    bool m_HasShown = false;
};
} // namespace GameEngine::Editor
