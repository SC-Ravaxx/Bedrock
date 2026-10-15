#include "Application.h"
#include "DevSwitch.h"
#include "ScanPanel.h"
#include "LoadoutReader.h"
#include "MiningHardware.h"
#include "GamePlaces.h"
#include <cctype>

#include "AppPaths.h"
#include "CraftingData.h"
#include "MiniJson.h"
#include "RefineryData.h"
#include "RefiningData.h"
#include "resource.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <chrono>
#include <thread>
#include <conio.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <opencv2/core/utility.hpp>   // cv::setNumThreads

namespace SCMiner {

// Bedrock's UEX app token, read from beside the program (see below)
static std::string uexToken();

namespace {

// The OCR floor lives in MiningData.h, shared with the offline benchmark

// Empty scans in a row before the search widens to the whole monitor
constexpr int REACQUIRE_AFTER_MISSES = 8;

// The rolling record: five clips of a minute, overwriting each other
constexpr int RECORD_SEGMENT_SECONDS = 60;
constexpr int RECORD_SEGMENTS = 5;
// Nothing under debug/ may grow without bound - this build goes to other
// people's machines. The ring overwrites itself (~450 MB for five lossless
// minutes), the image rings are 40 and 20 deep, the scan log rotates at
// SCAN_LOG_MAX_BYTES, and the copies Ctrl+Alt+K makes of the ring are capped:
// the newest few stay (keeps= in region_config.txt, default 3), older ones
// go, and together they stay under keepgb= (default 2 GB). Recording and
// keeping also refuse to fill a drive.
// (the caps themselves are members - maxKeeps_ and maxKeepGb_ - so that
// region_config.txt can raise them on a machine that collects footage)
constexpr std::uintmax_t RECORD_MIN_FREE_BYTES = 1536ull * 1024 * 1024;
constexpr std::uintmax_t SCAN_LOG_MAX_BYTES = 10ull * 1024 * 1024;

// Once a marker has been read, the search sticks to it: a small window around
// where it was, rather than the wide area around the aim point. The marker is
// anchored to a rock in 3D, so head movement and the ship's own sway shift it
// by a few centimetres at most between two scans - but the wide window also
// contained chat text and cockpit labels, and the scan kept locking onto those
// instead. Held for PATIENCE scans of nothing before the search opens up again.
constexpr int LOCK_MARGIN_PX = 150;
// How many scans that read nothing the lock survives.
//
// The marker travels 29 px between scans on average, 130 px at worst,
// while the pinned window reaches 150 px in every direction, so it almost
// never leaves the window. A short patience is what loses it: at 12 (~4 s)
// a stretch of unreadable frames - the ship's lamp, a bright planet, a hull
// whose HUD is a colour the templates have not seen - throws the search
// back to the aim point, where it settles on the dashboard and stays.
//
// Holding longer costs nothing: the full-screen sweep is driven by its own
// counter and still fires every 8 unread scans, so a lock that really is dead
// does not block re-acquisition.
constexpr int LOCK_PATIENCE = 60;   // ~20 s
// How well the last picture of the marker has to match before the lock is
// moved onto it. High enough that terrain or a different HUD element does not
// pull the lock away, low enough to survive the marker changing size slightly
// as the ship closes in.
constexpr double LOCK_FOLLOW_NCC = 0.65;
constexpr int LOCK_FOLLOW_WINDOW = 150;     // px about the lock the follow searches, after the camera's shift
// Scans in a row the locked marker's picture is found nowhere in the area -
// the whole area is matched once the window about it missed - before the
// lock is let go: ~6 s at 300 ms. LOCK_PATIENCE only counts scans whose
// locator found no label at all, and in a cockpit it nearly always finds
// one, the HUD's own figures; without this cap a lock outlives its marker
// by minutes and keeps the whole-area match and locator running (354 ms a
// scan against 99 without them).
constexpr int LOCK_LOST_AFTER = 20;

// How many of the located candidates a scan actually tries.
//
// An unscanned contact carries the SAME map pin and a line of text
// ("UNKNOWN 6.2km"), and a planet ping puts a dozen of them up at once, all
// of them perfectly marker-shaped. The one rock that has actually been
// scanned - the only one carrying a number - is then just one candidate
// among many, and with three slots handed out by distance to the aim point
// it is routinely never looked at.
//
// Widening this is cheap because the template reader throws a contact out in
// microseconds: "6.2km" is three digits, a signature is four to six. Tesseract
// is the expensive part, so it stays rationed - see OCR_FALLBACK_CANDIDATES.
constexpr int SCAN_CANDIDATES = 12;
constexpr int OCR_FALLBACK_CANDIDATES = 3;
// How many of the candidates nearest the aim may be read a second time with
// the locator's row hint dropped - see GlyphReader::read. The hint is
// usually right; where it is wrong it caught the HUD line above or below
// and the row is scored across two baselines. Three, because the marker is
// what the player is pointing at and the list is sorted by that.
constexpr int SECOND_CHANCE_CANDIDATES = 3;
// The second reader's idle pace: after this many seconds without a find it
// reads on its own one row a scan at most, and none it has looked at lately
// (see the scan)
constexpr double LINE_IDLE_AFTER_S = 5.0;
constexpr int LINE_IDLE_ROWS = 1;
// ...and how long a row it looked at and found nothing in is left
// alone, and how many such rows are remembered. Resting is per ROW:
// a HUD row that stands still is read once every few seconds instead
// of never, and a row that was not there before is read at once.
constexpr double LINE_LOOK_AGAIN_S = 5.0;
constexpr size_t LINE_LOOKED_MOST = 12;

// After this long with nothing read, most scans look only at the area
// around the aim box rather than at everything the margin allows, and every
// WIDE_SWEEP_EVERY-th scan goes wide again to catch a marker off to the
// side. The capture stays the same rectangle either way - the tracking
// compares frame with frame and needs that - only the locator is handed
// less to sift.
//
// Measured under the six threads a scan may use, on the busiest frames
// there are (an ice field, 3,600 bright specks): the whole area costs
// 740 ms of processor time a sweep, the near area 155 ms. Seven near
// sweeps and one wide one, at 300 ms each, come to three quarters of a
// core.
constexpr double NEAR_SWEEP_AFTER = 15.0;   // seconds without a reading
constexpr int WIDE_SWEEP_EVERY = 8;         // ...and every eighth scan still goes wide
constexpr int NEAR_SWEEP_MARGIN = 150;      // px around the aim box

// A marker is anchored to a rock in 3D. The ship is never perfectly still -
// even parked it sways, and the HUD with it - so the plate never stops moving
// by a pixel or two. A row of digits that reads the same value at the SAME
// pixel over and over is therefore not a rock: it is part of the cockpit.
//
// This is what the ship's own status pills are - a coloured icon, white digits,
// a rounded plate, sitting in the corner forever. They pass every test the
// marker passes: the pin matches at 73 % (real markers run 52-100 %, so no
// threshold separates them), and what they read - 8390 - is an exact table
// value, 2x Tin. Standing still is the one thing they do that a rock cannot,
// and it is measurable; unchecked, the scan sits on them while the real
// marker is never looked at.
//
// Once a marker HAS been read, keeping the frame on it is worth far more than
// reading it again. The plate is the same handful of pixels from frame to
// frame, so matching the picture of it survives what the reader does not: a
// ship's lamp bleaching the plate, a different hull's HUD colour, a bright
// planet, the marker shrinking with distance. Recognition then only has to
// succeed ONCE instead of on every frame.
//
// So a successful match keeps the lock alive on its own. The cap is a
// safeguard, not a schedule: without one, a patch of terrain that happens to
// correlate would hold the lock for the rest of the session.
constexpr int LOCK_FOLLOW_PATIENCE = 200;   // ~60 s of tracking with no reading
// A head turn moves the marker much further than the ship's own sway. When
// the picture is not found in the pinned window, the next scan looks in a
// wider one before the lock is given up.
constexpr int LOCK_WIDEN_FACTOR = 3;

// Reading the camera movement out of the picture. The response is phase
// correlation's own confidence that the two frames really are the same scene
// shifted; the cap rejects a jump no head turn could produce between two
// scans, which is what a scene change looks like.
// Measured against a recorded approach, comparing the shift this reports with
// how far the marker really travelled between the same two frames:
//
//     response 0.24 -> camera 14.3 px, marker 15 px
//     response 0.19 -> camera 25.3 px, marker 26 px
//     response 0.17 -> camera 33.2 px, marker 34 px
//     response 0.30 -> camera  2.1 px, marker  2 px
//
// Above about 0.15 the two agree to a pixel. Below 0.10 the answer is noise -
// jumps of several hundred pixels that no head turn produces. So the bar sits
// between them: when the view movement can be measured the box is carried by
// it, and when it cannot the box simply stays put, which is never worse.
constexpr double CAMERA_MIN_RESPONSE = 0.15;
constexpr double CAMERA_MAX_SHIFT_PX = 400.0;
// The picture is split into this many horizontal bands, and this many of them
// have to answer confidently before the box is moved. Measured on the live
// picture: with a region framed around the signature and the compass ribbon,
// four to six bands out of six answer on every single frame.
constexpr int CAMERA_BANDS = 6;
constexpr size_t CAMERA_MIN_BANDS = 3;

constexpr int STUCK_LIMIT = 40;          // ~15 s at the usual scan interval
constexpr int STUCK_DRIFT_PX = 3;        // what still counts as "the same pixel"
constexpr int BLACKLIST_SCANS = 150;     // ~1 min before it may be tried again
const char* const REGION_CONFIG_FILE = "region_config.txt";

// Everything the app writes goes under AppPaths::data() - %LOCALAPPDATA%\Bedrock,
// or beside the exe when portable.txt says so. Never simply beside the exe:
// a program folder gets replaced or deleted on an update, and the player's
// library, notes and settings must not be in the way when it does.
std::string dataPath(const std::string& name) {
    return (AppPaths::data() / name).string();
}
// debug/snapshot_* files, oldest making room - the number is snapshotKeep_,
// snapshots= in region_config.txt, ten unless said otherwise

// Debug pictures follow the recording switch - see recordScans_
static bool g_debugImages = false;

void saveDebugCapture(const std::string& name, const cv::Mat& image) {
    if (!g_debugImages) return;
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (!ec) {
        cv::imwrite(dataPath("debug/" + name), image);
    }
}

// Appends one line per live scan to debug/scan_log.csv. The image ring
// buffer only holds the last 40 captures, so this log is the durable
// record of what was read, and why it was shown or held back.
std::uintmax_t folderBytes(const std::filesystem::path& folder) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec)) {
        std::error_code fec;
        if (entry.is_regular_file(fec)) total += entry.file_size(fec);
    }
    return total;
}

// Free space on the drive a folder lives on; 0 when it cannot be told
std::uintmax_t freeBytesAt(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto info = std::filesystem::space(dir, ec);
    return ec ? 0 : info.available;
}

// The duration of the scan being logged, set by the scan itself, split by
// stage (grab, recorder, tracking, locator) so the log shows where the time
// goes; the locator is usually most of it.
static double g_scanMs = 0.0;
static double g_captureMs = 0.0;   // the screen grab alone
static double g_recordMs = 0.0;    // handing the frame to ffmpeg
static double g_intervalMs = 0.0;  // what the scan waited before this one
// Where the marker that was read sat, against the aim box the player
// marked. off_x/off_y is its centre against the box's centre; outside is
// how far it reached past the box's edge, 0 while it is inside.
//
// The search area is the aim box plus trackmargin in every direction; this
// column sizes that margin. 250 px for a 1440-line picture (defaultAimBox):
// further out stand mostly the cockpit's own figures, read as signatures -
// one value at one spot for minutes, such as 10,110 (Ouratite x3) low in a
// fighter's canopy. 73 of 78 such readings stood more than 250 px out, and
// real readings that far out mostly reappear nearer the box within half a
// minute.
static int g_offX = 0, g_offY = 0, g_outside = 0;
static bool g_havePos = false;
// When something was last found - anything at all, including a reading
// that was held back. Set in logScan so no path can forget it.
static std::chrono::steady_clock::time_point g_lastFind =
    std::chrono::steady_clock::now();
static double g_trackMs = 0.0;     // camera motion + following the lock
static double g_locateMs = 0.0;    // finding the marker in the picture
// Whether the pin under this reading only cleared the bar once the
// comparison was allowed to slide. Such a reading is deliberately held
// back until it repeats, and without this column the log shows the
// holding back but never the reason for it.
static bool g_pinSlid = false;
// How many candidates the locator handed the reader on this scan, and how
// the best of them scored as a pin. Without these two a "no-read" line
// says nothing about WHY: a marker on screen that no channel could cut
// looks exactly like empty space.
static int g_found = 0;
static int g_bestPin = 0;
static std::string g_ship;         // the ship Game.log names, for the scan log ("" when unknown)
static std::string g_shipLabel;    // ...and as a name, for the notes
static std::string g_place;        // where Game.log puts the player, "System - Place", for the scan log
static const char* g_font = "";    // "panel" or "canopy": the set the accepted row was read against
// What the second reader said on this scan - agrees, differs, doubts, reads,
// none; "agreed before" when it was not asked again, "resting" when the idle
// pace kept it from reading on its own, blank when it was not asked - and
// what it cost (TextLineReader)
static std::string g_line;
static double g_lineMs = 0.0;

void logScan(int slot, const std::string& result, int value,
             double confidence, int agreement) {
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (ec) return;

    // One line per scan adds up to megabytes a day, so the log rotates:
    // over the cap it becomes scan_log.old.csv and a fresh one starts.
    // Checked every 500 lines, not on every one.
    static int lines = 0;
    if (lines++ % 500 == 0) {
        std::error_code sec;
        const auto size = std::filesystem::file_size(dataPath("debug/scan_log.csv"), sec);
        if (!sec && size > SCAN_LOG_MAX_BYTES) {
            std::filesystem::remove(dataPath("debug/scan_log.old.csv"), sec);
            std::filesystem::rename(dataPath("debug/scan_log.csv"), dataPath("debug/scan_log.old.csv"), sec);
        }
    }

    static const char* const LOG_HEADER =
        "time,slot,result,value,confidence,agreement,scan_ms,capture_ms,record_ms,track_ms,"
        "locate_ms,pin_slid,interval_ms,found,best_pin,off_x,off_y,outside,second,second_ms,ship,font,place";
    bool writeHeader = !std::filesystem::exists(dataPath("debug/scan_log.csv"), ec);
    // A log from an older build has fewer columns. Appending to it would
    // leave both halves unreadable, so it is put aside once and started over.
    if (!writeHeader) {
        static bool headerChecked = false;
        if (!headerChecked) {
            headerChecked = true;
            std::string first;
            std::ifstream old(dataPath("debug/scan_log.csv"));
            if (old.is_open() && std::getline(old, first)) {
                if (!first.empty() && first.back() == 13) first.pop_back();
                if (first != LOG_HEADER) {
                    old.close();
                    std::error_code rec;
                    std::filesystem::remove(dataPath("debug/scan_log.old.csv"), rec);
                    std::filesystem::rename(dataPath("debug/scan_log.csv"),
                                            dataPath("debug/scan_log.old.csv"), rec);
                    writeHeader = !rec;
                }
            }
        }
    }
    std::ofstream log(dataPath("debug/scan_log.csv"), std::ios::app);
    if (!log.is_open()) return;

    if (writeHeader) {
        log << LOG_HEADER << "\n";
    }

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t);
    char timeStr[16];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

    log << timeStr << "," << slot << "," << result << "," << value << ","
        << static_cast<int>(confidence) << "," << agreement << ","
        << static_cast<int>(g_scanMs) << "," << static_cast<int>(g_captureMs) << ","
        << static_cast<int>(g_recordMs) << "," << static_cast<int>(g_trackMs) << ","
        << static_cast<int>(g_locateMs) << "," << (g_pinSlid ? 1 : 0) << ","
        << static_cast<int>(g_intervalMs) << "," << g_found << "," << g_bestPin << ",";
    // Blank rather than 0 where there was no marker: a zero offset is a
    // marker dead centre, which is the opposite of not having one.
    if (g_havePos) log << g_offX << "," << g_offY << "," << g_outside;
    else log << ",,";
    log << "," << g_line << "," << static_cast<int>(g_lineMs);
    log << "," << g_ship << "," << g_font << "," << g_place;
#ifdef BEDROCK_DEV_TOOLS
    // the recording's frame on show, while one is replayed (SCMINER_REPLAY)
    if (ScreenCapture::replayFrame() >= 0) log << "," << ScreenCapture::replayFrame();
#endif
    log << "\n";

    // Anything but a blank screen counts as the player being at work,
    // and puts the scan back to full speed on the very next round.
    if (result != "no-read") g_lastFind = std::chrono::steady_clock::now();
}
} // namespace

// Static instance for the hotkey callback
static Application* g_appInstance = nullptr;

// Overlay window procedure - draggable and resizable
LRESULT CALLBACK Application::OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rect;
            GetClientRect(hwnd, &rect);

            // Fill the background with the color key (turns transparent)
            HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rect, bgBrush);
            DeleteObject(bgBrush);

            // Cyan frame, 2px (matches the app theme, easy to grab)
            HPEN framePen = CreatePen(PS_SOLID, 2, RGB(56, 211, 255));
            HPEN oldPen = (HPEN)SelectObject(hdc, framePen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

            Rectangle(hdc, 0, 0, rect.right, rect.bottom);

            // Label only when the region is large enough that the text does
            // not cover the content being aligned underneath
            if (rect.right >= 160 && rect.bottom >= 60) {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(56, 211, 255));
                HFONT font = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(hdc, font);
                TextOutA(hdc, 6, 4, "SCAN REGION", 11);
                SelectObject(hdc, oldFont);
                DeleteObject(font);
            }

            // Resize handle marker (bottom right)
            HBRUSH cornerBrush = CreateSolidBrush(RGB(255, 166, 87));
            const int cs = 10;
            RECT corner = {rect.right - cs, rect.bottom - cs, rect.right, rect.bottom};
            FillRect(hdc, &corner, cornerBrush);
            DeleteObject(cornerBrush);

            SelectObject(hdc, oldPen);
            SelectObject(hdc, oldBrush);
            DeleteObject(framePen);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCHITTEST: {
            // Enables dragging and resizing
            POINT pt = {LOWORD(lParam), HIWORD(lParam)};
            ScreenToClient(hwnd, &pt);

            RECT rect;
            GetClientRect(hwnd, &rect);

            const int borderSize = 10;

            // Bottom right = resize both
            if (pt.x >= rect.right - borderSize && pt.y >= rect.bottom - borderSize) {
                return HTBOTTOMRIGHT;
            }
            // Right = resize width
            if (pt.x >= rect.right - borderSize) {
                return HTRIGHT;
            }
            // Bottom = resize height
            if (pt.y >= rect.bottom - borderSize) {
                return HTBOTTOM;
            }
            // Anywhere else = drag
            return HTCAPTION;
        }

        case WM_MOVE:
        case WM_SIZE: {
            // Report position/size back to the application
            if (g_appInstance) {
                RECT rect;
                GetWindowRect(hwnd, &rect);
                g_appInstance->onOverlayMoved(rect.left, rect.top,
                    rect.right - rect.left, rect.bottom - rect.top);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

// Whether the second reader has already looked at this row and found no
// signature in it, lately. The HUD sways a pixel or two and the row is cut
// a little differently every scan, so "the same row" is one about the same
// place and about the same width.
bool Application::lineLookedAt(const cv::Rect& where) {
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = lineLooked_.size(); i-- > 0;) {
        if (std::chrono::duration<double>(now - lineLooked_[i].when).count() > LINE_LOOK_AGAIN_S) {
            lineLooked_.erase(lineLooked_.begin() + (std::ptrdiff_t)i);
        }
    }
    for (const LineLooked& seen : lineLooked_) {
        const int slackX = (std::max)(6, where.width / 3);
        const int slackY = (std::max)(4, where.height / 2);
        if (std::abs(seen.where.x - where.x) <= slackX && std::abs(seen.where.y - where.y) <= slackY &&
            std::abs(seen.where.width - where.width) <= slackX) {
            return true;
        }
    }
    return false;
}

void Application::rememberLineLook(const cv::Rect& where) {
    lineLooked_.push_back({where, std::chrono::steady_clock::now()});
    while (lineLooked_.size() > LINE_LOOKED_MOST) lineLooked_.erase(lineLooked_.begin());
}

void Application::onOverlayMoved(int x, int y, int w, int h) {
    signatureRegion_.x = x;
    signatureRegion_.y = y;
    signatureRegion_.width = w;
    signatureRegion_.height = h;

    // Update the GUI
    if (gui_) {
        gui_->setRegionValues(x, y, w, h);
    }

    // Persist the region
    saveRegionConfig();
}

void Application::createOverlayWindow(HINSTANCE hInstance) {
    // Register the window class
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "SCMinerOverlay";
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
    RegisterClassExA(&wc);

    // Create the window - WITHOUT WS_EX_TRANSPARENT so mouse clicks work
    overlayHwnd_ = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "SCMinerOverlay",
        "Overlay",
        WS_POPUP,
        signatureRegion_.x, signatureRegion_.y,
        signatureRegion_.width, signatureRegion_.height,
        nullptr, nullptr, hInstance, nullptr
    );

    if (overlayHwnd_) {
        // Color-key transparency - black becomes see-through
        SetLayeredWindowAttributes(overlayHwnd_, RGB(0, 0, 0), 0, LWA_COLORKEY);
        ShowWindow(overlayHwnd_, SW_SHOW);
    }
}

void Application::updateOverlayPosition() {
    if (overlayHwnd_) {
        SetWindowPos(overlayHwnd_, HWND_TOPMOST,
            signatureRegion_.x, signatureRegion_.y,
            signatureRegion_.width, signatureRegion_.height,
            SWP_SHOWWINDOW);
        InvalidateRect(overlayHwnd_, nullptr, TRUE);
    }
}

void Application::destroyOverlay() {
    if (overlayHwnd_) {
        DestroyWindow(overlayHwnd_);
        overlayHwnd_ = nullptr;
    }
}

Application::Application() {
    g_appInstance = this;
}

Application::~Application() {
    stop();
    destroyOverlay();
    g_appInstance = nullptr;
}

bool Application::initialize() {
    // Load the saved capture region first
    loadRegionConfig();
    g_debugImages = recordScans_;

    // How much of the machine one scan may take at once. Left alone,
    // OpenCV spreads across every core there is, and measured on a
    // sixteen-core machine that came to nearly eight cores held down for
    // as long as the program ran - beside a game that wants those cores
    // itself. A quarter of the cores, never fewer than two, never more
    // than six: the scan takes a little longer and the game keeps its
    // frame times. On a four-core machine this is the difference between
    // leaving it two cores and leaving it none.
    {
        const int cores = (std::max)(1, (int)std::thread::hardware_concurrency());
        const int want = scanThreads_ > 0 ? scanThreads_
                                          : std::clamp(cores / 4, 2, 6);
        cv::setNumThreads(want);
        std::cout << "[cpu] " << cores << " cores, scan uses " << want << std::endl;
    }
    refreshAutoAim(true);
    refreshHudScale();

    // The blueprint library: what was ticked and read so far, then the
    // game's log - its backups are read one per tick from here on
    library_.load();
    library_.setGameLog(locateGameLog());

    // Ask once, in the background, whether there is a newer version.
    // Nothing is downloaded by asking and nothing is installed without
    // being asked; update=0 in region_config.txt stops it entirely.
    updater_.configure(updateUrl_.empty() ? BEDROCK_UPDATE_URL : updateUrl_, updateCheck_);
    updater_.checkInBackground();
    lastUpdateCheck_ = std::chrono::steady_clock::now();

    std::cout << "=========================================" << std::endl;
    std::cout << "  BEDROCK" << std::endl;
    std::cout << "  Mining Companion for Star Citizen" << std::endl;
    std::cout << "=========================================\n" << std::endl;

    // Initialize the OCR engine
    OCRConfig ocrConfig;
    ocrConfig.tessdataPath = "./tessdata";
    ocrConfig.language = "eng";
    ocrConfig.pageSegMode = 8;      // PSM 8 = single word (best for lone numbers)
    ocrConfig.minConfidence = 20.0; // low threshold, we add a consistency check on top

#ifdef BEDROCK_TESSERACT
    std::cout << "Initializing Tesseract OCR..." << std::endl;
    std::cout << "tessdata path: " << ocrConfig.tessdataPath << std::endl;
#endif

    // Digit templates for the OCR-free reader, harvested from real captures
    const int glyphTemplates = glyphReader_.loadTemplates("./glyphs");
    std::cout << "Loaded " << glyphTemplates << " glyph templates." << std::endl;

    // Tesseract is opt-in (SCMINER_OCR=1) and its data is not shipped; without
    // it the app simply has no OCR, which is the normal state
    ocrAvailable_ = ocrEngine_.initialize(ocrConfig);
#ifdef BEDROCK_TESSERACT
    if (!ocrAvailable_) std::cout << "OCR: no tessdata beside the exe - off (the reader does not need it)" << std::endl;
#endif
    std::cout << (glyphReader_.usesEmbeddedTemplates() ? "Templates: built in" : "Templates: from ./glyphs") << std::endl;

    // The second reader. Without it the scan runs on the glyph reader alone;
    // SCMINER_NO_TEXTLINE=1 asks for that on purpose.
    if (!DEV_ENV("SCMINER_NO_TEXTLINE")) {
        const auto lineStart = std::chrono::steady_clock::now();
        const bool lines = textLines_.loadEmbedded();
        std::cout << "Second reader: " << (lines ? "ready" : "could not be loaded - the glyph reader reads alone")
                  << " (" << static_cast<int>(std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - lineStart).count())
                  << " ms)" << std::endl;
    }

    analyzer_.setSystemFilter(currentSystem_);
    writeSessionInfo();

    std::cout << "\nControls:" << std::endl;
    std::cout << "  L        - toggle live mode" << std::endl;
    std::cout << "  S / F9   - single scan" << std::endl;
    std::cout << "  M        - manual signature input (console)" << std::endl;
    std::cout << "  R        - re-detect the capture region" << std::endl;
    std::cout << "  +/-      - adjust the scan interval" << std::endl;
    std::cout << "  Q/ESC    - quit" << std::endl;
    std::cout << "  Ctrl+Alt+S (anywhere) - snapshot of the game picture to debug/" << std::endl;
    std::cout << "\n=========================================\n" << std::endl;

    return true;
}

void Application::run() {
    running_ = true;

    std::cout << "Press 'L' for live mode or 'M' for manual input...\n" << std::endl;

    lastScanTime_ = std::chrono::steady_clock::now();

    while (running_) {
        // Poll keyboard input
        if (_kbhit()) {
            int key = _getch();

            switch (key) {
                case 'l':
                case 'L':
                    toggleLiveMode();
                    break;

                case 'm':
                case 'M': {
                    // Pause live mode for manual input
                    bool wasLive = liveModeActive_;
                    liveModeActive_ = false;

                    std::cout << "\nEnter signature value: ";
                    int manualValue = 0;
                    if (!(std::cin >> manualValue)) {
                        std::cin.clear();
                        std::cin.ignore(1024, '\n');
                        manualValue = 0;
                    }

                    if (manualValue > 0) {
                        std::string report = analyzer_.generateReport(manualValue);
                        std::cout << report << std::endl;
                    }

                    // Resume live mode if it was active
                    if (wasLive) {
                        liveModeActive_ = true;
                        std::cout << "\n[LIVE] mode resumed..." << std::endl;
                    }
                    break;
                }

                case 's':
                case 'S':
                    triggerScan();
                    break;

                case 'r':
                case 'R':
                    regionDetected_ = false;
                    lastRecognizedSignature_ = 0;
                    std::cout << "\nThe region will be re-detected on the next scan..." << std::endl;
                    break;

                case '+':
                case '=':
                    captureIntervalMs_ = (std::max)(100, captureIntervalMs_ - 100);
                    std::cout << "\nScan interval: " << captureIntervalMs_ << "ms" << std::endl;
                    break;

                case '-':
                case '_':
                    captureIntervalMs_ = (std::min)(3000, captureIntervalMs_ + 100);
                    std::cout << "\nScan interval: " << captureIntervalMs_ << "ms" << std::endl;
                    break;

                case 'q':
                case 'Q':
                case 27:  // ESC
                    running_ = false;
                    break;

                case 0:
                case 224: {
                    // Function keys (extended key code)
                    int fKey = _getch();
                    if (fKey == 67) {  // F9
                        triggerScan();
                    }
                    break;
                }

                default:
                    break;
            }
        }

        // Live mode: automatic scan at a fixed interval
        if (liveModeActive_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScanTime_).count();

            if (elapsed >= captureIntervalMs_) {
                lastScanTime_ = now;
                processScanLive();
            }
        }

        // Manual scan if requested
        if (scanRequested_) {
            scanRequested_ = false;
            processScan();
        }

        // Short sleep to spare the CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "\nApplication closed." << std::endl;
}

