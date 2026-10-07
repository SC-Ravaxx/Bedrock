#include "ScreenCapture.h"
#include <cstring>
#include "DevSwitch.h"
#ifndef _WIN32
#include "LinuxCapture.h"
#endif
#ifdef _WIN32
#include <dwmapi.h>   // DWMWA_CLOAKED, for ownWindows
#endif
#include <cmath>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <chrono>

namespace SCMiner {

namespace {
// Glyph sizes the readout can have on screen, in pixels
constexpr int MIN_GLYPH_PX = 6;
constexpr int MAX_GLYPH_PX = 40;
// A signature is 4 to 6 digits; the comma is filtered out by height
constexpr int MIN_ROW_GLYPHS = 4;
constexpr int MAX_ROW_GLYPHS = 6;
}  // namespace


namespace {
// Saves OCR debug snapshots into ./debug (relative to the working directory,
// which main() sets to the executable's folder). Fails silently if not writable.
void saveDebugImage(const std::string& name, const cv::Mat& image) {
    std::error_code ec;
    std::filesystem::create_directories("debug", ec);
    if (!ec) {
        cv::imwrite("debug/" + name, image);
    }
}

// How lopsided the colour channels are: 1.0 is neutral, higher means the whole
// view is washed in one colour.
double colourCastStrength(const cv::Mat& bgr) {
    if (bgr.channels() < 3) return 1.0;
    const cv::Scalar mean = cv::mean(bgr);
    const double lo = (std::max)(1.0, (std::min)({mean[0], mean[1], mean[2]}));
    const double hi = (std::max)({mean[0], mean[1], mean[2]});
    return hi / lo;
}

// LAMP night vision lays a manufacturer tint over the entire cockpit view
// (Drake amber, RSI purple, MISC grey-blue), which collapses the colour
// differences the analysis relies on - yellow text on a yellow panel. Such a
// tint is a per-channel gain, so equalising the channel means inverts it and
// brings the relative colours back. On an untinted capture the gains are ~1,
// so this only engages where it is needed.
cv::Mat neutralizeTint(const cv::Mat& bgr) {
    if (bgr.channels() < 3) return bgr;

    const cv::Scalar mean = cv::mean(bgr);
    const double target = (mean[0] + mean[1] + mean[2]) / 3.0;
    if (target < 1.0) return bgr;

    std::vector<cv::Mat> planes;
    cv::split(bgr, planes);
    for (int i = 0; i < 3; i++) {
        const double channelMean = (std::max)(1.0, mean[i]);
        // Bounded gain: a nearly empty channel must not be amplified to noise
        const double gain = std::clamp(target / channelMean, 0.3, 3.5);
        planes[i].convertTo(planes[i], CV_8U, gain);
    }

    cv::Mat out;
    cv::merge(planes, out);
    return out;
}

// A tint this strong is a night-vision or star-glare cast, not scene colour
constexpr double CAST_THRESHOLD = 1.6;

// Set SCMINER_OCR_TRACE=1 to learn why a rendering was discarded
void traceReject(const char* reason, double value) {
    static const bool trace = DEV_ENV("SCMINER_OCR_TRACE") != nullptr;
    if (trace) {
        std::cerr << "  [trace] rendering dropped: " << reason << " (" << value << ")"
                  << std::endl;
    }
}
} // namespace

ScreenCapture::ScreenCapture()
#ifdef _WIN32
    : hdcScreen_(nullptr), hdcMemDC_(nullptr), hBitmap_(nullptr)
#endif
{
}

ScreenCapture::~ScreenCapture() {
#ifdef _WIN32
    if (hBitmap_) DeleteObject(hBitmap_);
    if (hdcMemDC_) DeleteDC(hdcMemDC_);
    if (hdcScreen_) ReleaseDC(nullptr, hdcScreen_);
#endif
}

#ifdef _WIN32
// Taking the picture: the desktop DC, and finding the game in the window
// list. Both are Win32 through and through. The Linux counterpart lives in
// LinuxCapture.cpp and reads the window through Composite instead, because
// the root window is not readable on a Wayland session.
cv::Mat ScreenCapture::captureScreen() {
    // Get the virtual screen size (all monitors combined)
    int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);      // X offset (can be negative)
    int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);      // Y offset (can be negative)
    int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);  // Total width of all monitors
    int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN); // Total height of all monitors

    // Duplication covers one output, so it can only serve this when there is
    // a single monitor; with several, fall through to GDI for the whole
    // virtual desktop (only region captures run in the hot path anyway).
    if (GetSystemMetrics(SM_CMONITORS) == 1) {
        if (cv::Mat frame = dxgi_.grab(cv::Rect(virtualX, virtualY, screenWidth, screenHeight));
            !frame.empty()) {
            lastSource_ = dxgi_.lastFrameWasHdr() ? Source::DxgiHdr : Source::Dxgi;
            return frame;
        }
    }
    lastSource_ = Source::Gdi;

    // Create device contexts
    hdcScreen_ = GetDC(nullptr);
    hdcMemDC_ = CreateCompatibleDC(hdcScreen_);

    // Create the bitmap
    hBitmap_ = CreateCompatibleBitmap(hdcScreen_, screenWidth, screenHeight);
    SelectObject(hdcMemDC_, hBitmap_);

    // Copy all screens into the bitmap (starting at virtualX, virtualY)
    BitBlt(hdcMemDC_, 0, 0, screenWidth, screenHeight, hdcScreen_, virtualX, virtualY, SRCCOPY);

    // Convert to cv::Mat
    cv::Mat result = hBitmapToMat(hBitmap_, screenWidth, screenHeight);

    // Clean up
    DeleteObject(hBitmap_);
    DeleteDC(hdcMemDC_);
    ReleaseDC(nullptr, hdcScreen_);
    hBitmap_ = nullptr;
    hdcMemDC_ = nullptr;
    hdcScreen_ = nullptr;

    return result;
}

// Bedrock's own windows as the picture of the screen shows them, to be
// masked out. Above all the overlay: a player lays it over the game
// wherever it is least in the way, often inside the search area, and its
// own figures would be read back as a signature on every scan.
//
// Only where they are on top, though. The window list comes topmost
// first, and a window of Bedrock's that comes after the game's is under
// it: the game is what the picture shows there. Masking such a window
// whole blacks out the game with it - on a one-screen machine the full
// window left open behind the game sits in the middle, where the
// signatures, the scan panel and the mining HUD come up.
static std::vector<cv::Rect> ownWindows() {
    struct Walk {
        std::vector<cv::Rect> rects;
        std::optional<cv::Rect> game;   // the game's window, once the walk is past it
    } walk;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            Walk& walk = *reinterpret_cast<Walk*>(param);
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
            // A window on another virtual desktop counts as visible and is not
            // on the screen - the game's no more than one of Bedrock's: taken
            // for the one in front, it would leave Bedrock's own window in the
            // picture unmasked
            BOOL cloaked = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
                return TRUE;
            }
            RECT r{};
            if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return TRUE;
            const cv::Rect rect(r.left, r.top, r.right - r.left, r.bottom - r.top);
            char cls[64] = {};
            GetClassNameA(hwnd, cls, sizeof(cls));
            // the game's window, as gameWindow() finds it: the first of its class
            if (!walk.game && std::strcmp(cls, "CryENGINE") == 0) {
                walk.game = rect;
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != GetCurrentProcessId()) return TRUE;
            // Not the frame round the search area: it is see-through but for
            // its edge, and put away before every look (Application)
            if (std::strcmp(cls, "SCMinerOverlay") == 0) return TRUE;
            // ...nor one the picture leaves out on its own (the overlay,
            // see MinerGUI::toggleOverlayMode): what is under it is the game,
            // and the game is what is to be read there
            DWORD affinity = 0;
            if (GetWindowDisplayAffinity(hwnd, &affinity) && affinity == 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */) {
                return TRUE;
            }
            if (!walk.game) {
                walk.rects.push_back(rect);
            } else {
                for (const cv::Rect& part : ScreenCapture::uncovered(rect, *walk.game)) walk.rects.push_back(part);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&walk));
    return walk.rects;
}

