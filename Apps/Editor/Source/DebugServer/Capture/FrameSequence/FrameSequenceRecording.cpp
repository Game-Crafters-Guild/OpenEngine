#include "DebugServer/Capture/FrameSequence/FrameSequenceRecording.h"

#include "Types/FormatNumber.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace GameEngine::Editor
{

FrameSequenceRecording::FrameSequenceRecording(uint32_t maxFrames, FrameFileFormat format, double frameRate)
    : m_MaxFrames(maxFrames), m_Format(format), m_FrameIntervalMs(frameRate > 0.0 ? 1000.0 / frameRate : 0.0)
{
    m_Frames.reserve(std::min(maxFrames, kFrameRecordingDefaultMaxFrames));
}

std::optional<uint32_t> FrameSequenceRecording::OnFramePresented(uint64_t renderFrameIndex, double msSinceStart)
{
    if (!IsCapturing())
        return std::nullopt;
    ++m_Presented;
    if (!m_FirstPresentedMs)
        m_FirstPresentedMs = msSinceStart;
    m_LastPresentedMs = msSinceStart;
    if (m_FrameIntervalMs > 0.0)
    {
        if (msSinceStart < m_NextTickMs)
        {
            ++m_Skipped;
            return std::nullopt;
        }
        // The next tick follows this frame's time, so a late frame never makes the next ones bunch up.
        m_NextTickMs = (std::floor(msSinceStart / m_FrameIntervalMs) + 1.0) * m_FrameIntervalMs;
    }
    if (m_ReadbacksInFlight >= kFrameRecordingMaxReadbacks || m_EncodesInFlight >= kFrameRecordingMaxEncodes)
    {
        ++m_Dropped;
        return std::nullopt;
    }
    ++m_ReadbacksInFlight;
    m_Frames.push_back(CapturedFrame{.RenderFrameIndex = renderFrameIndex, .MsSinceStart = msSinceStart});
    return static_cast<uint32_t>(m_Frames.size() - 1);
}

void FrameSequenceRecording::OnReadbackFinished(uint32_t index, uint32_t width, uint32_t height)
{
    m_Frames[index].Width = width;
    m_Frames[index].Height = height;
    m_Frames[index].State = FrameState::Encoding;
    --m_ReadbacksInFlight;
    ++m_EncodesInFlight;
}

void FrameSequenceRecording::OnReadbackLost(uint32_t index)
{
    m_Frames[index].State = FrameState::Lost;
    --m_ReadbacksInFlight;
    ++m_Dropped;
}

void FrameSequenceRecording::OnFrameWritten(uint32_t index, double encodeMs)
{
    m_Frames[index].State = FrameState::Written;
    --m_EncodesInFlight;
    ++m_Written;
    m_EncodeMsTotal += encodeMs;
    m_EncodeMsMax = std::max(m_EncodeMsMax, encodeMs);
}

void FrameSequenceRecording::OnFrameWriteFailed(uint32_t index)
{
    m_Frames[index].State = FrameState::Failed;
    --m_EncodesInFlight;
    ++m_Failed;
}

void FrameSequenceRecording::Stop()
{
    m_Stopped = true;
}

bool FrameSequenceRecording::IsCapturing() const
{
    // The cap counts the frames the clock takes, dropped ones included, so maxFrames / frameRate
    // is the recording's length and a recording that drops still ends on time.
    return !m_Stopped && m_Presented - m_Skipped < m_MaxFrames;
}

bool FrameSequenceRecording::IsDrained() const
{
    return m_ReadbacksInFlight == 0 && m_EncodesInFlight == 0;
}

bool FrameSequenceRecording::TakeDropNoticeDue(double msNow)
{
    if (m_Dropped == m_DroppedAtLastNotice)
        return false;
    if (m_LastNoticeMs && msNow - *m_LastNoticeMs < kFrameRecordingDropNoticeIntervalMs)
        return false;
    m_LastNoticeMs = msNow;
    m_DroppedAtLastNotice = m_Dropped;
    return true;
}

double FrameSequenceRecording::PresentCapFps() const
{
    return IsCapturing() && m_FrameIntervalMs > 0.0 ? kFrameRecordingPresentsPerTick * 1000.0 / m_FrameIntervalMs : 0.0;
}

FrameSequenceRecording::Summary FrameSequenceRecording::GetSummary() const
{
    Summary summary;
    summary.Presented = m_Presented;
    summary.Written = m_Written;
    summary.Skipped = m_Skipped;
    summary.Dropped = m_Dropped;
    summary.Failed = m_Failed;
    summary.DurationMs = m_LastPresentedMs;
    const double presentedSpanMs = m_FirstPresentedMs ? m_LastPresentedMs - *m_FirstPresentedMs : 0.0;
    summary.PresentedFps = presentedSpanMs > 0.0 ? (m_Presented - 1) * 1000.0 / presentedSpanMs : 0.0;
    summary.EncodeMsMean = m_Written > 0 ? m_EncodeMsTotal / m_Written : 0.0;
    summary.EncodeMsMax = m_EncodeMsMax;
    return summary;
}

std::string FrameSequenceRecording::BuildListing() const
{
    constexpr int kMsDecimals = 3;
    std::string listing = "index,renderFrame,msSinceStart,deltaMs,width,height,file\n";
    std::optional<double> previousMs;
    for (uint32_t index = 0; index < m_Frames.size(); ++index)
    {
        const CapturedFrame& frame = m_Frames[index];
        if (frame.State != FrameState::Written)
            continue;
        const double deltaMs = previousMs ? frame.MsSinceStart - *previousMs : 0.0;
        previousMs = frame.MsSinceStart;
        listing += std::to_string(index) + ',' + std::to_string(frame.RenderFrameIndex) + ',' +
                   FormatFixed(frame.MsSinceStart, kMsDecimals) + ',' + FormatFixed(deltaMs, kMsDecimals) + ',' +
                   std::to_string(frame.Width) + ',' + std::to_string(frame.Height) + ',' + FrameFileName(index) +
                   '\n';
    }
    return listing;
}

std::string FrameSequenceRecording::FrameFileName(uint32_t index) const
{
    return std::format("frame_{:06}.{}", index, m_Format == FrameFileFormat::Raw ? "rgba" : "png");
}

} // namespace GameEngine::Editor
