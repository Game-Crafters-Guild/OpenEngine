#include "Audio/AudioDecoderConfig.h"

#include <miniaudio_libvorbis.h>

namespace GameEngine::Audio
{

ma_decoder_config MakeDecoderConfig()
{
    // libvorbis, not miniaudio's stb_vorbis backend: stb_vorbis corrupts the heap on some
    // malformed comment headers, and an .ogg in a project is decoded at import.
    static ma_decoding_backend_vtable* backends[] = {ma_decoding_backend_libvorbis};

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, /*channels*/ 0, /*sampleRate*/ 0);
    cfg.ppCustomBackendVTables = backends;
    cfg.customBackendCount = static_cast<ma_uint32>(sizeof(backends) / sizeof(backends[0]));
    return cfg;
}

} // namespace GameEngine::Audio