void Application::runWithGUI(HINSTANCE hInstance) {
    running_ = true;

    // Below normal priority: a reading waits for a core the game leaves free
    // rather than taking one from it - the second reader keeps several busy
    // for most of a second, enough to make the game stutter. The scan is light
    // and gets what it needs; on a full machine it only comes a moment later.
#ifdef _WIN32
    if (!DEV_ENV("SCMINER_NORMAL_PRIORITY")) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif

    // Create the GUI
    gui_ = std::make_unique<MinerGUI>();
    if (!gui_->initialize(hInstance)) {
        std::cerr << "Failed to create the GUI!" << std::endl;
        MessageBoxA(nullptr, "Failed to create the GUI!", "Bedrock", MB_ICONERROR);
        return;
    }

    // The capture region overlay, only when asked for (overlay=1); every
    // use of overlayHwnd_ below is guarded, so without it nothing shows
    if (overlayEnabled_) createOverlayWindow(hInstance);

    // System selection callback
    gui_->setSystemChangedCallback([this](const std::string& system) {
        currentSystem_ = system;
        analyzer_.setSystemFilter(system);
        // The wider choice, made on purpose: the place filter steps back
        // until the log names a new place (see applyGameState)
        if (!analyzer_.placeFilter().empty()) {
            analyzer_.setPlaceFilter("");
            if (gui_) gui_->setPlace(placeLine_, "");
        }
        std::cout << "System switched to: " << system << std::endl;

        // Re-run the analysis for the current signature with the new filter
        if (lastRecognizedSignature_ > 0) {
            auto results = analyzer_.analyzeSignature(lastRecognizedSignature_);
            gui_->updateResults(lastRecognizedSignature_, results, -1.0, analyzer_.outOfPlace());
        }
    });

    // Live mode callback
    gui_->setPlaceOverrideCallback([this](const std::string& place) { setPlaceOverride(place); });
    // The Blueprints window's box that puts the marks into the game's texts
    gui_->setGameTexts(gameTextsWanted_, gameTextsNote_);
    // Reporting to UEX: the player's key, where they gave one, and what the
    // window says of it
    uexKey_ = UexKey::load();
    gui_->setUexAccountCallback([this](int action, const std::string& text) { uexAccountAction(action, text); });
    pushUexAccount();
    gui_->setGameTextsCallback([this](bool on) { setGameTextsWanted(on); });
    // The Refinery window's box that shares refinery workloads
    gui_->setRefineryShare(refineryShare_, !refineryServiceUrl().empty());
    gui_->setRefineryShareCallback([this](bool on) { setRefineryShare(on); });
    loadPlaceOverride();
    gui_->setLiveModeCallback([this](bool active) { applyLiveMode(active); });
    // The update strip: one click fetches, the next installs. Nothing
    // happens without one, and the app only quits once the script that
    // replaces it has actually started.
    gui_->setUpdateActionCallback([this] {
        switch (updater_.state()) {
            case Updater::State::Available:
                updater_.downloadInBackground();
                break;
            case Updater::State::Ready:
                if (updater_.apply() && gui_) PostMessage(gui_->windowHandle(), WM_CLOSE, 0, 0);
                break;
            case Updater::State::Downloading:
            case Updater::State::Checking:
                break;      // already busy; let it finish
            default:
                updater_.checkInBackground();   // ask again, now
                break;
        }
    });

    // Manual signature input callback
    gui_->setManualAnalyzeCallback([this](int signature) {
        std::cout << "Manual signature: " << signature << std::endl;
        lastRecognizedSignature_ = signature;
        auto results = analyzer_.analyzeSignature(signature);
        gui_->updateResults(signature, results, -1.0, analyzer_.outOfPlace());
        displayResultsCompact(results, signature);
    });

    // Single scan callback (SCAN button / S key)
    gui_->setScanRequestedCallback([this]() {
        triggerScan();
    });

    // Auto-tracking toggle
    gui_->setTrackModeCallback([this](bool enabled) {
        setAutoTrack(enabled);
    });

    // The zoom, if this machine has been told one before
    if (uiScalePercent_ > 0) gui_->setUiScale(uiScalePercent_);
    gui_->setUiScaleCallback([this](int percent) {
        uiScalePercent_ = percent;
        saveRegionConfig();
    });

    // A rank picked in the Grind Planner, or the logs' word taken again
    gui_->setRankSetCallback([this](int faction, int ladder, int rank) {
        library_.setRank(faction, ladder, rank);
        pushRanks();
    });

    // A blueprint ticked or unticked by hand in the Blueprints window
    gui_->setOwnedToggleCallback([this](int blueprint, bool owned) {
        library_.setByHand(blueprint, owned);
        pushLibrary();
        // ...and into the game's texts, a moment later: ticks come in runs
        gameTextsDue_ = true;
        gameTextsDueAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    });

    // A backup put back in the Your data card. What it wrote is the truth
    // from here on: the library is read again at once - left alone, its next
    // save would put the one in memory back over it - and the settings file
    // stays as the backup has it until the next start reads it. It is
    // written on every close, so without the hold it would never be read.
    gui_->setDataRestoredCallback([this]() {
        library_.load();
        const std::filesystem::path log = library_.gameLog();
        library_.setGameLog(log);
        pushLibrary();
        gameTextsDue_ = true;
        gameTextsDueAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        settingsHeld_ = true;
    });

    // Ctrl+Alt+K, system-wide: keep the last few minutes of recording -
    // only while recording is on at all; otherwise the key stays free
    if (recordScans_) {
        gui_->setKeepRecordingCallback([this]() {
            keepRecording();
        });
    } else {
        gui_->releaseKeepHotkey();
    }
    // Ctrl+Alt+S, system-wide, always: the picture as the scan sees it
    gui_->setSnapshotCallback([this]() { requestSnapshot(); });

    // The scan pumps the window's messages at every expensive step, so the
    // window stays draggable while live mode works; otherwise a scan holds the
    // thread, and the title bar with it, for 0.6-2.7 s.
    auto breathe = [this]() { if (gui_ && inScan_) gui_->processMessages(); };
    screenCapture_.setYield(breathe);
    ocrEngine_.setYield(breathe);

    // Capture region callback - applies the region and runs a test scan
    // AUTO: back to the box derived from the game's viewport
    gui_->setRegionAutoCallback([this]() {
        aimAuto_ = true;
        aimViewport_ = cv::Rect();
        if (!refreshAutoAim(true) && gui_) gui_->setRegionAuto(true);
        saveRegionConfig();
        updateOverlayPosition();
    });

    gui_->setRegionChangedCallback([this](int x, int y, int w, int h) {
        signatureRegion_.x = x;
        signatureRegion_.y = y;
        signatureRegion_.width = w;
        signatureRegion_.height = h;
        // Typed by hand: the box stays where it was put
        aimAuto_ = false;
        aimViewport_ = cv::Rect();
        if (gui_) gui_->setRegionAuto(false);
        saveRegionConfig();
        std::cout << "Region set: " << x << "," << y << " (" << w << "x" << h << ")" << std::endl;

        // Hide the overlay so its frame is not captured
        if (overlayHwnd_) {
            ShowWindow(overlayHwnd_, SW_HIDE);
            Sleep(100);
        }

        hasLastLabel_ = false;   // the aim point moved, restart tracking
        cv::Rect takenFrom;
        cv::Mat signatureImage = captureSignatureImage(&takenFrom);

        if (!signatureImage.empty()) {
            saveDebugCapture("region_apply.png", signatureImage);
            if (autoTrack_) {
                std::cout << (lastScanTracked_
                                  ? "[label found at " + std::to_string(takenFrom.x) + "," +
                                        std::to_string(takenFrom.y) + "]"
                                  : "[no label found - using the fixed region]")
                          << std::endl;
            }

            auto candidates = screenCapture_.preprocessCandidates(signatureImage);
            auto recognizedValue = ocrEngine_.recognizeBest(
                candidates, readingPolicy());

            if (recognizedValue) {
                missStreak_ = 0;

    int scannedValue = *recognizedValue;   // already repaired by the reading policy
                std::cout << "[OCR recognized: " << scannedValue << "]" << std::endl;
                lastRecognizedSignature_ = scannedValue;
                auto results = analyzer_.analyzeSignature(scannedValue);
                gui_->updateResults(scannedValue, results, ocrEngine_.getLastConfidence(), analyzer_.outOfPlace());
                displayResultsCompact(results, scannedValue);
            } else {
                std::cout << "[OCR failed - check debug/region_apply.png]" << std::endl;
            }
        }

        // Show the overlay again for further adjustments
        if (overlayHwnd_) {
            ShowWindow(overlayHwnd_, SW_SHOW);
        }

        saveRegionConfig();
    });

    // Hand the loaded region values (from initialize()) to the GUI
    gui_->setRegionValues(signatureRegion_.x, signatureRegion_.y,
                          signatureRegion_.width, signatureRegion_.height);
    gui_->setRegionAuto(aimAuto_);
    gui_->setTrackingStatus(autoTrack_, false, signatureRegion_.x, signatureRegion_.y);
    updateOverlayPosition();

    // The work orders read off refinery terminals before, and the window's
    // "Collected" taking one off the list
    refineryOrders_.load(AppPaths::file(REFINERY_ORDERS_FILE));
    gui_->setRefineryOrderDoneCallback([this](const RefineryOrder& done) {
        const auto& all = refineryOrders_.orders();
        for (size_t i = 0; i < all.size(); i++) {
            if (all[i].station == done.station && all[i].number == done.number && all[i].due == done.due) {
                refineryOrders_.remove(i);
                refineryOrders_.save(AppPaths::file(REFINERY_ORDERS_FILE));
                std::cout << "\n[refinery] " << done.station << ": work order " << done.number << " collected" << std::endl;
                break;
            }
        }
        pushRefineryOrders();
    });
    pushRefineryOrders();

    gui_->show();

    // ...and, once, what changed since the version they had
    maybeShowWhatsNew();
    // UEX, offered once: at the first start, when nobody has a key in yet.
    // It waits for the window to be free. A build without Bedrock's app token
    // can do nothing with an account.
    if (uexWanted_ && !uexOffered_ && uexKey_.empty() && !uexToken().empty()) gui_->offerUex();

    // Come back scanning if that is how the app was left. An instance that
    // starts idle looks exactly like one that is working, and the difference
    // only surfaces much later - in a saved recording that turns out to be
    // minutes old, or in readings that never arrive.
    if (startLive_) {
        applyLiveMode(true);
        gui_->setLiveModeActive(true);
    }

    lastScanTime_ = std::chrono::steady_clock::now();

    // Main loop with GUI
    while (running_ && gui_->isOpen()) {
        // Pump Windows messages
        if (!gui_->processMessages()) {
            break;
        }

        // Poll console input (when a console is attached)
        if (_kbhit()) {
            int key = _getch();

            switch (key) {
                case 'l':
                case 'L':
                    toggleLiveMode();
                    gui_->setLiveModeActive(liveModeActive_);
                    break;

                case 's':
                case 'S':
                    triggerScan();
                    break;

                case 'r':
                case 'R':
                    regionDetected_ = false;
                    lastRecognizedSignature_ = 0;
                    std::cout << "The region will be re-detected..." << std::endl;
                    break;

                case 'q':
                case 'Q':
                case 27:
                    running_ = false;
                    break;
            }
        }

        // The blueprint library follows the game's log
        pollLibrary();
        // ...and a refinery's terminal, at a station that has one
        lookForRefinery();
        handRefineryOrdersToUex();
        serviceRefineryShare();

        // Live mode
        if (liveModeActive_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScanTime_).count();

            const int wait = effectiveIntervalMs();
            if (elapsed >= wait) {
                g_intervalMs = (double)wait;   // logged, so it can be checked
                lastScanTime_ = now;
                processScanWithGUI();
            }
        }

        // Single scan
        if (scanRequested_) {
            scanRequested_ = false;
            processScanWithGUI();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Application closed." << std::endl;
}

// How long to wait before the next scan. The base is what the player
// set; on top of it comes what the situation allows.
//
// One thing stretches it, and the very next find undoes it:
//
//  - The game is not the window in front. Then there is nothing on
//    screen to read at all and the scan is pure heat.
//
// The ceiling is deliberately low. Two seconds saves nearly everything
// there is to save; ten would save a little more and would now and then
// lose the first sight of a signature, which is the one thing this
// program is for. And nothing here returns zero scans: the slowest
// setting still looks four times a minute, so a wrong guess about what
// the player is doing shows up as latency, never as a dead program.
// Whether the window in front lies over the game. Not when it is the game,
// one of this app's own windows, or the desktop and the taskbar behind
// everything - and not when it is on another screen: the game is in full
// view there and still being played, and a chat open on a second monitor
// must not slow the scan down.
static bool gameCovered(HWND game) {
#ifdef _WIN32
    HWND front = GetForegroundWindow();
    if (!front || front == game) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(front, &process);
    if (process == GetCurrentProcessId()) return false;
    char cls[64] = {};
    GetClassNameA(front, cls, sizeof(cls));
    for (const char* shell : {"Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd"}) {
        if (std::strcmp(cls, shell) == 0) return false;
    }
    if (IsIconic(front) || !IsWindowVisible(front)) return false;
    RECT f{}, g{}, both{};
    if (!GetWindowRect(front, &f) || !GetWindowRect(game, &g)) return true;
    if (!IntersectRect(&both, &f, &g)) return false;
    const long long overlap = (long long)(both.right - both.left) * (both.bottom - both.top);
    const long long whole = (long long)(g.right - g.left) * (g.bottom - g.top);
    return whole > 0 && overlap > whole / 20;   // more than a twentieth of it
#else
    (void)game;
    return false;
#endif
}

int Application::effectiveIntervalMs() const {
    if (!easeIdle_) return captureIntervalMs_;
    const double quiet = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_lastFind).count();

    // Is the game in view? Only meaningful when one was actually FOUND:
    // with none, this says nothing about whether the game is running, and
    // guessing from silence is how a scanner talks itself into never
    // scanning. In view unless another window lies over it (gameCovered) -
    // not merely "in front", which a window on another screen is too.
    const HWND game = ScreenCapture::gameWindow();
    const bool watching = !game || !gameCovered(game);

    // Cost per scan on six threads: a sweep of the whole search area ~740 ms
    // of CPU, the area around the crosshair ~155 ms. With near sweeps doing
    // most of the work, 300 ms between quiet scans costs about 3/4 of a core
    // and sees a new marker within ~1/3 s. So while the game is in view a
    // quiet stretch does not slow the scan at all: a deeper step would only
    // add a wait after every rock.
    int ms = captureIntervalMs_;

    // Deeper than a second only when the game is covered.
    // A signature sometimes flashes up for a moment and is gone - that is
    // what accepted-flash is for, and it is a real reading. While the game
    // is in front, one second is as slow as this gets, so a flash is still
    // caught; once the window is behind something else there is nothing on
    // screen to flash at all, and two seconds is free.
    if (!watching) ms = (std::max)(ms, quiet > 60.0 ? 2000 : 1000);

    return (std::max)(ms, captureIntervalMs_);
}

// The middle reading of the recent ones, figure by figure.
//
// A median rather than an average: OCR fails by reading a whole digit wrong,
// so a bad reading is not a little off but a lot, and an average carries it
// into the answer where a median throws it away. The ore's name is not a
// number, so it takes the one seen most often.
RockStats Application::steadyRock(const std::deque<RockStats>& readings) {
    RockStats out;
    if (readings.empty()) return out;

    auto middle = [&](float RockStats::*field) {
        std::vector<float> seen;
        for (const RockStats& r : readings) {
            if (r.*field > 0) seen.push_back(r.*field);
        }
        if (seen.empty()) return 0.0f;
        std::sort(seen.begin(), seen.end());
        return seen[seen.size() / 2];
    };
    out.mass = middle(&RockStats::mass);
    out.instability = middle(&RockStats::instability);
    out.scu = middle(&RockStats::scu);
    // Resistance is the one figure that is legitimately zero - plenty of
    // rocks have none - so it cannot be filtered on "greater than zero". It
    // CAN be missing, though, and a reader that failed hands back -1: that is
    // not a reading and must not be averaged with ones that are, or shown as
    // "-1 %".
    //
    // And a reading from the cockpit and one from the turret are two figures
    // for one rock - 75 % white and 52 % green, or red - whose middle is
    // neither. The kind seen more often wins, the newer one on a tie, and only
    // its own readings go into the middle.
    {
        int lowered = 0, plain = 0;
        for (const RockStats& r : readings) {
            if (r.resistance < 0) continue;
            if (r.resistanceAdjusted) lowered++; else plain++;
        }
        bool useLowered = lowered > plain;
        if (lowered == plain && lowered > 0) {
            for (auto it = readings.rbegin(); it != readings.rend(); ++it) {
                if (it->resistance >= 0) { useLowered = it->resistanceAdjusted; break; }
            }
        }
        std::vector<float> seen;
        for (const RockStats& r : readings) {
            if (r.resistance >= 0 && r.resistanceAdjusted == useLowered) {
                seen.push_back(r.resistance);
            }
        }
        std::sort(seen.begin(), seen.end());
        out.resistance = seen.empty() ? -1.0f : seen[seen.size() / 2];
        out.resistanceAdjusted = useLowered && !seen.empty();
    }
    {
        std::map<std::string, int> votes;
        for (const RockStats& r : readings) {
            if (!r.ore.empty()) votes[r.ore]++;
        }
        int best = 0;
        for (const auto& v : votes) {
            if (v.second > best) { best = v.second; out.ore = v.first; }
        }
    }
    return out;
}

// The rock in front of the ship, if the game is showing its figures: this
// is what makes the window switch to the rock view.
//
// On its own clock, and a slow one. The panel slides with the rock, so the
// search has to cover the whole viewport (5120x1440 on a 32:9 screen)
// where the signature reader only ever looks at a few hundred pixels. Once
// every second and a half is cheap enough not to be felt, and a glance at
// the place the last panel stood goes with every scan, so that a rock is
// not waited for.
// One constant rather than a setting, so that the reader, the search and
// the harvest tools stay testable with it either way.
bool Application::rockViewWanted() {
    return true;
}

// What the reader made of a panel, one line per look, for laying against
// a recording of the same minutes. The figures of this one look sit
// beside what the median over the last five made of them: a misread shows up
// in the first set, and whether the smoothing swallowed it or passed it on
// shows up in the second.
//
// Local time to the millisecond, because a video's file name says when it
// started and nothing finer. Written only while a panel is on screen, so
// flying about with nothing scanned writes nothing.
void Application::logRockReading(const ScanPanel& panel, const RockStats* steady) {
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    const std::string path = dataPath("debug") + "/rock_readings.csv";
    const bool fresh = !std::filesystem::exists(path, ec);
    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) return;
    if (fresh) {
        out << "time,figures,ore,mass,res,res_printed,inst,scu,"
               "steady_mass,steady_res,steady_res_printed,steady_inst,steady_scu,"
               "box_x,box_y,box_w,box_h,note" << std::endl;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const int millis = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &secs);
    char when[40] = {};
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);

    // A name or a note may hold a comma; quoted, and a quote inside turned
    // into an apostrophe rather than escaped, so any spreadsheet opens it
    auto quoted = [](std::string text) {
        for (char& c : text) {
            if (c == '"') c = '\'';
            if (c == '\n' || c == '\r') c = ' ';
        }
        return "\"" + text + "\"";
    };
    auto colour = [](const RockStats& r) {
        return r.resistance < 0 ? "" : (r.resistanceAdjusted ? "colour" : "white");
    };

    const RockStats& read = panel.stats;
    char line[512];
    std::snprintf(line, sizeof(line), "%s.%03d,%d,%s,%.0f,%.0f,%s,%.2f,%.2f,",
                  when, millis, panel.figuresRead, quoted(read.ore).c_str(), read.mass,
                  read.resistance, colour(read), read.instability, read.scu);
    out << line;
    if (steady) {
        std::snprintf(line, sizeof(line), "%.0f,%.0f,%s,%.2f,%.2f,", steady->mass,
                      steady->resistance, colour(*steady), steady->instability, steady->scu);
        out << line;
    } else {
        out << ",,,,,";
    }
    out << panel.box.x << "," << panel.box.y << "," << panel.box.width << ","
        << panel.box.height << "," << quoted(panel.note) << std::endl;
}

// Every look at the screen for a rock, read or not, with what it cost and
// when it came. rock_readings.csv holds only the looks that read something,
// and what makes a rock come in late is the looks in between.
static void logRockLook(double lookMs, double sinceLastMs, int waitMs, bool panelUp, bool quick, int misses,
                        double glance, const ScanPanel& panel) {
#ifndef BEDROCK_DEV_TOOLS
    // The notes in this log say in words how the reader works. A release
    // keeps the figures of a scan (scan_log.csv) and not this.
    (void)lookMs; (void)sinceLastMs; (void)waitMs; (void)panelUp; (void)quick; (void)misses; (void)glance; (void)panel;
    return;
#endif
    static std::ofstream out;
    static bool opened = false;
    if (!opened) {
        opened = true;
        std::error_code ec;
        std::filesystem::create_directories(dataPath("debug"), ec);
        const std::string path = dataPath("debug") + "/rock_looks.csv";
        const char* HEADER =
            "time,look_ms,since_last_ms,wait_ms,panel_up,quick,misses,found,figures,labels_x,labels_y,scale,ink,glance,note";
        // a long session's worth, then it starts over - and a log with other
        // columns in it starts over too
        bool stale = std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 20u * 1024u * 1024u;
        if (!stale && std::filesystem::exists(path, ec)) {
            std::ifstream in(path);
            std::string first;
            std::getline(in, first);
            while (!first.empty() && (first.back() == '\r' || first.back() == '\n')) first.pop_back();
            stale = first != HEADER;
        }
        if (stale) std::filesystem::rename(path, dataPath("debug") + "/rock_looks.old.csv", ec);
        const bool fresh = !std::filesystem::exists(path, ec);
        out.open(path, std::ios::app);
        if (fresh && out.is_open()) out << HEADER << std::endl;
    }
    if (!out.is_open()) return;
    const auto clock = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(clock);
    const int millis = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(clock.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &secs);
    char when[16] = {};
    std::strftime(when, sizeof(when), "%H:%M:%S", &local);
    std::string note = panel.note.substr(0, 110);
    for (char& c : note) {
        if (c == '"') c = '\'';
        if (c == '\n' || c == '\r') c = ' ';
    }
    char line[220];
    std::snprintf(line, sizeof(line), "%s.%03d,%.0f,%.0f,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%d,%.2f,", when, millis, lookMs,
                  (std::min)(sinceLastMs, 99999.0), waitMs, panelUp ? 1 : 0, quick ? 1 : 0, misses, panel.found ? 1 : 0,
                  panel.figuresRead, panel.labels.x, panel.labels.y, panel.labelScale, panel.ink, glance);
    out << line << '"' << note << '"' << std::endl;
}

// What happens to a rock's list, step by step: a reading started, what it
// came back with and whether it was taken, the list shown, the list taken off
// and why. The other two logs say when a rock was looked at and what its
// figures read; this one says why its list came late or not at all.
static void logRockList(const char* event, float jobMass, float rockMass, const RockContents* read, int tries,
                        const std::string& why) {
#ifndef BEDROCK_DEV_TOOLS
    (void)event; (void)jobMass; (void)rockMass; (void)read; (void)tries; (void)why;
    return;
#endif
    static std::ofstream out;
    static bool opened = false;
    if (!opened) {
        opened = true;
        std::error_code ec;
        std::filesystem::create_directories(dataPath("debug"), ec);
        const std::string path = dataPath("debug") + "/rock_lists.csv";
        const char* HEADER = "time,event,job_mass,rock_mass,whole,lines,sum,ms,tries,note";
        bool stale = std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 20u * 1024u * 1024u;
        if (!stale && std::filesystem::exists(path, ec)) {
            std::ifstream in(path);
            std::string first;
            std::getline(in, first);
            while (!first.empty() && (first.back() == '\r' || first.back() == '\n')) first.pop_back();
            stale = first != HEADER;
        }
        if (stale) std::filesystem::rename(path, dataPath("debug") + "/rock_lists.old.csv", ec);
        const bool fresh = !std::filesystem::exists(path, ec);
        out.open(path, std::ios::app);
        if (fresh && out.is_open()) out << HEADER << std::endl;
    }
    if (!out.is_open()) return;
    const auto clock = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(clock);
    const int millis = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(clock.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &secs);
    char when[16] = {};
    std::strftime(when, sizeof(when), "%H:%M:%S", &local);
    std::string note = why + (read ? (why.empty() ? "" : "; ") + read->note : std::string());
    note = note.substr(0, 160);
    for (char& c : note) {
        if (c == '"') c = '\'';
        if (c == '\n' || c == '\r') c = ' ';
    }
    char line[200];
    std::snprintf(line, sizeof(line), "%s.%03d,%s,%.0f,%.0f,%d,%d,%.2f,%.0f,%d,", when, millis, event, jobMass, rockMass,
                  read ? (read->whole ? 1 : 0) : -1, read ? (int)read->parts.size() : -1, read ? read->sum : -1.0f,
                  read ? read->ms : -1.0, tries);
    out << line << '"' << note << '"' << std::endl;
}

// The ship the log names mines - or no ship is named yet, which says nothing
// either way. The three Bedrock reads, and whatever else carries a mining
// head: its scan panel comes up there too, even where the view cannot read
// its HUD yet. Standing beside the ship, or in one of a MOLE's turrets, the
// ship is still the one last boarded.
bool Application::shipCanMine() const {
    std::string ship = gameState_.ship();
    if (ship.empty()) return true;
    for (auto& c : ship) c = (char)std::tolower((unsigned char)c);
    for (const char* miner : {"prospector", "mole", "golem", "roc", "arrastra", "orion"}) {
        if (ship.find(miner) != std::string::npos) return true;
    }
    return false;
}

