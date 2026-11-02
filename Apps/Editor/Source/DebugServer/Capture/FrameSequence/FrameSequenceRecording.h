#pragma once

#include "JobSystem/TaskTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{

// Default cap of one recording, in frames the clock takes: 120 s at the default frame rate.
inline constexpr uint32_t kFrameRecordingDefaultMaxFrames = 3600;
// Default rate of the recording's clock. A higher rate gives a smoother sequence and costs
// one GPU encode, one readback and one file write for each frame it adds.
inline constexpr double kFrameRecordingDefaultFrameRate = 30.0;
// Readbacks a recording keeps in flight. The GPU copy of a frame resolves about two
// frames after its declaration, so three keeps up with every presented frame.
inline constexpr uint32_t kFrameRecordingMaxReadbacks = 3;
// Frames a recording keeps queued for the encode jobs. A frame past this backlog is
// dropped, so a slow encode costs frames, never memory.
inline constexpr uint32_t kFrameRecordingMaxEncodes = 8;
// The job priority of a frame's encode. A recording is the user's foreground task, so its
// encodes share the lane of the frame's own jobs; the Background lane runs only when that lane is empty.
inline constexpr JobSystem::JobPriority kFrameRecordingEncodePriority = JobSystem::JobPriority::Normal;
// Presents for each tick of the recording's clock. The frame limiter holds each present to at
// least its interval, so at one present per tick the presents drift behind the clock and miss ticks.
inline constexpr uint32_t kFrameRecordingPresentsPerTick = 2;
// Shortest interval between two drop notices in the log.
inline constexpr double kFrameRecordingDropNoticeIntervalMs = 1000.0;

// How a recording writes each frame. Png compresses on the encode job; Raw writes the
// RGBA8 bytes with no header, row 0 first, which costs disk space instead of encode time.
enum class FrameFileFormat : uint8_t
{
    Png,
    Raw,
};

// The bookkeeping of one frame-sequence recording: which presented frames are read
// back, which are dropped and why, and the listing of the frames written to disk.
// It holds no GPU state and is used from the main thread only.
class FrameSequenceRecording
{
  public:
    struct Summary
    {
        uint32_t Presented = 0; // frames presented between start and the stop or the cap
        uint32_t Written = 0;   // frames on disk, one listing row each
        uint32_t Skipped = 0;   // frames between two ticks of the frame rate's clock
        uint32_t Dropped = 0;   // frames not read back, or whose readback was lost
        uint32_t Failed = 0;    // frames read back whose file write failed
        double DurationMs = 0.0;
        double PresentedFps = 0.0; // from the first presented frame to the last
        double EncodeMsMean = 0.0;
        double EncodeMsMax = 0.0;
    };

    // frameRate 0 takes every presented frame. A positive frameRate takes the first
    // presented frame at or after each tick of a clock at that rate, so the frames on
    // disk are evenly spaced when the encode cannot keep up with every presented frame.
    FrameSequenceRecording(uint32_t maxFrames, FrameFileFormat format, double frameRate);

    // One presented frame. Returns the index to read the frame back into, or nothing
    // when the frame is skipped or dropped, or when the recording is stopped or at its frame cap.
    std::optional<uint32_t> OnFramePresented(uint64_t renderFrameIndex, double msSinceStart);
    // The readback of `index` holds its pixels, `width` by `height`; the frame goes to the encode queue.
    void OnReadbackFinished(uint32_t index, uint32_t width, uint32_t height);
    // The frame that declared the readback of `index` was abandoned. Counts as a drop.
    void OnReadbackLost(uint32_t index);
    void OnFrameWritten(uint32_t index, double encodeMs);
    void OnFrameWriteFailed(uint32_t index);

    // Ends the capture. OnFramePresented returns nothing for each later frame.
    void Stop();
    // True until Stop or until the frame cap is reached.
    bool IsCapturing() const;
    // True when no readback and no encode is in flight.
    bool IsDrained() const;
    // True at most once per notice interval, when frames dropped since the last notice.
    bool TakeDropNoticeDue(double msNow);
    // The rate the editor presents at while the recording captures: kFrameRecordingPresentsPerTick
    // times its frame rate, so each tick has a frame. 0 (no cap) after the capture ends or when it takes every frame.
    double PresentCapFps() const;

    Summary GetSummary() const;
    // CSV with a header row and one row per written frame, in capture order:
    // index, render-graph frame index, ms since start, ms since the previous row, width,
    // height, file name. A raw file has no header, so its row gives its size.
    std::string BuildListing() const;

    std::string FrameFileName(uint32_t index) const;

  private:
    enum class FrameState : uint8_t
    {
        ReadingBack,
        Encoding,
        Written,
        Lost,
        Failed,
    };

    struct CapturedFrame
    {
        uint64_t RenderFrameIndex = 0;
        double MsSinceStart = 0.0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        FrameState State = FrameState::ReadingBack;
    };

    uint32_t m_MaxFrames = 0;
    FrameFileFormat m_Format = FrameFileFormat::Png;
    double m_FrameIntervalMs = 0.0; // 0: every presented frame
    double m_NextTickMs = 0.0;
    std::vector<CapturedFrame> m_Frames;
    uint32_t m_Presented = 0;
    uint32_t m_Skipped = 0;
    uint32_t m_Dropped = 0;
    uint32_t m_ReadbacksInFlight = 0;
    uint32_t m_EncodesInFlight = 0;
    uint32_t m_Written = 0;
    uint32_t m_Failed = 0;
    std::optional<double> m_FirstPresentedMs;
    double m_LastPresentedMs = 0.0;
    double m_EncodeMsTotal = 0.0;
    double m_EncodeMsMax = 0.0;
    uint32_t m_DroppedAtLastNotice = 0;
    std::optional<double> m_LastNoticeMs;
    bool m_Stopped = false;
};

} // namespace GameEngine::Editor
