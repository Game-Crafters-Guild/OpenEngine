"""Class 07, logging, memory and time: stream and printf output outside the Logger, raw new and delete outside the allocators,
wall-clock reads outside std::chrono.

Each sub-class skips its owner module: Logger for output, Memory for allocation, Platform for clocks. String literals are blanked
first, so a keyword inside a message is not a site.
"""
import re

from gate_common import Site, strip_strings

CLASS_ID = "07_log_memory_time"

kLogPatterns = [
    ("stream_output", re.compile(r"\bstd::(?:cout|cerr|clog)\b"), "LOG_INFO, LOG_WARNING or LOG_ERROR (Logger/Logger.h)"),
    ("printf_output", re.compile(r"(?<![\w:])(?:printf|puts|putchar|perror|wprintf)\s*\(|\bfprintf\s*\(\s*std(?:out|err)|\bfputs\s*\([^;]*std(?:out|err)|\bstd::print(?:ln)?\s*\("), "LOG_* (Logger/Logger.h)"),
    ("debug_output", re.compile(r"\bOutputDebugString[AW]?\s*\(|\bNSLog\s*\(|\b__android_log_print\s*\("), "LOG_* (Logger/Logger.h) or a Logger sink"),
]
kMemoryPatterns = [
    ("raw_new", re.compile(r"(?<![\w:.>])new\s+(?!\()(?:\(std::nothrow\)\s*)?[A-Za-z_][\w:<>,\s]*?\s*(?:\(|\{|\[|;)"), "std::make_unique or std::make_shared; Memory::ObjectPool or Memory::Arena for pools and arenas"),
    ("raw_delete", re.compile(r"(?<![\w:])delete\b\s*(?:\[\]\s*)?[A-Za-z_(]"), "an owning smart pointer or the pool's Release"),
    ("placement_new", re.compile(r"(?<![\w:])new\s*\(\s*[^)]*\)\s*[A-Za-z_]"), "placement new belongs inside an allocator or a container that owns the storage"),
    ("malloc_free", re.compile(r"\b(?:std::)?(?:malloc|calloc|realloc|free|_aligned_malloc|_aligned_free|aligned_alloc|posix_memalign)\s*\("), "the Memory module's allocators or a std container"),
]
kTimePatterns = [
    ("c_time", re.compile(r"(?<![\w:])(?:time|clock|gettimeofday|clock_gettime|localtime|localtime_s|localtime_r|gmtime|gmtime_s|strftime|ctime|difftime)\s*\("), "std::chrono (steady_clock for durations, system_clock and std::format for a timestamp)"),
    ("os_time", re.compile(r"\b(?:GetTickCount(?:64)?|QueryPerformanceCounter|QueryPerformanceFrequency|timeGetTime|GetSystemTime(?:AsFileTime|PreciseAsFileTime)?|mach_absolute_time|CFAbsoluteTimeGetCurrent|CACurrentMediaTime|emscripten_get_now|SDL_GetTicks|glfwGetTime)\s*\("), "std::chrono::steady_clock"),
    ("system_clock_now", re.compile(r"std::chrono::system_clock::now\s*\("), "std::chrono::steady_clock for a duration or a timeout; system_clock only for a wall-clock timestamp"),
]
PATTERNS = [name for name, _, _ in kLogPatterns + kMemoryPatterns + kTimePatterns]


class Rule:
    def __init__(self, files):
        pass

    def scan_file(self, source):
        groups = []
        if source.module != "Logger":
            groups.append(kLogPatterns)
        if source.module != "Memory":
            groups.append(kMemoryPatterns)
        if source.module != "Platform":
            groups.append(kTimePatterns)
        sites = []
        for line_number, code, _ in source.code:
            if not code.strip() or code.lstrip().startswith("#"):
                continue
            code = strip_strings(code)
            for patterns in groups:
                for name, pattern, piece in patterns:
                    if pattern.search(code):
                        sites.append(Site(source.path, line_number, name, piece))
        return sites

    def finish(self):
        return []