void Application::lookForRock() {
    if (!gui_ || !rockViewWanted()) return;
    const auto now = std::chrono::steady_clock::now();
    // While a panel is up its figures settle over several looks, so it is
    // looked at twice as often then; with none on screen, every second and a
    // half is enough to notice one come up. A panel whose labels are up and
    // whose figures are not - the zeros while the scan runs, or a look caught
    // while the HUD swayed - is looked at again on the next scan: a rock's
    // figures can stand for only two seconds after four of zeros, and the
    // regular looks can miss them altogether.
    // A panel the face reader read counts as up too: in the MOLE's turrets
    // that is the only reader of the figures, and at a look every second and
    // a half the rock comes in late and goes out late.
    const bool panelUp = rockMisses_ < 4 && (rockSteadier_.looks() > 0 || !rockReadings_.empty());
    // Only where a rock panel can come up: in a ship that mines, or before the
    // log has named a ship at all. A full look with nothing on screen costs
    // 216 ms of the whole viewport every second and a half - for nothing in a
    // ship that cannot mine.
    if (!panelUp && !shipCanMine()) return;
    // ...three times running at most: labels whose figures cannot be read at
    // all would otherwise be looked at on every scan, and the signatures wait
    const bool quick = rockLabelsOnly_ && rockQuickLooks_ < 3;
    // A panel found whose figures have not settled is looked at again on the
    // very next scan, four times at most: the steadier wants a second look
    // that agrees, and at 700 ms between looks that second look comes 0.9 to
    // 1.0 s after the first.
    const bool settling = rockSettling_ && rockSettlingLooks_ < 4;
    int waitMs = quick || settling ? 0 : (panelUp ? 700 : 1500);
    // The glance: between two full looks, with nothing on screen, the place
    // the last panel stood is captured on its own - a few hundred pixels
    // each way - and looked at for that one column at that one size. A few
    // milliseconds; the full look with nothing on screen costs a tenth of a
    // second, which is why it waits a second and a half, and that wait is
    // most of the time to a rock (1.8 s between looks in the median). When
    // the column is there the full look comes now; when that finds nothing
    // the glance rests for three scans, so a cockpit that looks like a
    // column at that spot costs no more than the plain wait.
    double glance = -1;
    if (!panelUp && !quick && now - lastRockLook_ < std::chrono::milliseconds(waitMs)) {
        if (glanceQuiet_ > 0) {
            glanceQuiet_--;
        } else if (const PanelSpot spot = lastPanelSpot(); spot.known) {
            if (auto viewport = ScreenCapture::gameViewport()) {
                const cv::Rect window = panelSpotWindow(spot, cv::Size(viewport->width, viewport->height));
                if (window.width > 20 && window.height > 20) {
                    CaptureRegion part;
                    part.x = viewport->x + window.x;
                    part.y = viewport->y + window.y;
                    part.width = window.width;
                    part.height = window.height;
                    part.autoDetect = false;
                    glance = glanceAtPanelSpot(spot, screenCapture_.captureRegion(part));
                    if (glance >= glanceBar(spot)) {
                        waitMs = 0;
                        glanceQuiet_ = 3;
                        preferPanelInk(spot.ink);
                    }
                }
            }
        }
    }
    if (now - lastRockLook_ < std::chrono::milliseconds(waitMs)) return;
    const double sinceLastMs = std::chrono::duration<double, std::milli>(now - lastRockLook_).count();
    lastRockLook_ = now;

    auto viewport = ScreenCapture::gameViewport();
    if (!viewport) return;          // the game is not up; leave the view alone

    CaptureRegion whole;
    whole.x = viewport->x;
    whole.y = viewport->y;
    whole.width = viewport->width;
    whole.height = viewport->height;
    whole.autoDetect = false;
    const cv::Mat frame = screenCapture_.captureRegion(whole);
    if (frame.empty()) return;

    if (!rockOcrTried_) {
        rockOcrTried_ = true;
        OCRConfig config;
        rockOcrReady_ = rockOcr_.initialize(config);
        throttlePanelSearch(true);
        loadPanelSpot(cv::Size(viewport->width, viewport->height));
        std::cout << "[rock] text reader: " << (rockOcrReady_ ? "on" : "off - no tessdata beside the exe; "
                     "the figures are read by their shapes, the ore's name is not read") << std::endl;
    }
    // NOT a condition for looking. The shapes read the panel's figures on
    // their own; Tesseract only names the ore and backs the figures up where
    // it is there, and an engine that did not start answers every question
    // with nothing. Packages carry no tessdata: returning here without one
    // blinds the rock view for every download while it still works beside a
    // build tree that has one.
    // ...and the second reader behind the shapes, for a HUD whose numerals
    // they were not cut for - the MOLE's turret (see readPanelByLines)
    const ScanPanel panel = readScanPanel(frame, rockOcr_, &glyphReader_, &textLines_);
    if (panel.found || panel.labels.x >= 0) lastRockPanel_ = std::chrono::steady_clock::now();
    if (panel.found) keepPanelSpot(cv::Size(viewport->width, viewport->height));
    readLoadoutOnHud(frame, panel);
    rockLabelsOnly_ = !panel.found && panel.labels.x >= 0;
    rockQuickLooks_ = quick ? rockQuickLooks_ + 1 : 0;
    rockSettlingLooks_ = settling ? rockSettlingLooks_ + 1 : 0;
    rockSettling_ = false;
    if (panel.found) glanceQuiet_ = 0;
    logRockLook(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count(), sinceLastMs,
                waitMs, panelUp, quick, rockMisses_, glance, panel);

    // The shape reader's looks go to the steadier, which says when the
    // figures are settled; until then the view keeps what it had
    if (panel.found && !panel.figures[0].likeness.empty()) {
        rockMisses_ = 0;
        rockReadings_.clear();
        RockStats rock;
        const bool settled = rockSteadier_.add(panel, rock);
        rockSettling_ = !settled;
        logRockReading(panel, settled ? &rock : nullptr);
        if (!settled) {
            // A panel that will not settle for twelve seconds is not the
            // rock the view shows any more
            if (++rockUnsettled_ >= 16) {
                rockUnsettled_ = 0;
                if (rockSeen_) {
                    rockSeen_ = false;
                    std::cout << "\n[rock] gone" << std::endl;
                }
                clearRockView();
                shownMass_ = 0;
            }
            return;
        }
        rockUnsettled_ = 0;
        gui_->updateRock(rock);
        loadoutCheckForRock(rock.mass);
        shownMass_ = rock.mass;
        shownOre_ = rock.ore;
        readContentsOnHud(frame, panel, rock);
        if (!rockSeen_) {
            rockSeen_ = true;
            std::cout << "\n[rock] " << (int)rock.mass << " kg, " << (int)rock.resistance
                      << " % resistance, instability " << rock.instability
                      << (rock.ore.empty() ? "" : "  - " + rock.ore) << std::endl;
        }
        return;
    }

    // The mass is the one figure nothing works without, so it is the test for
    // "this is a panel and it was read" rather than the box alone.
    // A panel that named at least three of its four figures is a panel, mass
    // or no mass on this particular look - see readScanPanel. Anything less
    // is a pair of rules somewhere in the cockpit, and taking that for a
    // panel leaves the view stuck on a rock that is no longer in front of the
    // ship.
    if (panel.found && panel.figuresRead >= 3) {
        rockMisses_ = 0;
        // A panel the face reader read and the shapes did not see at all is
        // the one picture worth keeping: the shapes are what the app is fast
        // with, and the only way to find out why they miss the MOLE's pilot
        // seat is to look at a frame where they did. Two a session, and only
        // in a build with the rock view on.
        if (panel.labels.x < 0 && rockMissShots_ < 2) {
            rockMissShots_++;
            std::error_code ec;
            std::filesystem::create_directories(dataPath("debug"), ec);
            cv::imwrite(dataPath("debug/panel_miss_" + std::to_string(rockMissShots_) + ".png"), frame);
            std::cout << "[rock] kept a picture of the panel the shapes missed: debug/panel_miss_"
                      << rockMissShots_ << ".png" << std::endl;
        }
        // A mass a long way off the last one is a different rock, not a
        // misread: start the window again rather than average two rocks.
        if (!rockReadings_.empty()) {
            const float was = steadyRock(rockReadings_).mass;
            if (was > 0 && std::fabs(panel.stats.mass - was) > was * 0.25f) {
                rockReadings_.clear();
            }
        }
        rockReadings_.push_back(panel.stats);
        while (rockReadings_.size() > 5) rockReadings_.pop_front();

        RockStats rock = steadyRock(rockReadings_);
        logRockReading(panel, &rock);
        // ...but the view needs a mass to say anything at all, so it waits
        // for one rather than showing a rock with no figures on it.
        if (rock.mass <= 0) return;
        // The distance is not in the panel - it is printed under the rock's
        // own marker - so it is left at zero, which the rock view reads as
        // full power. Until that is read too, the figures it shows are the
        // laser at its best rather than at this range.
        gui_->updateRock(rock);
        loadoutCheckForRock(rock.mass);
        shownMass_ = rock.mass;
        shownOre_ = rock.ore;
        readContentsOnHud(frame, panel, rock);
        if (!rockSeen_) {
            rockSeen_ = true;
            std::cout << "\n[rock] " << (int)rock.mass << " kg, "
                      << (int)rock.resistance << " % resistance, instability "
                      << rock.instability
                      << (rock.ore.empty() ? "" : "  - " + rock.ore) << std::endl;
        }
        return;
    }

    // A panel seen but not read well enough to count is logged all the same:
    // those are exactly the looks where the reader is losing the rock.
    if (panel.found) logRockReading(panel, nullptr);

    // A look that found the panel's labels and read none of its figures is
    // not a miss: the panel is still on screen. In the MOLE's turrets the
    // face reader reads the figures in one look of ten or fewer, and counted
    // as misses those looks would send the view back to the signatures
    // between two readings.
    if (panel.labels.x >= 0) {
        // ...unless its mass - white on every HUD, and read here - says
        // another rock is in front of the ship than the one on view, twice
        // running. While its laser is on the Golem prints a rock's
        // resistance and instability in red over its labels, which nothing
        // reads, so the rock looked at before would otherwise stay on view
        // for the next one.
        if (rockSeen_ && shownMass_ > 0 && panel.figures[0].placed) {
            const std::string& text = panel.figures[0].text;
            const bool digits = !text.empty() && text.size() <= 7 &&
                                std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
            const float mass = digits ? (float)std::atof(text.c_str()) : 0.0f;
            if (mass > 0 && std::fabs(mass - shownMass_) > shownMass_ * 0.25f) {
                if (otherMass_ > 0 && std::fabs(mass - otherMass_) <= otherMass_ * 0.05f) {
                    otherLooks_++;
                } else {
                    otherMass_ = mass;
                    otherLooks_ = 1;
                }
                if (otherLooks_ >= 2) {
                    rockSeen_ = false;
                    std::cout << "\n[rock] another rock (" << (int)mass << " kg) whose figures cannot be read"
                              << std::endl;
                    rockReadings_.clear();
                    rockSteadier_.clear();
                    rockUnsettled_ = 0;
                    shownMass_ = 0;
                    otherMass_ = 0;
                    otherLooks_ = 0;
                    clearRockView();
                }
            } else {
                otherMass_ = 0;
                otherLooks_ = 0;
            }
        }
        // The list under the labels is the rock's on view all the same, and
        // is read on these looks too: the Golem's figures stand in red for
        // as long as its laser is on, and for three Golem rocks tested not one
        // list came in on the looks that read them. Not while the mass says
        // another rock may be there.
        if (rockSeen_ && shownMass_ > 0 && otherLooks_ == 0) {
            RockStats shown;
            shown.mass = shownMass_;
            shown.ore = shownOre_;
            readContentsOnHud(frame, panel, shown);
        }
        return;
    }

    // Eight misses - twelve seconds - before the view goes back: fewer made
    // the window flick between the two layouts whenever a reading was
    // missed. Nobody scans a new rock inside twelve seconds, and a layout
    // that changes under the eye is worse than one that is late.
    if (++rockMisses_ >= 8) {
        if (rockSeen_) {
            rockSeen_ = false;
            std::cout << "\n[rock] gone" << std::endl;
        }
        rockReadings_.clear();
        rockSteadier_.clear();
        rockUnsettled_ = 0;
        clearRockView();
        shownMass_ = 0;
    }
}