cv::Mat ScreenCapture::captureRegion(const CaptureRegion& region) {
    // IMPORTANT: region.x and region.y are already VIRTUAL screen coordinates
    // (coming from the drag-and-drop overlay window),
    // so do NOT add the virtual screen offset again!
    const cv::Rect area(region.x, region.y, region.width, region.height);

    // Duplication first. On an HDR display GDI hands back an image whose
    // highlights have all been cut to pure white - measured on one scene,
    // 22.7 % of the screen centre against 0.4 % with HDR off - and the HUD
    // text drowns in it. Duplication reads the float buffer instead and tone
    // maps it here. On an SDR display it returns the same bytes GDI would,
    // and when it is unavailable at all the GDI path below still runs.
    if (cv::Mat frame = dxgi_.grab(area); !frame.empty()) {
        lastSource_ = dxgi_.lastFrameWasHdr() ? Source::DxgiHdr : Source::Dxgi;
        captureNote_.clear();
        // The picture is only the part of the area on that monitor, so the masks
        // are clipped to it; an area reaching past its left or top edge would
        // otherwise shift them by as much.
        if (maskOwnWindows) maskCovered(frame, area & dxgi_.outputRect(), ownWindows());
        return frame;
    }
    lastSource_ = Source::Gdi;
    captureNote_ = dxgi_.lastError();

    // Capture the screen
    hdcScreen_ = GetDC(nullptr);
    hdcMemDC_ = CreateCompatibleDC(hdcScreen_);

    // Create a bitmap for the region
    hBitmap_ = CreateCompatibleBitmap(hdcScreen_, region.width, region.height);
    SelectObject(hdcMemDC_, hBitmap_);

    // Copy the region (coordinates are already virtual)
    BitBlt(hdcMemDC_, 0, 0, region.width, region.height,
           hdcScreen_, region.x, region.y, SRCCOPY);

    // Convert to cv::Mat
    cv::Mat result = hBitmapToMat(hBitmap_, region.width, region.height);

    // Clean up
    DeleteObject(hBitmap_);
    DeleteDC(hdcMemDC_);
    ReleaseDC(nullptr, hdcScreen_);
    hBitmap_ = nullptr;
    hdcMemDC_ = nullptr;
    hdcScreen_ = nullptr;

    if (maskOwnWindows) maskCovered(result, area, ownWindows());
    return result;
}

cv::Mat ScreenCapture::hBitmapToMat(HBITMAP hBitmap, int width, int height) {
    BITMAP bmp;
    GetObject(hBitmap, sizeof(BITMAP), &bmp);

    BITMAPINFOHEADER bi;
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;  // Negative for a top-down DIB
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = 0;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed = 0;
    bi.biClrImportant = 0;

    cv::Mat mat(height, width, CV_8UC4);

    HDC hdc = GetDC(nullptr);
    GetDIBits(hdc, hBitmap, 0, height, mat.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);

    // Convert BGRA to BGR
    cv::Mat result;
    cv::cvtColor(mat, result, cv::COLOR_BGRA2BGR);

    return result;
}

HWND ScreenCapture::gameWindow() {
    // CryENGINE is the window class Star Citizen's renderer carries: the title
    // is localised and the executable has been renamed across builds, the
    // class has stayed put. Found by walking the window list rather than with
    // FindWindow, which returns nothing for this window even while it is on
    // screen - the class is registered privately by the game's process, and
    // the atom lookup FindWindow does never sees it.
    struct Search {
        HWND hwnd = nullptr;
    } search;

    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
        char cls[64] = {};
        if (GetClassNameA(hwnd, cls, sizeof(cls)) == 0) return TRUE;
        if (std::strcmp(cls, "CryENGINE") != 0) return TRUE;
        reinterpret_cast<Search*>(param)->hwnd = hwnd;
        return FALSE;   // found it, stop walking
    }, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}

std::optional<cv::Rect> ScreenCapture::gameViewport() {
    struct Search {
        HWND hwnd = nullptr;
    } search;
    search.hwnd = gameWindow();

    if (!search.hwnd) return std::nullopt;

    // The CLIENT rectangle, not the window rectangle: in windowed mode the
    // latter includes the title bar and the border, and cropping those in
    // would shift every coordinate by the frame thickness.
    RECT client{};
    POINT origin{0, 0};
    if (!GetClientRect(search.hwnd, &client)) return std::nullopt;
    if (!ClientToScreen(search.hwnd, &origin)) return std::nullopt;

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width < 320 || height < 240) return std::nullopt;   // minimised or loading
    return cv::Rect(origin.x, origin.y, width, height);
}

#ifdef BEDROCK_DEV_TOOLS
int ScreenCapture::replayFrame() {
    return -1;   // the replay is on the Linux side only
}
#endif

#else  // the Linux side: the same four doors, LinuxCapture behind them

namespace {
// One connection to the display for the process: gameViewport() is static,
// and opening the display per call would be the slow part of every scan
LinuxCapture& linuxCapture() {
    static LinuxCapture capture;
    return capture;
}

#ifdef BEDROCK_DEV_TOOLS
// A recording played back as the game window. SCMINER_REPLAY names a folder
// of pictures, shown in name order SCMINER_REPLAY_MS apart (300 - the pace
// they were cut at), the last one held once they run out; the clock starts
// with the first capture. With QT_QPA_PLATFORM=offscreen the live scan then
// runs on footage with nothing on screen.
struct Replay {
    std::vector<std::filesystem::path> frames;
    std::chrono::steady_clock::time_point start;
    long long ms = 300;
    int shown = -1;
    cv::Mat picture;
    cv::Size frameSize;

    const cv::Mat& current() {
        const auto now = std::chrono::steady_clock::now();
        if (shown < 0) start = now;
        const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        const int at = (int)(std::min)(elapsed / ms, (long long)frames.size() - 1);
        if (at != shown) {
            picture = cv::imread(frames[(size_t)at].string());
            shown = at;
        }
        return picture;
    }
    cv::Size size() {
        if (frameSize.area() == 0) frameSize = cv::imread(frames.front().string()).size();
        return frameSize;
    }
};

Replay* replay() {
    static Replay* made = []() -> Replay* {
        const char* dir = DEV_ENV("SCMINER_REPLAY");
        if (!dir) return nullptr;
        auto* r = new Replay();
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::string ending = entry.path().extension().string();
            for (auto& c : ending) c = (char)std::tolower((unsigned char)c);
            if (ending == ".jpg" || ending == ".png") r->frames.push_back(entry.path());
        }
        std::sort(r->frames.begin(), r->frames.end());
        if (const char* ms = DEV_ENV("SCMINER_REPLAY_MS")) r->ms = (std::max)(1, std::atoi(ms));
        if (r->frames.empty()) {
            delete r;
            return nullptr;
        }
        std::cout << "[replay] " << r->frames.size() << " pictures from " << dir << ", " << r->ms
                  << " ms apart" << std::endl;
        return r;
    }();
    return made;
}
#endif
}  // namespace

#ifdef BEDROCK_DEV_TOOLS
int ScreenCapture::replayFrame() {
    Replay* r = replay();
    return r ? r->shown : -1;
}
#endif

cv::Mat ScreenCapture::captureScreen() {
#ifdef BEDROCK_DEV_TOOLS
    if (Replay* r = replay()) {
        lastSource_ = Source::Composite;
        return r->current().clone();
    }
#endif
    cv::Mat picture = linuxCapture().captureWindow();
    lastSource_ = picture.empty() ? Source::None : Source::Composite;
    if (picture.empty()) captureNote_ = linuxCapture().lastError();
    return picture;
}

cv::Mat ScreenCapture::captureRegion(const CaptureRegion& region) {
#ifdef BEDROCK_DEV_TOOLS
    if (Replay* r = replay()) {
        const cv::Mat& whole = r->current();
        const cv::Rect want = cv::Rect(region.x, region.y, region.width, region.height) &
                              cv::Rect(0, 0, whole.cols, whole.rows);
        lastSource_ = want.area() > 0 ? Source::Composite : Source::None;
        return want.area() > 0 ? whole(want).clone() : cv::Mat();
    }
#endif
    cv::Mat picture = linuxCapture().captureRegion(cv::Rect(region.x, region.y, region.width, region.height));
    lastSource_ = picture.empty() ? Source::None : Source::Composite;
    if (picture.empty()) captureNote_ = linuxCapture().lastError();
    return picture;
}

HWND ScreenCapture::gameWindow() {
    return nullptr;
}

std::optional<cv::Rect> ScreenCapture::gameViewport() {
#ifdef BEDROCK_DEV_TOOLS
    if (Replay* r = replay()) {
        const cv::Size s = r->size();
        return cv::Rect(0, 0, s.width, s.height);
    }
#endif
    return linuxCapture().gameViewport();
}

#endif  // _WIN32

