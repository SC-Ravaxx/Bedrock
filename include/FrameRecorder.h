#pragma once

#include <opencv2/opencv.hpp>
#include <chrono>
#include <cstdio>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace SCMiner {

// Records what the scanner actually looked at, as a ring of short lossless
// clips.
//
// A single screenshot comes too late: by the time a missed reading is
// reported, the marker has moved on. A rolling recording keeps the last few
// minutes of exactly the frames the scan worked on, so a "this value was not
// recognised" report can be replayed instead of reconstructed.
//
// Frames go to ffmpeg through a pipe rather than through OpenCV's VideoWriter:
// this OpenCV build has no ffmpeg backend, so it could only offer lossy
// codecs, and the digits are 13 px tall - compression artefacts there would
// change what the reader sees and make the recording lie about the problem.
class FrameRecorder {
public:
    ~FrameRecorder();

    // Starts recording into dir. Frame size is taken from the first frame;
    // later frames are fitted into it. Returns false when ffmpeg is missing,
    // in which case nothing is recorded and the scan runs as before.
    bool start(const std::string& dir, double fps, int segmentSeconds, int ringSize);
    void write(const cv::Mat& frame);
    void stop();

    bool active() const { return active_; }
    const std::string& lastError() const { return lastError_; }

    // ffmpeg.exe from PATH, or one of the places it usually ships with other
    // software on this machine. Empty when there is none.
    static std::string findFfmpeg();

private:
    void openSegment();
    void closeSegment();

    bool active_ = false;
    std::string dir_;
    std::string ffmpeg_;
    std::string lastError_;
    double fps_ = 3.0;
    int segmentSeconds_ = 60;
    int ringSize_ = 5;
    int segment_ = 0;
    cv::Size frameSize_{0, 0};
    std::chrono::steady_clock::time_point segmentStart_;

#ifdef _WIN32
    HANDLE pipe_ = nullptr;          // write end, we own it
    PROCESS_INFORMATION process_{};
#else
    FILE* pipe_ = nullptr;           // popen()'s end of it
#endif
    bool processRunning_ = false;
};

} // namespace SCMiner
