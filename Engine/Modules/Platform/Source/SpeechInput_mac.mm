#include "Platform/SpeechInput.h"

#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace GameEngine {
namespace Platform {
namespace {

// How long Finish() waits for the recognizer's last words after the microphone stops.
constexpr int64_t kFinishGraceNanoseconds = 3 * NSEC_PER_SEC;

std::string Utf8(NSString* text)
{
    const char* utf8 = text.UTF8String;
    return utf8 ? utf8 : "";
}

// Level meter range: a slice at kQuietDecibels or below reads 0, at kLoudDecibels or above 1.
// Decibels, not linear energy, so quiet speech still moves the meter.
constexpr float kQuietDecibels = -65.0f;
constexpr float kLoudDecibels = -20.0f;
// Smoothing per buffer: rise fast to follow speech, fall slowly so the bars do not flicker.
constexpr float kLevelRise = 0.8f;
constexpr float kLevelFall = 0.2f;

// Splits the buffer into SpeechInput::kLevelCount time slices and moves each smoothed level
// toward that slice's loudness (its loudest channel's mean energy, in decibels).
void UpdateLevels(AVAudioPCMBuffer* buffer, SpeechInput::LevelArray& smoothed)
{
    const size_t frames = buffer.frameLength;
    const AVAudioChannelCount channels = buffer.format.channelCount;
    if (frames == 0 || channels == 0)
        return;
    const bool interleaved = buffer.format.interleaved;
    for (size_t slice = 0; slice < smoothed.size(); ++slice)
    {
        const size_t begin = frames * slice / smoothed.size();
        const size_t end = frames * (slice + 1) / smoothed.size();
        float loudestEnergy = 0.0f;
        for (AVAudioChannelCount channel = 0; channel < channels; ++channel)
        {
            const size_t dataChannel = interleaved ? 0 : channel;
            float energy = 0.0f;
            for (size_t frame = begin; frame < end; ++frame)
            {
                const size_t index = frame * buffer.stride + (interleaved ? channel : 0);
                float sample = 0.0f;
                if (buffer.floatChannelData)
                    sample = buffer.floatChannelData[dataChannel][index];
                else if (buffer.int16ChannelData)
                    sample = buffer.int16ChannelData[dataChannel][index] / 32768.0f;
                else if (buffer.int32ChannelData)
                    sample = buffer.int32ChannelData[dataChannel][index] / 2147483648.0f;
                energy += sample * sample;
            }
            loudestEnergy = std::max(loudestEnergy, energy / std::max<size_t>(1, end - begin));
        }
        const float decibels = 10.0f * std::log10(std::max(loudestEnergy, 1.0e-10f));
        const float target = std::clamp((decibels - kQuietDecibels) / (kLoudDecibels - kQuietDecibels), 0.0f, 1.0f);
        float& level = smoothed[slice];
        level += (target - level) * (target > level ? kLevelRise : kLevelFall);
    }
}

} // namespace

// Every member is touched only on `queue`, except `running` and `delivery`. A session's
// callbacks carry the generation they began in; a stop moves the generation on, so a late
// callback from an ended session finds a different one and does nothing.
struct SpeechInput::Impl : std::enable_shared_from_this<SpeechInput::Impl>
{
    Callbacks callbacks;
    AVAudioEngine* engine = nil;
    SFSpeechRecognizer* recognizer = nil;
    SFSpeechAudioBufferRecognitionRequest* request = nil;
    SFSpeechRecognitionTask* task = nil;
    bool tapped = false;
    uint64_t generation = 0;
    // True while a level update waits on `queue`; the audio thread skips buffers until it lands.
    std::atomic<bool> levelPending{false};
    std::atomic<bool> running{false};
    // False once the owner cancels: the main-thread wrappers drop anything still queued.
    std::shared_ptr<std::atomic<bool>> delivery;
    dispatch_queue_t queue = dispatch_queue_create("com.openengine.speech-input", DISPATCH_QUEUE_SERIAL);

    void StopAudio()
    {
        if (engine)
        {
            [engine stop];
            if (tapped)
                [engine.inputNode removeTapOnBus:0];
        }
        tapped = false;
        [request endAudio];
    }

    void Stop()
    {
        ++generation;
        running = false;
        StopAudio();
        [task cancel];
        task = nil;
        request = nil;
        engine = nil;
        recognizer = nil;
        callbacks = {};
    }

    void End(const std::string& message)
    {
        const auto state = callbacks.State;
        Stop();
        if (state)
            state(false, message);
    }