// The loadout a one-seat ship's mining HUD shows: the laser at the bottom
// left, the modules over it (LoadoutReader). The log names the very hull
// only when a target is picked in it or its seat is left - a quantum jump
// too short to be logged names none, so two Prospectors may be told apart
// only a jump later - while the HUD says it as soon as the mining mode is
// on. The MOLE's pilot never sees its heads, so its fit stays the log's
// and the player's.
void Application::readLoadoutOnHud(const cv::Mat& frame, const ScanPanel& panel) {
    if (!gui_ || !textLines_.ready()) return;
    // Whose HUD: the Prospector's or the Golem's, by the scan panel's label
    // column - and with no panel up, by the ship the log has the player
    // sitting in (hudShipOf). The HUD lists the loadout in mining mode with
    // no rock on it at all; read only beside a panel, the switch would come a
    // rock or two late, as a reading starts on one look, is taken on a later
    // one, and the switch wants two.
    // The label column of a panel read for sure; one only tried, only where
    // the log has no seat to say it - a tried one takes a Golem's sand for a
    // Prospector's L now and then, the seat's key flips between the two, and
    // every flip throws the readings so far away.
    const char glyph = panel.labels.x >= 0 ? panel.labelGlyph : gameState_.inSeat() ? 0 : panel.tryGlyph;
    const char* kindId = hudShipOf(glyph, gameState_.ship(), gameState_.inSeat());
    const MiningShip* kind = nullptr;
    for (int i = 0; i < MINING_SHIP_COUNT && kindId; i++) {
        if (std::strcmp(MINING_SHIPS[i].id, kindId) == 0) kind = &MINING_SHIPS[i];
    }
    // another seat, another hull named: read afresh
    if (kind) {
        const std::string seat = g_ship + "|" + gameState_.hull() + "|" + (gameState_.inSeat() ? "in" : "out") + "|" +
                                 std::to_string(gameState_.boardings()) + "|" + kindId;
        if (seat != loadoutSeat_) {
            loadoutSeat_ = seat;
            loadoutSeen_.clear();
            loadoutSaid_.clear();
            loadoutAgreeing_ = 0;
            loadoutUnsure_ = 0;
            loadoutDisagreeing_ = 0;
            loadoutNoHud_ = false;
            loadoutSettled_ = false;
            lastLoadoutRead_ = {};
        }
    }
    // A reading is a line of the second reader for the laser, one for each
    // line of the module list and one for each figure of the table's Head
    // column - some fifteen of them at 50 ms each, 750 to 900 ms in all. The
    // window would stand still for that, so it is read off the window's
    // thread, on a copy of the picture, and taken here once it is in - for
    // the seat it was read in only, and whatever this look found, so a panel
    // gone in the meantime does not hold it back until the next rock.
    if (loadoutJob_.valid()) {
        if (loadoutJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        // an exception in the job comes out here, and with nothing to catch
        // it the app would end: a reading lost is all it is
        SeenLoadout seen;
        try {
            seen = loadoutJob_.get();
        } catch (const std::exception& e) {
            std::cerr << "[loadout] reading failed: " << e.what() << std::endl;
        }
        if (loadoutJobSeat_ == loadoutSeat_ && loadoutJobKind_) takeSeenLoadout(seen, *loadoutJobKind_);
    }
    if (!kind) return;
    // Every four seconds until the readings agree - the next one on this very
    // look, as soon as one has come in whole - then every three minutes,
    // which a reading that was wrong those times running gets put right by:
    // a ship's modules do not change while it is flown, and to change them
    // the player gets up, which the log says (loadoutLookAgain) - mostly:
    // so a new rock has it read again as well (loadoutCheckForRock). Six
    // unsure ones in a row - a HUD over ground too busy to read it against,
    // or one this does not know - and it is every fifteen seconds: a reading
    // keeps several of the computer's cores busy. So it is after four whole
    // readings running that each said something else than the one before -
    // otherwise a HUD read two ways turn about has the next reading start on
    // the very next look, for good.
    // No HUD in the picture at all - the mining mode off - is looked for
    // again every two seconds: the table is not there, and finding that out
    // is a matter of some 40 ms.
    const auto now = std::chrono::steady_clock::now();
    // The reading a new rock asked for (loadoutCheckForRock) comes once the
    // rock's list is on view, or six seconds on where none comes in. Both go
    // through the one second reader, one line at a time, and started side by
    // side at the same rock the list - which is what the player is looking
    // at - comes the better part of a second later.
    if (loadoutRockCheck_ && (contentsFor_ > 0 || contentsTries_ >= 8 ||
                              now - loadoutRockCheckSince_ >= std::chrono::seconds(6))) {
        loadoutRockCheck_ = false;
        lastLoadoutRead_ = {};
    }
    const int every = loadoutSettled_ ? 180
                      : loadoutNoHud_ ? 2
                      : loadoutUnsure_ >= 6 || loadoutDisagreeing_ >= 4 ? 15
                                                                       : 4;
    if (now - lastLoadoutRead_ < std::chrono::seconds(every)) return;
    lastLoadoutRead_ = now;
    loadoutJobSeat_ = loadoutSeat_;
    loadoutJobKind_ = kind;
    try {
        loadoutJob_ = std::async(std::launch::async, [this, picture = frame.clone()]() {
            SeenLoadout seen;
            readSeenLoadout(picture, textLines_, seen);
            return seen;
        });
    } catch (const std::system_error&) {
        // no thread to be had: the reading waits for the next look
    }
}

void Application::loadoutLookAgain() {
    loadoutRockCheck_ = false;
    loadoutSettled_ = false;
    loadoutDisagreeing_ = 0;
    lastLoadoutRead_ = {};
}

// A new rock on view. A settled loadout is read again only every three
// minutes, and a change of ship the log does not report (two Prospectors,
// one boarded after the other) would leave the overlay judging the rock for
// the wrong lasers that long. So the first look at each new rock reads the
// loadout again when the last reading is half a minute old - one reading a
// rock at the most.
void Application::loadoutCheckForRock(float mass) {
    const bool newRock = shownMass_ <= 0 || std::fabs(mass - shownMass_) > shownMass_ * 0.25f;
    if (!newRock || !loadoutSettled_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastLoadoutRead_ >= std::chrono::seconds(LOADOUT_ROCK_CHECK_S)) {
        // ...but after the rock's list: see readLoadoutOnHud
        loadoutRockCheck_ = true;
        loadoutRockCheckSince_ = now;
    }
}

void Application::takeSeenLoadout(const SeenLoadout& seen, const MiningShip& kind) {
    if (!seen.complete()) {
        // No table in the picture: no mining HUD up, which is no HUD too
        // busy to read, and does not slow the readings down
        loadoutNoHud_ = !seen.tableFound;
        if (seen.tableFound) loadoutUnsure_++;
        return;
    }
    loadoutNoHud_ = false;
    loadoutUnsure_ = 0;
    std::vector<std::string> ids;
    for (const MiningModule* m : seen.modules) ids.push_back(m->id);
    std::sort(ids.begin(), ids.end());
    std::string key = seen.laser->id;
    for (const std::string& id : ids) key += "+" + id;
    const bool disagrees = !loadoutSeen_.empty() && key != loadoutSeen_;
    loadoutDisagreeing_ = disagrees ? loadoutDisagreeing_ + 1 : 0;
    loadoutAgreeing_ = key == loadoutSeen_ ? loadoutAgreeing_ + 1 : 1;
    loadoutSeen_ = key;
    if (loadoutAgreeing_ < 2) {
        // the next reading at once - unless they keep contradicting each other
        if (loadoutDisagreeing_ < 4) lastLoadoutRead_ = {};
        return;
    }
    if (key == loadoutSaid_) return;   // said already
    // Two readings alike pick the ship on the list that carries this. Where
    // none does, the ship flown takes it for its own - a module was swapped -
    // but what is kept for a ship is changed only on a reading seen three
    // times running, the third on the very next look.
    bool changed = false;
    const std::string fit =
        gui_->setSeenLoadout(kind, *seen.laser, seen.modules, loadoutAgreeing_ >= OWN_READINGS, &changed);
    if (fit.empty() && loadoutAgreeing_ < OWN_READINGS) {
        lastLoadoutRead_ = {};
        return;
    }
    loadoutSettled_ = true;
    loadoutSaid_ = key;
    std::string names = seen.laser->name;
    for (const MiningModule* m : seen.modules) names += std::string(", ") + m->name;
    std::cout << "\n[loadout] the HUD shows " << names
              << (fit.empty() ? std::string(" - no ship in the list has that, and none could take it")
                              : " - ship '" + fit + "'" + (changed ? ", which has it from now on" : ""))
              << std::endl;
}

void Application::clearRockView() {
    if (contentsFor_ > 0) logRockList("off", contentsFor_, 0, nullptr, contentsTries_, "the rock went off view");
    gui_->clearRock();
    contentsFor_ = 0;
    contentsWanted_ = 0;
    contentsTries_ = 0;
}

// What the rock on view holds: the composition list under the panel's
// figures - a share and a quality for every part of it (RockContents). A
// list is some five lines of two or three words, at 50 ms a word for the
// second reader, so it is read off the window's thread like the loadout, on
// a copy of that corner of the picture, and only while the view has no list
// for this rock: on every look until two readings say the same - two looks,
// over a ground that lets the list be read. A reading counts only when its
// shares add up to the rock and every quality is one its ore has; two alike
// are asked for all the same, because a 6 read for an 8 in a share's last
// place passes both (54 readings to 3 on one Titanium rock).
//
// Which rock a list is of: the one whose mass the view had when the picture
// was taken, and no other - a pilot pans from one rock to the next of its
// kind without the panel going away, say two Hephaestanite rocks of 8,513
// and 7,058 kg one after the other. Two readings that say the same are one
// rock's whatever the mass did in between, for no two rocks have one list.
// The same mass, or the same mass with one figure misread (21,460 and
// 21,464 on one Copper rock, look by look), counts as one: otherwise a list
// read at the one is thrown away at the other, and the second reading that
// would agree with the first never comes. Two rocks a pilot looks at one
// after the other do not differ in a single figure.
static bool sameMassRead(float a, float b) {
    if (a <= 0 || b <= 0) return false;
    const std::string x = std::to_string(std::lround(a)), y = std::to_string(std::lround(b));
    if (x == y) return true;
    if (x.size() != y.size()) return false;
    int differ = 0;
    for (size_t i = 0; i < x.size(); i++) differ += x[i] != y[i] ? 1 : 0;
    return differ == 1;
}

void Application::readContentsOnHud(const cv::Mat& frame, const ScanPanel& panel, const RockStats& rock) {
    if (!gui_ || !textLines_.ready() || rock.mass <= 0) return;
    // Under 900 lines the list's figures are a few pixels high: at 720 a
    // reading can pass every check with a figure wrong, twice running, where
    // from 1080 up none did across nine resolutions tested. No list rather
    // than a wrong one.
    if (frame.rows < 900) return;
    if (contentsJob_.valid()) {
        if (contentsJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        // (as the loadout's: an exception in the job is a list not read)
        RockContents read;
        try {
            read = contentsJob_.get();
        } catch (const std::exception& e) {
            std::cerr << "[rock] list reading failed: " << e.what() << std::endl;
        }
        if (DEV_ENV("SCMINER_CONTENTS_LOG")) {
            std::cout << "\n[rock] list for " << (int)contentsJobMass_ << " kg at " << (int)rock.mass << " kg: "
                      << (read.whole ? "whole" : "not whole") << ", " << read.parts.size() << " lines, sum " << read.sum
                      << "; " << read.note << std::endl;
        }
        const bool thisRock = sameMassRead(contentsJobMass_, rock.mass);
        logRockList("result", contentsJobMass_, rock.mass, &read, contentsTries_,
                    thisRock ? "for this rock" : "for another mass than on view: dropped");
        if (thisRock) takeRockContents(read, rock);
    }
    if (sameMassRead(rock.mass, contentsFor_)) return;   // read, and on view
    // Another mass than the list on view was read at. It may be that rock
    // still - with less of it left, or its mass read another way: the same
    // ore, where both looks named one, and the mass no more than a quarter
    // off - and then its list stays while it is read again. Of any other
    // rock it says nothing.
    const auto alike = [&](float mass, const std::string& ore) {
        return mass > 0 && (ore.empty() || rock.ore.empty() || ore == rock.ore) &&
               std::fabs(rock.mass - mass) <= mass * 0.25f;
    };
    bool shown = alike(contentsFor_, contentsOre_);
    if (!shown && contentsFor_ > 0) {
        logRockList("off", contentsFor_, rock.mass, nullptr, contentsTries_, "the rock on view is another");
        contentsFor_ = 0;
        gui_->setRockContents(RockContents());
    }
    if (!alike(contentsWanted_, contentsWantedOre_)) {
        contentsTries_ = 0;
        lastContentsRead_ = {};
    }
    contentsWanted_ = rock.mass;
    contentsWantedOre_ = rock.ore;
    const cv::Rect part = rockContentsRegion(panel, frame.size());
    if (part.area() <= 0) return;   // this look found no label column to place the list by
    // How often. A rock without a list: on every look, eight running, then
    // every five seconds - a list over ground too busy to read it against
    // keeps several cores busy for nothing. A rock that may be the one whose
    // list is on view: at once, and then every five seconds for as long as
    // the readings say it is - a mass that goes down or flickers would have
    // the list read on every look for good. Three readings running that do
    // not say so, and the list comes off.
    const auto now = std::chrono::steady_clock::now();
    if (shown && contentsTries_ >= 3) {
        logRockList("off", contentsFor_, rock.mass, nullptr, contentsTries_, "three readings did not say it again");
        contentsFor_ = 0;
        gui_->setRockContents(RockContents());
        shown = false;
    }
    if (shown ? contentsSeen_ == contentsShown_ && now - lastContentsRead_ < std::chrono::seconds(5)
              : contentsTries_ >= 8 && now - lastContentsRead_ < std::chrono::seconds(5)) {
        return;
    }
    lastContentsRead_ = now;
    contentsTries_++;
    contentsJobMass_ = rock.mass;
    logRockList("start", rock.mass, rock.mass, nullptr, contentsTries_, shown ? "again, its list on view" : "");
    ScanPanel moved = panel;
    moved.labels -= part.tl();
    try {
        contentsJob_ = std::async(std::launch::async, [this, picture = frame(part).clone(), moved, ore = rock.ore]() {
            RockContents read;
            readRockContents(picture, moved, ore, textLines_, read);
            return read;
        });
    } catch (const std::system_error&) {
        // no thread to be had: the reading waits for the next look
    }
}

void Application::takeRockContents(const RockContents& read, const RockStats& rock) {
    if (!read.whole) return;
    std::string key, words;
    char text[96];
    for (const RockPartRead& part : read.parts) {
        std::snprintf(text, sizeof(text), "%s:%.2f:%d;", part.inert ? "-" : part.ore.c_str(), part.percent, part.quality);
        key += text;
        if (part.inert) std::snprintf(text, sizeof(text), "%.2f %% inert", part.percent);
        else std::snprintf(text, sizeof(text), "%.2f %% %s at %d", part.percent, part.ore.c_str(), part.quality);
        words += (words.empty() ? "" : ", ") + std::string(text);
    }
    if (key != contentsSeen_) {
        // Once: the next look says whether it stands. And a whole list that
        // is not the one on view says that one is another rock's.
        contentsSeen_ = key;
        logRockList("first", rock.mass, rock.mass, &read, contentsTries_, "waits for a second that agrees");
        if (contentsFor_ > 0 && key != contentsShown_) {
            logRockList("off", contentsFor_, rock.mass, nullptr, contentsTries_, "a whole list of another rock read");
            contentsFor_ = 0;
            gui_->setRockContents(RockContents());
        }
        return;
    }
    const bool said = contentsFor_ > 0 && key == contentsShown_;
    if (!said) logRockList("shown", rock.mass, rock.mass, &read, contentsTries_, "");
    contentsFor_ = rock.mass;
    contentsOre_ = rock.ore;
    contentsShown_ = key;
    contentsTries_ = 0;
    gui_->setRockContents(read);
    // ...and what players on UEX pay for a part this good
    uexRockOres_.clear();
    for (const RockPartRead& part : read.parts) {
        if (!part.inert && !part.ore.empty() && part.quality >= UEX_MARKET_QUALITY) uexRockOres_.push_back(part.ore);
    }
    if (!said) std::cout << "\n[rock] holds " << words << std::endl;
}

// The last panel's place, across sessions.
//
// A player who quits in the pilot seat sits down in it again: the panel
// comes back where it was, and the glance can look there from the first
// scan instead of after the first full search - which, with nothing known,
// tries the orange ink only every other look and takes up to three seconds
// to notice the first rock. The viewport's size goes with it: another
// resolution puts the panel elsewhere, and a spot that does not fit reaches
// no bar and costs a few milliseconds a scan until a panel is read.
void Application::loadPanelSpot(const cv::Size& viewport) {
    std::ifstream in(dataPath(PANEL_SPOT_FILE));
    if (!in.is_open()) return;
    int w = 0, h = 0, x = -1, y = -1, ink = 0;
    double scale = 0;
    std::string glyph;
    in >> w >> h >> x >> y >> scale >> glyph >> ink;
    if (!in || w != viewport.width || h != viewport.height || x < 0 || y < 0 || scale <= 0 || glyph.size() != 1) return;
    PanelSpot spot;
    spot.known = true;
    spot.labels = cv::Point(x, y);
    spot.scale = scale;
    spot.glyph = glyph[0];
    spot.ink = ink;
    setPanelSpot(spot);
    preferPanelInk(spot.ink);
    keptSpot_ = spot;
}

void Application::keepPanelSpot(const cv::Size& viewport) {
    const PanelSpot spot = lastPanelSpot();
    if (!spot.known) return;
    // a few pixels of sway are the same place; the file is written for a
    // seat change, not for every rock
    if (keptSpot_.known && keptSpot_.glyph == spot.glyph && keptSpot_.ink == spot.ink &&
        std::abs(keptSpot_.scale - spot.scale) < 0.03 && std::abs(keptSpot_.labels.x - spot.labels.x) < 20 &&
        std::abs(keptSpot_.labels.y - spot.labels.y) < 20) {
        return;
    }
    std::ofstream out(dataPath(PANEL_SPOT_FILE), std::ios::trunc);
    if (!out.is_open()) return;
    out << viewport.width << ' ' << viewport.height << ' ' << spot.labels.x << ' ' << spot.labels.y << ' ' << spot.scale
        << ' ' << spot.glyph << ' ' << spot.ink << '\n';
    keptSpot_ = spot;
}

void Application::processScanWithGUI() {
    // Messages are pumped between the candidates below, so a click on SCAN
    // or a hotkey can arrive while a scan runs - it must not start another
    // one inside this one
    if (inScan_) return;
    struct ScanGuard { bool& flag; ~ScanGuard() { flag = false; } } scanGuard{inScan_};
    inScan_ = true;

    lookForRock();

    // Clocked from here to the log line: a scan slower than the interval is
    // what the player experiences as a slow reading. The stages start at
    // zero because not every scan runs all of them, and carrying the last
    // scan's figure into a stage that never ran is how a log starts lying.
    const auto scanStart = std::chrono::steady_clock::now();
    g_captureMs = g_recordMs = g_trackMs = g_locateMs = 0.0;
    g_havePos = false;
    g_offX = g_offY = g_outside = 0;
    g_pinSlid = false;
    g_found = 0;
    g_bestPin = 0;
    g_font = "";
    auto msSince = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    struct ScanClock {
        std::chrono::steady_clock::time_point start;
        ~ScanClock() {
            g_scanMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
    } scanClock{scanStart};

    // Debug: print the region once
    static bool showRegionOnce = true;
    if (showRegionOnce) {
        std::cout << "\n[Scan region: x=" << signatureRegion_.x << " y=" << signatureRegion_.y
                  << " w=" << signatureRegion_.width << " h=" << signatureRegion_.height << "]" << std::endl;
        showRegionOnce = false;
    }

    std::cout << "." << std::flush;  // scan heartbeat

    static int debugCounter = 0;
    static int unreadCounter = 0;
    // Recent readings, for the consistency window below - each with the
    // moment it was read, because six readings are not a length of time: a
    // scan that finds nothing puts nothing in the window, so through a quiet
    // stretch the six slots can span ten minutes.
    //
    // In a replayed scan log of ~3,600 readings, a confirmed reading found its
    // partner one second old half the time, and five seconds old at the 95th
    // percentile. The exceptions are the cockpit's own furniture. "111,000" is
    // the mining gauge - "0.00 SCU/S" with the gauge arc for a pin, named in
    // goodEnough above - and "10,110" is the MOLE's right-hand display, whose
    // thin tick marks read as ones and zeroes beside an icon that passes for a
    // pin at 81 %. Both are table values, so two agreeing readings are enough
    // to show them; both turn up once a minute or so, far outside the aim box,
    // at 62-81 %. One of eleven 111,000s there was confirmed by a reading 32
    // seconds older.
    //
    // 20 seconds is the bar. In that log every confirmed reading whose partner
    // was older - and which was not read again a moment later anyway - was one
    // of these phantoms, bar a 7,200 whose two readings were 19 and 16 seconds
    // apart. A real marker sits on a rock the player is looking at; it comes
    // back in the same breath, not in the next minute.
    static constexpr int RECENT_SLOTS = 6;
    static constexpr double RECENT_SECONDS = 20.0;
    // `alone` marks a value only the second reader saw. The fragment rule
    // below may not lean on such a value: where that reader takes the pin
    // for a leading figure, every correct reading afterwards looks like a
    // fragment of its mistake and is rewritten into it - a 26,800 then
    // holds the display for twenty seconds over a marker that says 6,800.
    struct RecentRead {
        int value = 0;
        std::chrono::steady_clock::time_point when{};
        bool alone = false;
    };
    static RecentRead recentReads[RECENT_SLOTS];
    static int recentPos = 0;

    // Hide the overlay before capturing (its frame would end up in the image!)
    // Only when it is actually up: in live mode it goes once and stays gone,
    // and waiting 50 ms on every scan for a window long gone costs a sixth of
    // the median scan on a 5120x1440 screen.
    if (overlayHwnd_ && IsWindowVisible(overlayHwnd_)) {
        ShowWindow(overlayHwnd_, SW_HIDE);
        Sleep(50);
    }

    // The game may have appeared, moved to another monitor or changed size
    // since the last scan: an automatic box follows it, and the reader's
    // scale follows the picture
    if (refreshAutoAim(false)) updateOverlayPosition();
    refreshHudScale();
    if (snapshotPending_) {
        snapshotPending_ = false;
        saveSnapshot();
    }

    auto labelCandidates = captureSignatureCandidates(SCAN_CANDIDATES);
    g_found = static_cast<int>(labelCandidates.size());
    g_scanMs = msSince(scanStart);
    // The capture path is only known once something was captured; the
    // settings are written out here too, so debug/ carries them from the
    // first scan on, not only from the first orderly exit
    static bool sessionNoted = false;
    if (!sessionNoted) {
        sessionNoted = true;
        saveRegionConfig();
        writeSessionInfo();
    }


    // Show the overlay again outside of live mode
    if (overlayHwnd_ && !liveModeActive_) {
        ShowWindow(overlayHwnd_, SW_SHOW);
    }

    if (labelCandidates.empty()) {
        std::cout << "[no capture]" << std::flush;
        return;
    }

    // Debug: rolling buffer of the last 40 captures. The image is saved after
    // the candidate loop so it shows the capture the reading came from, not
    // whichever candidate happened to be first.
    const int debugSlot = debugCounter % 40;
    debugCounter++;

    // Try the candidates in order and keep the first one that reads as a real
    // table value. This is what makes tracking self-correcting: locking onto
    // some other warm HUD element simply yields no valid signature, so the
    // scan moves on to the next candidate.
    std::optional<int> recognizedValue;
    double confidence = 0.0;
    int agreement = 0;
    cv::Rect takenFrom = labelCandidates.front().screenRect;
    bool exactValue = false;
    bool fromGlyphs = false;
    bool repairedRead = false;
    // The template reader read a value the table does not know, and read it
    // cleanly: no hesitant glyph resolved, no pin slid into place. See the
    // read count below - this is what lets such a value be shown at all.
    bool cleanUnknown = false;
    double winningPin = 0.0;        // the pin beside the winning row, 0..1
    bool winningSeparator = false;  // its thousands comma was seen
    cv::Rect winningPlate;   // where the plate sat inside the winning crop
    int ocrTries = 0;
    cv::Mat winningImage = labelCandidates.front().image;


    // A hinted read that comes back weak gets one pass with the hint dropped
    // - but only for the candidates nearest the aim, where the marker is.
    // See GlyphReader::read; the pass costs as much as the first one.
    int secondChancesLeft = SECOND_CHANCE_CANDIDATES;
    // The second reader: how many candidates it may still read on its own
    // this scan, and what it said (for the scan log).
    // A line costs it 110-180 ms of processor time (one thread to six), and
    // away from the rocks the HUD offers pinned rows that are no marker on
    // most scans - "UNKNOWN", "TARGET", "70m/s" - which it would read twice
    // a scan, three scans a second: a core of the game's, for nothing. So
    // once nothing at all has been found for a while, the rows it has looked
    // at lately rest on every scan, and it reads one row a scan at most, a
    // new one: a marker just come into view is still read at once, and
    // another new row waits a scan.
    // From its first reading on - which counts as a find - it reads on
    // every scan again, as many rows as before.
    const bool linesIdle =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_lastFind).count() > LINE_IDLE_AFTER_S;
    int linesLeft = linesIdle ? LINE_IDLE_ROWS : TextLineReader::FALLBACK_CANDIDATES;
    auto lineKnown = [this](int v) { return analyzer_.explainsExactly(v); };
    g_line = "";
    g_lineMs = 0.0;
    // Both readers read this value off the same row: evidence enough for
    // a single frame, even where the glyph reader alone would have had to
    // repeat itself (172 of 172 such values right on the labelled frames)
    bool bothAgreed = false;
    // The marker-like candidates the glyph reader could not read, for the
    // second reader to try once the glyph reader has tried them all - not
    // before, or a row it gives up on would take the place of a later one
    // it reads cleanly
    struct LineTry {
        const LabelCandidate* candidate;
        GlyphReader::Reading reading;
    };
    std::vector<LineTry> lineTries;
    for (const auto& candidate : labelCandidates) {
        // Keep the window alive. A scan with twelve candidates and a
        // Tesseract pass or two held this thread for seconds, and every
        // message in that time - a drag of the title bar above all - waited
        // for it. The callbacks this can fire are deferred or harmless: SCAN
        // only sets a flag, a region edit takes effect with the next capture.
        if (gui_) gui_->processMessages();

        // Template reader first: no OCR involved, and exact whenever the
        // plate and its white digits are intact. Tesseract only gets what
        // the template reader cannot segment.
        // Accepted when every glyph is a fair match and the row as a whole a
        // good one; the live consistency check still demands a second frame
        // unless the weakest glyph is strong.
        // The reader resolves its own hesitations against the signature
        // table, which is narrower than repairing the finished number: it
        // only ever swaps in the digit the classifier nearly chose.
        SCMiner::GlyphReader::Reading glyphReading;
        struct PinNote {   // the best pin of the scan, read or not
            const SCMiner::GlyphReader::Reading& r;
            ~PinNote() { g_bestPin = (std::max)(g_bestPin, static_cast<int>(r.pinScore * 100)); }
        } pinNote{glyphReading};
        auto isSignature = [this](int v) { return analyzer_.isSingleSignature(v); };
        auto isSum = [this](int v) { return analyzer_.isKnownSignature(v); };
        static const std::vector<int> known = analyzer_.knownSignatures();
        if (auto glyphValue = glyphReader_.read(candidate.image, &glyphReading, isSignature, &known, isSum,
                                                candidate.row.area() > 0 ? &candidate.row : nullptr,
                                                secondChancesLeft-- > 0)) {
            std::string glyphName;
            int glyphCluster = 0;
            // A reading that needed a hesitant glyph resolved has to be
            // cleaner than one read outright: in sun glare the digits bloat
            // into lumps that fit several templates, and letting those be
            // repaired turns a blur into a confident wrong number.
            // ...or read cleanly and known to no table (Reading::unknown): a
            // value the table lacks, shown as read once four scans agree,
            // with the nearest table entries beside it - never a repaired
            // neighbour of it.
            // One ore or one field, base times count, on the reader's own
            // bar; anything the table only reaches by adding two things - or
            // not at all - on the strict one (see strictlyGood)
            std::string exactName; int exactCount = 0;
            const bool single = analyzer_.isExactMatch(*glyphValue, exactName, exactCount);
            const bool sum = !single && analyzer_.isKnownSignature(*glyphValue);
            if (GlyphReader::goodEnough(glyphReading) &&
                (single || (sum && GlyphReader::wellPinned(glyphReading)) ||
                 (glyphReading.unknown && GlyphReader::strictlyGood(glyphReading))) &&
                !GlyphReader::ambiguousValue(glyphReading, [this](int v) { return analyzer_.isKnownSignature(v); },
                                             GlyphReader::ambiguityLead())) {
                recognizedValue = *glyphValue;
                confidence = glyphReading.weakest;
                // A two-ore sum is rarer than a single signature and is only
                // ever accepted as read, so it earns the slower path a
                // repaired reading takes: three agreeing scans, never one.
                // A pin that only matched once the comparison was allowed to
                // slide counts as weak evidence, the same as a repaired
                // glyph: shown only after the reading repeats itself.
                repairedRead = glyphReading.repaired || glyphReading.pinSlid ||
                               !analyzer_.isSingleSignature(*glyphValue);
                cleanUnknown = glyphReading.unknown && !glyphReading.repaired &&
                               !glyphReading.pinSlid && GlyphReader::strictlyGood(glyphReading);
                g_pinSlid = glyphReading.pinSlid;
                g_font = glyphReading.mfd ? "panel" : "canopy";
                // The ship from the log says which font to expect - the Drake's panel
                // face, everyone else's canopy face. The picture decides (the comma
                // gap, see GlyphReader); a disagreement is worth a line in the console
                // because it is either a new hull or a misread.
                if (!g_ship.empty() && (GameState::maker(g_ship) == "Drake") != glyphReading.mfd) {
                    std::cout << "\nnote: Game.log says " << g_shipLabel << ", the row was read in the "
                              << (glyphReading.mfd ? "panel" : "canopy") << " font" << std::endl;
                }
                agreement = 4;
                takenFrom = candidate.screenRect;
                winningImage = candidate.image;
                exactValue = analyzer_.isKnownSignature(*glyphValue);
                fromGlyphs = true;
                winningPlate = glyphReading.plate;
                winningPin = glyphReading.pinScore;
                winningSeparator = glyphReading.separator;

                // The second reader reads the same row. On 230 labelled frames,
                // where both read, they agreed 172 times and were right every
                // time - so an agreed value stands on one frame. Where they
                // differ, TextLineReader::judge says who is heard: the second
                // reader overrules a glyph reader that hesitated over a digit or
                // lost one (Differs) - every wrong value the glyph reader showed
                // was of that kind - and its value is then held to what a repair
                // is held to: shown once later scans repeat it. Against a glyph
                // reader that was sure it is only noted (Doubts): in all four
                // such labelled cases the glyph reader was right.
                // Not asked again while the value it last agreed to stands
                // where it stood.
                const bool agreedHere = *glyphValue == agreedValue_ &&
                                        (candidate.screenRect & agreedRect_).area() > 0;
                if (textLines_.ready() && !agreedHere && !candidate.source.empty()) {
                    const auto opinion = textLines_.judge(candidate.source, candidate.origin, glyphReading,
                                                          *glyphValue, lineKnown);
                    g_lineMs += opinion.line.ms;
                    g_line = TextLineReader::name(opinion.verdict);
                    if (opinion.verdict == TextLineReader::Verdict::Agrees) {
                        agreedValue_ = *glyphValue;
                        agreedRect_ = candidate.screenRect;
                        bothAgreed = true;
                    } else if (opinion.verdict == TextLineReader::Verdict::Doubts) {
                        agreedValue_ = 0;
                    } else if (opinion.verdict == TextLineReader::Verdict::Differs) {
                        std::cout << "\n[second reader: " << opinion.value << " where the glyphs read "
                                  << *glyphValue << "]" << std::flush;
                        recognizedValue = opinion.value;
                        confidence = opinion.line.mean * 100.0;
                        agreement = 1;
                        exactValue = analyzer_.isKnownSignature(opinion.value);
                        fromGlyphs = false;
                        repairedRead = true;
                        cleanUnknown = false;
                        agreedValue_ = 0;
                    }
                } else if (agreedHere) {
                    g_line = "agreed before";
                    bothAgreed = true;
                    // ...and "where it stood" moves with it. Kept at the spot where
                    // the second reader first agreed, it would ask again as soon as
                    // the marker slid off it - every other scan while a rock is read
                    // from a moving ship, 40 ms each - only to hear the same value.
                    agreedRect_ = candidate.screenRect;
                }
                break;
            }
        }

        // No row of digits with the pin beside it means this is not a
        // signature marker at all - the OCR would only hallucinate a value
        // out of whatever text or instrument the tracker is looking at
        // Tesseract only gets a marker the pin vouches for; it turned a
        // "CPLD" button into 4,210 when asked on weaker evidence
        if (!glyphReading.markerLikely || glyphReading.pinScore < GlyphReader::OCR_MIN_PIN) continue;

        // A marker the glyph reader could not read: kept for the second
        // reader, which tries the first few of them once every candidate
        // has had its turn with the glyph reader (below)
        if (textLines_.ready() && linesLeft > 0 && glyphReading.digitBox.area() > 0 &&
            !candidate.source.empty()) {
            // Resting is per ROW, not per scan. A row already looked
            // at and found empty gives the same answer again and is
            // left alone; a row not seen before is read at once,
            // resting or not - that is a marker just come into view.
            // With per-scan resting a new marker once went unread for
            // 55 scans (18 s), 22 of them spent resting.
            if (linesIdle && lineLookedAt(candidate.screenRect)) {
                if (g_line.empty()) g_line = "resting";
            } else {
                linesLeft--;
                lineTries.push_back({&candidate, glyphReading});
            }
        }

        // Tesseract on twelve candidates would blow the scan interval, and
        // the ones past the first few are the least likely anyway - they sit
        // furthest from both the previous marker and the aim point.
        // Tesseract may only CONFIRM the cutter (see below). When the
        // cutter's own value is no signature there is nothing to confirm, and
        // the scan's most expensive step is skipped where it could not help
        if (!analyzer_.isKnownSignature(glyphReading.value)) continue;

        if (++ocrTries > OCR_FALLBACK_CANDIDATES) continue;
        // Tesseract is opt-in (SCMINER_OCR=1): measured on 924 live frames
        // it confirmed none of the 68 readings, and one attempt held the
        // scan for up to 1.5 s.
        static const bool useOcr = DEV_ENV("SCMINER_OCR") != nullptr;
        if (!useOcr || !ocrAvailable_) continue;

        auto renderings = screenCapture_.preprocessCandidates(candidate.image);

        auto value = ocrEngine_.recognizeBest(renderings, readingPolicy());
        if (!value) continue;
        // The fallback only ever sees verified markers, so a reading this
        // uncertain is not a hard marker - it is glare the OCR guessed at,
        // and guesses that happen to land on a table value are the worst kind
        if (ocrEngine_.getLastConfidence() < MIN_OCR_CONFIDENCE) continue;
        // Tesseract may only CONFIRM what the cutter nearly read. Left to
        // its own devices over a rejected row it turned a "CPLD" button into
        // 4,210 and a chat line into 39,000 - both table values, both
        // shown with confidence. Two readers agreeing is evidence.
        if (*value != glyphReading.value) continue;

        std::string exactName;
        int exactCluster = 0;
        const bool exact = analyzer_.isKnownSignature(*value);

        if (exact || !recognizedValue) {
            recognizedValue = value;
            confidence = ocrEngine_.getLastConfidence();
            agreement = ocrEngine_.getLastAgreement();
            takenFrom = candidate.screenRect;
            winningImage = candidate.image;
            exactValue = exact;
            fromGlyphs = false;
        }
        if (exact) break;   // a table hit settles it
    }

    // Nothing the glyph reader would stand behind on any candidate: the
    // second reader reads the marker-like rows it kept. On the labelled
    // frames that found 25 values the glyph reader missed, 2 of them wrong
    // (a 6 read as 8, a 3 as 2) - so what it reads alone is held to what a
    // repair is held to: shown only once later scans repeat it.
    if (!recognizedValue) {
        for (const auto& tried : lineTries) {
            const LabelCandidate& candidate = *tried.candidate;
            const auto opinion = textLines_.judge(candidate.source, candidate.origin, tried.reading, std::nullopt,
                                                  lineKnown);
            g_lineMs += opinion.line.ms;
            if (g_line.empty() || g_line == "none") g_line = TextLineReader::name(opinion.verdict);
            if (opinion.verdict != TextLineReader::Verdict::Reads) {
                rememberLineLook(candidate.screenRect);
                continue;
            }
            recognizedValue = opinion.value;
            confidence = opinion.line.mean * 100.0;
            agreement = 1;
            takenFrom = candidate.screenRect;
            winningImage = candidate.image;
            exactValue = analyzer_.isKnownSignature(opinion.value);
            fromGlyphs = false;
            repairedRead = true;
            cleanUnknown = false;
            winningPlate = tried.reading.plate;
            winningPin = tried.reading.pinScore;
            winningSeparator = tried.reading.separator;
            break;
        }
    }

    saveDebugCapture("region_" + std::to_string(debugSlot) + ".png", winningImage);

    // Remember where the winning label sits so the next scan starts there
    if (recognizedValue && lastScanTracked_) {
        lastLabelRect_ = takenFrom;
        hasLastLabel_ = true;
        lockMisses_ = 0;
        followOnly_ = 0;
        followMissed_ = false;
        lockLost_ = 0;
        // Track the PLATE, not the crop it was cut from. The crop carries a
        // margin of terrain and sky around the marker, and that margin is
        // exactly the part that changes when the view turns - matching it
        // dragged the correlation down to 0.3-0.6 and the lock fell off after
        // a couple of frames. The plate itself is the same pixels every time.
        cv::Rect plate = winningPlate & cv::Rect(0, 0, winningImage.cols, winningImage.rows);
        if (plate.width < 12 || plate.height < 6) plate = cv::Rect();
        if (!winningImage.empty()) {
            lockPatch_ = plate.area() > 0 ? winningImage(plate).clone() : winningImage.clone();
        }

    }

    // Where that marker sat against the box the player marked. Taken
    // once the winning candidate is settled, so it describes the reading
    // that is about to be logged and not whichever candidate came first.
    if (recognizedValue && takenFrom.area() > 0) {
        const cv::Rect aim(signatureRegion_.x, signatureRegion_.y,
                           signatureRegion_.width, signatureRegion_.height);
        g_offX = (takenFrom.x + takenFrom.width / 2) - (aim.x + aim.width / 2);
        g_offY = (takenFrom.y + takenFrom.height / 2) - (aim.y + aim.height / 2);
        g_outside = (std::max)({0,
                                aim.x - takenFrom.x,
                                takenFrom.x + takenFrom.width - (aim.x + aim.width),
                                aim.y - takenFrom.y,
                                takenFrom.y + takenFrom.height - (aim.y + aim.height)});
        g_havePos = true;
    }

    // Report where the label currently sits so the GUI can show it
    if (gui_) {
        gui_->setTrackingStatus(autoTrack_, lastScanTracked_ && recognizedValue.has_value(),
                                takenFrom.x, takenFrom.y);
    }

    if (!recognizedValue) {
        std::cout << "x" << std::flush;  // OCR failed
        g_scanMs = msSince(scanStart);
        logScan(debugSlot, "no-read", 0, 0.0, 0);
        // Keep failures in their own ring: the normal buffer is overwritten
        // within seconds of scanning, so the frames worth investigating would
        // be gone by the time anyone looks at them
        saveDebugCapture("unread_" + std::to_string(unreadCounter % 20) + ".png",
                         labelCandidates.front().image);
        unreadCounter++;
        missStreak_++;
        return;
    }

    int scannedValue = *recognizedValue;   // already repaired by the reading policy

    // Valid signature range. Nothing above MAX_VALID_SIGNATURE exists in the
    // published data, but keep a margin so the GUI can show and explain an
    // out-of-range reading instead of dropping it silently.
    if (scannedValue < 1500 || scannedValue > MAX_VALID_SIGNATURE + 5000) {
        std::cout << "." << std::flush;  // out of range
        g_scanMs = msSince(scanStart);
        logScan(debugSlot, "out-of-range", scannedValue, confidence, agreement);
        return;
    }

    // "Moments ago" is what the window means: a reading older than
    // RECENT_SECONDS is not in it.
    const auto readAt = std::chrono::steady_clock::now();
    auto stillCounts = [&](const RecentRead& recent) {
        return recent.value != 0 &&
               std::chrono::duration<double>(readAt - recent.when).count() <= RECENT_SECONDS;
    };

    // A digit fallen off a value read moments ago is not a new value:
    // "17,080" with its thin 1 lost reads 7,080 (Beryl x2), "63,000" reads
    // 3000, "39,000" 3900. Each is what a recent reading becomes with its
    // first or last digit gone, and the table cannot object: every multiple
    // of 3000 or 4000 is a signature. So a reading that is such a fragment
    // of a value in the window counts FOR that value - the marker is still
    // there and still says the whole number - and never as a value of its
    // own.
    {
        auto fragmentOf = [](int part, int whole) {
            if (part <= 0 || whole <= part) return false;
            if (whole == part * 10) return true;                    // last digit gone
            for (int p = 10; p <= 100000 && p <= whole; p *= 10) {
                if (p > part && whole % p == part) return true;     // leading digit(s) gone
            }
            return false;
        };
        for (const RecentRead& recent : recentReads) {
            if (!stillCounts(recent) || recent.alone) continue;
            const int value = recent.value;
            if (fragmentOf(scannedValue, value)) {
                std::cout << "f" << std::flush;
                g_scanMs = msSince(scanStart);
                logScan(debugSlot, "fragment-of-" + std::to_string(value), scannedValue, confidence, agreement);
                scannedValue = value;
                exactValue = analyzer_.isKnownSignature(value);
                repairedRead = true;   // a fragment is never single-frame proof
                cleanUnknown = false;  // ...nor is it what the glyphs actually said
                bothAgreed = false;    // ...nor what the two readers agreed on
                break;
            }
        }
    }

    // Consistency check: how often this exact value appears among the recent
    // readings. Misreads scatter across different wrong values, so seeing the
    // identical number more than once is what separates them.
    //
    // This counts over a WINDOW rather than consecutive frames: the marker is
    // anchored to the rock, so with head tracking a good frame is routinely
    // followed by one that caught terrain. Demanding two reads in a row threw
    // away values that had already been read correctly.
    int consistentCount = 1;
    for (const RecentRead& recent : recentReads) {
        if (stillCounts(recent) && recent.value == scannedValue) consistentCount++;
    }
    recentReads[recentPos % RECENT_SLOTS] = {scannedValue, readAt, !fromGlyphs && !bothAgreed};
    recentPos++;

    // In-game values are always exact base x cluster multiples (exactValue was
    // determined while picking the label). A read that hits a real table value
    // only needs one confirmation; anything else (leftover misreads, aggregated
    // multi-cluster blips) must hold still for longer before it is shown.
    //
    // Signatures sometimes flash up for a single frame (in-game bug), so a
    // second frame to confirm may never come. Accept immediately when this one
    // frame is already strong evidence: every reading comes from a capture
    // whose pin was matched, so it IS a signature marker rather than some text
    // the tracker drifted onto. An exact table value on such a capture - read
    // either by the template reader or by several agreeing OCR renderings -
    // needs no repetition.
    // A reading whose ambiguous glyph had to be resolved against the table
    // is NOT single-frame proof. Two table values can fit the same picture -
    // at 13 px a 6 and an 8 score alike - and showing whichever won this
    // frame makes the display flip between them. Let the window decide which
    // one keeps coming back.
    // A reading that matches nothing in the table is normally noise. But the
    // signature data is not guaranteed complete - 39,350 was read identically
    // by twenty independent renderings at 87 %, and no entry explains it - so
    // a reading that strong is shown as an unknown signature rather than
    // swallowed. The GUI lists the nearest known values beside it.
    // What makes an unknown strong is HOW its glyphs matched (cleanUnknown),
    // not a count of agreeing renderings: the template reader hands back one
    // reading, and asking an unknown for four agreeing scans is more than
    // flying past a rock gives.
    const bool strongUnknown = !exactValue && cleanUnknown;

    // A single frame may settle it only when the reading is not merely valid
    // but CLEAN.
    //
    // With the locator's wide reach, markers beside the sun are found in 9 of
    // 21 recorded frames, and two of those nine read wrong: a 10,000 as 7080,
    // an 8,390 as 8360. Both are real values in the signature table, so
    // nothing downstream can tell them from a correct reading - and a wrong
    // number sends the player to the wrong rock, which is worse than no number.
    //
    // What separates them is how well the glyphs actually matched. A marker
    // stays on screen for many scans, so demanding a second frame for anything
    // less than clean costs a third of a second and no readings at all, while
    // a misread rarely repeats itself the same way twice.
    //
    // The bar is 90, not 95: some real markers stand in the picture for
    // exactly one scan (64,000 and 21,200 on live footage), and 95 with pin
    // and comma would hide them.
    //
    // An unknown value never settles on one frame, though: two readers can
    // agree once on HUD text such as "2074".
    constexpr double SINGLE_FRAME_CONFIDENCE = 90.0;
    // ...or when both readers read it: two readers that fail in different
    // ways agreeing on a table value is stronger than any glyph score (see
    // bothAgreed above)
    const bool singleFrameProof = (exactValue && !repairedRead &&
                                   confidence >= SINGLE_FRAME_CONFIDENCE &&
                                   (fromGlyphs || agreement >= 3)) ||
                                  (exactValue && bothAgreed);
    (void)winningPin;
    (void)winningSeparator;

    // A repaired reading repeats itself: the same plate in the same light
    // is read the same wrong way again ("16,960" repaired to 18,000 twice in
    // ten seconds), so a repair needs three matching reads, not two.
    // A value no table knows is still never shown off one frame - two
    // readers can agree once on HUD text such as "2074" - but a clean one
    // asks for two scans rather than four.
    const int requiredReads = exactValue ? (repairedRead ? 3 : 2)
                                         : (strongUnknown ? 2 : 4);

    if (!singleFrameProof && consistentCount < requiredReads) {
        std::cout << "?" << std::flush;  // uncertain, wait for confirmation
        g_scanMs = msSince(scanStart);
        logScan(debugSlot, "held-back", scannedValue, confidence, agreement);
        return;
    }

    std::cout << "\n[OCR: " << scannedValue << " (" << (int)confidence << "%)]" << std::flush;
    g_scanMs = msSince(scanStart);
    logScan(debugSlot, singleFrameProof ? "accepted-flash" : "accepted",
            scannedValue, confidence, agreement);

    // Has the lock settled on something that never moves? Then it is cockpit
    // furniture wearing a marker's clothes, and holding on to it costs every
    // real marker on screen - the search never looks anywhere else again.
    if (scannedValue == stuckValue_ &&
        std::abs(takenFrom.x - stuckRect_.x) <= STUCK_DRIFT_PX &&
        std::abs(takenFrom.y - stuckRect_.y) <= STUCK_DRIFT_PX) {
        stuckCount_++;
    } else {
        stuckValue_ = scannedValue;
        stuckRect_ = takenFrom;
        stuckCount_ = 1;
    }
    if (stuckCount_ >= STUCK_LIMIT) {
        std::cout << "\n[stuck on " << scannedValue << " at " << takenFrom.x << ","
                  << takenFrom.y << " - cockpit, not a rock; ignoring it for a while]"
                  << std::endl;
        blacklistRect_ = takenFrom;
        blacklistLeft_ = BLACKLIST_SCANS;
        hasLastLabel_ = false;
        lockPatch_.release();
        lockMisses_ = 0;
        missStreak_ = REACQUIRE_AFTER_MISSES;   // sweep the whole screen next
        stuckCount_ = 0;
        stuckValue_ = 0;
        lastRecognizedSignature_ = 0;
    }

    // Only update on change
    if (scannedValue == lastRecognizedSignature_) {
        return;
    }

    lastRecognizedSignature_ = scannedValue;

    auto results = analyzer_.analyzeSignature(scannedValue);

    // Update the GUI
    if (gui_) {
        gui_->updateResults(scannedValue, results, confidence, analyzer_.outOfPlace());
    }

    displayResultsCompact(results, scannedValue);

    if (resultCallback_) {
        resultCallback_(results);
    }
}

OCREngine::ReadingPolicy Application::readingPolicy() {
    return makeReadingPolicy(analyzer_);
}

void Application::stop() {
    running_ = false;
}

void Application::setResultCallback(ResultCallback callback) {
    resultCallback_ = callback;
}

void Application::setCaptureInterval(int milliseconds) {
    captureIntervalMs_ = milliseconds;
}

void Application::setHotkey(int vkCode) {
    hotkeyVkCode_ = vkCode;
}

void Application::triggerScan() {
    scanRequested_ = true;
    loadoutLookAgain();
}

void Application::toggleLiveMode() {
    liveModeActive_ = !liveModeActive_;

    if (liveModeActive_) {
        loadoutLookAgain();
        lastRecognizedSignature_ = 0;  // reset for a fresh detection
        lastScanTime_ = std::chrono::steady_clock::now();
        std::cout << "\n========================================" << std::endl;
        std::cout << "  [LIVE] mode ENABLED" << std::endl;
        std::cout << "  Scan interval: " << captureIntervalMs_ << "ms" << std::endl;
        std::cout << "  Press 'L' to stop" << std::endl;
        std::cout << "========================================\n" << std::endl;
    } else {
        std::cout << "\n[LIVE] mode DISABLED\n" << std::endl;
    }
}

bool Application::isLiveModeActive() const {
    return liveModeActive_;
}

void Application::setAutoTrack(bool enabled) {
    autoTrack_ = enabled;
    hasLastLabel_ = false;
    std::cout << "Auto-tracking: " << (enabled ? "ON" : "OFF") << std::endl;
    saveRegionConfig();
}

bool Application::isAutoTrackActive() const {
    return autoTrack_;
}

// The aim box for a game drawn in this viewport, in the proportions of a
// 5120x1440 viewport: 352x180 with its centre at 2560,475 - centred on the
// crosshair, a third of the way down - and 250 px of search margin around
// it (400 takes in the top of a Prospector's screen and the lower canopy
// of a fighter: see g_outside). On 53 markers read in that setup, the
// centres sat at y 402-547 (mean 486) and x 2522-2565, the plates from
// 382 to 567. Everything scales with the HEIGHT: the HUD keeps its size
// relative to the vertical field of view, an ultrawide only adds width at
// the sides. And it sits wherever the viewport is, so the box lands on the
// monitor the game runs on in a multi-monitor setup.
CaptureRegion Application::defaultAimBox(const cv::Rect& viewport, int* margin) {
    const double h = viewport.height;
    CaptureRegion box;
    box.width = (std::max)(40, static_cast<int>(std::lround(h * 352.0 / 1440.0)));
    box.height = (std::max)(20, static_cast<int>(std::lround(h * 180.0 / 1440.0)));
    box.x = viewport.x + viewport.width / 2 - box.width / 2;
    box.y = viewport.y + static_cast<int>(std::lround(h * 475.0 / 1440.0)) - box.height / 2;
    box.autoDetect = true;
    if (margin) *margin = (std::max)(50, static_cast<int>(std::lround(h * 250.0 / 1440.0)));
    return box;
}

bool Application::refreshAutoAim(bool announce) {
    if (!aimAuto_) return false;
    cv::Rect viewport;
    if (auto found = ScreenCapture::gameViewport()) {
        viewport = *found;
    } else {
        // No game window yet: the primary monitor, until there is one
        viewport = cv::Rect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }
    if (viewport == aimViewport_) return false;
    aimViewport_ = viewport;
    int margin = trackMargin_;
    signatureRegion_ = defaultAimBox(viewport, &margin);
    trackMargin_ = margin;
    hasLastLabel_ = false;   // the box moved, tracking starts over
    lockPatch_.release();
    if (announce) {
        std::cout << "Aim box from the " << viewport.width << "x" << viewport.height << " viewport at "
                  << viewport.x << "," << viewport.y << ": " << signatureRegion_.x << ","
                  << signatureRegion_.y << " " << signatureRegion_.width << "x" << signatureRegion_.height
                  << ", margin " << trackMargin_ << std::endl;
    }
    if (gui_) {
        gui_->setRegionValues(signatureRegion_.x, signatureRegion_.y,
                              signatureRegion_.width, signatureRegion_.height);
        gui_->setRegionAuto(true);
    }
    return true;
}

cv::Rect Application::allowedArea() const {
    // The signature always appears in one part of the HUD. The user marks that
    // part with the overlay, and this is the hard boundary drawn from it: that
    // region plus the tracking margin, and nothing else, ever.
    //
    // A search that may sweep the whole monitor locks onto anything
    // plate-shaped: the power management panel, the mining laser readout, the
    // status pills in the corner, chat. All of those produce digits, and
    // digits that resolve against the signature table get shown as a reading.
    // Bounding the search stops that at the source instead of trying to tell
    // a real reading from a false one after the fact.
    const cv::Rect aim(signatureRegion_.x, signatureRegion_.y,
                       signatureRegion_.width, signatureRegion_.height);
    cv::Rect area(aim.x - trackMargin_, aim.y - trackMargin_,
                  aim.width + 2 * trackMargin_, aim.height + 2 * trackMargin_);

    // ...and inside the game picture, never the desktop, the taskbar or a
    // window in front of it
    const POINT aimCentre{aim.x + aim.width / 2, aim.y + aim.height / 2};
    cv::Rect screen(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (HMONITOR monitor = MonitorFromPoint(aimCentre, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfo(monitor, &info)) {
            screen = cv::Rect(info.rcMonitor.left, info.rcMonitor.top,
                              info.rcMonitor.right - info.rcMonitor.left,
                              info.rcMonitor.bottom - info.rcMonitor.top);
        }
    }
    if (auto viewport = ScreenCapture::gameViewport()) {
        if (viewport->contains(cv::Point(aimCentre.x, aimCentre.y))) screen = *viewport;
    }

    area &= screen;
    return area;
}

CaptureRegion Application::trackingSearchArea() const {
    const cv::Rect aim(signatureRegion_.x, signatureRegion_.y,
                       signatureRegion_.width, signatureRegion_.height);
    const cv::Rect screen = allowedArea();

    // Lost the marker for a while? Search the whole ALLOWED AREA again - the
    // marked region and its margin, not the monitor. That is what the boundary
    // is for: re-acquiring may look anywhere the signature can legitimately
    // appear, and nowhere else.
    if (missStreak_ >= REACQUIRE_AFTER_MISSES) {
        missStreak_ = 0;
        CaptureRegion whole;
        whole.x = screen.x;
        whole.y = screen.y;
        whole.width = screen.width;
        whole.height = screen.height;
        return whole;
    }

    // Pinned to the marker while the lock holds, wide around the aim point
    // once it is gone
    cv::Rect anchor = aim;
    int margin = trackMargin_;
    if (hasLastLabel_ && (lastLabelRect_ & screen).area() > 0) {
        anchor = lastLabelRect_;
        margin = (std::max)(LOCK_MARGIN_PX, anchor.height * 3);
        // The picture was not where it was left - a head turn, most likely.
        // Look further before concluding the marker is gone.
        if (followMissed_) margin *= LOCK_WIDEN_FACTOR;
    }


    cv::Rect area(anchor.x - margin, anchor.y - margin,
                  anchor.width + 2 * margin, anchor.height + 2 * margin);
    area &= screen;

    CaptureRegion region;
    region.x = area.x;
    region.y = area.y;
    region.width = area.width;
    region.height = area.height;
    return region;
}

void Application::applyLiveMode(bool active) {
    liveModeActive_ = active;
    std::cout << "Live mode: " << (active ? "ON" : "OFF") << std::endl;
    // What the player flies may have changed while nothing was looked at: the
    // first look reads the loadout, so a ship switched while live was off is
    // picked up the first time live starts.
    if (active) loadoutLookAgain();

    // Record only while scanning, and only the area the scan looks at - five
    // one-minute clips that overwrite each other, so the last five minutes are
    // always on disk and nothing grows without bound.
    if (active && recordScans_) {
        const std::uintmax_t free = freeBytesAt(dataPath("debug/recording"));
        if (free < RECORD_MIN_FREE_BYTES) {
            std::cout << "[recorder] only " << (free >> 20)
                      << " MB free on this drive - recording stays off" << std::endl;
        } else if (!recorder_.start(dataPath("debug/recording"), 1000.0 / (std::max)(1, captureIntervalMs_),
                                    RECORD_SEGMENT_SECONDS, RECORD_SEGMENTS)) {
            std::cout << "[recorder] " << recorder_.lastError() << std::endl;
        }
    } else {
        recorder_.stop();
    }

    // Hide the overlay in live mode (it would disturb the OCR)
    if (overlayHwnd_) {
        ShowWindow(overlayHwnd_, active ? SW_HIDE : SW_SHOW);
    }

    saveRegionConfig();   // so a restart comes back in the same state
}

void Application::keepRecording() {
    const std::string dir = dataPath("debug/recording");
    const bool wasRunning = recorder_.active();

    // Nothing is being recorded, so the clips on disk are from some earlier
    // session. Copying those and beeping success is worse than doing nothing:
    // it hands over a folder that looks like the moment just captured and is
    // not.
    if (!wasRunning) {
        std::cout << "\n[recorder] not running - nothing to keep. "
                     "Live mode records; turn it on first." << std::endl;
        Beep(400, 160);
        Beep(300, 320);
        return;
    }

    // Finalise the clip being written: the most recent seconds - the ones
    // worth keeping - are still inside ffmpeg until its input is closed.
    recorder_.stop();

    std::time_t now = std::time(nullptr);
    std::tm parts{};
    localtime_s(&parts, &now);
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &parts);
    const std::string target = dir + "/keep_" + stamp;

    std::error_code ec;

    // Room for the copy? The ring is up to ~450 MB, and a drive filled by
    // a diagnostic recording is worse than a recording lost.
    std::uintmax_t ringBytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code fec;
        if (entry.is_regular_file(fec) && entry.path().extension() == ".mkv") ringBytes += entry.file_size(fec);
    }
    if (freeBytesAt(dir) < ringBytes + 512ull * 1024 * 1024) {
        std::cout << "\n[recorder] not enough free space to keep the recording" << std::endl;
        Beep(400, 160);
        Beep(300, 320);
        recorder_.start(dir, 1000.0 / (std::max)(1, captureIntervalMs_),
                        RECORD_SEGMENT_SECONDS, RECORD_SEGMENTS);
        return;
    }

    std::filesystem::create_directories(target, ec);

    int kept = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mkv") continue;
        std::filesystem::copy_file(entry.path(),
                                   target + "/" + entry.path().filename().string(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) kept++;
    }

    // The copies are capped - see maxKeeps_. Oldest first: the stamp in the
    // name sorts them.
    std::vector<std::filesystem::path> keeps;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code fec;
        if (entry.is_directory(fec) && entry.path().filename().string().rfind("keep_", 0) == 0) {
            keeps.push_back(entry.path());
        }
    }
    std::sort(keeps.begin(), keeps.end());
    std::uintmax_t total = 0;
    for (const auto& keep : keeps) total += folderBytes(keep);
    const std::uintmax_t keepBudget = static_cast<std::uintmax_t>(maxKeepGb_) << 30;
    while (keeps.size() > 1 &&
           (static_cast<int>(keeps.size()) > maxKeeps_ || total > keepBudget)) {
        total -= folderBytes(keeps.front());
        std::filesystem::remove_all(keeps.front(), ec);
        std::cout << "[recorder] dropped old " << keeps.front().filename().string() << std::endl;
        keeps.erase(keeps.begin());
    }

    // Audible, because the whole point is not having to leave the game to see
    // whether it worked: two rising notes for saved, two falling for nothing.
    std::cout << "\n[recorder] kept " << kept << " clip(s) in " << target << std::endl;
    if (kept > 0) {
        Beep(1200, 90);
        Beep(1600, 90);
    } else {
        Beep(400, 160);
        Beep(300, 320);
    }

    recorder_.start(dir, 1000.0 / (std::max)(1, captureIntervalMs_),
                    RECORD_SEGMENT_SECONDS, RECORD_SEGMENTS);
}

