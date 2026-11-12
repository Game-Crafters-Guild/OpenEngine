// cmake/ThreadSanitizerOptions.cmake compiles this file into every test executable of a
// ThreadSanitizer tree. The runtime reads TSAN_OPTIONS after these defaults, so a value set there wins.

// On macOS the runtime ignores the accesses of interceptors (memcpy, memset) and of uninstrumented
// libraries such as libc++ by default. Then a write into a std::string buffer is not seen, and a
// race on it is not reported.
extern "C" __attribute__((no_sanitize("thread"), visibility("default"), used)) const char* __tsan_default_options()
{
    return "ignore_interceptors_accesses=0:ignore_noninstrumented_modules=0";
}

// The GPU driver stack and the OS trace buffer pass memory between their own threads through
// synchronization the runtime cannot see, so their reports are false. The suppressions keep these
// libraries ignored as before, and libc++ stays checked.
extern "C" __attribute__((no_sanitize("thread"), visibility("default"), used)) const char* __tsan_default_suppressions()
{
    return
        // Interceptor calls (memcpy, memset, malloc, free) made by the library itself. The Vulkan
        // driver and Metal complete work on Metal threads; os_signpost writes the trace buffer in libdispatch.
        "called_from_lib:/libMoltenVK.dylib$\n"
        "called_from_lib:/Metal.framework/Versions/A/Metal$\n"
        "called_from_lib:/IOGPU.framework/Versions/A/IOGPU$\n"
        "called_from_lib:/CoreFoundation.framework/Versions/A/CoreFoundation$\n"
        "called_from_lib:/libdispatch.dylib$\n"
        "called_from_lib:/libxpc.dylib$\n"
        "called_from_lib:/libsystem_blocks.dylib$\n"
        // Driver objects that MoltenVK and the Apple GPU driver allocate on one thread and free on a Metal
        // thread through the test executable's operator new and delete (Memory/AllocationHook.inl). A race entry
        // drops each report with a frame of its library in an access or thread creation stack, engine code included.
        "race:libMoltenVK.dylib\n"
        "race:AGXMetal\n"
        // IOKit allocates the dispatch block of a notification port and libdispatch frees it on a worker
        // thread; io_connect_method copies driver arguments on a worker thread. Each entry matches one function.
        "race:^IONotificationPortSetDispatchQueue$\n"
        "race:^io_connect_method$\n";
}