// From here on it is pixels and nothing else - the locator, the channel
// splitting, the plate search. Shared by both platforms.
std::vector<cv::Rect> ScreenCapture::locateSignatureLabels(const cv::Mat& image, int maxResults) {
    if (image.empty() || image.channels() < 3) {
        return {};
    }
    // Every pixel bar in the search below - the 6..40 px glyph window, the
    // 51 px top-hat kernel, the row gaps - was measured on a 1440 px tall
    // picture, and the HUD grows with the picture: on a 4K monitor a plate
    // welded by glow stands 50 px tall and falls through the ceiling, so
    // nothing is ever found there (measured: 1440p footage shown at 1.5x
    // read nothing at all). The search therefore runs on the picture
    // brought to that height, and the rects come back in the caller's pixels.
    const double hud = GlyphReader::hudScale();
    if (std::abs(hud - 1.0) < 0.01) {
        std::vector<cv::Rect> found = locateAtReferenceScale(image, maxResults);
        lastLabels_ = referenceLabels_;
        return found;
    }

    cv::Mat reference;
    cv::resize(image, reference, cv::Size(), 1.0 / hud, 1.0 / hud,
               hud > 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    std::vector<cv::Rect> found = locateAtReferenceScale(reference, maxResults);
    const cv::Rect bounds(0, 0, image.cols, image.rows);
    for (auto& r : found) r = GlyphReader::fromReferenceScale(r, hud) & bounds;
    lastLabels_.clear();
    for (const auto& [rect, row] : referenceLabels_) {
        lastLabels_.push_back({GlyphReader::fromReferenceScale(rect, hud) & bounds,
                               GlyphReader::fromReferenceScale(row, hud) & bounds});
    }
    found.erase(std::remove_if(found.begin(), found.end(),
                               [](const cv::Rect& r) { return r.width <= 10 || r.height <= 6; }),
                found.end());
    return found;
}

std::optional<cv::Rect> ScreenCapture::rowOf(const cv::Rect& label) const {
    for (const auto& [rect, row] : lastLabels_) {
        if (rect == label) return row;
    }
    return std::nullopt;
}

std::vector<cv::Rect> ScreenCapture::locateAtReferenceScale(const cv::Mat& image, int maxResults) {
    referenceLabels_.clear();

    // ------------------------------------------------------------------
    // Colour plays no part here.
    //
    // Neither a "warm" nor a "coloured" plate can be searched for: over
    // sunlit terrain the ground has exactly the same saturation as the marker
    // (chroma median 89 for both, on a Pyro planet), and the ship UI colour
    // changes per ship anyway. Brightness is no better - in daylight the
    // ground, the plate and the digits all peak at 255.
    //
    // What does separate them is STRUCTURE. Beside the marker the ground is
    // almost flat (measured spread between median and 95th percentile: 5-9
    // levels), while the readout is a cluster of small, sharp, bright shapes
    // (spread 42-85). So: keep small bright structures with a top-hat, then
    // demand the one arrangement no landscape produces - a row of 4 to 6
    // glyphs of equal height sitting on one baseline.
    // ------------------------------------------------------------------
    cv::Mat bgr;
    if (image.channels() == 4) {
        cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
    } else {
        bgr = image;
    }

    // NO tint neutralisation here, although LAMP night vision casts one colour
    // over the screen. A sunlit planet fills the whole search area with one
    // colour too, so a grey-world correction pulls the pink ground towards
    // grey - erasing the very contrast between the coloured plate and its white
    // digits that the neutrality channel below depends on. The reader does its
    // own tint handling on the small marker crop, where a cast is measured
    // against the marker rather than against half a planet.

    // Digits are only ~10-20 px tall, so this runs at full resolution: any
    // downscale that makes a wide search area cheap also makes the glyphs
    // too small to group into a row.
    //
    // Two channels, neither of which asks what colour anything is:
    //   luminance   - the readout is brighter than its surroundings at night
    //                 and in space
    //   neutrality  - in daylight the plate is as BRIGHT as the white digits
    //                 (measured: luma 201 vs 213), so brightness separates
    //                 them no better than hue does. What always holds is that
    //                 the digits are white while the plate is coloured, in
    //                 whatever colour the ship UI happens to use.
    // A top-hat on each keeps only what stands out from its own neighbourhood,
    // so no absolute level is assumed anywhere.
    using LocClock = std::chrono::steady_clock;
    auto locMs = [](LocClock::time_point since) {
        return std::chrono::duration<double, std::milli>(LocClock::now() - since).count();
    };
    const auto locStart = LocClock::now();
    std::vector<cv::Mat> planes;
    cv::split(bgr, planes);
    cv::Mat high = cv::max(planes[0], cv::max(planes[1], planes[2]));
    cv::Mat low = cv::min(planes[0], cv::min(planes[1], planes[2]));
    cv::Mat neutral = 255 - (high - low);

    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

    // A kernel larger than a glyph, so the opening erases the glyphs and the
    // difference keeps them
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(51, 51));
    cv::Mat tophatGray, tophatNeutral, tophatWhite;
    cv::morphologyEx(gray, tophatGray, cv::MORPH_TOPHAT, kernel);
    cv::morphologyEx(neutral, tophatNeutral, cv::MORPH_TOPHAT, kernel);
    // And whiteness itself - min(R,G,B) - the channel the reader reads the
    // yellow HUD's plates with. On an olive plate luminance welds digits and
    // plate into one lump, and the glow's tint pushes the digits below the
    // neutrality cut; their whiteness stands clear of both plate and pin,
    // which have no blue in them. Without this channel a clean marker on such
    // a plate is proposed in only 5 of 30 frames.
    cv::morphologyEx(low, tophatWhite, cv::MORPH_TOPHAT, kernel);

    // The cut is LEARNT from each channel (Otsu), never fixed: the top-hat
    // response of ground texture ranges from almost nothing in space to most
    // of the scale on a detailed planet surface, so any constant either buries
    // the readout in terrain or loses it in the dark. The floor only stops
    // Otsu from splitting pure noise on an empty view.
    //
    // One global level does better against the sun than a level per region:
    //   - a level per 192 px tile loses markers: a tile containing the sun
    //     gets a high level of its own, and the marker beside the sun sits in
    //     that same tile.
    //   - a second pass at half the learnt level adds rows that crowd the
    //     real marker out of the candidate list.
    // Markers beside the sun read perfectly once cropped (pin 95-99 %), so
    // what fails there is the row grouping in a picture whose brightness
    // spans decades, not where the cut sits.
    auto learnedMask = [](const cv::Mat& channel) {
        cv::Mat binary;
        const double level = cv::threshold(channel, binary, 0, 255,
                                           cv::THRESH_BINARY | cv::THRESH_OTSU);
        if (level < 25.0) {
            binary = channel >= 25;
        }
        return binary;
    };

    // The same cut learnt a second time, inside the bright class of the
    // first. Over an asteroid field the first Otsu has nothing to split but
    // black space and black cockpit from everything else, so it lands at 25
    // or so - and at that level the sky between the rocks, the plate and
    // the digits are all "bright" and weld into one region that is no lump
    // and no row. Learnt again on the bright pixels alone, the cut falls
    // between sky and plate on one side and the marker on the other, and the
    // plate comes out as the one clean lump the row rules were written for.
    // The reader does the same on its crops (its "white+" and "green+"
    // channels).
    auto peeledMask = [&](const cv::Mat& channel, const cv::Mat& first) {
        std::vector<uchar> bright;
        bright.reserve(channel.total() / 8);
        for (int y = 0; y < channel.rows; y++) {
            const uchar* c = channel.ptr<uchar>(y);
            const uchar* f = first.ptr<uchar>(y);
            for (int x = 0; x < channel.cols; x++) {
                if (f[x]) bright.push_back(c[x]);
            }
        }
        if (bright.size() < 64) return cv::Mat(channel.size(), CV_8U, cv::Scalar(0));
        cv::Mat row(1, static_cast<int>(bright.size()), CV_8U, bright.data()), unused;
        const double level = cv::threshold(row, unused, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        return cv::Mat(channel > level);
    };

    static const bool traceLocate = DEV_ENV("SCMINER_LOCATE_TRACE") != nullptr;

    // `lump`: the whole readout came as one welded shape (see collectRows)
    // rather than as a chain of glyphs - the case a tight re-read is for
    struct Row { cv::Rect box; int members; int height; bool lump; };
    std::vector<Row> rows;

    // Each channel is searched on its own rather than merged: the luminance
    // channel turns the whole plate into one bright blob, which would swallow
    // the digits the neutrality channel separates so cleanly.
    auto collectRows = [&](const cv::Mat& mask, const char* what) {
        if (traceLocate) {
            std::filesystem::create_directories("debug");
            cv::imwrite(std::string("debug/locate_mask_") + what + ".png", mask);
        }
        cv::Mat labels, stats, centroids;
        const int count = cv::connectedComponentsWithStats(mask, labels, stats, centroids);

        // Glyph-sized pieces only. The upper bound is the tallest readout seen
        // in game, the lower bound the smallest that still carries readable
        // digits.
        std::vector<cv::Rect> pieces;
        pieces.reserve(256);
        for (int i = 1; i < count; i++) {
            const int h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
            const int w = stats.at<int>(i, cv::CC_STAT_WIDTH);
            if (h < MIN_GLYPH_PX || h > MAX_GLYPH_PX) continue;
            const double fillAll = stats.at<int>(i, cv::CC_STAT_AREA) /
                                   static_cast<double>((std::max)(1, w * h));

            // The glow routinely welds the whole readout into ONE lump. That
            // lump is the thing being looked for, so take it as a row rather
            // than discarding it for being too wide - the reader splits the
            // digits apart afterwards, and the pin check decides whether it
            // was a signature at all.
            if (w >= h * 2.2 && w <= h * 9.0 && fillAll > 0.2 && fillAll < 0.92) {
                const cv::Rect lump(stats.at<int>(i, cv::CC_STAT_LEFT),
                                    stats.at<int>(i, cv::CC_STAT_TOP), w, h);
                if (traceLocate) {
                    std::cerr << "  [locate]   lump " << lump.x << "," << lump.y << " " << lump.width
                              << "x" << lump.height << " (" << what << ")" << std::endl;
                }
                rows.push_back({lump, MIN_ROW_GLYPHS, h, true});
                continue;
            }
            if (w < 1 || w > h * 1.4) continue;
            // A glyph is a stroke, not a speck: it must fill a fair part of
            // its box without being a solid block
            const double fill = stats.at<int>(i, cv::CC_STAT_AREA) /
                                static_cast<double>((std::max)(1, w * h));
            if (fill < 0.15 || fill > 0.95) continue;
            pieces.push_back(cv::Rect(stats.at<int>(i, cv::CC_STAT_LEFT),
                                      stats.at<int>(i, cv::CC_STAT_TOP), w, h));
        }

        std::sort(pieces.begin(), pieces.end(),
                  [](const cv::Rect& a, const cv::Rect& b) { return a.x < b.x; });

        if (traceLocate) {
            std::cerr << "  [locate] " << what << ": " << (count - 1) << " components, "
                      << pieces.size() << " glyph-sized:";
            for (size_t p = 0; p < pieces.size() && p < 30; p++) {
                std::cerr << " " << pieces[p].x << "," << pieces[p].y << " "
                          << pieces[p].width << "x" << pieces[p].height;
            }
            std::cerr << std::endl;
        }

        // Chain the pieces into rows, left to right: same height, same
        // baseline, no wide gap. This is the step that rejects landscape -
        // snow speckles and rock edges never line up like text.
        std::vector<bool> used(pieces.size(), false);
        for (size_t i = 0; i < pieces.size(); i++) {
            if (used[i]) continue;
            cv::Rect box = pieces[i];
            int members = 1;
            size_t last = i;
            std::vector<size_t> chain{i};
            int commaGaps = 0;

            for (size_t j = i + 1; j < pieces.size(); j++) {
                if (used[j]) continue;
                const cv::Rect& prev = pieces[last];
                const cv::Rect& next = pieces[j];
                const double h = prev.height;
                const int gap = next.x - prev.br().x;
                if (gap > h * 3.0) break;            // nothing else is near enough
                // A number carries at most one thousands comma, and the comma
                // is too short to be a piece here (MIN_GLYPH_PX), so it leaves a
                // hole in the chain. On the canopy that hole is under one glyph
                // height; on the Drake instrument panel the same face is set wider
                // (13 px against 11 tall), which would break 7,200 into 7 | 200,
                // both under MIN_ROW_GLYPHS. ONE gap may be a comma's width - the
                // same 1.6 the reader allows - and a second wide gap still ends the
                // row: two words are not a number.
                if (gap > h * 1.0) {
                    if (commaGaps > 0 || gap > h * 1.6) continue;
                    commaGaps++;
                }
                if (gap < -static_cast<int>(h * 0.4)) continue;   // overlapping
                if (std::abs(next.height - prev.height) > h * 0.35) continue;
                if (std::abs(next.br().y - prev.br().y) > h * 0.4) continue;

                box |= next;
                chain.push_back(j);
                last = j;
                members++;
                if (members >= MAX_ROW_GLYPHS) break;
            }

            if (members < MIN_ROW_GLYPHS) continue;
            for (size_t k : chain) used[k] = true;
            if (traceLocate) {
                std::cerr << "  [locate]   row " << box.x << "," << box.y << " "
                          << box.width << "x" << box.height << " (" << members
                          << " glyphs, " << what << ")" << std::endl;
            }
            rows.push_back({box, members, box.height, false});
        }
    };

    std::vector<cv::Rect> results;
    struct Candidate { cv::Rect rect; cv::Rect row; bool lump; };
    std::vector<Candidate> candidates;
    // The frame's colourfulness and brightness, for the pin gate below
    const cv::Mat chroma = high - low;

    // Turns whatever rows have been collected into verified marker rects
    auto verifyRows = [&]() {
        // Longer rows first (a full signature beats a stray pair), taller ones
        // next: a nearer marker is the one the player is looking at
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.members != b.members) return a.members > b.members;
            return a.height > b.height;
        });

        candidates.clear();
        for (const auto& row : rows) {
            if (candidates.size() >= 40) break;

            // Widen to the whole readout: the pin sits to the left of the
            // digits and the plate extends a little past them on every side.
            // The reader needs both - it verifies the marker by that pin.
            // A row that touches the edge of the picture has been cut by it:
            // the search area ends where it ends, and a marker half outside
            // it shows half its digits. Measured on live footage: "16,000"
            // clipped along the top read as 6,000 twice - a table value, and
            // wrong. What is not entirely inside cannot be read entirely.
            if (row.box.x <= 1 || row.box.y <= 1 ||
                row.box.x + row.box.width >= image.cols - 1 ||
                row.box.y + row.box.height >= image.rows - 1) {
                continue;
            }

            cv::Rect r = row.box;
            const int h = (std::max)(1, row.height);
            // How much sky to take with the digits. The reader learns its
            // threshold over what it is given, so a box cut tight to the row
            // teaches it the digits instead of the plate they sit on: the
            // same Drake marker read 8,400 at 82 % in a box 39 px tall and
            // 3,400 at 99 % in one 55 px tall. Tunable so the right amount
            // can be measured rather than argued about.
            static const double padUp = DEV_ENV("SCMINER_PAD_UP")
                ? std::atof(DEV_ENV("SCMINER_PAD_UP")) : 0.6;
            // 0.6 below as well. A taller box (1.8) sweeps the compass ribbon in, so
            // the box holds shapes on two baselines, and reads fewer markers: 73
            // against 83 over 1,100 Prospector frames, and one wrong of 56 Golem
            // frames where 0.6 has none. With the row hint the reader thresholds over
            // the row itself, so the box needs no extra plate context.
            static const double padDown = DEV_ENV("SCMINER_PAD_DOWN")
                ? std::atof(DEV_ENV("SCMINER_PAD_DOWN")) : 0.6;
            r.x -= static_cast<int>(h * 1.7);
            r.y -= static_cast<int>(h * padUp);
            r.width += static_cast<int>(h * 2.5);
            r.height += static_cast<int>(h * (padUp + padDown));
            // A plate the search area cuts through is not a plate to read: the
            // top or bottom of the digits goes with it - measured, a "16,000"
            // against the upper edge read as 6,000 and then as 18,000 - and a
            // pin outside the area cannot vouch for anything. So the plate (0.6 h
            // above and below the digits) has to fit vertically, and the row has
            // to stand far enough from the left edge for its pin to be in the
            // picture. The rule is about the ROW: extra padding of the box (see
            // padUp/padDown) must not count as leaving the picture. What does not
            // fit is clamped below.
            const int coreTop = row.box.y - static_cast<int>(h * 0.6);
            const int coreBottom = row.box.y + row.box.height + static_cast<int>(h * 0.6);
            if (coreTop < 0 || coreBottom > image.rows || row.box.x < static_cast<int>(h * 1.5)) {
                if (traceLocate) {
                    std::cerr << "  [locate]   row " << row.box.x << "," << row.box.y << " "
                              << row.box.width << "x" << row.box.height << " dropped: plate would leave the picture"
                              << std::endl;
                }
                continue;
            }
            r &= cv::Rect(0, 0, image.cols, image.rows);
            if (r.width <= 10 || r.height <= 6) continue;

            // Several channels usually find the same readout - keep it once
            bool duplicate = false;
            for (const auto& taken : candidates) {
                if ((taken.rect & r).area() > r.area() * 0.5) { duplicate = true; break; }
            }
            if (!duplicate) candidates.push_back({r, row.box, row.lump});
        }

        // Decide what is a marker by looking INSIDE each candidate: the reader
        // only returns glyphs when it finds a row of digits with the map pin
        // beside it. Terrain lumps are the right size and shape often enough
        // that ranking alone let them crowd the real readout out of the list.
        int probesLeft = GlyphReader::MAX_PROBES;
        // A second, tight read when the generous probe read nothing on a lump
        // or found a flat plate - at most four per frame.
        int tightReadsLeft = 4;
        for (const auto& candidate : candidates) {
            if (static_cast<int>(results.size()) >= maxResults) break;
            if (probesLeft <= 0) break;
            const cv::Rect& r = candidate.rect;

            // The cheap question first: is there anything pin-shaped left
            // of this row? Judged from the frame's own channels, in
            // microseconds. Measured on live footage: 35-38 candidate rows
            // per frame, 3 of them markers; the full reader on all of them
            // costs 110-125 ms of a 165 ms frame. The bar sits well
            // under the probe's, which asks the same question again of the
            // row the reader finds.
            const double gate = GlyphReader::pinScoreForRow(chroma, high, candidate.row);
            if (gate < GlyphReader::PIN_GATE) {
                if (traceLocate) {
                    std::cerr << "  [locate]   row " << candidate.row.x << "," << candidate.row.y << " "
                              << candidate.row.width << "x" << candidate.row.height << " gate "
                              << static_cast<int>(gate * 100) << "% - not probed" << std::endl;
                }
                continue;
            }
            if (traceLocate) {
                std::cerr << "  [locate]   row " << candidate.row.x << "," << candidate.row.y << " gate "
                          << static_cast<int>(gate * 100) << "%" << std::endl;
            }

            // Verify on a slightly wider crop: sideways generously, downwards
            // hardly at all. The pin stands two digit-heights to the LEFT, and a
            // box cut tight around the digits clips it, which reads as "no pin"
            // and throws the marker away. Height is the opposite problem: the
            // candidate has already been grown once, and padding it again
            // vertically reaches into the next line of the HUD - the compass
            // ribbon below adds shapes on a second baseline, which the row test
            // rejects outright.
            const int padX = (std::max)(8, r.height / 2);
            const int padY = (std::max)(2, r.height / 12);
            cv::Rect probe(r.x - 2 * padX, r.y - padY, r.width + 3 * padX, r.height + 2 * padY);
            probe &= cv::Rect(0, 0, image.cols, image.rows);
            if (probe.width <= 10 || probe.height <= 6) continue;
            if (yield_) yield_();

            GlyphReader::Trace trace;
            GlyphReader::profile().probes++;
            probesLeft--;
            // The reader is told where the row is inside the probe - see
            // the row hint in GlyphReader::read
            const cv::Rect hint(candidate.row.x - probe.x, candidate.row.y - probe.y,
                                candidate.row.width, candidate.row.height);
            auto probed = GlyphReader::extractGlyphs(image(probe), &trace, true,
                                                     GlyphReader::PIN_MIN_NCC_STRICT, &hint);
            if (traceLocate) {
                std::cerr << "  [locate]   probe " << probe.x << "," << probe.y << " " << probe.width
                          << "x" << probe.height << " -> " << probed.size() << " glyph(s), pin "
                          << static_cast<int>(trace.pinScore * 100) << "%, plate " << trace.plate.width
                          << "x" << trace.plate.height << " luma " << static_cast<int>(trace.plateLuma)
                          << " std " << static_cast<int>(trace.plateStd) << " thr " << trace.threshold
                          << ", " << trace.channel << std::endl;
            }
            // A lump is the whole readout as one shape, and on a dark plate
            // the generous probe around it is its undoing: the reader finds
            // no plate inside the padding, learns its cuts over the black
            // around it, and cuts the plate's own glow into glyph-sized
            // pieces that outnumber the digits. Handed the lump itself with
            // a hair of margin - which is what the locator saw in the first
            // place - the same reader reads the same plate at once (probe:
            // nothing at pin 45 %; lump plus 30 %: read at pin 94 %).
            // Only after a failure, only for lumps, and at most four times a
            // frame - but not only while nothing has been verified yet: HUD
            // text with a pin-shaped icon beside it is verified on most
            // frames before the marker's lump comes up.
            //
            // The same crop is the undoing of a BRIGHT plate, in a way that
            // is worse because it comes back with an answer. Over a lit
            // horizon the generous crop fills with sky, and the reader's own
            // cut then lands where only the cores of the strokes survive:
            // the right number of glyphs at half the size, read against
            // templates they never fit (4,270 read as 4,279 from 7x13 glyphs
            // on a "plate" of texture 1; the row on its own has texture 9
            // and reads 4,270 from 14x23 glyphs).
            //
            // So a plate with no texture in it gets the same second opinion
            // the dark plate gets. The row has to say at least as much as the
            // probe did to take its place, so a crop that is genuinely too
            // tight - a marker with no plate left under the mining lamp,
            // only strokes - keeps the generous reading. The test is the
            // texture, not the level the cut landed at: a plate that really
            // is blazing (luma 243) is cut at 237 and read perfectly well.
            const bool flatPlate = trace.plateStd < 4.0;
            if (tightReadsLeft > 0 && (probed.empty() ? candidate.lump : flatPlate)) {
                // A lump is the whole readout, pin and all, so a hair of
                // margin all round is right. A row is the digits alone -
                // the pin stands a good two digit-heights to its left, and
                // a crop that clips it reads as "no pin" and is thrown
                // away downstream. So the row gets its plate back: room
                // for the pin on the left, half a height everywhere else.
                const int h = (std::max)(1, candidate.row.height);
                const int m = (std::max)(3, static_cast<int>(std::lround(h * 0.3)));
                const int left = candidate.lump ? m : (std::max)(6, static_cast<int>(std::lround(h * 2.5)));
                const int side = candidate.lump ? m : (std::max)(3, static_cast<int>(std::lround(h * 0.5)));
                cv::Rect tight(candidate.row.x - left, candidate.row.y - side,
                               candidate.row.width + left + side, candidate.row.height + 2 * side);
                tight &= cv::Rect(0, 0, image.cols, image.rows);
                if (tight.width > 10 && tight.height > 6) {
                    tightReadsLeft--;
                    GlyphReader::Trace tightTrace;
                    GlyphReader::profile().probes++;
                    auto again = GlyphReader::extractGlyphs(image(tight), &tightTrace, true,
                                                            GlyphReader::PIN_MIN_NCC_STRICT, nullptr);
                    if (traceLocate) {
                        std::cerr << "  [locate]   tight " << tight.x << "," << tight.y << " " << tight.width
                                  << "x" << tight.height << " -> " << again.size() << " glyph(s), pin "
                                  << static_cast<int>(tightTrace.pinScore * 100) << "%, "
                                  << tightTrace.channel << std::endl;
                    }
                    if (!again.empty() && again.size() >= probed.size()) {
                        probed = std::move(again);
                        probe = tight;
                    }
                }
            }
            if (probed.empty()) continue;

            // Hand back the very crop the reader just succeeded on. Cutting it
            // down to the plate afterwards goes wrong in both directions: too
            // tight and the reader loses the context it thresholds against (a
            // marker blown out by daylight under LAMP has no plate left at all,
            // only digit strokes), too loose and it measures the ground instead
            // of the marker. The probe is by construction neither - the reader
            // has just proved it can read it.
            results.push_back(probe);
            referenceLabels_.push_back({probe, candidate.row});
        }
        if (traceLocate) {
            std::cerr << "  [locate] " << candidates.size() << " candidates, "
                      << results.size() << " verified by the pin" << std::endl;
        }
    };

    GlyphReader::profile().locTophatMs += locMs(locStart);
    const auto locRowsStart = LocClock::now();
    collectRows(learnedMask(tophatNeutral), "neutrality");
    collectRows(learnedMask(tophatGray), "luminance");
    const cv::Mat whiteMask = learnedMask(tophatWhite);
    collectRows(whiteMask, "white");
    collectRows(peeledMask(tophatWhite, whiteMask), "white+");

    // One more channel, and the only one here with a FIXED cut: pixels that
    // are near-grey, whatever their brightness - the reader's "neutral"
    // channel at 255-18, through which it finds the hard markers.
    //
    // A fixed level is defensible here because it is not a brightness: it says
    // "this pixel has almost no colour", and the digits are white in every
    // lighting while sky, terrain and the ship UI all carry colour. Against
    // the sun that difference survives where every brightness-based cut has
    // already been dragged up out of reach.
    collectRows(neutral >= 237, "neutral/fixed");
    GlyphReader::profile().locRowsMs += locMs(locRowsStart);
    const auto locProbeStart = LocClock::now();
    verifyRows();
    GlyphReader::profile().locProbeMs += locMs(locProbeStart);

    // Both channels above look for something BRIGHTER than its surroundings,
    // which is what the readout is in every normal scene. Under LAMP night
    // vision in daylight the view blows out to white and the polarity flips:
    // the plate is gone entirely and the digits are coloured strokes on a
    // neutral ground (measured: background chroma 0, digit strokes up to
    // 132). The inverse pair costs a second sweep, so it only runs when the
    // usual one turned up no verified marker.
    if (results.empty()) {
        const auto fallbackStart = LocClock::now();
        cv::Mat tophatChroma, blackhatGray;
        cv::morphologyEx(high - low, tophatChroma, cv::MORPH_TOPHAT, kernel);
        cv::morphologyEx(gray, blackhatGray, cv::MORPH_BLACKHAT, kernel);
        rows.clear();
        collectRows(learnedMask(tophatChroma), "colour");
        collectRows(learnedMask(blackhatGray), "dark");
        GlyphReader::profile().locFallbackRowsMs += locMs(fallbackStart);
        const auto fallbackProbeStart = LocClock::now();
        verifyRows();
        GlyphReader::profile().locFallbackProbeMs += locMs(fallbackProbeStart);
    }
    // Nothing verified: hand back the best-looking candidates anyway, so a
    // marker the reader cannot segment still reaches the OCR fallback
    if (results.empty()) {
        for (const auto& candidate : candidates) {
            if (static_cast<int>(results.size()) >= maxResults) break;
            results.push_back(candidate.rect);
            referenceLabels_.push_back({candidate.rect, candidate.row});
        }
    }

    return results;
}