void Application::trackCameraMotion(const cv::Mat& view) {
    // The marker hangs on a rock in the world. When the head turns it slides
    // across the screen - and so does everything else the world contains:
    // terrain, the horizon, the other contacts, the compass ribbon. That whole
    // shift IS the camera movement, and phase correlation reads it straight
    // out of two consecutive frames without knowing what any of it depicts.
    //
    // So the box does not need to recognise the marker again to stay on it;
    // it needs to move exactly as far as the view moved. Recognition then
    // only has to succeed once.
    //
    // This works because the area is captured at a FIXED rectangle, so a
    // shift measured between two frames is a shift on screen, with no window
    // bookkeeping in between.
    // Nothing to move without a lock: the shift measured then goes nowhere,
    // and measuring it costs 36 ms a scan over the 1152 x 965 area.
    // The view kept from the last scan is let go too; the scan that finds a
    // marker starts the pair afresh, and the box it draws is fresh anyway.
    if (!hasLastLabel_) {
        previousView_.release();
        return;
    }
    cv::Mat grey;
    cv::cvtColor(view, grey, cv::COLOR_BGR2GRAY);
    grey.convertTo(grey, CV_32F, 1.0 / 255.0);

    if (previousView_.size() == grey.size() && hasLastLabel_) {
        // Measured live while the view was turning: one correlation over the
        // whole area answers with a confidence of 0.26, while single
        // horizontal bands answer with 0.57 to 0.66 - and all of them agree on
        // the shift to within half a pixel. The area contains things that do
        // NOT move with the view (the canopy frame, the ship's instruments)
        // and they drag a single whole-picture correlation towards zero.
        //
        // So each band is asked separately and the median of those that answer
        // confidently is taken. The median is the point: a band that gets
        // fooled by a static instrument cannot move the result, because the
        // bands that saw the world outvote it.
        std::vector<double> dx, dy;
        const int bandHeight = grey.rows / CAMERA_BANDS;
        for (int b = 0; bandHeight > 8 && b < CAMERA_BANDS; b++) {
            const cv::Rect strip(0, b * bandHeight, grey.cols, bandHeight);
            double response = 0.0;
            const cv::Point2d shift =
                cv::phaseCorrelate(previousView_(strip), grey(strip), cv::noArray(), &response);
            if (response < CAMERA_MIN_RESPONSE) continue;
            if (std::abs(shift.x) >= CAMERA_MAX_SHIFT_PX ||
                std::abs(shift.y) >= CAMERA_MAX_SHIFT_PX) continue;
            dx.push_back(shift.x);
            dy.push_back(shift.y);
        }

        // Too few bands agreeing means the picture changed rather than moved -
        // a menu opening, a cut, the area going dark. Then the box stays where
        // it is, which is never worse than moving it on a guess.
        if (dx.size() >= CAMERA_MIN_BANDS) {
            std::sort(dx.begin(), dx.end());
            std::sort(dy.begin(), dy.end());
            lastLabelRect_.x += static_cast<int>(std::lround(dx[dx.size() / 2]));
            lastLabelRect_.y += static_cast<int>(std::lround(dy[dy.size() / 2]));
        }
    }
    previousView_ = grey;
}

void Application::followLockedMarker(const cv::Mat& searchImage, const CaptureRegion& search) {
    followedNow_ = false;
    if (!hasLastLabel_ || lockPatch_.empty() || searchImage.empty()) return;
    if (lockPatch_.type() != searchImage.type()) return;
    if (searchImage.cols < lockPatch_.cols || searchImage.rows < lockPatch_.rows) return;
    // A full-monitor sweep is both far too big to match a template across and
    // the wrong situation for it: sweeping means the marker has been gone for
    // a while, not that it moved a little since the last scan.
    if (static_cast<long long>(searchImage.total()) > 1200000LL) return;

    // About where the lock was - the camera's shift is already in it - and
    // not over the whole area: the marker moves a few pixels between scans,
    // and a match over the whole 1152 x 965 area costs 189 ms a scan where
    // one over 420 x 340 costs 19. When the last follow missed, the whole
    // area is searched.
    cv::Rect window(0, 0, searchImage.cols, searchImage.rows);
    if (!followMissed_) {
        const cv::Rect about(lastLabelRect_.x - search.x - LOCK_FOLLOW_WINDOW, lastLabelRect_.y - search.y - LOCK_FOLLOW_WINDOW,
                             lastLabelRect_.width + 2 * LOCK_FOLLOW_WINDOW, lastLabelRect_.height + 2 * LOCK_FOLLOW_WINDOW);
        const cv::Rect clipped = about & window;
        if (clipped.width > lockPatch_.cols + 8 && clipped.height > lockPatch_.rows + 8) window = clipped;
    }
    cv::Mat corr;
    cv::matchTemplate(searchImage(window), lockPatch_, corr, cv::TM_CCOEFF_NORMED);
    double best = 0.0;
    cv::Point at;
    cv::minMaxLoc(corr, nullptr, &best, nullptr, &at);
    at += window.tl();
    if (best < LOCK_FOLLOW_NCC) {
        followMissed_ = true;   // widen the window for the next scan
        // ...and a picture found nowhere for LOCK_LOST_AFTER scans has left
        // the screen: the lock goes with it
        if (++lockLost_ >= LOCK_LOST_AFTER) {
            hasLastLabel_ = false;
            lockPatch_.release();
            lockLost_ = 0;
            lockMisses_ = 0;
            followOnly_ = 0;
            followMissed_ = false;
        }
        return;
    }

    lockLost_ = 0;
    followMissed_ = false;
    followedNow_ = true;
    lastLabelRect_ = cv::Rect(at.x + search.x, at.y + search.y,
                              lockPatch_.cols, lockPatch_.rows);

    // Finding the plate again IS the confirmation. The value it carries has
    // not changed just because this frame could not be read - the marker is
    // still there, in view, and now the frame is back on top of it. Letting
    // the lock die here throws the search back onto the dashboard every time
    // the light changes.
    lockMisses_ = 0;
    if (++followOnly_ > LOCK_FOLLOW_PATIENCE) {
        hasLastLabel_ = false;
        lockPatch_.release();
        followOnly_ = 0;
    }
}


std::vector<Application::LabelCandidate> Application::captureSignatureCandidates(int maxCandidates) {
    const cv::Rect fixedRect(signatureRegion_.x, signatureRegion_.y,
                             signatureRegion_.width, signatureRegion_.height);

    auto fixedOnly = [&]() -> std::vector<LabelCandidate> {
        lastScanTracked_ = false;
        cv::Mat image = screenCapture_.captureRegion(signatureRegion_);
        if (image.empty()) return {};
        return {{image, fixedRect, cv::Rect(), image, cv::Point()}};
    };

    if (!autoTrack_) {
        return fixedOnly();
    }

    // Always the same rectangle: the whole allowed area. A window that moves
    // with the lock makes every frame incomparable with the last, and
    // comparing them is exactly how the view movement is measured below.
    const cv::Rect bounds = allowedArea();
    CaptureRegion search;
    search.x = bounds.x;
    search.y = bounds.y;
    search.width = bounds.width;
    search.height = bounds.height;
    if (search.width < 40 || search.height < 20) {
        return fixedOnly();
    }

    const auto stageClock = std::chrono::steady_clock::now();
    auto stageMs = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t).count();
    };
    cv::Mat searchImage = screenCapture_.captureRegion(search);
    g_captureMs = stageMs(stageClock);
    // Every frame is wider than the pipe to ffmpeg, so this waits for the
    // encoder on every scan. Worth seeing as its own number: recording is
    // a diagnosis tool, and it should be obvious what it costs to leave on.
    const auto recStart = std::chrono::steady_clock::now();
    recorder_.write(searchImage);
    g_recordMs = stageMs(recStart);
    if (captureStats_.empty()) noteCapture(searchImage);
    if (gui_ && inScan_) gui_->processMessages();   // the capture is in; let the window breathe
    if (searchImage.empty()) {
        lastScanTracked_ = false;
        hasLastLabel_ = false;
        return {};
    }

    // Before anything is located: drag the lock onto wherever the marker has
    // moved to since the last scan. Done here rather than only when the scan
    // comes up empty, because it serves three purposes at once - it keeps the
    // window travelling with the marker through unreadable frames, it gives
    // the candidate ordering below the marker's CURRENT position to measure
    // against, and it covers the case where candidates were found but none of
    // them read. A successful reading overwrites it further down anyway.
    // First how far the view turned - that applies whether or not anything is
    // recognisable this frame - then refine on the marker itself if it can be
    // found. The order matters: the picture match starts from the position the
    // camera movement already corrected.
    const auto trackStart = std::chrono::steady_clock::now();
    trackCameraMotion(searchImage);
    followLockedMarker(searchImage, search);
    g_trackMs = stageMs(trackStart);
    if (blacklistLeft_ > 0) blacklistLeft_--;

    // How much of the captured area the locator is handed - worked out after
    // the tracking, so that a lock stands where the marker is now. Everything
    // on every WIDE_SWEEP_EVERY-th scan, and whenever the follow lost the
    // marker this scan. Otherwise:
    //  - nothing read for a while and no lock: the area around the aim box -
    //    see NEAR_SWEEP_AFTER;
    //  - a marker locked and found again: the area around the aim box and
    //    around the marker. Searching the whole area here costs 115 ms of a
    //    170 ms scan for the one marker the lock already has; a new one comes
    //    up at the aim box, where the player turns to it, and one anywhere
    //    else is picked up by the next wide sweep.
    cv::Rect lookIn(0, 0, searchImage.cols, searchImage.rows);
    {
        const double quiet = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_lastFind).count();
        static int sweepTick = 0;
        const bool wide = (++sweepTick % WIDE_SWEEP_EVERY) == 0;
        // Not "near": <windows.h> still defines that as nothing at all
        const cv::Rect aimArea(signatureRegion_.x - search.x - NEAR_SWEEP_MARGIN,
                               signatureRegion_.y - search.y - NEAR_SWEEP_MARGIN,
                               signatureRegion_.width + 2 * NEAR_SWEEP_MARGIN,
                               signatureRegion_.height + 2 * NEAR_SWEEP_MARGIN);
        cv::Rect around;
        if (!wide && !hasLastLabel_ && quiet > NEAR_SWEEP_AFTER) {
            around = aimArea & lookIn;
        } else if (!wide && hasLastLabel_ && followedNow_) {
            const cv::Rect lockArea(lastLabelRect_.x - search.x - NEAR_SWEEP_MARGIN,
                                    lastLabelRect_.y - search.y - NEAR_SWEEP_MARGIN,
                                    lastLabelRect_.width + 2 * NEAR_SWEEP_MARGIN,
                                    lastLabelRect_.height + 2 * NEAR_SWEEP_MARGIN);
            around = (aimArea | lockArea) & lookIn;
        }
        if (around.width > 40 && around.height > 20) lookIn = around;
    }

    if (gui_ && inScan_) gui_->processMessages();   // ...and again after the tracking, before the locator
    const auto locStart = std::chrono::steady_clock::now();
    const cv::Mat lookImage = (lookIn.width == searchImage.cols && lookIn.height == searchImage.rows)
                                  ? searchImage : searchImage(lookIn);
    auto labels = screenCapture_.locateSignatureLabels(lookImage, maxCandidates);
    g_locateMs = stageMs(locStart);

    if (labels.empty()) {
        // Nothing in view right now. Keep the lock for a while: a marker
        // blinks out for a frame all the time, and letting go immediately
        // sends the search back to the wide area, where it picks up chat text.
        // Only a run of empty scans really means the marker is gone.
        lastScanTracked_ = false;
        if (hasLastLabel_ && ++lockMisses_ > LOCK_PATIENCE) {
            hasLastLabel_ = false;
            lockMisses_ = 0;
            lockPatch_.release();
        }
        cv::Rect local(signatureRegion_.x - search.x, signatureRegion_.y - search.y,
                       signatureRegion_.width, signatureRegion_.height);
        local &= cv::Rect(0, 0, searchImage.cols, searchImage.rows);
        if (local.width > 4 && local.height > 4) {
            return {{searchImage(local).clone(), fixedRect, cv::Rect(), searchImage, local.tl()}};
        }
        return fixedOnly();
    }

    // Several rocks can be marked at once, and other warm HUD elements can
    // look like markers. Order by plausibility: prefer the label the previous
    // scan used (stable readings while the view drifts), then the one closest
    // to the aim point the user configured.
    const cv::Point aim(signatureRegion_.x + signatureRegion_.width / 2 - search.x - lookIn.x,
                        signatureRegion_.y + signatureRegion_.height / 2 - search.y - lookIn.y);
    const cv::Point previous(hasLastLabel_
                                 ? lastLabelRect_.x + lastLabelRect_.width / 2 - search.x - lookIn.x
                                 : aim.x,
                             hasLastLabel_
                                 ? lastLabelRect_.y + lastLabelRect_.height / 2 - search.y - lookIn.y
                                 : aim.y);

    std::sort(labels.begin(), labels.end(),
              [&](const cv::Rect& a, const cv::Rect& b) {
                  auto cost = [&](const cv::Rect& r) {
                      const cv::Point c(r.x + r.width / 2, r.y + r.height / 2);
                      const double toAim = cv::norm(c - aim);
                      const double toPrev = hasLastLabel_ ? cv::norm(c - previous) : toAim;
                      // Weight continuity higher than the configured aim point
                      return toPrev * 0.7 + toAim * 0.3;
                  };
                  return cost(a) < cost(b);
              });

    std::vector<LabelCandidate> candidates;
    for (const auto& label : labels) {
        if (static_cast<int>(candidates.size()) >= maxCandidates) break;
        const cv::Rect onScreen(label.x + lookIn.x + search.x, label.y + lookIn.y + search.y,
                                label.width, label.height);
        // Outside the marked region it is not a signature, whatever it looks
        // like. Checked per candidate as well as per window, because the
        // window can be widened past the boundary while chasing a head turn.
        if ((onScreen & bounds).area() < onScreen.area()) continue;
        // Skip whatever the scan proved to be cockpit furniture, until the
        // ban runs out - otherwise the very next sweep locks onto it again
        if (blacklistLeft_ > 0 && (onScreen & blacklistRect_).area() > 0) continue;
        cv::Rect row;
        if (auto r = screenCapture_.rowOf(label)) {
            row = cv::Rect(r->x - label.x, r->y - label.y, r->width, r->height);
        }
        candidates.push_back({lookImage(label).clone(), onScreen, row, lookImage, label.tl()});
    }

    lastScanTracked_ = !candidates.empty();
    return candidates;
}

cv::Mat Application::captureSignatureImage(cv::Rect* outScreenRect) {
    auto candidates = captureSignatureCandidates(1);
    if (candidates.empty()) {
        return {};
    }

    if (lastScanTracked_) {
        lastLabelRect_ = candidates.front().screenRect;
        hasLastLabel_ = true;
        lockPatch_ = candidates.front().image.clone();
    }
    if (outScreenRect) *outScreenRect = candidates.front().screenRect;
    return candidates.front().image;
}

void Application::processScan() {
    std::cout << "\nScanning the screen..." << std::endl;

    cv::Rect takenFrom;
    cv::Mat signatureImage = captureSignatureImage(&takenFrom);

    if (signatureImage.empty()) {
        std::cerr << "Error: could not capture the screen!" << std::endl;
        return;
    }

    if (autoTrack_) {
        std::cout << (lastScanTracked_
                          ? "Label located at " + std::to_string(takenFrom.x) + "," +
                                std::to_string(takenFrom.y)
                          : "No label in the search area - using the fixed region")
                  << std::endl;
    }

    // Prepare for OCR
    auto candidates = screenCapture_.preprocessCandidates(signatureImage);

    // Recognize the number
    auto recognizedValue = ocrEngine_.recognizeBest(
        candidates, readingPolicy());

    if (!recognizedValue) {
        std::cout << "No number recognized. Try manual input (key 'M')." << std::endl;
        return;
    }

    int scannedValue = *recognizedValue;
    std::cout << "Recognized signature: " << scannedValue
              << " (confidence: " << ocrEngine_.getLastConfidence() << "%)" << std::endl;

    // Run the analysis
    auto results = analyzer_.analyzeSignature(scannedValue);

    // Show the results
    displayResults(results, scannedValue);

    if (resultCallback_) {
        resultCallback_(results);
    }
}

void Application::displayResults(const std::vector<AnalysisResult>& results, int scannedValue) {
    std::string report = analyzer_.generateReport(scannedValue);
    std::cout << report << std::endl;
}

void Application::displayResultsCompact(const std::vector<AnalysisResult>& results, int scannedValue) {
    if (results.empty()) return;

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time_t);

    char timeStr[12];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

    // Compact one-line output
    std::cout << "[" << timeStr << "] Signature: " << scannedValue << " -> ";

    // Top 3 results
    int count = 0;
    for (const auto& r : results) {
        if (count > 0) std::cout << " | ";
        std::cout << r.name << " x" << r.clusterSize << " "
                  << std::fixed << std::setprecision(0) << r.probability << "%";
        if (++count >= 3) break;
    }

    // Highlight a clear best match
    if (!results.empty() && results[0].probability > 50.0) {
        std::cout << " << " << results[0].name << " x" << results[0].clusterSize;
    }

    std::cout << std::endl;
}

