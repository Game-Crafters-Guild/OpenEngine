#include <gtest/gtest.h>

#include "GameViewMovieCapture.h"
#include "Video/AsyncVideoRecorder.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>

namespace GameEngine::Editor
{
struct GameViewMovieCaptureTestAccess
{
    // The state a stop or the frame limit leaves behind: the session is closed
    // and the encoder still holds the file. A started recorder that was never
    // finished is that encoder, held open for as long as the test needs.
    static void InstallUnfinishedRecorder(GameViewMovieCapture& capture,
                                          std::shared_ptr<Video::AsyncVideoRecorder> recorder)
    {
        std::lock_guard<std::mutex> lock(capture.m_MovieRecorderMutex);
        capture.m_MovieRecorder = std::move(recorder);
    }

    static std::shared_ptr<Video::AsyncVideoRecorder> Recorder(GameViewMovieCapture& capture)
    {
        std::lock_guard<std::mutex> lock(capture.m_MovieRecorderMutex);
        return capture.m_MovieRecorder;
    }
};
} // namespace GameEngine::Editor

namespace
{
using GameEngine::Editor::GameViewMovieCapture;
using GameEngine::Editor::GameViewMovieCaptureTestAccess;
namespace Video = GameEngine::Video;

// A size the software H.264 encoder opens at.
constexpr uint32_t kFrameWidth = 128;
constexpr uint32_t kFrameHeight = 96;

Video::VideoWriterOptions MakeOptions(const char* fileName)
{
    Video::VideoWriterOptions options;
    options.path = (std::filesystem::temp_directory_path() / fileName).string();
    options.width = kFrameWidth;
    options.height = kFrameHeight;
    options.fps = 30.0;
    options.codec = Video::VideoCodec::H264;
    options.recordAudio = false;
    options.requireHardwareAcceleration = false;
    return options;
}

// The frame the GPU readback would hand the recorder. The first frame makes the
// encoder thread open the file, so the close after it takes real time.
void SubmitOneFrame(Video::AsyncVideoRecorder& recorder)
{
    Video::OwnedVideoFrame frame;
    frame.Width = kFrameWidth;
    frame.Height = kFrameHeight;
    frame.StrideBytes = kFrameWidth * 4u;
    frame.Format = Video::VideoPixelFormat::BGRA8;
    frame.Pixels.assign(static_cast<size_t>(frame.StrideBytes) * kFrameHeight, 0u);
    frame.TimestampSeconds = 0.0;
    ASSERT_EQ(recorder.SubmitVideoFrame(frame), Video::SubmitResult::Accepted);
}

TEST(GameViewMovieCaptureStatusTest, RecordingStaysTrueWhileTheEncoderStillHoldsTheFile)
{
    GameViewMovieCapture capture(nullptr);
    auto recorder = std::make_shared<Video::AsyncVideoRecorder>();
    ASSERT_TRUE(recorder->Start(MakeOptions("ge_movie_capture_status_unfinished.mp4")));
    GameViewMovieCaptureTestAccess::InstallUnfinishedRecorder(capture, recorder);

    const auto closing = capture.GetRecordingStatus();
    EXPECT_TRUE(closing.Recording) << "the status reads done while the encoder has not closed the container";
    EXPECT_TRUE(closing.Finalizing);

    recorder->CancelAndWait();
    const auto closed = capture.GetRecordingStatus();
    EXPECT_FALSE(closed.Recording);
    EXPECT_FALSE(closed.Finalizing);
}

TEST(GameViewMovieCaptureStatusTest, AStopThatWaitsLeavesNothingRecording)
{
    GameViewMovieCapture capture(nullptr);
    ASSERT_TRUE(capture.StartRecording(MakeOptions("ge_movie_capture_status_stop.mp4"), 0));

    const auto capturing = capture.GetRecordingStatus();
    EXPECT_TRUE(capturing.Recording);
    EXPECT_FALSE(capturing.Finalizing);

    capture.StopRecording("stopped by the test", /*waitForFinalization=*/true);
    const auto stopped = capture.GetRecordingStatus();
    EXPECT_FALSE(stopped.Recording);
    EXPECT_FALSE(stopped.Finalizing);
}

// A stop that does not wait (the frame limit's path) leaves the encoder closing
// the file on its thread. The capture keeps the recorder until it reports the
// file closed, so the status never reads done before the file is complete.
TEST(GameViewMovieCaptureStatusTest, AStopThatDoesNotWaitReadsRecordingUntilTheFileIsClosed)
{
    GameViewMovieCapture capture(nullptr);
    const Video::VideoWriterOptions options = MakeOptions("ge_movie_capture_status_closing.mp4");
    ASSERT_TRUE(capture.StartRecording(options, 0));
    const auto recorder = GameViewMovieCaptureTestAccess::Recorder(capture);
    ASSERT_NE(recorder, nullptr);
    SubmitOneFrame(*recorder);

    capture.StopRecording("stopped by the test", /*waitForFinalization=*/false);
    const auto closing = capture.GetRecordingStatus();
    EXPECT_TRUE(closing.Recording || recorder->IsFinalized())
        << "the status reads done while the encoder is still closing the file";

    recorder->WaitForFinalization();
    const auto closed = capture.GetRecordingStatus();
    EXPECT_FALSE(closed.Recording);
    EXPECT_EQ(closed.Failure, "");
    EXPECT_TRUE(std::filesystem::exists(options.path));
    std::error_code error;
    std::filesystem::remove(options.path, error);
}

// A file the encoder cannot open ends the recording with its failure in the
// status, so a caller can tell it from a finished file.
TEST(GameViewMovieCaptureStatusTest, AnEncoderThatCannotOpenTheFileReportsTheFailure)
{
    GameViewMovieCapture capture(nullptr);
    Video::VideoWriterOptions options = MakeOptions("unused.mp4");
    options.path = "/ge_movie_capture_test_missing_dir/ge_movie_capture_status_failure.mp4";
    ASSERT_TRUE(capture.StartRecording(options, 0));
    SubmitOneFrame(*GameViewMovieCaptureTestAccess::Recorder(capture));

    capture.StopRecording("stopped by the test", /*waitForFinalization=*/true);
    const auto failed = capture.GetRecordingStatus();
    EXPECT_FALSE(failed.Recording);
    EXPECT_NE(failed.Failure.find("Movie capture failed"), std::string::npos) << "failure: '" << failed.Failure << "'";
}
} // namespace