std::vector<cv::Rect> ScreenCapture::uncovered(const cv::Rect& rect, const cv::Rect& over) {
    const cv::Rect cut = rect & over;
    if (cut.area() == 0) return {rect};
    std::vector<cv::Rect> parts;
    if (cut.y > rect.y) parts.emplace_back(rect.x, rect.y, rect.width, cut.y - rect.y);
    if (cut.br().y < rect.br().y) parts.emplace_back(rect.x, cut.br().y, rect.width, rect.br().y - cut.br().y);
    if (cut.x > rect.x) parts.emplace_back(rect.x, cut.y, cut.x - rect.x, cut.height);
    if (cut.br().x < rect.br().x) parts.emplace_back(cut.br().x, cut.y, rect.br().x - cut.br().x, cut.height);
    return parts;
}

void ScreenCapture::maskCovered(cv::Mat& picture, const cv::Rect& area, const std::vector<cv::Rect>& covers) {
    if (picture.empty()) return;
    const cv::Rect inPicture(0, 0, picture.cols, picture.rows);
    for (const cv::Rect& cover : covers) {
        const cv::Rect hit = (cover & area) - area.tl();
        const cv::Rect fill = hit & inPicture;
        if (fill.area() > 0) picture(fill).setTo(cv::Scalar::all(0));
    }
}

