#pragma once
// Reading the game's picture under X11 - what ScreenCapture does on Windows.
//
// The shape of the problem is different enough that this is not a port of
// ScreenCapture but its counterpart, and the difference is worth stating
// because it decides the whole design:
//
// On Windows the capture takes the WHOLE desktop and then crops to the game's
// client rectangle, because that is what BitBlt on the desktop DC gives you.
// Under X11 that route does not work at all on a Wayland session: the root
// window has no readable contents there, and XGetImage on it comes back with
// BadMatch - measured, not assumed. What does work is addressing the window
// itself through the Composite extension, which hands back the off-screen
// pixmap the compositor draws from.
//
// That turns out to be the better source anyway:
//   - nothing can be in front of it, so no overlay and no notification can
//     end up in a reading
//   - the coordinates are the window's own, so the viewport is simply
//     (0, 0, width, height) and the whole business of mapping screen
//     coordinates into the client area falls away
//
// Star Citizen on Linux runs under Wine or Proton, so its window is an
// XWayland or X11 client like any other. Wine does not carry the CryENGINE
// class across to X11 - it sets WM_CLASS from the module name - so the window
// is identified by title, class and the process behind _NET_WM_PID instead.

#include <opencv2/opencv.hpp>

#include <optional>
#include <string>
#include <vector>

namespace SCMiner {

class LinuxCapture {
public:
    LinuxCapture();
    ~LinuxCapture();
    LinuxCapture(const LinuxCapture&) = delete;
    LinuxCapture& operator=(const LinuxCapture&) = delete;

    // False when there is no display to talk to, or no Composite extension.
    // lastError() then says which.
    bool ok() const { return display_ != nullptr && haveComposite_; }
    const std::string& lastError() const { return error_; }

    // One window the search found, for diagnostics and for letting a person
    // pick when several match.
    struct WindowInfo {
        unsigned long id = 0;
        int width = 0, height = 0;
        std::string title;
        std::string wmClass;
        std::string process;     // from _NET_WM_PID, via /proc
        bool viable = false;     // big enough to be a game window
    };

    // Every mapped window of a usable size, outermost first.
    std::vector<WindowInfo> windows();

    // The game's window. Tried in this order, because each step is a weaker
    // claim than the one before it:
    //   1. what setWindow() was told to use, if it is still there
    //   2. a process whose name looks like the game's
    //   3. a title or class that does
    //   4. when exactly one window is big enough to be a game, that one
    // Empty when nothing convincing is up.
    std::optional<WindowInfo> gameWindow();

    // Use this window and no other, until it goes away (0 to go back to
    // searching). For a picker in the UI, and for the odd install whose
    // window is named something the list below does not know.
    void setWindow(unsigned long id);

    // The drawing surface, in the window's own coordinates - so always at
    // the origin. Kept as a rect so callers read the same as on Windows.
    std::optional<cv::Rect> gameViewport();

    // The window's picture, as BGR. Empty when there is no window, or when
    // the pixmap could not be read this time (it goes stale across a resize,
    // and the next call picks the new one up).
    cv::Mat captureWindow();

    // A piece of it, in the same coordinates gameViewport() reports.
    cv::Mat captureRegion(const cv::Rect& region);

    // Which route the last capture took, so a caller can say what it is
    // looking at - the same idea as ScreenCapture::lastSource().
    enum class Source { None, Composite, Direct };
    Source lastSource() const { return lastSource_; }

private:
    void* display_ = nullptr;          // Display*, kept opaque to spare callers Xlib.h
    unsigned long root_ = 0;
    bool haveComposite_ = false;
    std::string error_;

    unsigned long chosen_ = 0;         // setWindow
    unsigned long pixmapOf_ = 0;       // which window pixmap_ belongs to
    unsigned long pixmap_ = 0;
    int pixmapW_ = 0, pixmapH_ = 0;
    Source lastSource_ = Source::None;

    void releasePixmap();
    bool ensurePixmap(const WindowInfo& win);
};

}  // namespace SCMiner