void Application::processScanLive() {
    cv::Mat signatureImage = captureSignatureImage();

    if (signatureImage.empty()) {
        return;  // fail silently in live mode
    }

    // Prepare for OCR
    auto candidates = screenCapture_.preprocessCandidates(signatureImage);

    // Recognize the number
    auto recognizedValue = ocrEngine_.recognizeBest(
        candidates, readingPolicy());

    if (!recognizedValue) {
        return;  // nothing recognized, skip
    }

    int scannedValue = *recognizedValue;

    // Only report when the value changed
    if (scannedValue == lastRecognizedSignature_) {
        return;  // same signature, do not repeat
    }

    // New signature detected!
    lastRecognizedSignature_ = scannedValue;

    // Run the analysis
    auto results = analyzer_.analyzeSignature(scannedValue);

    // Show compact results
    displayResultsCompact(results, scannedValue);

    if (resultCallback_) {
        resultCallback_(results);
    }
}

void Application::setupHotkey() {
    // Register the hotkey (F9)
    if (!RegisterHotKey(nullptr, 1, 0, hotkeyVkCode_)) {
        std::cerr << "Warning: could not register the hotkey." << std::endl;
    }
}

#ifdef _WIN32
LRESULT CALLBACK Application::KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* kbStruct = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (g_appInstance && kbStruct->vkCode == static_cast<DWORD>(g_appInstance->hotkeyVkCode_)) {
            g_appInstance->triggerScan();
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
#endif

// What changed, put up once on the first start after an update. The marker
// lives beside the player's things, not beside the exe: the program folder
// is what an update replaces, and a note that reappeared after every update
// would be exactly the thing it is trying not to be.
//
// A first-ever run is not an update. Nothing is new about a program someone
// is opening for the first time, so the first start only writes the marker.
// region_config.txt is the tell - it is written as soon as anything is
// changed or the app is closed, so anyone who has used Bedrock before has one.
void Application::maybeShowWhatsNew() {
    if (!gui_) return;
    const std::string marker = dataPath("seen_version.txt");
    std::string seen;
    {
        std::ifstream in(marker);
        if (in.is_open()) std::getline(in, seen);
    }
    while (!seen.empty() && (seen.back() == '\r' || seen.back() == ' '))
        seen.pop_back();
    if (seen == BEDROCK_VERSION) return;

    std::error_code ec;
    const bool firstEverRun =
        seen.empty() && !std::filesystem::exists(dataPath(REGION_CONFIG_FILE), ec);
    {
        std::ofstream out(marker, std::ios::trunc);
        if (out.is_open()) out << BEDROCK_VERSION << std::endl;
    }
    if (firstEverRun) return;
    // Coming from anything before 2.5 (1.x, 2.0.x, or a build older than the
    // marker): the news window, since the rock view and the game's contract
    // texts change what the window is for and a scrolling card would bury
    // them. From 2.5 on: the What's New card.
    int major = 0, minor = 0;
    {
        size_t at = 0;
        while (at < seen.size() && std::isdigit((unsigned char)seen[at])) major = major * 10 + (seen[at++] - '0');
        if (at < seen.size() && seen[at] == '.') at++;
        while (at < seen.size() && std::isdigit((unsigned char)seen[at])) minor = minor * 10 + (seen[at++] - '0');
    }
    const bool before25 = seen.empty() || major < 2 || (major == 2 && minor < 5);
    if (before25) gui_->showNews();
    else gui_->showWhatsNew();
}

void Application::saveRegionConfig() {
    // After a restore the file is the backup's until the next start
    if (settingsHeld_) return;
    // Relative to the working directory (main() sets it to the exe folder)
    std::ofstream file(dataPath(REGION_CONFIG_FILE));
    if (file.is_open()) {
        file << "x=" << signatureRegion_.x << std::endl;
        file << "y=" << signatureRegion_.y << std::endl;
        file << "width=" << signatureRegion_.width << std::endl;
        file << "height=" << signatureRegion_.height << std::endl;
        file << "autotrack=" << (autoTrack_ ? 1 : 0) << std::endl;
        file << "trackmargin=" << trackMargin_ << std::endl;
        // Written for the reader above; the box is automatic unless this
        // file says otherwise on purpose
        file << "fixedaim=" << (aimAuto_ ? 0 : 1) << std::endl;
        // The frame on screen, off unless asked for - see overlayEnabled_
        file << "overlay=" << (overlayEnabled_ ? 1 : 0) << std::endl;
        file << "record=" << (recordScans_ ? 1 : 0) << std::endl;
        file << "ease=" << (easeIdle_ ? 1 : 0) << std::endl;
        file << "threads=" << scanThreads_ << std::endl;
        file << "keeps=" << maxKeeps_ << std::endl;
        file << "snapshots=" << snapshotKeep_ << std::endl;
        file << "keepgb=" << maxKeepGb_ << std::endl;
        // 100: pictures are read as captured (measured best); 0: the HUD scale
        // follows the game picture (1.5 on 4K); another number forces it
        file << "hudscale=" << hudScalePercent_ << std::endl;
        // How big the app draws itself, as set with Ctrl+plus / Ctrl+minus
        file << "uiscale=" << uiScalePercent_ << std::endl;
        // gamelog=<path to Game.log> pins the log the blueprint library
        // reads; normally it is found from the running game or the launcher
        if (!gameLogOverride_.empty()) file << "gamelog=" << gameLogOverride_ << std::endl;
        // Whether the scan was running when the app was last closed. A restart
        // that comes back idle looks identical to one that is working, and the
        // difference only shows up much later - in a recording that turns out
        // to be minutes old.
        file << "live=" << (liveModeActive_ ? 1 : 0) << std::endl;
        // Written back, not only read: this file is rewritten on every close, so
        // a value set by hand (update=0) would otherwise last one session.
        file << "update=" << (updateCheck_ ? 1 : 0) << std::endl;
        file << "gametexts=" << (gameTextsWanted_ ? 1 : 0) << std::endl;
        // sharerefinery=0: the box in the Refinery window unticked - see
        // RefineryShare. On until then; an earlier test build's
        // "refineryshare" was off before anyone could choose, and is let go.
        file << "sharerefinery=" << (refineryShare_ ? 1 : 0) << std::endl;
        // uex=0: the Ore Finder never asks uexcorp.space for prices
        file << "uex=" << (uexWanted_ ? 1 : 0) << std::endl;
        // Reporting to UEX: 0, or when the player agreed to it (on a UEX
        // card); the key is not here but in uex_key.dat
        file << "uexreport=" << uexReportSince_ << std::endl;
        // ...and whether the first start's offer of it was made
        file << "uexoffered=" << (uexOffered_ ? 1 : 0) << std::endl;
        if (!updateUrl_.empty()) file << "updateurl=" << updateUrl_ << std::endl;
        // The viewport the aim point was picked in. The point was chosen by
        // eye against the HUD, and the HUD is laid out in the viewport, not on
        // the desktop - so noting the viewport lets the point be carried over
        // when the game is later drawn at a different size or place.
        if (auto viewport = ScreenCapture::gameViewport()) {
            file << "viewx=" << viewport->x << std::endl;
            file << "viewy=" << viewport->y << std::endl;
            file << "vieww=" << viewport->width << std::endl;
            file << "viewh=" << viewport->height << std::endl;
        }
    }
    mirrorConfigToDebug();
}

void Application::loadRegionConfig() {
    std::ifstream file(dataPath(REGION_CONFIG_FILE));
    cv::Rect storedViewport;   // the viewport the saved aim point belongs to
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                // The one key whose value is a path, not a number
                if (key == "gamelog") {
                    gameLogOverride_ = line.substr(pos + 1);
                    continue;
                }
                if (key == "updateurl") {
                    updateUrl_ = line.substr(pos + 1);
                    continue;
                }
                // a Unix second, past what an int holds in 2038
                if (key == "uexreport") {
                    try {
                        uexReportSince_ = std::stoll(line.substr(pos + 1));
                    } catch (const std::exception&) {
                        uexReportSince_ = 0;
                    }
                    continue;
                }
                int value = 0;
                try {
                    value = std::stoi(line.substr(pos + 1));
                } catch (const std::exception&) {
                    continue;
                }

                if (key == "x") signatureRegion_.x = value;
                else if (key == "y") signatureRegion_.y = value;
                else if (key == "width") signatureRegion_.width = value;
                else if (key == "height") signatureRegion_.height = value;
                // "autotrack" is read for compatibility and ignored: tracking is
                // always on.
                else if (key == "autotrack") autoTrack_ = true;
                else if (key == "trackmargin") trackMargin_ = std::clamp(value, 50, 2000);
                // "aimauto" is read and dropped: an older file can hold a manual
                // setting with no button left to switch it back. The automatic box
                // is the right one.
                else if (key == "aimauto") { /* ignored */ }
                // A fixed box is still possible, but it has to be asked for
                // on purpose: fixedaim=1 with x/y/width/height below.
                else if (key == "fixedaim") aimAuto_ = (value == 0);
                else if (key == "overlay") overlayEnabled_ = (value != 0);
                // record=0 turns the rolling recording off. It is lossless and
                // therefore large - about 90 MB per minute of scanning, ~460 MB
                // for the full ring of five.
                else if (key == "record") recordScans_ = (value != 0);
                // ease=0 makes the scan run flat out. threads= caps how much of the
                // machine one scan may take at once; 0 works it out from the core count.
                else if (key == "ease") easeIdle_ = (value != 0);
                else if (key == "threads") scanThreads_ = std::clamp(value, 0, 64);
                else if (key == "keeps") maxKeeps_ = std::clamp(value, 1, 1000);
                else if (key == "snapshots") snapshotKeep_ = std::clamp(value, 1, 1000);
                else if (key == "keepgb") maxKeepGb_ = std::clamp(value, 1, 10000);
                else if (key == "live") startLive_ = (value != 0);
                else if (key == "update") updateCheck_ = (value != 0);
                else if (key == "uex") uexWanted_ = (value != 0);
                else if (key == "uexoffered") uexOffered_ = (value != 0);
                // gametexts=1: the box in the Blueprints window - see GameTexts
                else if (key == "gametexts") gameTextsWanted_ = (value != 0);
                else if (key == "sharerefinery") refineryShare_ = (value != 0);
                else if (key == "hudscale") hudScalePercent_ = std::clamp(value, 0, 500);
                else if (key == "uiscale") uiScalePercent_ = std::clamp(value, 0, 250);
                else if (key == "viewx") storedViewport.x = value;
                else if (key == "viewy") storedViewport.y = value;
                else if (key == "vieww") storedViewport.width = value;
                else if (key == "viewh") storedViewport.height = value;
            }
        }
        // Carry the aim point into the viewport the game is drawing right now.
        // Absolute desktop coordinates picked in a 5120x1440 session mean
        // nothing in a 2560x1440 one, or after the game moved to another
        // monitor; the same fraction of the viewport still points at the same
        // spot on the HUD.
        // A typed box is carried over to a viewport of another size; an
        // automatic one is simply made again for it
        if (!aimAuto_ && storedViewport.width > 0 && storedViewport.height > 0) {
            if (auto viewport = ScreenCapture::gameViewport()) {
                if (*viewport != storedViewport) {
                    const double sx = static_cast<double>(viewport->width) / storedViewport.width;
                    const double sy = static_cast<double>(viewport->height) / storedViewport.height;
                    signatureRegion_.x = viewport->x + static_cast<int>(std::lround(
                        (signatureRegion_.x - storedViewport.x) * sx));
                    signatureRegion_.y = viewport->y + static_cast<int>(std::lround(
                        (signatureRegion_.y - storedViewport.y) * sy));
                    signatureRegion_.width = (std::max)(20, static_cast<int>(std::lround(
                        signatureRegion_.width * sx)));
                    signatureRegion_.height = (std::max)(10, static_cast<int>(std::lround(
                        signatureRegion_.height * sy)));
                    std::cout << "Viewport changed (" << storedViewport.width << "x"
                              << storedViewport.height << " -> " << viewport->width << "x"
                              << viewport->height << ") - aim point carried over" << std::endl;
                }
            }
        }

        std::cout << "Region loaded: " << signatureRegion_.x << "," << signatureRegion_.y
                  << " (" << signatureRegion_.width << "x" << signatureRegion_.height << ")" << std::endl;
    } else {
        // Defaults when no config exists yet
        signatureRegion_.x = 100;
        signatureRegion_.y = 100;
        signatureRegion_.width = 200;
        signatureRegion_.height = 50;
    }
}

// The picture the game is drawn in: its window when it is running, otherwise
// the monitor under the aim box - the box was made for that monitor too
static cv::Rect pictureFor(const CaptureRegion& aim) {
    if (auto viewport = ScreenCapture::gameViewport()) return *viewport;
    const POINT centre{aim.x + aim.width / 2, aim.y + aim.height / 2};
    if (HMONITOR monitor = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfo(monitor, &info)) {
            return cv::Rect(info.rcMonitor.left, info.rcMonitor.top,
                            info.rcMonitor.right - info.rcMonitor.left,
                            info.rcMonitor.bottom - info.rcMonitor.top);
        }
    }
    return cv::Rect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

bool Application::refreshHudScale() {
    const cv::Rect picture = pictureFor(signatureRegion_);
    double scale = picture.height > 0
        ? static_cast<double>(picture.height) / GlyphReader::REFERENCE_VIEWPORT_HEIGHT
        : 1.0;
    if (hudScalePercent_ > 0) scale = hudScalePercent_ / 100.0;   // forced by the config
    if (std::abs(scale - hudScale_) < 0.005 && std::abs(GlyphReader::hudScale() - scale) < 0.005) {
        return false;
    }
    hudScale_ = scale;
    GlyphReader::setHudScale(scale);
    // The lock patch was cut at the old scale
    hasLastLabel_ = false;
    lockPatch_.release();
    std::ostringstream line;
    line << "HUD scale " << std::fixed << std::setprecision(2) << scale << " (picture "
         << picture.width << "x" << picture.height << ", reference "
         << GlyphReader::REFERENCE_VIEWPORT_HEIGHT << " px tall)";
    std::cout << line.str() << std::endl;
    return true;
}

static const char* sourceName(ScreenCapture::Source source) {
    switch (source) {
        case ScreenCapture::Source::DxgiHdr: return "Desktop Duplication, HDR (tone mapped in the app)";
        case ScreenCapture::Source::Dxgi: return "Desktop Duplication, SDR";
        case ScreenCapture::Source::Gdi: return "GDI BitBlt (duplication unavailable)";
        case ScreenCapture::Source::Composite: return "the game's X11 window (Composite)";
        default: return "nothing captured yet";
    }
}

static std::string timeStamp(const char* format) {
    char stamp[40] = {};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), format, &local);
    return stamp;
}

// Everything a scan on this machine works from, for the session file and
// the snapshot note
std::filesystem::path Application::locateGameLog() const {
    std::error_code ec;
    // 1. pinned by hand
    if (!gameLogOverride_.empty()) {
        std::filesystem::path pinned(gameLogOverride_);
        if (std::filesystem::exists(pinned, ec)) return pinned;
    }
#ifdef _WIN32
    // 2. the running game: its exe sits in <channel>\Bin64, the log one up
    if (HWND window = ScreenCapture::gameWindow()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t exe[1024] = {};
            DWORD length = 1024;
            std::filesystem::path log;
            if (QueryFullProcessImageNameW(process, 0, exe, &length)) {
                log = std::filesystem::path(exe).parent_path().parent_path() / "Game.log";
            }
            CloseHandle(process);
            if (!log.empty() && std::filesystem::exists(log, ec)) return log;
        }
    }
    // 3. the launcher's log names the folder it last started the game from:
    //    "[Launcher::launch] Launching Star Citizen LIVE from (E:\\RSI\\StarCitizen\\LIVE)"
    char appdata[1024] = {};
    if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) > 0) {
        std::ifstream in(std::filesystem::path(appdata) / "rsilauncher" / "logs" / "log.log");
        std::string line, folder;
        while (std::getline(in, line)) {
            const size_t k = line.find("Launching Star Citizen ");
            if (k == std::string::npos) continue;
            const size_t a = line.find(" from (", k);
            const size_t b = (a == std::string::npos) ? a : line.find(')', a);
            if (a == std::string::npos || b == std::string::npos) continue;
            const std::string raw = line.substr(a + 7, b - a - 7);
            std::string path;
            for (size_t i = 0; i < raw.size(); i++) {
                if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') i++;   // JSON doubles them
                path += raw[i];
            }
            folder = path;   // the last launch wins
        }
        if (!folder.empty()) {
            const std::filesystem::path log = std::filesystem::path(folder) / "Game.log";
            if (std::filesystem::exists(log, ec)) return log;
        }
    }
#else
    // 2. Linux: the game runs in a Wine prefix, and the prefix is wherever
    //    Steam, Lutris or Bottles put it. The usual places, and of the logs
    //    found the one written to last - the one the running game writes.
    //    gamelog= in region_config.txt names it outright (step 1 above).
    const char* home = std::getenv("HOME");
    if (home && *home) {
        const std::filesystem::path h(home);
        const std::filesystem::path inPrefix =
            std::filesystem::path("drive_c") / "Program Files" / "Roberts Space Industries" / "StarCitizen";
        std::vector<std::filesystem::path> prefixes;
        for (const auto& steam : {h / ".steam" / "steam" / "steamapps" / "compatdata",
                                  h / ".local" / "share" / "Steam" / "steamapps" / "compatdata",
                                  h / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam" / "steamapps" / "compatdata"}) {
            for (const auto& entry : std::filesystem::directory_iterator(steam, ec)) {
                if (entry.is_directory(ec)) prefixes.push_back(entry.path() / "pfx");
            }
        }
        for (const auto& lutris : {h / "Games", h / ".local" / "share" / "lutris" / "runners" / "wine"}) {
            for (const auto& entry : std::filesystem::directory_iterator(lutris, ec)) {
                if (entry.is_directory(ec)) prefixes.push_back(entry.path());
            }
        }
        for (const auto& bottles : {h / ".var" / "app" / "com.usebottles.bottles" / "data" / "bottles" / "bottles",
                                    h / ".local" / "share" / "bottles" / "bottles"}) {
            for (const auto& entry : std::filesystem::directory_iterator(bottles, ec)) {
                if (entry.is_directory(ec)) prefixes.push_back(entry.path());
            }
        }
        prefixes.push_back(h / ".wine");
        std::filesystem::path newest;
        std::filesystem::file_time_type newestTime{};
        for (const auto& prefix : prefixes) {
            for (const char* channel : {"LIVE", "PTU", "EPTU", "HOTFIX", "TECH-PREVIEW"}) {
                const std::filesystem::path log = prefix / inPrefix / channel / "Game.log";
                if (!std::filesystem::exists(log, ec)) continue;
                const auto written = std::filesystem::last_write_time(log, ec);
                if (newest.empty() || written > newestTime) {
                    newest = log;
                    newestTime = written;
                }
            }
        }
        if (!newest.empty()) return newest;
    }
#endif
    return {};
}

// "STANTON" -> "tanton", for the place line's "Stanton - Daymar"
static std::string toLowerRest(const std::string& word) {
    std::string rest = word.size() > 1 ? word.substr(1) : std::string();
    for (char& c : rest) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return rest;
}

// "2026-09-27T09:27:53": a moment in the spelling the game log uses, UTC
static std::string utcStamp(std::chrono::system_clock::time_point at) {
    using namespace std::chrono;
    const auto secs = floor<seconds>(at);
    const auto day = floor<days>(secs);
    const year_month_day ymd{day};
    const hh_mm_ss<seconds> tod{secs - day};
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%04d-%02u-%02uT%02d:%02d:%02d", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                  static_cast<int>(tod.hours().count()), static_cast<int>(tod.minutes().count()),
                  static_cast<int>(tod.seconds().count()));
    return text;
}

// What Game.log says, into the app: the system onto the buttons and the
// filter, the ship into the header and the scan log. The log sets the
// system when IT changes; a button pressed by hand holds until then.
void Application::loadPlaceOverride() {
    std::ifstream in(AppPaths::file(PLACE_OVERRIDE_FILE));
    if (!in) return;
    std::string name, stamp;
    std::getline(in, name);
    std::getline(in, stamp);
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
    while (!stamp.empty() && (stamp.back() == '\r' || stamp.back() == ' ')) stamp.pop_back();
    // Only a place the tables still know: one renamed or dropped between
    // versions would otherwise pin the app to something that does not exist.
    if (!name.empty() && !placeByLogName(name, "").known()) return;
    placeOverride_ = name;
    // When it was set: the second line. Older files have the name alone,
    // and the file's own time on disk stands in for it.
    if (!name.empty() && (stamp.size() != 19 || stamp[10] != 'T')) {
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(AppPaths::file(PLACE_OVERRIDE_FILE), ec);
#ifdef _WIN32
        stamp = ec ? std::string() : utcStamp(std::chrono::clock_cast<std::chrono::system_clock>(written));
#else
        // clock_cast is GCC 13's; the Linux package is built with GCC 11,
        // whose file_clock knows the way to system time itself
        stamp = ec ? std::string() : utcStamp(std::chrono::file_clock::to_sys(written));
#endif
    }
    placeOverrideStamp_ = name.empty() ? std::string() : stamp;
    if (gui_) gui_->setPlaceOverride(placeOverride_);
}

void Application::setPlaceOverride(const std::string& placeName) {
    placeOverride_ = placeName;
    placeOverrideStamp_ = placeName.empty() ? std::string() : utcStamp(std::chrono::system_clock::now());
    std::ofstream out(AppPaths::file(PLACE_OVERRIDE_FILE), std::ios::trunc);
    if (out) out << placeOverride_ << "\n" << placeOverrideStamp_ << "\n";
    placeLine_.clear();       // force the block below to recompute and repaint
    placeOre_ = "\x01";
    applyGameState();
    std::cout << "\nPlace set by hand: "
              << (placeOverride_.empty() ? "no - following the game log" : placeOverride_)
              << std::endl;
}

void Application::applyGameState() {
    // A place set by hand holds until the game itself says otherwise: a
    // line written AFTER the setting that names another place the tables
    // know, or another system, hands the place back to the log. Otherwise a
    // stale setting keeps showing the wrong place and filters the ores by
    // it. A line written before the setting says nothing: it is what the
    // setting overruled.
    if (!placeOverride_.empty() && !gameState_.namedStamp().empty() &&
        gameState_.namedStamp() > placeOverrideStamp_ &&
        contradictsHandPlace(placeOverride_, gameState_.namedPlace(), gameState_.namedSystem())) {
        std::cout << "\nPlace set by hand (" << placeOverride_ << ") handed back to the game log, which put the ship "
                  << (gameState_.namedPlace().empty() ? "in " + gameState_.namedSystem()
                                                      : "at " + gameState_.namedPlace())
                  << " at " << gameState_.namedStamp() << " UTC" << std::endl;
        placeOverride_.clear();
        placeOverrideStamp_.clear();
        std::ofstream out(AppPaths::file(PLACE_OVERRIDE_FILE), std::ios::trunc);
        if (gui_) gui_->setPlaceOverride("");
    }
    // The log spells the system "Nyx", the buttons spell it "NYX", and the
    // app compares against the buttons' spelling - whether a button is lit
    // is a plain string compare. So the buttons' spelling it is; with the
    // log's, the filter works but no button lights.
    std::string system = gameState_.system();
    for (char& c : system) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!system.empty() && system != logSystem_) {
        logSystem_ = system;
        if (system != currentSystem_) {
            currentSystem_ = system;
            analyzer_.setSystemFilter(system);
            std::cout << "\nSystem from Game.log: " << system << std::endl;
            if (gui_) {
                gui_->setCurrentSystem(system);
                if (lastRecognizedSignature_ > 0) {
                    auto results = analyzer_.analyzeSignature(lastRecognizedSignature_);
                    gui_->updateResults(lastRecognizedSignature_, results, -1.0, analyzer_.outOfPlace());
                }
            }
        }
    }
    // Where exactly, for the header, the scan log and the ore filter: the
    // place the log names, with the ore tables' location under it when
    // they have one (GamePlaces). A system button pressed by hand lifts
    // the place filter until the log names a new place - the button is
    // the wider choice, and the player made it on purpose.
    {
        // A place set by hand wins over the log, and over the system the log
        // last named with it: someone who says "I am at Lyria" has said
        // Stanton too. Nothing else about the log is switched off - the ship,
        // the seat and the system all keep coming from it.
        const bool byHand = !placeOverride_.empty();
        const GamePlace place = byHand
            ? placeByLogName(placeOverride_, gameState_.system())
            : placeByLogName(gameState_.place(), gameState_.system());
        // A place that knows its own system settles the question, by hand or
        // not: "Nyx - Bloom" is not a place, and the half that is certain is
        // the one the tables can name.
        const std::string shownSystem = !place.system.empty() ? place.system : system;
        const std::string pretty = shownSystem.empty()
            ? std::string()
            : shownSystem.substr(0, 1) + toLowerRest(shownSystem);
        std::string line;
        if (place.known()) {
            line = pretty.empty() ? place.name : pretty + " - " + place.name;
        } else if (!pretty.empty()) {
            // Out in open space between the bodies - which is what a route
            // starting from the system itself means. The system is all the
            // log says, and saying it beats the line vanishing; when the log
            // has actually said "open space", the line says so too.
            line = gameState_.inSpace() ? pretty + " - open space" : pretty;
        }
        if (byHand && !line.empty()) line += " (set by hand)";
        const std::string ore = place.known() ? place.oreLocation : std::string();
        if (line != placeLine_ || ore != placeOre_) {
            placeLine_ = line;
            placeOre_ = ore;
            g_place = line;
            analyzer_.setPlaceFilter(ore);
            // The ground under the ship: a body, open space, or no idea. A
            // station or a belt the tables know counts as space too.
            using Ground = SignatureAnalyzer::Ground;
            const Ground ground = place.known()
                ? (place.surface == 1 ? Ground::Surface : place.surface == 0 ? Ground::Space : Ground::Unknown)
                : (gameState_.inSpace() ? Ground::Space : Ground::Unknown);
            analyzer_.setGround(ground);
            if (gui_) {
                gui_->setPlace(line, analyzer_.placeFilter(), ground == Ground::Space);
                if (lastRecognizedSignature_ > 0) {
                    auto results = analyzer_.analyzeSignature(lastRecognizedSignature_);
                    gui_->updateResults(lastRecognizedSignature_, results, -1.0, analyzer_.outOfPlace());
                }
            }
            std::cout << "\nYou are at: " << (line.empty() ? "unknown" : line)
                      << (analyzer_.placeFilter().empty() ? "" : " - ores of " + analyzer_.placeFilter() + " only")
                      << std::endl;
        }
    }
    const std::string label = gameState_.shipLabel();
    g_ship = GameState::model(gameState_.ship());
    g_shipLabel = label;
    // Hulls lost with the player in them, before the ship: a replacement the
    // same lines name is then already waited for
    const auto& lost = gameState_.lostHulls();
    if (lost.size() < lostHullsPassed_) lostHullsPassed_ = 0;   // a new log, read from the start
    for (; lostHullsPassed_ < lost.size(); lostHullsPassed_++) {
        if (gui_) gui_->shipLost(lost[lostHullsPassed_]);
    }
    if (gui_) gui_->setShip(g_ship, gameState_.inSeat(), gameState_.hull());
    // Sat down, got up, boarded another: the loadout is read on the next look,
    // live mode on or not. Left to the looks alone, a ship change made with
    // live mode off could leave the last ship's reading settled for good.
    const std::string logSeat = g_ship + "|" + gameState_.hull() + "|" + (gameState_.inSeat() ? "in" : "out") + "|" +
                                std::to_string(gameState_.boardings());
    if (logSeat != loadoutLogSeat_) {
        loadoutLogSeat_ = logSeat;
        loadoutLookAgain();
    }
    const std::string announce = label.empty() ? "" : label + (gameState_.inSeat() ? "" : " (left the seat)");
    if (announce != logShipLabel_) {
        logShipLabel_ = announce;
        std::cout << "\nShip from Game.log: " << (announce.empty() ? "unknown" : announce) << std::endl;
    }
}

