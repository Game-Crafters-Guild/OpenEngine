#if defined(_MSC_VER)
#pragma warning(push)
// miniaudio and stb_vorbis are third-party; silence known warnings in the implementation TU.
// C4456: declaration hides previous local declaration (miniaudio and stb_vorbis internal scopes)
// C4457: declaration hides function parameter (stb_vorbis internal scopes)
// C4245: signed/unsigned mismatch in return conversions (miniaudio internal APIs)
// C4098: void function returning a value (miniaudio ARM64 atomic macro bug)
#pragma warning(disable : 4456 4457 4245 4098)
#endif

// macOS: Use static linking for CoreAudio frameworks instead of runtime linking
// This avoids notarization issues and ensures proper framework access
#if defined(__APPLE__)
#define MA_NO_RUNTIME_LINKING
#endif

// Ogg Vorbis: miniaudio has no Vorbis decoder of its own. It compiles its stb_vorbis
// backend when stb_vorbis's declarations precede the implementation (it tests
// STB_VORBIS_INCLUDE_STB_VORBIS_H), and stb_vorbis's implementation follows it.
// stb_vorbis.c comes from the vcpkg stb port, like the rest of stb.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION

// We intentionally build miniaudio as part of the Audio static library so consumers
// do not need to manage a separate dependency. The vcpkg port provides the header.
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