cv::Mat ScreenCapture::extractNumberRegion(const cv::Mat& image) {
    // Convert to grayscale
    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image.clone();
    }

    // Threshold for bright digits on a dark background
    cv::Mat binary;
    cv::threshold(gray, binary, 200, 255, cv::THRESH_BINARY);

    // Find contours
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    if (contours.empty()) {
        return image;
    }

    // Find the largest contour (most likely the number)
    size_t maxIdx = 0;
    double maxArea = 0;
    for (size_t i = 0; i < contours.size(); i++) {
        double area = cv::contourArea(contours[i]);
        if (area > maxArea) {
            maxArea = area;
            maxIdx = i;
        }
    }

    cv::Rect numberRect = cv::boundingRect(contours[maxIdx]);

    // Add some padding
    int pad = 5;
    numberRect.x = (std::max)(0, numberRect.x - pad);
    numberRect.y = (std::max)(0, numberRect.y - pad);
    numberRect.width = (std::min)(image.cols - numberRect.x, numberRect.width + 2 * pad);
    numberRect.height = (std::min)(image.rows - numberRect.y, numberRect.height + 2 * pad);

    return image(numberRect).clone();
}

namespace {

// Builds an OCR-ready binary image from one intensity channel.
// thresholdBias > 0 keeps only the glyph cores (a fraction of the way from
// the Otsu level towards the extreme): UI glow bloats glyphs until digits
// merge at the plain Otsu level, while the cores stay separated.
// unsharpSigma/unsharpAmount control how much of the HUD's glow halo is
// subtracted before thresholding.
// Returns an empty Mat when the channel does not contain a digit-like line,
// so the caller can try the next channel or fail cleanly.
cv::Mat buildOcrImageFromChannel(const cv::Mat& channel, double thresholdBias = 0.0,
                                 double unsharpSigma = 4.0, double unsharpAmount = 1.2,
                                 bool adaptive = false, bool equalize = false) {
    // Truly flat captures (empty sky/panel) die here immediately
    double minVal, maxVal;
    cv::minMaxLoc(channel, &minVal, &maxVal);
    if (maxVal - minVal < 12.0) {
        return {};
    }

    // Stretch to full range: scanner-ping glow washes the HUD out so badly
    // that digits sit only a few intensity steps above the background -
    // normalization makes such weak signals separable again
    cv::Mat norm;
    cv::normalize(channel, norm, 0, 255, cv::NORM_MINMAX);

    // Upscale FIRST, while the antialiasing detail is still intact.
    // In-game captures are tiny (digits ~10px). Cubic interpolation to a
    // ~120px capture height gives Tesseract smooth, readable glyphs. Measured:
    // going higher (240) loses 10 points on real captures, because the glow
    // halo scales with the image and swamps the deglow filter.
    double factor = 120.0 / (std::max)(1, norm.rows);
    factor = std::clamp(factor, 2.0, 10.0);
    cv::Mat big;
    cv::resize(norm, big, cv::Size(), factor, factor, cv::INTER_CUBIC);

    // Night vision (LAMP) amplifies the scene into one tint and squeezes the
    // brightness range, leaving the digits barely above the panel. Remove the
    // amplified sensor grain first - at this scale a median clears speckle
    // without eating the strokes - then equalise the contrast tile by tile to
    // pull that faint difference apart, whatever colour it is.
    if (equalize) {
        cv::medianBlur(big, big, 3);
        auto clahe = cv::createCLAHE(2.5, cv::Size(8, 8));
        clahe->apply(big, big);
    }

    // Undo the HUD's glow bleed. The emissive UI blurs each glyph into its
    // neighbors, so plain thresholding fuses "8,480" into one blob that no
    // OCR can segment. Unsharp masking subtracts that low-frequency halo and
    // restores the gaps between digits.
    cv::Mat halo;
    cv::GaussianBlur(big, halo, cv::Size(0, 0), unsharpSigma);
    cv::addWeighted(big, 1.0 + unsharpAmount, halo, -unsharpAmount, 0.0, big);
    cv::GaussianBlur(big, big, cv::Size(3, 3), 0);

    cv::Mat mask;
    if (adaptive) {
        // Local thresholding for washed-out scenes (flying near a star, bright
        // atmosphere): the background carries a strong brightness gradient, so
        // a single global level either floods or erases the glyphs.
        int block = (big.rows / 2) | 1;
        block = (std::max)(11, block);
        cv::adaptiveThreshold(big, mask, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                              cv::THRESH_BINARY, block, -8.0);
        if (cv::countNonZero(mask) > static_cast<int>(mask.total()) / 2) {
            cv::bitwise_not(mask, mask);
        }
    } else {
        // Otsu threshold - adapts to UI glow, exposure and color shifts
        const double otsuLevel = cv::threshold(big, mask, 0, 255,
                                               cv::THRESH_BINARY | cv::THRESH_OTSU);

        // If most pixels ended up "on", the background was the bright part.
        // Work on a copy where the text is always the BRIGHT side, so every
        // step below can simply raise the threshold.
        const bool invertedText = cv::countNonZero(mask) > static_cast<int>(mask.total()) / 2;
        cv::Mat work = big;
        double level = otsuLevel;
        if (invertedText) {
            cv::bitwise_not(big, work);
            level = 255.0 - otsuLevel;
        }
        cv::threshold(work, mask, level, 255, cv::THRESH_BINARY);

        // Optional tighter threshold that keeps only the glyph cores
        if (thresholdBias > 0.0) {
            level += thresholdBias * (255.0 - level);
            cv::threshold(work, mask, level, 255, cv::THRESH_BINARY);
        }

        // The strongest brightness step is not always the text. Under night
        // vision the panel stands out from the scene far more than the digits
        // stand out from the panel, so the first split lands one layer too
        // early and swallows the whole plate. Threshold again inside what was
        // kept, peeling one layer at a time until it is glyph-sized.
        for (int pass = 0; pass < 3; pass++) {
            const int on = cv::countNonZero(mask);
            if (on == 0) break;

            // Peel when what was kept cannot be text. Two symptoms give that
            // away: it covers far too much of the capture, or it is a densely
            // filled block spanning the whole height - that is the marker
            // PLATE, with the digits sitting inside it as holes. Digits
            // themselves are sparse, so their fill ratio stays low.
            const cv::Rect bounds = cv::boundingRect(mask);
            const double fill = bounds.area() > 0
                ? static_cast<double>(on) / bounds.area()
                : 0.0;
            const bool tooLarge = on > static_cast<int>(mask.total() * 0.45);
            const bool solidPlate = fill > 0.62 && bounds.height > mask.rows * 0.5;
            if (!tooLarge && !solidPlate) break;

            std::vector<uchar> insideValues;
            insideValues.reserve(on);
            for (int y = 0; y < work.rows; y++) {
                const uchar* src = work.ptr<uchar>(y);
                const uchar* sel = mask.ptr<uchar>(y);
                for (int x = 0; x < work.cols; x++) {
                    if (sel[x]) insideValues.push_back(src[x]);
                }
            }
            if (insideValues.size() < 16) break;

            cv::Mat inside(static_cast<int>(insideValues.size()), 1, CV_8U, insideValues.data());
            cv::Mat discard;
            level = cv::threshold(inside, discard, 0, 255,
                                  cv::THRESH_BINARY | cv::THRESH_OTSU);
            cv::threshold(work, mask, level, 255, cv::THRESH_BINARY);
        }
    }

    // Text covers a modest share of the capture; near-empty or half-covered
    // masks are gradients/noise, not digits
    const double fgShare = static_cast<double>(cv::countNonZero(mask)) / mask.total();
    if (fgShare < 0.005 || fgShare > 0.45) {
        traceReject("foreground share", fgShare);
        return {};
    }

    // Connected component analysis - drop speckles, the comma and the pin
    // icon, using thresholds RELATIVE to the glyph size
    cv::Mat labels, stats, centroids;
    int numLabels = cv::connectedComponentsWithStats(mask, labels, stats, centroids);

    struct Comp { int x, y, w, h, area, label; };
    std::vector<Comp> comps;
    int maxH = 0;
    for (int i = 1; i < numLabels; i++) {  // label 0 is the background
        Comp c{stats.at<int>(i, cv::CC_STAT_LEFT),
               stats.at<int>(i, cv::CC_STAT_TOP),
               stats.at<int>(i, cv::CC_STAT_WIDTH),
               stats.at<int>(i, cv::CC_STAT_HEIGHT),
               stats.at<int>(i, cv::CC_STAT_AREA),
               i};
        comps.push_back(c);
        maxH = (std::max)(maxH, c.h);
    }

    // Keep glyph-shaped components: at least ~45% as tall as the tallest one
    // (removes the comma and noise, keeps thin digits like "1"), not
    // extremely wide (rejects horizontal UI lines), and not near-solid
    // (digits fill 20-65% of their box; glow blobs fill almost all of it)
    std::vector<Comp> kept;
    for (const auto& c : comps) {
        const double fill = static_cast<double>(c.area) / ((std::max)(1, c.w * c.h));
        const bool solidBlob = fill > 0.8 && c.w > c.h;
        const double aspect = static_cast<double>(c.w) / (std::max)(1, c.h);

        // Normally one component is one digit, so anything much wider than
        // tall is a UI rule. Under night vision the glow welds the digits into
        // a single wide block, though - that block still spans the text line's
        // full height, so keep it and let the OCR do the segmenting.
        const bool digitShaped = aspect <= 3.0;
        // ... but a hollow outline is the marker plate's BORDER, not text:
        // a welded digit row fills a good part of its box, a frame almost none
        const bool mergedLine = aspect <= 15.0 && c.h >= mask.rows * 0.30 &&
                                fill >= 0.35 && fill <= 0.80;

        // A large, hollow shape that encloses most of the capture is the
        // marker plate's outline. Tesseract treats such a box as part of the
        // word and fails on the whole line, so it has to go - whatever its
        // aspect ratio looks like.
        const bool frameLike = fill < 0.35 &&
                               c.w > mask.cols * 0.55 && c.h > mask.rows * 0.45;

        if (c.h >= (std::max)(4, static_cast<int>(maxH * 0.45)) &&
            (digitShaped || mergedLine) && !solidBlob && !frameLike) {
            kept.push_back(c);
        }
    }
    // A signature line has a handful of digits - anything else is noise
    if (kept.empty() || kept.size() > 12) {
        traceReject("glyph count", static_cast<double>(kept.size()));
        return {};
    }

    // Split into text ROWS first: a capture can catch a second readout above
    // or below the signature (compass marks, distance labels). Components
    // whose vertical centers are far apart belong to different lines, and
    // merging them would produce a nonsense number.
    if (kept.size() >= 2) {
        std::sort(kept.begin(), kept.end(),
                  [](const Comp& a, const Comp& b) { return a.y + a.h / 2 < b.y + b.h / 2; });

        std::vector<std::vector<Comp>> rows;
        rows.emplace_back();
        rows.back().push_back(kept[0]);
        for (size_t i = 1; i < kept.size(); i++) {
            const int prevCenter = kept[i - 1].y + kept[i - 1].h / 2;
            const int center = kept[i].y + kept[i].h / 2;
            if (center - prevCenter > kept[i].h * 0.6) {
                rows.emplace_back();
            }
            rows.back().push_back(kept[i]);
        }
        auto widestRow = std::max_element(rows.begin(), rows.end(),
            [](const std::vector<Comp>& a, const std::vector<Comp>& b) {
                return a.size() < b.size();
            });
        kept = *widestRow;
    }

    std::sort(kept.begin(), kept.end(), [](const Comp& a, const Comp& b) { return a.x < b.x; });

    // Split into visual groups: a horizontal gap wider than ~1.2x the line
    // height separates unrelated readouts (e.g. compass marks at both edges
    // of the capture). Keep only the largest group so distinct numbers are
    // never merged into one bogus value.
    if (kept.size() >= 2) {
        std::vector<std::vector<Comp>> groups;
        groups.emplace_back();
        groups.back().push_back(kept[0]);
        for (size_t i = 1; i < kept.size(); i++) {
            const int gap = kept[i].x - (kept[i - 1].x + kept[i - 1].w);
            if (gap > static_cast<int>(mask.rows * 1.2)) {
                groups.emplace_back();
            }
            groups.back().push_back(kept[i]);
        }
        auto largest = std::max_element(groups.begin(), groups.end(),
            [](const std::vector<Comp>& a, const std::vector<Comp>& b) {
                return a.size() < b.size();
            });
        kept = *largest;
    }

    // Pin icon removal: the pin sits left of the number, separated by the
    // widest gap in the line, and overtops the digits. Drop the leftmost
    // component when it is clearly taller than the rest, or when the gap
    // behind it is both the widest one and wider than half a glyph height.
    if (kept.size() >= 2) {
        std::vector<int> heights;
        for (size_t i = 1; i < kept.size(); i++) {
            heights.push_back(kept[i].h);
        }
        std::sort(heights.begin(), heights.end());
        const int medianH = heights[heights.size() / 2];
        const bool taller = kept[0].h > static_cast<int>(medianH * 1.15);

        const int firstGap = kept[1].x - (kept[0].x + kept[0].w);
        int widestOtherGap = 0;
        for (size_t i = 1; i + 1 < kept.size(); i++) {
            widestOtherGap = (std::max)(widestOtherGap,
                                        kept[i + 1].x - (kept[i].x + kept[i].w));
        }
        const bool gapped = firstGap >= widestOtherGap &&
                            firstGap > static_cast<int>(medianH * 0.5);

        if (taller || gapped) {
            kept.erase(kept.begin());
        }
    }

    // Rebuild a clean mask from the kept components and crop to their bounds
    cv::Mat cleaned = cv::Mat::zeros(mask.size(), CV_8UC1);
    int minX = mask.cols, minY = mask.rows, maxX = 0, maxY = 0;
    for (const auto& c : kept) {
        cleaned.setTo(255, labels == c.label);
        minX = (std::min)(minX, c.x);
        minY = (std::min)(minY, c.y);
        maxX = (std::max)(maxX, c.x + c.w);
        maxY = (std::max)(maxY, c.y + c.h);
    }

    // The digit line should have real height relative to the capture
    if (maxY - minY < static_cast<int>(mask.rows * 0.25)) {
        traceReject("line height", static_cast<double>(maxY - minY) / mask.rows);
        return {};
    }

    cv::Rect bounds(minX, minY, maxX - minX, maxY - minY);
    cv::Mat cropped = cleaned(bounds).clone();

    // Normalize the GLYPH height (not the capture height) to what Tesseract
    // is trained for. The capture-based upscale above leaves digits around
    // 35px tall, well below the ~60-90px where recognition gets reliable;
    // scaling the cropped line fixes that regardless of capture size.
    const double glyphFactor = 80.0 / (std::max)(1, cropped.rows);
    if (glyphFactor > 1.05) {
        cv::resize(cropped, cropped, cv::Size(), glyphFactor, glyphFactor, cv::INTER_CUBIC);
    }

    // OCR expects dark text on a bright background, plus generous padding
    cv::Mat result;
    cv::bitwise_not(cropped, result);
    cv::copyMakeBorder(result, result, 40, 40, 40, 40,
                       cv::BORDER_CONSTANT, cv::Scalar(255));
    return result;
}

// Learns the colour axis that separates the text from its background instead
// of guessing fixed channels. Clustering the pixels finds the few colours the
// capture actually consists of; projecting onto the line from the background
// colour to a foreground colour then gives maximum contrast whatever those
// colours are - orange on blue, white on white-ish, green on black alike.
std::vector<std::pair<std::string, cv::Mat>> learnedChannels(const cv::Mat& image,
                                                             int maxChannels) {
    std::vector<std::pair<std::string, cv::Mat>> out;
    if (image.channels() < 3 || image.total() < 64) {
        return out;
    }

    cv::Mat bgr;
    if (image.channels() == 4) {
        cv::cvtColor(image, bgr, cv::COLOR_BGRA2BGR);
    } else {
        bgr = image;
    }

    // CIELAB: euclidean distance here tracks perceived colour difference, so
    // the clusters split the way a human sees the readout
    cv::Mat lab;
    cv::cvtColor(bgr, lab, cv::COLOR_BGR2Lab);

    cv::Mat samples;
    lab.convertTo(samples, CV_32F);
    samples = samples.reshape(1, static_cast<int>(lab.total()));

    // Three clusters cover the usual case: panel, digits, and the surrounding
    // scene or the pin icon
    const int clusterCount = 3;
    cv::Mat labels, centers;
    cv::kmeans(samples, clusterCount, labels,
               cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 10, 1.0),
               2, cv::KMEANS_PP_CENTERS, centers);