// The REFINEMENT CENTER, read off the screen (see Application.h). Not tied
// to live mode: the player at a refinery has put the ship away, and the
// terminal is the one thing on screen worth reading then.
void Application::lookForRefinery() {
    if (!gui_ || !textLines_.ready()) return;
    if (refineryJob_.valid()) {
        if (refineryJob_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        RefineryScreen read;
        try {
            read = refineryJob_.get();
        } catch (const std::exception& e) {
            std::cerr << "[refinery] reading failed: " << e.what() << std::endl;
        }
        takeRefineryScreen(read, refineryJobStation_, refineryJobTaken_);
    }
    // Where a terminal can be used at all: at a station the log names - or
    // on foot anywhere, since the log may name only the system (logged in at
    // Levski, it says "Nyx" and no place). In a seat no terminal is in reach,
    // and the scan has the picture to itself.
    const std::string station = refineryStationAt(gameState_.place(), gameState_.system());
    if (station.empty() && gameState_.inSeat()) return;
    const auto now = std::chrono::steady_clock::now();
    // ...and not while mining: a rock's panel on screen in the last half
    // minute says the player is at a rock, not at a terminal
    if (now - lastRockPanel_ < std::chrono::seconds(30)) return;
    // Twice a second while the terminal is up: a quote stands on it for a
    // second or two before CONFIRM takes it away
    if (now - lastRefineryLook_ < std::chrono::milliseconds(refineryUp_ ? 500 : 1000)) return;
    lastRefineryLook_ = now;
    auto viewport = ScreenCapture::gameViewport();
    if (!viewport) return;
    CaptureRegion whole;
    whole.x = viewport->x;
    whole.y = viewport->y;
    whole.width = viewport->width;
    whole.height = viewport->height;
    whole.autoDetect = false;
    const cv::Mat frame = screenCapture_.captureRegion(whole);
    if (frame.empty()) return;
    std::vector<RefineryBar> bars = findRefineryBars(frame);
    refineryUp_ = !bars.empty();
    // "WORK ORDER DELIVERED" is looked for while an order is done: over the
    // panels, or - the last one collected - over an empty list, for half a
    // minute after the last panels
    const long long taken = (long long)std::time(nullptr);
    const bool notice = refineryOrders_.anyDone(taken);
    const bool atTerminal = now - refineryLastPanels_ < std::chrono::seconds(30);
    // ...twice a second then too: over an empty list it stands two or three
    // seconds, and two looks have to see it
    if (notice && atTerminal) refineryUp_ = true;
    if (bars.empty() && !(notice && atTerminal)) {
        // away from the terminal: the next reading has nothing to agree with
        refineryOrders_.take(station, RefineryScreen(), taken);
        return;
    }
    refineryJobStation_ = station;
    refineryJobTaken_ = taken;
    try {
        refineryJob_ = std::async(std::launch::async, [this, picture = frame.clone(), bars, notice]() {
            return readRefineryScreen(picture, bars, textLines_, notice);
        });
    } catch (const std::system_error&) {
        // no thread to be had: the next look tries again
    }
}

// What two readings in a row agree on goes into the book (RefineryOrderBook::take)
void Application::takeRefineryScreen(const RefineryScreen& read, const std::string& placed, long long taken) {
    // The station the terminal names itself, else the one the log has the
    // player at; neither, and there is nowhere to put an order
    // ...and with no panel up to name it, the station of the last panels
    std::string station = refineryStationNamed(read.stationText, gameState_.system());
    if (station.empty() && !refineryLastStation_.empty() &&
        std::chrono::steady_clock::now() - refineryLastPanels_ < std::chrono::seconds(30)) {
        station = refineryLastStation_;
    }
    if (station.empty()) station = placed;
    if (!station.empty() && !read.panels.empty()) {
        refineryLastStation_ = station;
        refineryLastPanels_ = std::chrono::steady_clock::now();
    }
    if (station.empty()) {
        refineryOrders_.take("", RefineryScreen(), taken);
        return;
    }
    shareRefineryScreen(read, station, taken);
    const size_t had = refineryOrders_.orders().size();
    const bool changed = refineryOrders_.take(station, read, taken);
    keepRefineryNotes(refineryOrders_.takeNotes());
    if (!changed) return;
    refineryOrders_.save(AppPaths::file(REFINERY_ORDERS_FILE));
    if (refineryOrders_.orders().size() < had) {
        std::cout << "\n[refinery] " << station << ": " << (had - refineryOrders_.orders().size())
                  << " work order(s) collected" << std::endl;
    }
    if (refineryOrders_.orders().size() > had) {
        const RefineryOrder& added = refineryOrders_.orders().back();
        std::cout << "\n[refinery] " << station << ": work order " << added.number << ",";
        for (const RefineryOrder::Part& p : added.parts) {
            std::cout << " " << p.material << " " << p.quality << " -> " << p.yield << " cSCU";
        }
        std::cout << ", done in " << (added.due - taken) << " s" << std::endl;
    }
    pushRefineryOrders();
}

// ---- refinery workloads (RefineryShare) ---------------------------------

std::string Application::refineryServiceUrl() {
    if (const char* dev = DEV_ENV("SCMINER_REFINERY_URL")) return dev;
#ifdef BEDROCK_REFINERY_URL
    return BEDROCK_REFINERY_URL;
#else
    return "";
#endif
}

void Application::setRefineryShare(bool on) {
    refineryShare_ = on;
    if (!on) shareQueue_.clear();
    saveRegionConfig();
    if (gui_) gui_->setRefineryShare(refineryShare_, !refineryServiceUrl().empty());
}

// What a reading of the terminal says of its station's workload, and of an
// order being set up: like the orders themselves, taken only where two
// readings in a row agree - one misread figure never stands - and then shown
// and, where the player shares, sent
void Application::shareRefineryScreen(const RefineryScreen& read, const std::string& station, long long taken) {
    if (read.capacity >= 0 || read.workload >= 0) {
        const WorkloadReading now{read.capacity, read.workload, taken};
        const auto last = workloadLast_.find(station);
        const bool agrees = last != workloadLast_.end() && last->second.capacity == now.capacity &&
                            last->second.workload == now.workload && taken - last->second.at <= 120;
        workloadLast_[station] = now;
        if (agrees) {
            workloadMine_[station] = now;
            pushRefineryWorkloads();
            WorkloadReading& sent = workloadSent_[station];
            if (RefineryShare::worthSending(now.capacity, now.workload, taken, sent.capacity, sent.workload, sent.at)) {
                RefineryShare::Reading r;
                r.station = station;
                r.observed = taken;
                r.capacity = now.capacity;
                r.workload = now.workload;
                if (queueRefineryShare(r)) sent = now;
            }
        }
    }
    for (const RefineryPanel& panel : read.panels) {
        if (panel.kind != RefineryBar::Band::Setup || !panel.whole()) continue;
        RefineryShare::Reading r;
        r.station = station;
        r.observed = taken;
        r.capacity = read.capacity;
        r.workload = read.workload;
        for (const RefiningMethod& m : REFINING_METHODS) {
            if (panel.method == m.name) r.method = m.key;
        }
        r.cost = panel.cost;
        r.seconds = panel.seconds;
        std::string key = station + "|" + std::to_string(panel.order) + "|" + r.method + "|" +
                          std::to_string(std::llround(panel.cost)) + "|" + std::to_string(panel.seconds);
        for (const RefineryRow& row : panel.rows) {
            if (row.inert || row.material.empty() || row.quantity <= 0 || row.yield < 0) continue;
            r.items.push_back({row.material, row.quantity, row.quality, row.yield});
            key += "|" + row.material + ":" + std::to_string(row.quantity) + ":" + std::to_string(row.yield);
        }
        if (r.method.empty() || r.items.empty()) continue;
        const bool agrees = key == quoteLast_;
        quoteLast_ = key;
        if (!agrees || quotesSent_.count(key)) continue;
        if (queueRefineryShare(r)) quotesSent_.insert(key);
    }
}

bool Application::queueRefineryShare(const RefineryShare::Reading& reading) {
    if (!refineryShare_ || refineryServiceUrl().empty()) return false;
    if (library_.channel().rfind("LIVE", 0) != 0) return false;   // the PTU's refineries are not LIVE's
    shareQueue_.push_back(RefineryShare::body(reading, BEDROCK_VERSION, library_.liveGameVersion(), "LIVE"));
    while (shareQueue_.size() > 200) shareQueue_.pop_front();
    return true;
}

// The reports, one at a time off the UI thread - again a minute later where
// the service could not be reached or was busy - and, while the Refinery
// window is open, every five minutes what the service knows of each station
void Application::serviceRefineryShare() {
    const std::string base = refineryServiceUrl();
    if (base.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (shareJob_.valid() && shareJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const int status = shareJob_.get();
        if (status == 0 || status >= 500) {
            if (refineryShare_) shareQueue_.push_front(shareSending_);
            shareRetryAt_ = now + std::chrono::minutes(1);
        } else if (status == 429) {
            // enough of this station - or from this address - for now: a
            // fresher reading comes along, so this one goes, and the rest wait
            shareRetryAt_ = now + std::chrono::minutes(10);
        } else if (status >= 400) {
            std::cout << "\nRefinery workloads: the service turned a report down (" << status << ")" << std::endl;
        }
        shareSending_.clear();
    }
    if (!shareJob_.valid() && refineryShare_ && !shareQueue_.empty() && now >= shareRetryAt_) {
        shareSending_ = shareQueue_.front();
        shareQueue_.pop_front();
        shareJob_ = std::async(std::launch::async, [url = base + "/v1/refinery/observations", body = shareSending_]() {
            HttpAnswer answer;
            httpsPost(url, {"Content-Type: application/json"}, body, answer, 64 * 1024);
            return answer.status;
        });
    }
    if (stationsJob_.valid() && stationsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto answer = stationsJob_.get();
        if (answer.first.empty()) {
            stationsShared_ = std::move(answer.second);
            pushRefineryWorkloads();
        } else {
            std::cout << "\nRefinery workloads: " << answer.first << std::endl;
        }
    }
    const bool asked = stationsAsked_.time_since_epoch().count() != 0;
    if (!stationsJob_.valid() && gui_ && gui_->refineryShown() &&
        (!asked || now - stationsAsked_ >= std::chrono::minutes(5))) {
        stationsAsked_ = now;
        stationsJob_ = std::async(std::launch::async, [url = base + "/v1/refinery/stations"]() {
            std::string body, why;
            std::map<std::string, RefineryShare::StationState> states;
            if (httpsGet(url, {}, body, why, 1024 * 1024)) RefineryShare::readStations(body, states, why);
            return std::make_pair(why, states);
        });
    }
}

// The window's figures: each station's newest - the player's own reading or
// the service's
void Application::pushRefineryWorkloads() {
    if (!gui_) return;
    std::map<std::string, MinerGUI::RefineryWorkload> out;
    for (const auto& [name, s] : stationsShared_) {
        out[name] = {s.capacity, s.workload, s.observed, s.sources, false};
    }
    for (const auto& [name, w] : workloadMine_) {
        const auto it = out.find(name);
        if (it == out.end() || w.at >= it->second.observed) out[name] = {w.capacity, w.workload, w.at, 1, true};
    }
    gui_->setRefineryWorkloads(std::move(out));
}

// Each work order read whole - method, bill, time, what went in and what
// comes out - goes to the UEX report once; reportRefineryJob decides whether
// the player reports at all (opted in, agreed before the order was placed).
// Only once the player has left the terminal: a long list has been scrolled
// through by then, and the order has all its parts.
void Application::handRefineryOrdersToUex() {
    const auto now = std::chrono::steady_clock::now();
    if (now - refineryHandCheck_ < std::chrono::seconds(5)) return;
    refineryHandCheck_ = now;
    if (refineryUp_ || now - refineryLastPanels_ < std::chrono::seconds(10)) return;
    bool handed = false;
    for (size_t i = 0; i < refineryOrders_.orders().size(); i++) {
        const RefineryOrder& order = refineryOrders_.orders()[i];
        if (order.handed > 0 || !order.complete()) continue;
        UexRefineryJob job;
        job.station = order.station;
        job.number = order.number;
        job.method = order.method;
        job.cost = order.cost;
        job.seconds = order.seconds;
        job.placed = order.placed;
        for (const RefineryOrder::Part& p : order.parts) job.items.push_back({p.material, p.quantity, p.yield});
        reportRefineryJob(job);
        handed |= refineryOrders_.markHanded(i, (long long)std::time(nullptr));
        keepRefineryNotes({"work order " + std::to_string(order.number) + " at " + order.station + " goes to UEX" +
                           (uexReportSince_ > 0 ? "" : " - but reporting is off")});
    }
    if (handed) refineryOrders_.save(AppPaths::file(REFINERY_ORDERS_FILE));
}

void Application::keepRefineryNotes(const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    std::error_code ec;
    const std::filesystem::path folder = AppPaths::data() / "debug";
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path file = folder / "refinery_jobs.txt";
    // past 256 KB, the newer half kept
    const auto size = std::filesystem::file_size(file, ec);
    if (!ec && size > 256 * 1024) {
        std::ifstream in(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const size_t from = text.find('\n', text.size() / 2);
        std::ofstream(file, std::ios::binary | std::ios::trunc)
            << (from == std::string::npos ? std::string() : text.substr(from + 1));
    }
    std::ofstream out(file, std::ios::app);
    const std::string when = timeStamp("%Y-%m-%d %H:%M:%S");
    for (const std::string& line : lines) out << when << "  " << line << "\n";
}

void Application::pushRefineryOrders() {
    if (gui_) gui_->setRefineryOrders(refineryOrders_.sorted());
}

void Application::pollLibrary() {
    // A backup per 15 ms while the history is read, else the live log every
    // two seconds - quicker than anyone walks from a hand-in to the bench
    const auto now = std::chrono::steady_clock::now();
    const auto wait = std::chrono::milliseconds(library_.historyPending() ? 15 : 2000);
    if (libraryPushed_ && now - lastLibraryPoll_ < wait) return;
    lastLibraryPoll_ = now;
    if (library_.gameLog().empty() && ++libraryLooks_ % 15 == 0) {
        // The game may have started since - look again every half minute
        library_.setGameLog(locateGameLog());
    }
    const bool changed = library_.poll();
    if (changed || !libraryPushed_) pushLibrary();
    // ...and what was traded at the terminals, out of the same logs
    if (gui_ && library_.trades().changes() != tradesSeen_) {
        tradesSeen_ = library_.trades().changes();
        gui_->setTrades(library_.trades().trades());
    }
    // ...and the ranks out of the same logs: once they have all been read,
    // and again with every contract taken or finished
    {
        const bool known = !library_.gameLog().empty() && !library_.historyPending();
        const bool moved = library_.ranksChanged();
        if (known != ranksKnown_ || (known && moved) || !ranksPushed_) pushRanks();
        // ...and the contracts of the moment: one taken, one over. A game
        // that was closed takes its contracts with it and its log says
        // nothing of it, it only stops - so a log not written to for a
        // quarter of an hour has nothing running (the engine writes its
        // terrain statistics every ten minutes, whatever the player does).
        const long long written = library_.liveLogEpoch();
        const bool quiet = written > 0 && (long long)std::time(nullptr) - written > 15 * 60;
        if (moved || library_.runningChanges() != runningSeen_ || quiet != sessionQuiet_ || !contractsPushed_) {
            sessionQuiet_ = quiet;
            pushContracts();
        }
        if (gui_) gui_->tickContracts();
        if (gui_) gui_->tickUexOffer();
    }
    if (changed) {
        // A blueprint just received: the game's texts are rewritten for its
        // next start. A few seconds' grace, since hand-ins come several at once.
        gameTextsDue_ = true;
        gameTextsDueAt_ = now + std::chrono::seconds(5);
    }
    serviceGameTexts();
    keepGameTextsFresh();
    serviceUex();
    serviceUexReports();
    serviceUexMarket();
    // The same log, read for where the player is and what they fly
    if (gameState_.gameLog() != library_.gameLog()) {
        gameState_.setGameLog(library_.gameLog());
        applyGameState();
    } else if (gameState_.poll()) {
        applyGameState();
    }
    if (gameState_.gadgetChanges() != gadgetChangesSeen_) applyGadget();
    // Ask again once an hour. Only from UpToDate: a check in flight must
    // not be started twice, a version already found must not be forgotten
    // before it is fetched, and a check that failed is the player's to
    // retry - retrying it behind their back is how a broken network turns
    // into a request every hour that nobody asked for.
    if (updater_.enabled() && updater_.state() == Updater::State::UpToDate &&
        now - lastUpdateCheck_ >= std::chrono::minutes(UPDATE_RECHECK_MINUTES)) {
        lastUpdateCheck_ = now;
        updater_.checkInBackground();
    }
    if (gui_) {
        gui_->setUpdateStatus(updater_.enabled() ? (int)updater_.state() : -1,
                              updater_.latest().version,
                              updater_.state() == Updater::State::Failed
                                  ? updater_.couldNotSay()
                                  : updater_.latest().notes,
                              updater_.percent());
    }
}

// The box in the Blueprints window. Ticked, the game's texts are written at
// once and again whenever the library changes; unticked, Bedrock's file is
// taken away and the game shows its own texts from its next start. Either way
// the choice is gametexts= in region_config.txt, written at once.
void Application::setGameTextsWanted(bool wanted) {
    gameTextsWanted_ = wanted;
    saveRegionConfig();
    if (gameTextsJob_.valid()) {
        // a write in flight finishes first - it must not land after a removal
        gameTextsNote_ = gameTextsJob_.get().note;
    }
    if (wanted) {
        gameTextsDue_ = true;
        gameTextsDueAt_ = std::chrono::steady_clock::now();
        gameTextsNote_ = library_.gameLog().empty() ? "waiting for the game's folder" : "writing...";
        serviceGameTexts();
    } else if (library_.gameLog().empty()) {
        gameTextsNote_.clear();
    } else {
        // ...on every channel there is: one written while the PTU was played
        // would otherwise stay there, out of sight, as old as it is
        const std::filesystem::path followed = library_.gameLog().parent_path();
        for (const auto& channel : GameTexts::channelsBeside(followed)) {
            const GameTexts::Outcome taken = GameTexts::remove(channel);
            if (channel == followed) gameTextsNote_ = taken.note;
        }
    }
    std::cout << "\nGame texts: " << (wanted ? "on" : "off")
              << (gameTextsNote_.empty() ? "" : " - " + gameTextsNote_) << std::endl;
    if (gui_) gui_->setGameTexts(gameTextsWanted_, gameTextsNote_);
}

// Collects a write that has finished, and starts one that is owed: only once
// the whole library is known (the old logs read), and off the UI thread - the
// first write of a game build reads the game's text out of Data.p4k.
void Application::serviceGameTexts() {
    if (gameTextsJob_.valid() && gameTextsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const GameTexts::Outcome done = gameTextsJob_.get();
        gameTextsNote_ = done.note;
        std::cout << "\nGame texts: " << done.note << std::endl;
        if (gui_) gui_->setGameTexts(gameTextsWanted_, gameTextsNote_);
    }
    if (!gameTextsWanted_ || !gameTextsDue_ || gameTextsJob_.valid()) return;
    if (library_.gameLog().empty() || library_.historyPending()) return;
    if (std::chrono::steady_clock::now() < gameTextsDueAt_) return;
    gameTextsDue_ = false;
    std::vector<bool> owned(CRAFT_BLUEPRINT_COUNT);
    for (int b = 0; b < CRAFT_BLUEPRINT_COUNT; b++) owned[b] = library_.owned(b);
    const std::filesystem::path channel = library_.gameLog().parent_path();
    gameTextsJob_ = std::async(std::launch::async, [channel, owned]() { return GameTexts::write(channel, owned); });
}

// UEX's API, at the address its documentation gives (api.uexcorp.space answers too)
static const char* const UEX_API = "https://api.uexcorp.uk/2.0/";

// Bedrock's app token for UEX: uex_token.dat beside the program, which the
// build writes out of uex_token.txt and a release carries (CMakeLists.txt).
// Read once; "" without the file - a build that asks UEX anonymously.
static std::string uexToken() {
    static const std::string token = [] {
        std::ifstream in(AppPaths::program() / "uex_token.dat");
        std::string line;
        if (!in || !std::getline(in, line)) return std::string();
        return UexReports::unmixAppToken(line);
    }();
    return token;
}

// How long ago a file was written, in seconds; -1 where there is none
static long long secondsSinceWritten(const std::filesystem::path& file) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(file, ec);
    if (ec) return -1;
    const auto since = std::filesystem::file_time_type::clock::now() - written;
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(since).count();
}

static std::string wholeFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Written beside it and moved into place: a run that ends halfway leaves the
// list that was kept, not half a new one
static void keepFile(const std::filesystem::path& file, const std::string& bytes) {
    const std::filesystem::path fresh = file.string() + ".new";
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        out.write(bytes.data(), (std::streamsize)bytes.size());
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(fresh, file, ec);
}

// The Ore Finder's prices, off the UI thread: two lists of a megabyte each,
// fetched and read. Each is the one kept in uex\ while that is young enough -
// the price list for half an hour, the terminal list for a week - and UEX's
// after that. One that cannot be fetched leaves the one kept in place,
// however old: an old price says its age, a missing one says nothing.
Application::UexOutcome Application::loadUex(const std::filesystem::path& folder) {
    static const std::string API = UEX_API;
    UexOutcome out;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path pricesFile = folder / "prices.json";
    const std::filesystem::path terminalsFile = folder / "terminals.json";
    std::vector<std::string> headers;
    if (const std::string token = uexToken(); !token.empty()) headers.push_back("Authorization: Bearer " + token);
    const long long now = (long long)std::time(nullptr);
    // A token UEX no longer takes is answered 403, "Invalid bearer token.",
    // where the same question without one is answered in full - so a token
    // withdrawn must not cost every Bedrock its prices: asked again without
    auto ask = [&](const char* endpoint, std::string& body, std::string& why) {
        if (httpsGet(API + endpoint, headers, body, why)) return true;
        if (headers.empty() || (why != "the server answered 401" && why != "the server answered 403")) return false;
        out.tokenRefused = true;
        headers.clear();
        why.clear();
        return httpsGet(API + endpoint, headers, body, why);
    };

    // The terminals first: the prices are read against them for the places
    std::string terminals = wholeFile(terminalsFile);
    const long long terminalsAge = secondsSinceWritten(terminalsFile);
    if (terminals.empty() || terminalsAge < 0 || terminalsAge > 7ll * 24 * 3600) {
        std::string body, why;
        JsonValue answer;
        bool fetched = ask("terminals", body, why);
        if (fetched && (!parseJson(body, answer, why) || answer.textOr("status") != "ok")) {
            fetched = false;
            if (why.empty()) why = "UEX said " + answer.textOr("status", "nothing");
        }
        if (fetched) {
            terminals = std::move(body);
            keepFile(terminalsFile, terminals);
        } else if (terminals.empty()) {
            out.placesNote = "no terminal list: " + why;
        }
    }

    const long long pricesAge = secondsSinceWritten(pricesFile);
    auto prices = std::make_shared<UexPrices>();
    std::string why;
    if (pricesAge >= 0 && pricesAge < UEX_PRICES_MINUTES * 60 &&
        prices->read(wholeFile(pricesFile), terminals, now, why)) {
        out.prices = prices;
        out.fetchedAt = now - pricesAge;
        return out;
    }
    std::string body;
    why.clear();
    if (ask("commodities_prices_all", body, why) && prices->read(body, terminals, now, why)) {
        keepFile(pricesFile, body);
        out.prices = prices;
        out.fetchedAt = now;
        return out;
    }
    out.note = why;
    if (pricesAge >= 0) {
        auto kept = std::make_shared<UexPrices>();
        std::string skip;
        if (kept->read(wholeFile(pricesFile), terminals, now, skip)) {
            out.prices = kept;
            out.fetchedAt = now - pricesAge;
        }
    }
    return out;
}

// Collects an answer that has come, and asks when the Ore Finder is open and
// its prices are half an hour old - or there are none yet
void Application::serviceUex() {
    if (!gui_) return;
    if (!uexPushed_) {
        uexPushed_ = true;
        gui_->setUexPrices(uexWanted_, nullptr, 0, "");
    }
    if (uexJob_.valid() && uexJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        UexOutcome done = uexJob_.get();
        if (done.prices) {
            uexPrices_ = done.prices;
            uexFetchedAt_ = done.fetchedAt;
        }
        uexNote_ = done.note;
        std::cout << "\nUEX prices: "
                  << (uexPrices_ ? std::to_string(((long long)std::time(nullptr) - uexFetchedAt_) / 60) + " min old"
                                 : std::string("none"))
                  << (uexNote_.empty() ? "" : " - " + uexNote_)
                  << (done.placesNote.empty() ? "" : " - " + done.placesNote)
                  << (done.tokenRefused ? " (UEX turned the app token away; asked without it)" : "") << std::endl;
        gui_->setUexPrices(uexWanted_, uexPrices_, uexFetchedAt_, uexNote_);
    }
    if (!uexWanted_ || uexJob_.valid() || !gui_->plannerOpen()) return;
    const long long age = uexPrices_ ? (long long)std::time(nullptr) - uexFetchedAt_ : -1;
    if (uexPrices_ && age >= 0 && age < UEX_PRICES_MINUTES * 60) return;
    const auto now = std::chrono::steady_clock::now();
    if (uexTried_ && now - uexTriedAt_ < std::chrono::minutes(uexNote_.empty() ? 1 : UEX_RETRY_MINUTES)) return;
    uexTried_ = true;
    uexTriedAt_ = now;
    const std::filesystem::path folder = AppPaths::data() / "uex";
    uexJob_ = std::async(std::launch::async, [folder]() { return loadUex(folder); });
}

// A list of UEX's, the one kept in `file` while it is younger than `maxAge`
// seconds, else fetched (and kept); the one kept, however old, where it
// cannot be. "" and `why` when there is neither.
static std::string uexListCached(const std::filesystem::path& file, const char* endpoint, long long maxAge,
                                 std::string& why) {
    std::string kept = wholeFile(file);
    const long long age = secondsSinceWritten(file);
    if (!kept.empty() && age >= 0 && age < maxAge) return kept;
    std::vector<std::string> headers;
    if (const std::string token = uexToken(); !token.empty()) headers.push_back("Authorization: Bearer " + token);
    std::string body;
    bool fetched = httpsGet(std::string(UEX_API) + endpoint, headers, body, why);
    if (!fetched && !headers.empty() && (why == "the server answered 401" || why == "the server answered 403")) {
        why.clear();
        fetched = httpsGet(std::string(UEX_API) + endpoint, {}, body, why);
    }
    JsonValue answer;
    if (fetched && (!parseJson(body, answer, why) || answer.textOr("status") != "ok")) {
        fetched = false;
        if (why.empty()) why = "UEX said " + answer.textOr("status", "nothing");
    }
    if (fetched) {
        keepFile(file, body);
        return body;
    }
    return kept;
}

std::string Application::uexTokenForProbe() { return uexToken(); }

std::string Application::uexMarketForProbe(const std::filesystem::path& folder, const std::string& ore) {
    const auto started = std::chrono::steady_clock::now();
    const UexMarketAnswer answer = loadUexMarket(folder, ore, nullptr);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started).count();
    if (!answer.listings) return ore + ": no listings - " + answer.note + "\n";
    const long long now = (long long)std::time(nullptr);
    int buying = 0, selling = 0;
    for (const UexListing& listing : *answer.listings) (listing.buying ? buying : selling)++;
    std::string out = ore + ": " + std::to_string(buying) + " buying, " + std::to_string(selling) + " selling; " +
                      std::to_string(ms) + " ms\n";
    int shown = 0;
    for (const UexListing& listing : *answer.listings) {
        if (!listing.buying || shown >= 4) continue;
        shown++;
        out += "    buys " + (listing.quality > 0 ? "Q" + std::to_string(listing.quality) + " " : std::string()) +
               "for " + std::to_string((long long)listing.price) + " / " + listing.unit + ", posted " +
               std::to_string((now - listing.added) / 86400) + " d ago, renewed " +
               std::to_string((now - listing.renewed) / 86400) + " d ago\n";
    }
    return out;
}

// One question to UEX's API with Bedrock's app token, and once more without
// it where UEX turns the token away
static bool uexAsk(const std::string& path, std::string& body, std::string& why) {
    std::vector<std::string> headers;
    if (const std::string token = uexToken(); !token.empty()) headers.push_back("Authorization: Bearer " + token);
    if (httpsGet(std::string(UEX_API) + path, headers, body, why)) return true;
    if (headers.empty() || (why != "the server answered 401" && why != "the server answered 403")) return false;
    why.clear();
    return httpsGet(std::string(UEX_API) + path, {}, body, why);
}

// The marketplace's listings of one ore, both ways, off the UI thread - and
// the marketplace's items first where they are not known yet (kept a day)
Application::UexMarketAnswer Application::loadUexMarket(const std::filesystem::path& folder, const std::string& ore,
                                                       std::shared_ptr<const std::map<std::string, int>> items) {
    UexMarketAnswer out;
    out.ore = ore;
    std::string why;
    if (!items) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::string list =
            uexListCached(folder / "market_items.json", "marketplace_prices_averages_all", 24ll * 3600, why);
        auto read = std::make_shared<std::map<std::string, int>>();
        if (list.empty() || !UexMarket::readItems(list, *read, why)) {
            out.note = "the marketplace's items: " + why;
            return out;
        }
        out.items = read;
        items = read;
    }
    const int item = UexMarket::itemFor(*items, ore);
    if (!item) {
        out.listings = std::make_shared<std::vector<UexListing>>();
        out.note = "the marketplace has no " + ore;
        return out;
    }
    const long long now = (long long)std::time(nullptr);
    auto listings = std::make_shared<std::vector<UexListing>>();
    for (const char* operation : {"buy", "sell"}) {
        std::string body;
        why.clear();
        if (!uexAsk("marketplace_listings?id_item=" + std::to_string(item) + "&operation=" + operation, body, why)) {
            out.note = why;
            return out;
        }
        std::vector<UexListing> read;
        if (!UexMarket::readListings(body, now, read, why)) {
            out.note = why;
            return out;
        }
        listings->insert(listings->end(), read.begin(), read.end());
    }
    out.listings = listings;
    return out;
}

