#include "Platform/SpeechInput.h"

namespace GameEngine {
namespace Platform {

struct SpeechInput::Impl
{
};

bool SpeechInput::IsAvailable()
{
    return false;
}

SpeechInput::SpeechInput() = default;
SpeechInput::~SpeechInput() = default;

void SpeechInput::Start(Callbacks callbacks)
{
    if (callbacks.State)
        callbacks.State(false, "Speech input is available on macOS only");
}

void SpeechInput::Finish() {}
void SpeechInput::Cancel() {}

bool SpeechInput::IsRunning() const
{
    return false;
}

} // namespace Platform
} // namespace GameEngine
