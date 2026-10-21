#include "Scripting/ScriptStatusIndicator.h"

#include "UI/Controls/Label.h"

#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kBuildingClass = "is-building";
constexpr const char* kOkClass = "is-ok";
constexpr const char* kFailClass = "is-fail";

std::string DescribeManaged(const ScriptAssemblyStatus& managed)
{
    switch (managed.State)
    {
    case ScriptAssemblyState::Building:
        return "Building C# scripts...";
    case ScriptAssemblyState::Loaded:
        return "C# scripts loaded (" + std::to_string(managed.TypeCount) +
               (managed.TypeCount == 1 ? " type)" : " types)");
    case ScriptAssemblyState::Failed:
        return "C# build failed - see Log";
    case ScriptAssemblyState::Disabled:
        return "C# scripting is off in this build";
    case ScriptAssemblyState::NotLoaded:
        break;
    }
    return "C# scripts not loaded";
}

// Empty while the native side has not reported: a project without C++ modules says nothing about them.
std::string DescribeNative(NativeScriptState native)
{
    switch (native)
    {
    case NativeScriptState::Building:
        return "building C++ modules...";
    case NativeScriptState::Loaded:
        return "C++ modules loaded";
    case NativeScriptState::Failed:
        return "C++ build failed - using last good build";
    case NativeScriptState::None:
        break;
    }
    return {};
}

std::string StateClassFor(NativeScriptState native, ScriptAssemblyState managed)
{
    if (native == NativeScriptState::Failed || managed == ScriptAssemblyState::Failed)
        return kFailClass;
    if (native == NativeScriptState::Building || managed == ScriptAssemblyState::Building)
        return kBuildingClass;
    if (native == NativeScriptState::Loaded || managed == ScriptAssemblyState::Loaded)
        return kOkClass;
    return {};
}
} // namespace

ScriptStatusIndicator::ScriptStatusIndicator(LabelLookup findLabel, ShowLog showLog)
    : m_FindLabel(std::move(findLabel)), m_ShowLog(std::move(showLog))
{
}

void ScriptStatusIndicator::SetNativeState(NativeScriptState state)
{
    m_Native = state;
    if (state == NativeScriptState::Failed && m_ShowLog)
        m_ShowLog();
    Refresh();
}

void ScriptStatusIndicator::Update(const ScriptAssemblyStatus& managed)
{
    if (managed == m_Managed && m_HasShown)
        return;
    const bool newlyFailed =
        managed.State == ScriptAssemblyState::Failed && m_Managed.State != ScriptAssemblyState::Failed;
    m_Managed = managed;
    if (newlyFailed && m_ShowLog)
        m_ShowLog();
    Refresh();
}

ScriptStatusDisplay ScriptStatusIndicator::Describe(NativeScriptState native, const ScriptAssemblyStatus& managed)
{
    ScriptStatusDisplay display;
    display.Text = DescribeManaged(managed);
    const std::string nativeText = DescribeNative(native);
    if (!nativeText.empty())
        display.Text += ", " + nativeText;
    display.StateClass = StateClassFor(native, managed.State);
    return display;
}

void ScriptStatusIndicator::Refresh()
{
    const ScriptStatusDisplay display = Describe(m_Native, m_Managed);
    if (m_HasShown && display == m_Shown)
        return;
    Label* label = m_FindLabel ? m_FindLabel() : nullptr;
    if (!label)
        return;
    label->RemoveClass(kBuildingClass);
    label->RemoveClass(kOkClass);
    label->RemoveClass(kFailClass);
    if (!display.StateClass.empty())
        label->AddClass(display.StateClass);
    label->SetText(display.Text);
    m_Shown = display;
    m_HasShown = true;
}
} // namespace GameEngine::Editor
