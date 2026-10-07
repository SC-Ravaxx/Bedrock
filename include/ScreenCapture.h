#pragma once

#define NOMINMAX
#include <functional>
#include <opencv2/opencv.hpp>
#ifdef _WIN32
#include <Windows.h>
#else
#include "Win32Shim.h"   // HWND, for gameWindow(): a name Application.cpp calls
#endif
#include <optional>
#include "MiningData.h"
#include "GlyphReader.h"
#ifdef _WIN32
#include "DxgiCapture.h"
#endif

namespace SCMiner {

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    // Captures the entire (virtual) screen. On Linux: the game's window, in
    // its own coordinates - see LinuxCapture.h for why that is the picture
    cv::Mat captureScreen();

    // Captures a specific region (in the same coordinates captureScreen uses)
    cv::Mat captureRegion(const CaptureRegion& region);

    // Extracts the number area from the image (for the signature values)
    cv::Mat extractNumberRegion(const cv::Mat& image);

    // Builds OCR-ready binary images from several contrast channels
    // (grayscale, warm, cold, green) so digit color and background color do
    // not matter - ship UIs and backdrops vary. Channels that do not contain
    // a digit-like line are omitted; the result may be empty.
    std::vector<cv::Mat> preprocessCandidates(const cv::Mat& image);

    // Locates signature labels (the warm rounded panel carrying the pin icon
    // and the value) anywhere inside the given image. Needed for head
    // tracking: the marker is anchored to the rock in 3D, so it slides across
    // the screen whenever the view moves and a fixed crop misses it.
    // Returns rects in image coordinates, most label-like first.
    std::vector<cv::Rect> locateSignatureLabels(const cv::Mat& image, int maxResults = 6);

    // The digit row inside one of the rects the last locateSignatureLabels
    // returned (same coordinates), for the reader's row hint - or nothing
    // for a rect it did not hand out.
    std::optional<cv::Rect> rowOf(const cv::Rect& label) const;

    // The game's drawing surface, in virtual-screen coordinates. Everything
    // worth looking at lives inside it, so the search is pinned to this rather
    // than to a monitor: it cannot then reach the desktop, the taskbar or a
    // window in front of the game, and it stays correct when the game is moved
    // or run windowed. Empty when Star Citizen is not running. On Linux the
    // picture IS the window, so this is (0, 0, width, height).
    static std::optional<cv::Rect> gameViewport();
    // Blacks out, in a picture of the screen region `area`, every part one
    // of `covers` (screen rectangles) lies over. For Bedrock's own windows:
    // on Windows the picture is the desktop as it is shown, so the overlay
    // laid over the game is in it and would be read as a signature of its
    // own making. Public for the self-test.
    static void maskCovered(cv::Mat& picture, const cv::Rect& area, const std::vector<cv::Rect>& covers);
    // What of `rect` the rectangle `over` leaves showing, in up to four
    // pieces: a window of Bedrock's under the game's is in the picture only
    // where the game's window is not. Public for the self-test.
    static std::vector<cv::Rect> uncovered(const cv::Rect& rect, const cv::Rect& over);
    // Whether captureRegion blacks Bedrock's own windows out (on unless a
    // dev check wants the picture as the screen gives it)
    static inline bool maskOwnWindows = true;
    // The game's window itself (class CryENGINE), or null when it is not up.
    // Null on Linux always: the game is an X11 window there, not a Win32 one,
    // and the two callers only ask whether it has the focus and where its
    // exe is.
    static HWND gameWindow();
#ifdef BEDROCK_DEV_TOOLS
    // SCMINER_REPLAY=<folder>: a recording on show in place of the game
    // (Linux only for now) - the index of the frame on show, -1 when none is
    static int replayFrame();
#endif

    // Which path the last capture actually used, for diagnostics: callers
    // need to be able to say whether they are looking at an HDR desktop
    // through duplication or at GDI's clipped 8-bit version of it.
    enum class Source { None, Dxgi, DxgiHdr, Gdi, Composite };
    Source lastSource() const { return lastSource_; }
#ifdef _WIN32
    // What the last HDR capture measured, in scRGB units; empty for SDR
    std::string rawStats() const { return dxgi_.rawStats(); }
#else
    std::string rawStats() const { return {}; }   // no HDR path on Linux
#endif
    const std::string& captureNote() const { return captureNote_; }

    // Lets the caller pump its window between the locator's probe readings
    void setYield(std::function<void()> f) { yield_ = std::move(f); }

private:
#ifdef _WIN32
    // Desktop Duplication, tried before GDI. See DxgiCapture for why.
    DxgiCapture dxgi_;
#endif
    std::function<void()> yield_;
    Source lastSource_ = Source::None;
    std::string captureNote_;

#ifdef _WIN32
    // Windows GDI handles
    HDC hdcScreen_;
    HDC hdcMemDC_;
    HBITMAP hBitmap_;

    // Converts an HBITMAP to cv::Mat
    cv::Mat hBitmapToMat(HBITMAP hBitmap, int width, int height);
#endif

    // locateSignatureLabels on a picture already at the reader's reference
    // scale - the whole search, with every pixel bar as it was measured
    std::vector<cv::Rect> locateAtReferenceScale(const cv::Mat& image, int maxResults);
    // Every rect the last search handed out with the row it grew from: at
    // the reference scale as found, and in the caller's pixels for rowOf
    std::vector<std::pair<cv::Rect, cv::Rect>> referenceLabels_;
    std::vector<std::pair<cv::Rect, cv::Rect>> lastLabels_;
};

} // namespace SCMiner
