#pragma once

#include <miniaudio.h>

namespace GameEngine::Audio
{

// The miniaudio decoder configuration every engine decode uses: float output at the source's
// own channel count and rate, with the decoding backends miniaudio does not carry itself (Ogg
// Vorbis, through libvorbis). A decoder initialised from ma_decoder_config_init alone cannot
// read an .ogg clip.
ma_decoder_config MakeDecoderConfig();

} // namespace GameEngine::Audio
