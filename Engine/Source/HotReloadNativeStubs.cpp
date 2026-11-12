// Engine.dll's five GE_* hot-reload entry points. Each one calls the function that
// GameEngine.Native.dll (Source/Scripting/ScriptingABI.cpp) registers at load, or returns
// GE_Result_Ok if none is registered. GameEngine.Native.dll links Engine.dll, not the reverse.

#include "Scripting/ScriptingABI.h"

#include <atomic>

namespace {

struct CallbackSlots {
    std::atomic<GE_PreloadAssemblyContext_Fn> Preload{nullptr};
    std::atomic<GE_SwapPreloadedContext_Fn>   Swap{nullptr};
    std::atomic<GE_CleanupOldContext_Fn>      Cleanup{nullptr};
    std::atomic<GE_ClearCompilerCache_Fn>     ClearCache{nullptr};
    std::atomic<GE_GetCompilerStats_Fn>       GetStats{nullptr};
};

CallbackSlots& Slots() {
    // Function-local static — zero-initialized on first call, thread-safe under
    // C++11 magic statics. Avoids cross-DLL static-init order issues.
    static CallbackSlots s;
    return s;
}

} // namespace

extern "C" {

void GE_CDECL GE_RegisterHotReloadCallbacks(const GE_HotReloadCallbacks* cb)
{
    auto& s = Slots();
    if (cb == nullptr) {
        s.Preload   .store(nullptr, std::memory_order_release);
        s.Swap      .store(nullptr, std::memory_order_release);
        s.Cleanup   .store(nullptr, std::memory_order_release);
        s.ClearCache.store(nullptr, std::memory_order_release);
        s.GetStats  .store(nullptr, std::memory_order_release);
        return;
    }
    s.Preload   .store(cb->PreloadAssemblyContext, std::memory_order_release);
    s.Swap      .store(cb->SwapPreloadedContext,   std::memory_order_release);
    s.Cleanup   .store(cb->CleanupOldContext,      std::memory_order_release);
    s.ClearCache.store(cb->ClearCompilerCache,     std::memory_order_release);
    s.GetStats  .store(cb->GetCompilerStats,       std::memory_order_release);
}

GE_Result GE_CDECL GE_PreloadAssemblyContext(const char* assemblyPathUtf8, uint32_t pathLen)
{
    auto fn = Slots().Preload.load(std::memory_order_acquire);
    return fn ? fn(assemblyPathUtf8, pathLen) : GE_Result_Ok;
}

GE_Result GE_CDECL GE_SwapPreloadedContext(void)
{
    auto fn = Slots().Swap.load(std::memory_order_acquire);
    return fn ? fn() : GE_Result_Ok;
}

GE_Result GE_CDECL GE_CleanupOldContext(const char* assemblyPathUtf8, uint32_t pathLen)
{
    auto fn = Slots().Cleanup.load(std::memory_order_acquire);
    return fn ? fn(assemblyPathUtf8, pathLen) : GE_Result_Ok;
}

GE_Result GE_CDECL GE_ClearCompilerCache(void)
{
    auto fn = Slots().ClearCache.load(std::memory_order_acquire);
    return fn ? fn() : GE_Result_Ok;
}

GE_Result GE_CDECL GE_GetCompilerStats(void)
{
    auto fn = Slots().GetStats.load(std::memory_order_acquire);
    return fn ? fn() : GE_Result_Ok;
}

} // extern "C"