// The player's conversations and notifications, with their key - off the UI
// thread. Null where UEX would not answer.
std::shared_ptr<UexThreads> Application::loadUexThreads(const std::string& key) {
    const std::string token = uexToken();
    std::vector<std::string> headers = {"secret-key: " + key};
    if (!token.empty()) headers.push_back("Authorization: Bearer " + token);
    std::string body, why;
    auto threads = std::make_shared<UexThreads>();
    if (!httpsGet(std::string(UEX_API) + "marketplace_negotiations", headers, body, why) ||
        !UexMarket::readNegotiations(body, *threads, why)) {
        std::cout << "\nUEX conversations: " << why << std::endl;
        return nullptr;
    }
    if (!threads->hashByListing.empty() && httpsGet(std::string(UEX_API) + "user_notifications", headers, body, why)) {
        UexMarket::readNotifications(body, *threads);
    }
    return threads;
}

// Collects listings that have come, and asks for the next ore wanted: the
// UEX tab's first, then the rock's good parts - one at a time, each ore
// again only after ten minutes
void Application::serviceUexMarket() {
    if (!gui_) return;
    if (uexMarketJob_.valid() && uexMarketJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        UexMarketAnswer done = uexMarketJob_.get();
        if (done.items) uexMarketItems_ = done.items;
        if (done.listings || !done.note.empty()) {
            gui_->setUexMarket(done.ore, done.listings, (long long)std::time(nullptr), done.note);
        }
    }
    // the player's conversations, with their key, while the UEX tab is up
    if (uexThreadsJob_.valid() && uexThreadsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (auto threads = uexThreadsJob_.get()) gui_->setUexThreads(std::move(threads));
    }
    if (uexWanted_ && !uexKey_.empty() && !uexKeyTrouble_ && !uexThreadsJob_.valid() &&
        !gui_->uexMarketWanted().empty() &&
        (!uexThreadsEver_ || std::chrono::steady_clock::now() - uexThreadsAsked_ > std::chrono::minutes(2))) {
        uexThreadsEver_ = true;
        uexThreadsAsked_ = std::chrono::steady_clock::now();
        const std::string key = uexKey_;
        uexThreadsJob_ = std::async(std::launch::async, [key]() { return loadUexThreads(key); });
    }
    if (!uexWanted_ || uexMarketJob_.valid()) return;
    const auto now = std::chrono::steady_clock::now();
    auto due = [&](const std::string& ore) {
        const auto asked = uexMarketAsked_.find(ore);
        return asked == uexMarketAsked_.end() || now - asked->second >= std::chrono::minutes(UEX_MARKET_MINUTES);
    };
    std::string next;
    const std::string tab = gui_->uexMarketWanted();
    if (!tab.empty() && due(tab)) next = tab;
    for (const std::string& ore : uexRockOres_) {
        if (next.empty() && due(ore)) next = ore;
    }
    if (next.empty()) return;
    uexMarketAsked_[next] = now;
    const std::filesystem::path folder = AppPaths::data() / "uex";
    const auto items = uexMarketItems_;
    uexMarketJob_ = std::async(std::launch::async, [folder, next, items]() { return loadUexMarket(folder, next, items); });
}

// UEX's ids for what is reported, off the UI thread: its terminals,
// commodities and refining methods, each kept a week
std::shared_ptr<UexDirectory> Application::loadUexDirectory(const std::filesystem::path& folder) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const long long week = 7ll * 24 * 3600;
    std::string why;
    const std::string terminals = uexListCached(folder / "terminals.json", "terminals", week, why);
    const std::string commodities = uexListCached(folder / "commodities.json", "commodities", week, why);
    const std::string methods = uexListCached(folder / "methods.json", "refineries_methods", week, why);
    auto directory = std::make_shared<UexDirectory>();
    if (terminals.empty() || commodities.empty() || methods.empty() ||
        !directory->read(terminals, commodities, methods, why)) {
        std::cout << "\nUEX reports: UEX's lists could not be read - " << why << std::endl;
        return nullptr;
    }
    return directory;
}

void Application::readUexReported() {
    if (uexReportedRead_) return;
    uexReportedRead_ = true;
    std::ifstream in(AppPaths::data() / "uex" / "reported.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) uexReported_.insert(line);
    }
}

// Done, and so never again - kept for the next start. The newest 600 are
// kept; anything older is long past the two minutes a trade is reported in.
void Application::rememberUexReported(const std::string& key) {
    readUexReported();
    if (!uexReported_.insert(key).second) return;
    std::error_code ec;
    const std::filesystem::path folder = AppPaths::data() / "uex";
    std::filesystem::create_directories(folder, ec);
    std::ofstream out(folder / "reported.txt", std::ios::app);
    out << key << "\n";
    if (uexReported_.size() > 1200) {
        // written anew with the newest only: the file is in order, the set is not
        std::ifstream in(folder / "reported.txt");
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        in.close();
        if (lines.size() > 600) lines.erase(lines.begin(), lines.end() - 600);
        std::ofstream fresh(folder / "reported.txt", std::ios::trunc);
        for (const std::string& kept : lines) fresh << kept << "\n";
        uexReported_ = std::set<std::string>(lines.begin(), lines.end());
    }
}

// The game's picture, whole, for the snapshot. The overlay frame would end
// up in it, so it steps aside.
cv::Mat Application::grabGamePicture() {
    const cv::Rect picture = pictureFor(signatureRegion_);
    CaptureRegion whole;
    whole.x = picture.x;
    whole.y = picture.y;
    whole.width = picture.width;
    whole.height = picture.height;
    const bool overlayShown = overlayHwnd_ && IsWindowVisible(overlayHwnd_);
    if (overlayShown) {
        ShowWindow(overlayHwnd_, SW_HIDE);
        Sleep(50);
    }
    cv::Mat frame = screenCapture_.captureRegion(whole);
    if (overlayShown) ShowWindow(overlayHwnd_, SW_SHOW);
    return frame;
}

void Application::reportRefineryJob(const UexRefineryJob& job) {
    if (uexReportSince_ <= 0 || uexKey_.empty() || uexKeyTrouble_) return;
    if (job.placed > 0 && job.placed < uexReportSince_) return;   // placed before the player agreed
    readUexReported();
    const std::string key = "refinery|" + job.key();
    if (uexReported_.count(key)) return;
    if (std::any_of(uexQueue_.begin(), uexQueue_.end(), [&](const UexPending& p) { return p.key == key; })) return;
    UexPending item;
    item.key = key;
    item.job = job;
    uexQueue_.push_back(std::move(item));
}

// Collects a report that has gone, and sends the next one - one at a time,
// off the UI thread, and only what can be told for certain: a job whose
// station is not exactly one of UEX's refineries is left out, and the window
// says why
void Application::serviceUexReports() {
    const auto now = std::chrono::steady_clock::now();
    if (uexSendJob_.valid() && uexSendJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        UexSent sent = uexSendJob_.get();
        if (sent.keyTrouble) {
            uexKeyTrouble_ = true;      // the same key again would only be turned away again
        } else if (!sent.key.empty()) {
            rememberUexReported(sent.key);
        }
        // the account as the last answer had it: refused, or taken again (a
        // report that did not get through says nothing of it)
        if (sent.answered) uexAccountTrouble_ = sent.accountTrouble;
        if (!sent.user.empty()) uexUser_ = sent.user;
        uexReportNote_ = sent.note;
        uexReportNoteAt_ = (long long)std::time(nullptr);
        std::cout << "\nUEX report: " << sent.note << std::endl;
        keepRefineryNotes({sent.note});
        pushUexAccount();
    }
    if (uexDirectoryJob_.valid() && uexDirectoryJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        if (auto directory = uexDirectoryJob_.get()) uexDirectory_ = std::move(directory);
    }
    if (uexQueue_.empty() || uexSendJob_.valid()) return;
    if (uexReportSince_ <= 0 || uexKey_.empty() || uexKeyTrouble_) {
        uexQueue_.clear();
        return;
    }
    if (!uexDirectory_) {
        if (!uexDirectoryJob_.valid() &&
            (!uexDirectoryAsked_ || now - uexDirectoryTried_ > std::chrono::minutes(UEX_RETRY_MINUTES))) {
            uexDirectoryAsked_ = true;
            uexDirectoryTried_ = now;
            const std::filesystem::path folder = AppPaths::data() / "uex";
            uexDirectoryJob_ = std::async(std::launch::async, [folder]() { return loadUexDirectory(folder); });
        }
        return;
    }
    UexPending item = std::move(uexQueue_.front());
    uexQueue_.pop_front();
    const UexDirectory& directory = *uexDirectory_;
    const bool production = DEV_ENV("SCMINER_UEX_DRYRUN") == nullptr;
    std::string why;
    const UexRefineryJob& job = item.job;
    const int terminal = directory.refineryTerminal(job.station, why);
    const int method = directory.method(job.method);
    std::vector<std::pair<int, UexRefineryJob::Item>> items;
    std::string missing;
    for (const UexRefineryJob::Item& part : job.items) {
        const int commodity = directory.commodity(part.material);
        if (!commodity) missing = part.material;
        else if (part.quantity >= 0 && part.yield >= 0) items.push_back({commodity, part});
    }
    const std::string label = "Refinery job at " + job.station;
    if (!terminal || !method || !missing.empty() || items.empty() || job.cost < 0 || job.seconds <= 0) {
        uexReportNote_ = label + ": not reported (" +
                         (!terminal ? why : !method ? "UEX doesn't know the method " + job.method
                                    : !missing.empty() ? "UEX doesn't list " + missing
                                                       : "the job wasn't read completely") + ")";
        uexReportNoteAt_ = (long long)std::time(nullptr);
        keepRefineryNotes({uexReportNote_});
        rememberUexReported(item.key);
        pushUexAccount();
        return;
    }
    const std::string body = UexReports::refineryJob(terminal, method, job.cost, (job.seconds + 59) / 60, items, production);
    const std::string token = uexToken();
    if (token.empty()) {
        uexReportNote_ = "This build has no UEX app token, so nothing can be reported";
        uexReportNoteAt_ = (long long)std::time(nullptr);
        uexQueue_.clear();
        pushUexAccount();
        return;
    }
    const std::string key = uexKey_;
    const std::string url = std::string(UEX_API) + "user_refineries_jobs_add/";
    const std::string itemKey = item.key;
    uexSendJob_ = std::async(std::launch::async, [url, token, key, body, label, itemKey]() {
        UexSent sent;
        sent.key = itemKey;
        HttpAnswer answer;
        const std::vector<std::string> headers = {"Authorization: Bearer " + token, "secret-key: " + key,
                                                  "Content-Type: application/json"};
        if (!httpsPost(url, headers, body, answer)) {
            sent.note = label + ": not sent (" + answer.why + ")";
            return sent;
        }
        sent.answered = true;
        const std::string said = UexReports::verdict(answer.status, answer.body, &sent.user);
        sent.keyTrouble = UexReports::keyTrouble(answer.status, answer.body);
        sent.accountTrouble = UexReports::accountTrouble(answer.status, answer.body);
        if (sent.keyTrouble) sent.key.clear();   // not done: it goes again once the key is right
        sent.note = label + (said.empty() ? ": accepted by UEX" : ": " + said);
        return sent;
    });
}

void Application::pushUexAccount() {
    if (!gui_) return;
    MinerGUI::UexAccount account;
    account.on = uexReportSince_ > 0;
    account.since = uexReportSince_;
    account.keyEnd = uexKey_.size() >= 4 ? uexKey_.substr(uexKey_.size() - 4) : std::string();
    account.keyTrouble = uexKeyTrouble_;
    account.accountTrouble = uexAccountTrouble_;
    account.note = uexReportNote_;
    account.noteAt = uexReportNoteAt_;
    account.user = uexUser_;
    account.hasToken = !uexToken().empty();
    account.keyChanges = uexKeyChanges_;
    gui_->setUexAccount(account);
}

// What the player pressed on a UEX card: 1 a key pasted (the clipboard's
// text), 2 the key forgotten, 3 agreed - reporting on, 4 declined or switched
// off (the key stays), 5 the first start's offer made, whatever came of it
void Application::uexAccountAction(int action, const std::string& text) {
    const long long now = (long long)std::time(nullptr);
    if (action == 1) {
        std::string key = text;
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
                  key.end());
        if (!UexReports::keyLooksRight(key)) {
            uexReportNote_ = "That's not a UEX secret key. It should be 40 letters and digits.";
        } else if (!UexKey::save(key)) {
            uexReportNote_ = "The key couldn't be saved on this PC.";
        } else {
            uexKey_ = key;
            uexKeyTrouble_ = false;
            uexAccountTrouble_.clear();   // another key may be another account
            uexKeyChanges_++;
            uexReportNote_ = "Key saved";
        }
    } else if (action == 2) {
        UexKey::forget();
        uexKey_.clear();
        uexKeyTrouble_ = false;
        uexAccountTrouble_.clear();
        uexReportSince_ = 0;
        uexQueue_.clear();
        saveRegionConfig();
        uexReportNote_ = "Key removed, reporting off";
    } else if (action == 3) {
        if (uexKey_.empty()) {
            uexReportNote_ = "Paste your UEX secret key first";
        } else {
            uexReportSince_ = now;
            saveRegionConfig();
            uexReportNote_ = "Reporting on";
        }
    } else if (action == 4) {
        const bool was = uexReportSince_ > 0;
        uexReportSince_ = 0;
        uexAccountTrouble_.clear();   // the next report, reporting on again, tells anew
        uexQueue_.clear();
        saveRegionConfig();
        uexReportNote_ = was ? "Reporting off" : (uexKey_.empty() ? "" : "Account connected, reporting off");
    } else if (action == 5) {
        // the first start's offer was made: not again
        uexOffered_ = true;
        saveRegionConfig();
        pushUexAccount();
        return;
    }
    uexReportNoteAt_ = now;
    std::cout << "\nUEX reports: " << uexReportNote_ << std::endl;
    pushUexAccount();
}

// A patch moves the game's text on, and Bedrock's file - which the game reads
// in place of its own - stays on the build it was written from until it is
// written again: at Bedrock's next start, or with the next blueprint. What
// the patch added or changed is missing from the game until then, and on a
// channel Bedrock does not follow at the moment (the PTU while the log is
// LIVE's) until it is played with Bedrock open.
// So every half minute: on the channel followed the file is written anew,
// out of the patched game; on any other it is taken away - that channel's
// library is not the one at hand - and the game shows its own text there
// until it is played with Bedrock open again, rather than an old one.
void Application::keepGameTextsFresh() {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastGameTextsCheck_ < std::chrono::seconds(30)) return;
    lastGameTextsCheck_ = now;
    if (library_.gameLog().empty()) return;
    const std::filesystem::path followed = library_.gameLog().parent_path();
    for (const auto& channel : GameTexts::channelsBeside(followed)) {
        const long long patch = GameTexts::outdatedSince(channel, std::chrono::minutes(2));
        if (patch == 0) continue;
        long long& answered = gameTextsAnswered_[channel.string()];
        if (answered == patch) continue;
        answered = patch;
        if (channel == followed && gameTextsWanted_) {
            std::cout << "\nGame texts: the game was updated - written anew from its own text" << std::endl;
            gameTextsDue_ = true;
            gameTextsDueAt_ = now;
        } else {
            const GameTexts::Outcome taken = GameTexts::remove(channel);
            std::cout << "\nGame texts: " << channel.filename().string() << " was updated - " << taken.note << std::endl;
        }
    }
}

void Application::pushLibrary() {
    if (!gui_) return;
    std::string note;
    if (library_.gameLog().empty()) {
        note = "no Game.log found - tick by hand, or put gamelog=<path> in region_config.txt";
    } else if (library_.historyPending()) {
        note = "reading the game's old logs, " + std::to_string(library_.historyRead()) + " so far";
    } else {
        note = "from the game's log (" + library_.channel() + ")";
    }
    // Say that the evidence is being kept, and where. The game prunes its own
    // logbackups folder; once it has, this file is the only thing left that
    // says what was received.
    if (library_.archived() > 0) {
        note += "  -  " + std::to_string(library_.archived()) +
                " lines kept in blueprint_log_backup.txt";
    }
    // ...and what was concluded rather than read: a contract that paid
    // nothing had nothing left to give (see ContractScan)
    if (library_.assumedCount() > 0) {
        note += "  -  " + std::to_string(library_.assumedCount()) + " from contracts that paid none";
    }
    gui_->setOwnedBlueprints(library_.stamps(), note);
    libraryPushed_ = true;
}

// The contracts of the moment, for the window: what is running in the game,
// and what of the player's own was finished in the last three hours - the
// longest an outfit blocks a contract just finished is two. A gathering
// order newly taken is said in the log once.
void Application::pushContracts() {
    runningSeen_ = library_.runningChanges();
    contractsPushed_ = true;
    if (!gui_) return;
    std::vector<MinerGUI::ContractNow> list;
    for (const auto& r : library_.runningContracts()) {
        if (sessionQuiet_) break;      // the game is closed: nothing runs
        const RepContract* c = Reputation::contract(r.contract);
        if (!c) continue;
        MinerGUI::ContractNow now;
        now.contract = c;
        now.running = true;
        now.shared = r.shared;
        list.push_back(now);
        if (c->oreCount > 0 && ordersSaid_.insert(r.mission).second) {
            std::cout << "\n[order] " << c->title << (r.shared ? " (shared)" : "") << " wants";
            for (int k = 0; k < c->oreCount; k++) {
                const OrderOre& ore = ORDER_ORES[c->oreFirst + k];
                std::cout << (k ? (c->oreChoice ? " or " : ", ") : " ") << ore.least;
                if (ore.most > ore.least) std::cout << "-" << ore.most;
                std::cout << (ore.scu ? " SCU " : " ") << ore.name;
            }
            std::cout << std::endl;
        }
    }
    const long long nowUtc = (long long)std::time(nullptr);
    for (const auto& [key, e] : library_.contracts()) {
        (void)key;
        if (!e.completed) continue;
        const long long at = Reputation::epochOf(e.stamp);
        if (at < 0 || nowUtc - at > 3 * 3600 || at > nowUtc + 300) continue;
        const RepContract* c = Reputation::contract(e.contract);
        if (!c) continue;
        MinerGUI::ContractNow done;
        done.contract = c;
        done.finishedAt = at;
        list.push_back(done);
    }
    gui_->setContractsNow(std::move(list));
}

// A mining gadget stuck on a rock in the game, or taken off it again
// (GameState::gadgetOnRock): the rock view takes it from there. One the log
// put on a rock more than ten minutes ago - its tail, read at the start,
// reaches back - says nothing of the rock in front now.
void Application::applyGadget() {
    gadgetChangesSeen_ = gameState_.gadgetChanges();
    const std::string& cls = gameState_.gadgetOnRock();
    const MiningGadget* gadget = nullptr;
    if (cls.size() > 14) {
        // "Mining_Gadget_SHIN_Sabir" is "shin_sabir" in the tables
        std::string id = cls.substr(14);
        for (char& ch : id) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        gadget = gadgetById(id.c_str());
    }
    long long age = 0;
    if (const long long at = Reputation::epochOf(gameState_.gadgetStamp()); at > 0) {
        age = (std::max)(0LL, (long long)std::time(nullptr) - at);
    }
    const bool stale = gadget && age > 10 * 60;
    std::cout << "\n[gadget] ";
    if (cls.empty()) std::cout << "off the rock";
    else std::cout << cls << " on " << (gameState_.gadgetRock().empty() ? "a rock" : gameState_.gadgetRock());
    if (!cls.empty() && !gadget) std::cout << " - not one of the tables'";
    if (stale) std::cout << " - " << age / 60 << " min ago, left alone";
    std::cout << std::endl;
    if (gui_) gui_->setGadgetPlaced(stale ? nullptr : gadget, gameState_.gadgetRock(), age);
}

void Application::pushRanks() {
    ranksKnown_ = !library_.gameLog().empty() && !library_.historyPending();
    ranksPushed_ = true;
    if (gui_) gui_->setRanks(library_.standings(), ranksKnown_, library_.countsFrom());
}

void Application::describeSetup(std::ostream& out) const {
    out << "Bedrock " << BEDROCK_VERSION << "  " << timeStamp("%Y-%m-%d %H:%M:%S") << "\n";
    out << "commit:  " << BEDROCK_GIT_SHA << "\n";
    // Where the player's things are. First question in every support
    // conversation, and the answer moved in 4.9.
    out << "program: " << AppPaths::program().string() << "\n";
    out << "data:    " << AppPaths::data().string()
        << (AppPaths::portable() ? "  (portable.txt beside the exe)" : "") << "\n";
    out << "virtual screen: " << GetSystemMetrics(SM_XVIRTUALSCREEN) << ","
        << GetSystemMetrics(SM_YVIRTUALSCREEN) << " " << GetSystemMetrics(SM_CXVIRTUALSCREEN) << "x"
        << GetSystemMetrics(SM_CYVIRTUALSCREEN) << ", " << GetSystemMetrics(SM_CMONITORS)
        << " monitor(s)\n";
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        MONITORINFOEXA info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoA(monitor, &info)) {
            auto& out = *reinterpret_cast<std::ostream*>(param);
            out << "  " << info.szDevice << ": " << info.rcMonitor.left << "," << info.rcMonitor.top
                << " " << (info.rcMonitor.right - info.rcMonitor.left) << "x"
                << (info.rcMonitor.bottom - info.rcMonitor.top)
                << ((info.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "") << "\n";
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    if (auto viewport = ScreenCapture::gameViewport()) {
        out << "game viewport: " << viewport->width << "x" << viewport->height << " at "
            << viewport->x << "," << viewport->y << "\n";
    } else {
        out << "game viewport: not found (no visible CryENGINE window)\n";
    }
    out << "aim box: " << signatureRegion_.x << "," << signatureRegion_.y << " " << signatureRegion_.width
        << "x" << signatureRegion_.height << (aimAuto_ ? " (auto)" : " (manual)") << ", search margin "
        << trackMargin_ << "\n";
    const cv::Rect area = allowedArea();
    out << "search area: " << area.x << "," << area.y << " " << area.width << "x" << area.height << "\n";
    if (gui_) {
        out << "ui scale: " << gui_->uiScale() << " %"
            << (uiScalePercent_ > 0 ? " (from region_config.txt)" : " (worked out from the screen)")
            << "\n";
    } else {
        out << "ui scale: no window in this run"
            << (uiScalePercent_ > 0 ? ", region_config.txt says " + std::to_string(uiScalePercent_) + " %" : "")
            << "\n";
    }
    out << "snapshot hotkey: "
        << ((gui_ && gui_->snapshotHotkeyHeld()) ? "Ctrl+Alt+S held"
                                                 : "NOT held - taken by another program")
        << "\n";
    out << "scan: every " << captureIntervalMs_ << " ms at most, "
        << (easeIdle_ ? "eased when nothing is found" : "never eased (ease=0)")
        << ", " << cv::getNumThreads() << " threads\n";
    out << "hud scale: " << hudScale_ << " (hudscale=" << hudScalePercent_
        << "; 100 = as captured, 0 = follow the picture, reference "
        << GlyphReader::REFERENCE_VIEWPORT_HEIGHT << " px tall)\n";
    out << "capture path: " << sourceName(screenCapture_.lastSource()) << "\n";
    if (!screenCapture_.captureNote().empty()) out << "  duplication: " << screenCapture_.captureNote() << "\n";
    if (!screenCapture_.rawStats().empty()) out << "  light: " << screenCapture_.rawStats() << "\n";
    if (!captureStats_.empty()) out << "  picture: " << captureStats_ << "\n";
    out << "templates: " << (glyphReader_.usesEmbeddedTemplates() ? "built in" : "from ./glyphs") << "\n";
    out << "game log: "
        << (library_.gameLog().empty()
                ? std::string("not found (no game running, no launcher log, no gamelog= in region_config.txt)")
                : library_.gameLog().string())
        << (gameLogOverride_.empty() ? "" : " (pinned in region_config.txt)")
        << (library_.channel().empty() ? "" : " - channel " + library_.channel() + ", file " + library_.fileName())
        << "\n";
    out << "blueprint library: " << library_.ownedCount() << " owned (" << library_.byHandCount()
        << " by hand), " << library_.unknownNames().size() << " received but unknown to this build, "
        << library_.historyRead() << " log files read, "
        << library_.archived() << " evidence lines kept"
        << (library_.historyPending() ? " (still reading)" : "") << "\n";
    out << "ranks: " << library_.contractsKept() << " contracts of the player's own kept, counted from "
        << Reputation::versionName(library_.countsFrom()) << ", old logs checked for the game they are of: "
        << (library_.logsChecked() ? "yes" : "not yet") << "\n";
    for (const auto& [name, stamp] : library_.unknownNames()) {
        out << "  unknown: " << name << " (" << stamp << ")\n";
    }
    out << "recording: " << (recordScans_ ? "on" : "off") << ", live: "
        << (liveModeActive_ ? "on" : "off") << ", last value: " << lastRecognizedSignature_ << "\n";
    // The rock view reads a panel's figures by their shapes and needs nothing
    // for that; only the ore's name wants Tesseract's data beside the exe.
    // Noted here so a "the rock view does nothing" report can be diagnosed.
    {
        std::error_code ec;
        const bool data = std::filesystem::exists("./tessdata/eng.traineddata", ec);
        out << "rock view: " << (rockViewWanted() ? "on" : "off") << ", ore names: "
            << (data ? "read (tessdata beside the program)" : "not read (no tessdata beside the program)") << "\n";
    }
}

// What the capture looked like: a black picture means the capture path
// sees nothing (exclusive full screen, wrong adapter), a white one that HDR
// highlights were clipped - both read as "nothing recognised" otherwise
void Application::noteCapture(const cv::Mat& image) {
    if (image.empty()) {
        captureStats_ = "empty - nothing came back from the capture";
        return;
    }
    cv::Mat gray;
    if (image.channels() == 1) gray = image;
    else cv::cvtColor(image, gray, image.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    const double black = 100.0 * cv::countNonZero(gray <= 5) / gray.total();
    const double white = 100.0 * cv::countNonZero(gray >= 250) / gray.total();
    std::ostringstream s;
    s << image.cols << "x" << image.rows << ", mean brightness " << std::fixed << std::setprecision(1)
      << cv::mean(gray)[0] << ", black " << black << " %, white " << white << " %";
    captureStats_ = s.str();
}

void Application::writeSessionInfo() {
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (ec) return;
    {
        std::ofstream out(dataPath("debug/session.txt"), std::ios::trunc);
        if (out.is_open()) describeSetup(out);
    }
    mirrorConfigToDebug();
}

void Application::mirrorConfigToDebug() const {
    std::error_code ec;
    if (!std::filesystem::exists(dataPath(REGION_CONFIG_FILE), ec)) return;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (ec) return;
    std::filesystem::copy_file(dataPath(REGION_CONFIG_FILE), dataPath(std::string("debug/") + REGION_CONFIG_FILE),
                               std::filesystem::copy_options::overwrite_existing, ec);
}

void Application::snapshotFromConsole() {
    saveSnapshot();
    writeSessionInfo();   // now with the capture path the snapshot used
}

void Application::requestSnapshot() {
    // The hotkey arrives through the message pump, which a running scan
    // drives from inside itself: capturing again in there would cut into
    // the scan's own capture. Taken at the start of the next scan instead.
    if (inScan_) snapshotPending_ = true;
    else saveSnapshot();
}

void Application::saveSnapshot() {
    std::error_code ec;
    std::filesystem::create_directories(dataPath("debug"), ec);
    if (ec) return;

    // Ten at most, the oldest making room: a report needs one or two, and a
    // 4K picture is ten megabytes
    std::vector<std::filesystem::path> older;
    // dataPath("debug"), not "debug": the snapshots are written beside the
    // player data, while a bare relative path points at the folder the exe
    // sits in, where nothing is written and nothing would be pruned.
    for (const auto& entry : std::filesystem::directory_iterator(dataPath("debug"), ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("snapshot_", 0) == 0 && entry.path().extension() == ".png") older.push_back(entry.path());
    }
    std::sort(older.begin(), older.end());
    while (older.size() >= static_cast<size_t>(snapshotKeep_)) {
        std::filesystem::path note = older.front();
        note.replace_extension(".txt");
        std::filesystem::remove(older.front(), ec);
        std::filesystem::remove(note, ec);
        older.erase(older.begin());
    }

    const cv::Rect picture = pictureFor(signatureRegion_);
    cv::Mat frame = grabGamePicture();
    noteCapture(frame);   // the note describes THIS picture
    CaptureRegion whole;
    whole.x = picture.x;
    whole.y = picture.y;

    const std::string base = dataPath("debug/snapshot_") + timeStamp("%Y%m%d_%H%M%S");
    std::ofstream note(base + ".txt", std::ios::trunc);
    if (note.is_open()) {
        describeSetup(note);
        note << "snapshot: " << (frame.empty() ? "capture FAILED" : "the game picture") << " at "
             << whole.x << "," << whole.y << "\n";
    }
    bool saved = false;
    if (!frame.empty()) saved = cv::imwrite(base + ".png", frame);
    std::cout << "\n[snapshot] " << base << (saved ? ".png" : ".txt (no picture)") << std::endl;
    MessageBeep(saved ? MB_ICONASTERISK : MB_ICONHAND);
}

} // namespace SCMiner
