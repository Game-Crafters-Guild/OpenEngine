#pragma once

// GE_MEMORY_ASAN is defined when the translation unit is compiled with AddressSanitizer
// (clang's __has_feature, or GCC's and MSVC's __SANITIZE_ADDRESS__); the Memory allocators
// then poison the memory they hold but have not handed out.

#if defined(__SANITIZE_ADDRESS__)
#define GE_MEMORY_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GE_MEMORY_ASAN 1
#endif
#endif
#if defined(GE_MEMORY_ASAN)
#include <sanitizer/asan_interface.h>
#endif
