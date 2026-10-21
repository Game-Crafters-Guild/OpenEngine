#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace GameEngine {
namespace Platform {

// Speech to text from the default microphone, for dictation into a text field.
//
// macOS only (the Speech framework); on every other platform IsAvailable() is false and
// Start() ends at once with a message that says so. A session asks for speech recognition
// and microphone permission the first time it runs, so the app's Info.plist must carry
// NSSpeechRecognitionUsageDescription and NSMicrophoneUsageDescription. Every callback runs
// on the main thread, and none runs after Cancel() or the destructor.
class SpeechInput
{
public:
    // Slices of the latest microphone buffer that Callbacks::Levels reports.
    static constexpr std::size_t kLevelCount = 10;
    using LevelArray = std::array<float, kLevelCount>;

    struct Callbacks
    {
        // The best transcript of the session so far; each call replaces the previous one.
        std::function<void(const std::string& transcript)> Transcript;
        // Listening began (true) or the session ended (false), with a line for a status bar
        // that names the fix when the session could not start.
        std::function<void(bool listening, const std::string& message)> State;
        // Microphone loudness for a level meter while listening: kLevelCount consecutive
        // time slices of the latest audio buffer, oldest first, each 0 at -65 dB and below
        // and 1 at -20 dB and above, smoothed with a fast rise and a slow fall. Optional.
        std::function<void(const LevelArray& levels)> Levels;
    };

    static bool IsAvailable();

    SpeechInput();
    ~SpeechInput();
    SpeechInput(const SpeechInput&) = delete;
    SpeechInput& operator=(const SpeechInput&) = delete;

    // Starts a session, ending any running one without its callbacks.
    void Start(Callbacks callbacks);
    // Stops listening; the last words still arrive before State reports the end.
    void Finish();
    // Ends the session at once, with no further callbacks.
    void Cancel();
    bool IsRunning() const;

private:
    struct Impl;
    std::shared_ptr<Impl> m_Impl;
};

} // namespace Platform
} // namespace GameEngine
