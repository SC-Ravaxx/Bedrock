#include "FrameRecorder.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace SCMiner {

namespace {

#ifdef _WIN32
// ffmpeg ships with several common tools; look there before giving up
const char* const FFMPEG_CANDIDATES[] = {
    "C:\\Program Files\\Virtual Desktop Streamer\\ffmpeg.exe",
    "C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
    "C:\\ffmpeg\\bin\\ffmpeg.exe",
};
#endif

}  // namespace

#ifndef _WIN32
std::string FrameRecorder::findFfmpeg() {
    // Beside the program first, then the PATH: ffmpeg is a package on every
    // distribution and nobody ships it twice
    std::error_code ec;
    if (std::filesystem::exists("ffmpeg/ffmpeg", ec)) return std::filesystem::absolute("ffmpeg/ffmpeg", ec).string();
    if (const char* path = std::getenv("PATH")) {
        std::stringstream dirs(path);
        std::string dir;
        while (std::getline(dirs, dir, ':')) {
            if (dir.empty()) continue;
            const std::filesystem::path candidate = std::filesystem::path(dir) / "ffmpeg";
            if (std::filesystem::exists(candidate, ec)) return candidate.string();
        }
    }
    return {};
}
#else
std::string FrameRecorder::findFfmpeg() {
    // The hand-out build ships its own copy next to the app (ffmpeg\ffmpeg.exe
    // with its DLLs). The working directory is the app's own folder - main()
    // sets it - so these relative names mean "beside the exe".
    for (const char* local : {"ffmpeg\\ffmpeg.exe", "ffmpeg.exe"}) {
        std::error_code ec;
        if (std::filesystem::exists(local, ec)) {
            return std::filesystem::absolute(local, ec).string();
        }
    }
    char found[MAX_PATH] = {};
    if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        return found;
    }
    for (const char* candidate : FFMPEG_CANDIDATES) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}
#endif

FrameRecorder::~FrameRecorder() {
    stop();
}

bool FrameRecorder::start(const std::string& dir, double fps, int segmentSeconds, int ringSize) {
    stop();

    ffmpeg_ = findFfmpeg();
    if (ffmpeg_.empty()) {
        lastError_ = "ffmpeg.exe not found - scan recording is off";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    dir_ = dir;
    fps_ = fps;
    segmentSeconds_ = segmentSeconds;
    ringSize_ = (std::max)(1, ringSize);
    segment_ = 0;
    frameSize_ = cv::Size(0, 0);
    lastError_.clear();
    active_ = true;
    return true;
}

void FrameRecorder::openSegment() {
    closeSegment();

    const std::string file = dir_ + "/scan_" + std::to_string(segment_ % ringSize_) + ".mkv";

#ifndef _WIN32
    const std::string command =
        "\"" + ffmpeg_ + "\" -y -hide_banner -loglevel error"
        " -f rawvideo -pixel_format bgr24"
        " -video_size " + std::to_string(frameSize_.width) + "x" + std::to_string(frameSize_.height) +
        " -framerate " + std::to_string(static_cast<int>(fps_ + 0.5)) +
        " -i - -an -c:v ffv1 -level 3 -g 1 \"" + file + "\"";
    pipe_ = popen(command.c_str(), "w");
    if (!pipe_) {
        lastError_ = "could not start ffmpeg";
        active_ = false;
        return;
    }
    processRunning_ = true;
    segmentStart_ = std::chrono::steady_clock::now();
    std::cout << "[recorder] writing " << file << std::endl;
    return;
#else
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 20)) {
        lastError_ = "could not create the pipe to ffmpeg";
        active_ = false;
        return;
    }
    // The child must not inherit our end, or it never sees end-of-input
    SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);

    // ffv1 is mathematically lossless, so what is replayed later is exactly
    // what the reader was given. -g 1 makes every frame independent, which
    // costs size but means a truncated file (a crash, a power cut) still
    // replays up to the last complete frame.
    const std::string command =
        "\"" + ffmpeg_ + "\" -y -hide_banner -loglevel error"
        " -f rawvideo -pixel_format bgr24"
        " -video_size " + std::to_string(frameSize_.width) + "x" + std::to_string(frameSize_.height) +
        " -framerate " + std::to_string(static_cast<int>(fps_ + 0.5)) +
        " -i - -an -c:v ffv1 -level 3 -g 1 \"" + file + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = readEnd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    std::string mutableCommand = command;
    if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &process_)) {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        lastError_ = "could not start ffmpeg";
        active_ = false;
        return;
    }

    CloseHandle(readEnd);          // the child owns it now
    pipe_ = writeEnd;
    processRunning_ = true;
    segmentStart_ = std::chrono::steady_clock::now();
    std::cout << "[recorder] writing " << file << std::endl;