    std::vector<int> counts(clusterCount, 0);
    for (int i = 0; i < labels.rows; i++) {
        counts[labels.at<int>(i)]++;
    }

    // The biggest cluster is the background; the others are candidates for text
    const int background =
        static_cast<int>(std::max_element(counts.begin(), counts.end()) - counts.begin());

    std::vector<int> foreground;
    for (int k = 0; k < clusterCount; k++) {
        if (k != background) foreground.push_back(k);
    }
    std::sort(foreground.begin(), foreground.end(),
              [&counts](int a, int b) { return counts[a] > counts[b]; });

    for (size_t i = 0; i < foreground.size() && static_cast<int>(out.size()) < maxChannels; i++) {
        const int fg = foreground[i];

        cv::Vec3f axis;
        for (int c = 0; c < 3; c++) {
            axis[c] = centers.at<float>(fg, c) - centers.at<float>(background, c);
        }
        const float length = std::sqrt(axis.dot(axis));
        if (length < 10.0f) {
            continue;  // the two colours are too close to separate reliably
        }
        axis /= length;

        cv::Mat projection(lab.size(), CV_32F);
        for (int y = 0; y < lab.rows; y++) {
            const cv::Vec3b* src = lab.ptr<cv::Vec3b>(y);
            float* dst = projection.ptr<float>(y);
            for (int x = 0; x < lab.cols; x++) {
                dst[x] = axis[0] * src[x][0] + axis[1] * src[x][1] + axis[2] * src[x][2];
            }
        }

        cv::Mat channel;
        cv::normalize(projection, channel, 0, 255, cv::NORM_MINMAX);
        channel.convertTo(channel, CV_8U);
        out.emplace_back("learn" + std::to_string(i), channel);
    }