    void BeginCapture(uint64_t token)
    {
        if (!running || generation != token)
            return;
        @try
        {
            recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale currentLocale]];
            if (!recognizer || !recognizer.available)
            {
                End("Speech recognition is not available for the system language");
                return;
            }
            recognizer.queue = [[NSOperationQueue alloc] init];
            recognizer.queue.maxConcurrentOperationCount = 1;
            request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
            request.shouldReportPartialResults = YES;
            engine = [[AVAudioEngine alloc] init];
            AVAudioInputNode* input = engine.inputNode;
            AVAudioFormat* format = [input outputFormatForBus:0];
            if (format.channelCount == 0 || format.sampleRate <= 0)
            {
                End("No microphone is connected");
                return;
            }
            // The tap holds this request and its own smoothed levels until the tap is removed;
            // it reaches the session only through `weak` and `queue`.
            SFSpeechAudioBufferRecognitionRequest* audioRequest = request;
            std::weak_ptr<Impl> weak = shared_from_this();
            auto smoothed = std::make_shared<LevelArray>();
            [input installTapOnBus:0 bufferSize:1024 format:format block:^(AVAudioPCMBuffer* buffer, AVAudioTime*) {
                [audioRequest appendAudioPCMBuffer:buffer];
                auto owner = weak.lock();
                if (!owner || owner->levelPending.exchange(true))
                    return;
                UpdateLevels(buffer, *smoothed);
                const LevelArray levels = *smoothed;
                dispatch_async(owner->queue, ^{
                    auto self = weak.lock();
                    if (!self)
                        return;
                    self->levelPending = false;
                    if (self->running && self->generation == token && self->callbacks.Levels)
                        self->callbacks.Levels(levels);
                });
            }];
            tapped = true;
            task = [recognizer recognitionTaskWithRequest:request resultHandler:^(SFSpeechRecognitionResult* result, NSError* error) {
                auto owner = weak.lock();
                if (!owner)
                    return;
                const std::string transcript = result ? Utf8(result.bestTranscription.formattedString) : std::string();
                const bool isFinal = result && result.isFinal;
                const std::string failure = error ? Utf8(error.localizedDescription) : std::string();
                dispatch_async(owner->queue, ^{
                    auto self = weak.lock();
                    if (!self || !self->running || self->generation != token)
                        return;
                    if (result && self->callbacks.Transcript)
                        self->callbacks.Transcript(transcript);
                    if (isFinal)
                        self->End("Dictation complete");
                    else if (!failure.empty())
                        self->End("Speech input stopped: " + failure);
                });
            }];
            if (!task)
            {
                End("Speech recognition could not start");
                return;
            }
            [engine prepare];
            NSError* error = nil;
            if (![engine startAndReturnError:&error])
            {
                End("The microphone could not start: " + Utf8(error.localizedDescription));
                return;
            }
            if (callbacks.State)
                callbacks.State(true, "Listening. Click the microphone to stop.");
        }
        @catch (NSException* exception)
        {
            End("Speech input could not start: " + Utf8(exception.reason));
        }
    }

    void AuthorizeMicrophone(uint64_t token)
    {
        if (!running || generation != token)
            return;
        const AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
        if (status == AVAuthorizationStatusAuthorized)
        {
            BeginCapture(token);
            return;
        }
        if (status != AVAuthorizationStatusNotDetermined)
        {
            End("Allow microphone access for Open Engine Editor in System Settings > Privacy & Security");
            return;
        }
        std::weak_ptr<Impl> weak = shared_from_this();
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
            auto owner = weak.lock();
            if (!owner)
                return;
            dispatch_async(owner->queue, ^{
                auto self = weak.lock();
                if (!self || !self->running || self->generation != token)
                    return;
                if (granted)
                    self->BeginCapture(token);
                else
                    self->End("Microphone access was not granted");
            });
        }];
    }

    void Start(Callbacks next)
    {
        Stop();
        callbacks = std::move(next);
        running = true;
        const uint64_t token = generation;
        if (callbacks.State)
            callbacks.State(false, "Asking for speech recognition and microphone access");
        std::weak_ptr<Impl> weak = shared_from_this();
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
            auto owner = weak.lock();
            if (!owner)
                return;
            dispatch_async(owner->queue, ^{
                auto self = weak.lock();
                if (!self || !self->running || self->generation != token)
                    return;
                if (status == SFSpeechRecognizerAuthorizationStatusAuthorized)
                    self->AuthorizeMicrophone(token);
                else
                    self->End("Allow speech recognition for Open Engine Editor in System Settings > Privacy & Security");
            });
        }];
    }

    void Finish()
    {
        if (!running)
            return;
        if (!engine)
        {
            End("Dictation complete");
            return;
        }
        StopAudio();
        const uint64_t token = generation;
        std::weak_ptr<Impl> weak = shared_from_this();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kFinishGraceNanoseconds), queue, ^{
            auto self = weak.lock();
            if (self && self->running && self->generation == token)
                self->End("Dictation complete");
        });
    }
};

bool SpeechInput::IsAvailable()
{
    return true;
}

SpeechInput::SpeechInput() : m_Impl(std::make_shared<Impl>()) {}

SpeechInput::~SpeechInput()
{
    Cancel();
}

void SpeechInput::Start(Callbacks callbacks)
{
    Cancel();
    auto impl = m_Impl;
    auto delivery = std::make_shared<std::atomic<bool>>(true);
    impl->delivery = delivery;
    impl->running = true;
    // The wrappers hop to the main thread and drop the call once the owner has cancelled.
    Callbacks forwarded;
    forwarded.Transcript = [delivery, callback = std::move(callbacks.Transcript)](const std::string& text) {
        const std::string value = text;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (*delivery && callback)
                callback(value);
        });
    };
    if (callbacks.Levels)
    {
        forwarded.Levels = [delivery, callback = std::move(callbacks.Levels)](const LevelArray& levels) {
            const LevelArray value = levels;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (*delivery)
                    callback(value);
            });
        };
    }
    forwarded.State = [delivery, callback = std::move(callbacks.State)](bool listening, const std::string& text) {
        const std::string value = text;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (*delivery && callback)
                callback(listening, value);
        });
    };
    dispatch_async(impl->queue, ^{
        if (*delivery)
            impl->Start(forwarded);
    });
}

void SpeechInput::Finish()
{
    auto impl = m_Impl;
    dispatch_async(impl->queue, ^{ impl->Finish(); });
}

void SpeechInput::Cancel()
{
    auto impl = m_Impl;
    if (impl->delivery)
        *impl->delivery = false;
    impl->running = false;
    dispatch_async(impl->queue, ^{ impl->Stop(); });
}

bool SpeechInput::IsRunning() const
{
    return m_Impl->running;
}

} // namespace Platform
} // namespace GameEngine