#endif
}

void FrameRecorder::closeSegment() {
#ifndef _WIN32
    if (pipe_) {
        pclose(pipe_);             // end of input: ffmpeg finalises the file, and pclose waits for it
        pipe_ = nullptr;
    }
    processRunning_ = false;
#else
    if (pipe_) {
        CloseHandle(pipe_);        // end of input: ffmpeg finalises the file
        pipe_ = nullptr;
    }
    if (processRunning_) {
        WaitForSingleObject(process_.hProcess, 10000);
        CloseHandle(process_.hProcess);
        CloseHandle(process_.hThread);
        process_ = PROCESS_INFORMATION{};
        processRunning_ = false;
    }
#endif
}

void FrameRecorder::write(const cv::Mat& frame) {
    if (!active_ || frame.empty()) return;

    cv::Mat bgr;
    if (frame.channels() == 4) cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
    else if (frame.channels() == 1) cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
    else bgr = frame;

    if (frameSize_.width == 0) {
        frameSize_ = bgr.size();
        openSegment();
        if (!active_) return;
    }

    // The search area changes size - it widens to the whole monitor when the
    // marker has been lost - but a video needs one size. Fit whatever comes in
    // into the canvas: local frames land 1:1 and stay pixel-exact, a full
    // sweep is scaled down, which is enough to see WHERE the marker went.
    cv::Mat canvas;
    if (bgr.size() == frameSize_) {
        canvas = bgr;
    } else {
        canvas = cv::Mat::zeros(frameSize_, CV_8UC3);
        const double scale = (std::min)(
            static_cast<double>(frameSize_.width) / bgr.cols,
            static_cast<double>(frameSize_.height) / bgr.rows);
        cv::Mat fitted;
        cv::resize(bgr, fitted, cv::Size(), scale, scale, cv::INTER_AREA);
        fitted.copyTo(canvas(cv::Rect((frameSize_.width - fitted.cols) / 2,
                                      (frameSize_.height - fitted.rows) / 2,
                                      fitted.cols, fitted.rows)));
    }

    const auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - segmentStart_).count();
    if (age >= segmentSeconds_) {
        segment_++;
        openSegment();
        if (!active_) return;
    }

    if (!canvas.isContinuous()) canvas = canvas.clone();
#ifndef _WIN32
    const size_t bytes = canvas.total() * canvas.elemSize();
    if (!pipe_ || std::fwrite(canvas.data, 1, bytes, pipe_) != bytes) {
        lastError_ = "ffmpeg stopped accepting frames";
        stop();
    }
#else
    const DWORD bytes = static_cast<DWORD>(canvas.total() * canvas.elemSize());
    DWORD written = 0;
    if (!WriteFile(pipe_, canvas.data, bytes, &written, nullptr) || written != bytes) {
        lastError_ = "ffmpeg stopped accepting frames";
        stop();
    }
#endif
}

void FrameRecorder::stop() {
    if (!active_ && !processRunning_ && !pipe_) return;
    closeSegment();
    active_ = false;
}

} // namespace SCMiner