    return out;
}

} // namespace

std::vector<cv::Mat> ScreenCapture::preprocessCandidates(const cv::Mat& image) {
    // Debug: rolling buffer of the last 40 scans (slot = counter mod 40),
    // so the debug folder always shows the CURRENT situation, not the
    // first seconds after startup
    static int debugCounter = 0;
    const int slot = debugCounter % 40;
    debugCounter++;
    saveDebugImage("ocr_original_" + std::to_string(slot) + ".png", image);
    // Clear this slot's channel outputs from a previous round
    for (const char* ch : {"white", "gray", "warm", "learn0", "learn1",
                           "white_hi", "gray_hi", "warm_hi", "learn0_hi", "learn1_hi",
                           "white_sharp", "gray_sharp", "warm_sharp",
                           "learn0_sharp", "learn1_sharp",
                           "white_local", "gray_local", "warm_local",
                           "learn0_local", "learn1_local"}) {
        std::error_code ec;
        std::filesystem::remove("debug/ocr_processed_" + std::to_string(slot) +
                                "_" + ch + ".png", ec);
    }

    // Contrast channels. Ship HUD colors vary by manufacturer (orange, blue,
    // green, white) and backgrounds range from black space to bright skies,
    // so no single channel works everywhere:
    //  - white (min of B,G,R): only unsaturated pixels stay bright, so white
    //    text pops off ANY colored panel and the game's chromatic aberration
    //    fringes (saturated red/cyan glyph edges) collapse to black. Listed
    //    first because it is the strongest separator for the scanner readout.
    //  - grayscale:  bright/neutral text vs. dark (or dark vs. bright) scenes
    //  - learned:    the axis that actually separates text from background in
    //    THIS capture, found by clustering its colours (see learnedChannels)
    std::vector<std::pair<std::string, cv::Mat>> channels;

    // Under a night-vision tint every channel carries the same picture in a
    // different gain, which flattens "white text" into nothing. Undoing the
    // cast restores the separation; on normal captures this is a no-op.
    cv::Mat source = image;
    if (image.channels() >= 3 && colourCastStrength(image) > CAST_THRESHOLD) {
        cv::Mat bgrOnly;
        if (image.channels() == 4) {
            cv::cvtColor(image, bgrOnly, cv::COLOR_BGRA2BGR);
        } else {
            bgrOnly = image;
        }
        source = neutralizeTint(bgrOnly);
    }

    cv::Mat gray;
    if (source.channels() == 4) {
        cv::cvtColor(source, gray, cv::COLOR_BGRA2GRAY);
    } else if (source.channels() == 3) {
        cv::cvtColor(source, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = source.clone();
    }

    if (source.channels() >= 3) {
        std::vector<cv::Mat> bgr;
        cv::split(source, bgr);

        cv::Mat white;
        cv::min(bgr[0], bgr[1], white);
        cv::min(white, bgr[2], white);
        channels.emplace_back("white", white);
    }

    channels.emplace_back("gray", gray);

    // Warm channel (red minus blue). Measured on a daylight capture, this is
    // the only axis that actually separates the readout: the digits carry the
    // marker's warm tint (B=197) while sunlit terrain stays cold-white
    // (B=246). Brightness and saturation both point the WRONG way there - the
    // ground is brighter and less saturated than the text.
    if (source.channels() >= 3) {
        cv::Mat rgb = source;
        if (source.channels() == 4) {
            cv::cvtColor(source, rgb, cv::COLOR_BGRA2BGR);
        }
        std::vector<cv::Mat> bgrPlanes;
        cv::split(rgb, bgrPlanes);
        cv::Mat warm;
        cv::subtract(bgrPlanes[2], bgrPlanes[0], warm);   // saturates at 0
        channels.emplace_back("warm", warm);
    }

    // Colour-agnostic separators learned from this very capture - they replace
    // guessing at warm/cold/green hues and handle any UI colour scheme
    for (auto& learned : learnedChannels(source, 2)) {
        channels.push_back(std::move(learned));
    }

    // Renderings per channel, ordered so the usual winner comes first
    // (recognizeBest stops early on a confident exact hit):
    //   base   - standard deglow
    //   hi     - tighter threshold, keeps only glyph cores
    //   sharp  - aggressive deglow for badly bled frames
    //   local  - adaptive threshold for uneven, washed-out lighting
    //   eq     - local contrast equalisation for night vision's flat tint
    struct Variant {
        const char* suffix;
        double bias;
        double sigma;
        double amount;
        bool adaptive;
        bool equalize;
    };
    static const Variant VARIANTS[] = {
        {"",       0.0, 4.0, 1.2, false, false},
        {"_hi",    0.3, 4.0, 1.2, false, false},
        {"_sharp", 0.0, 2.5, 1.8, false, false},
        {"_local", 0.0, 4.0, 1.2, true,  false},
    };

    // Variant-major order: the first renderings handed to the OCR cover ALL
    // channels with the standard deglow rather than four flavours of the same
    // channel. Independent channels agreeing is what ends the vote early, so
    // this ordering usually settles a clean frame within a few passes.
    std::vector<cv::Mat> candidates;
    for (const auto& v : VARIANTS) {
        for (const auto& [name, channel] : channels) {
            cv::Mat result = buildOcrImageFromChannel(channel, v.bias, v.sigma,
                                                      v.amount, v.adaptive, v.equalize);
            if (result.empty()) {
                continue;
            }
            saveDebugImage("ocr_processed_" + std::to_string(slot) +
                           "_" + name + v.suffix + ".png", result);
            candidates.push_back(result);
        }
    }

    return candidates;
}

} // namespace SCMiner
