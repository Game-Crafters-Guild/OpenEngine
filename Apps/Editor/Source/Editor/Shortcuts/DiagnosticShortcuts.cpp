#include "Editor/Shortcuts/DiagnosticShortcuts.h"

#include "Core/CpuProfiler.h"
#include "EditorInputActions.h"
#include "Input/InputSystem.h"
#include "Logger/Logger.h"

#include <utility>

namespace GameEngine::Editor
{

DiagnosticShortcuts::DiagnosticShortcuts(std::function<std::string()> perfDumpLine,
                                         std::function<void()> showUiDemo)
    : m_PerfDumpLine(std::move(perfDumpLine))
    , m_ShowUiDemo(std::move(showUiDemo))
{
}

void DiagnosticShortcuts::Update(const Input::InputSystem& input) const
{
#if GE_ENABLE_CPU_PROFILING
    auto& profiler = Profiling::CpuProfiler::Get();
    if (input.WasActionTriggered(EditorInput::kEditorCpuProfilerOff))
    {
        profiler.SetEnabled(false);
        Logger::Log::Info("[CPU Profiler] OFF");
    }
    else if (input.WasActionTriggered(EditorInput::kEditorCpuProfiler))
    {
        if (!profiler.IsEnabled())
        {
            profiler.SetEnabled(true);
            Logger::Log::Info("[CPU Profiler] ON (press F2 again to dump)");
        }
        else
        {
            // The perf breakdown makes the dump self-contained.
            profiler.RequestDump(m_PerfDumpLine());
        }
    }
#endif

    if (input.WasActionTriggered(EditorInput::kEditorShowUIDemo))
        m_ShowUiDemo();
}

} // namespace GameEngine::Editor
