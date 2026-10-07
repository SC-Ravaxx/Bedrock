#include "LinuxCapture.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>

// Xlib spells None and Success as preprocessor macros, so every other use of
// those words in the translation unit breaks - here Source::None. Keep the
// values under names of their own, then drop the macros.
namespace {
constexpr Atom kNoAtom = None;
constexpr int kOk = Success;
}  // namespace
#undef None
#undef Success

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace SCMiner {
namespace {

// X errors are fatal by default - one BadMatch on a window that closed
// between the search and the read would take the whole program down. They
// are all recoverable here, so they are counted and otherwise ignored.
int g_errors = 0;
int quietHandler(Display*, XErrorEvent*) {
    g_errors++;
    return 0;
}

// What a Star Citizen window looks like from the outside. Wine names the X11
// window after the module, so this is the executable's name rather than the
// CryENGINE class the Windows build looks for.
const char* const GAME_NAMES[] = {
    "starcitizen",      // StarCitizen.exe under Wine/Proton
    "star citizen",     // the window title
    "rsi launcher",     // only to be able to say "that is the launcher, not the game"
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool looksLikeGame(const std::string& text) {
    if (text.empty()) return false;
    const std::string t = lower(text);
    // the launcher is explicitly not the game
    if (t.find("launcher") != std::string::npos) return false;
    return t.find("starcitizen") != std::string::npos ||
           t.find("star citizen") != std::string::npos;
}

// The command behind a pid, for the cases where neither title nor class says
// anything useful - a Proton window is often just "steam_app_starcitizen".
std::string processOf(unsigned long pid) {
    if (!pid) return {};
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    if (f && std::getline(f, name)) return name;
    return {};
}

std::string textProperty(Display* d, Window w, const char* name) {
    Atom atom = XInternAtom(d, name, True);
    if (atom == kNoAtom) return {};
    Atom actual = kNoAtom;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(d, w, atom, 0, 1024, False, AnyPropertyType, &actual, &format,
                           &count, &after, &data) != kOk || !data) {
        return {};
    }
    std::string out((const char*)data, count);
    XFree(data);
    return out;
}

unsigned long pidOf(Display* d, Window w) {
    Atom atom = XInternAtom(d, "_NET_WM_PID", True);
    if (atom == kNoAtom) return 0;
    Atom actual = kNoAtom;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(d, w, atom, 0, 1, False, XA_CARDINAL, &actual, &format,
                           &count, &after, &data) != kOk || !data) {
        return 0;
    }
    const unsigned long pid = *(unsigned long*)data;
    XFree(data);
    return pid;
}

}  // namespace

LinuxCapture::LinuxCapture() {
    XSetErrorHandler(quietHandler);
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        error_ = "no X display (is DISPLAY set? a pure Wayland session has no X server "
                 "unless XWayland is running)";
        return;
    }
    display_ = d;
    root_ = DefaultRootWindow(d);

    int event = 0, err = 0;
    haveComposite_ = XCompositeQueryExtension(d, &event, &err) != 0;
    if (!haveComposite_) {
        error_ = "the X server has no Composite extension - without it a window's "
                 "contents cannot be read while something is in front of it";
    }
}

LinuxCapture::~LinuxCapture() {
    releasePixmap();
    if (display_) XCloseDisplay((Display*)display_);
}

void LinuxCapture::releasePixmap() {
    if (pixmap_ && display_) XFreePixmap((Display*)display_, (Pixmap)pixmap_);
    pixmap_ = 0;
    pixmapOf_ = 0;
    pixmapW_ = pixmapH_ = 0;
}

void LinuxCapture::setWindow(unsigned long id) {
    if (chosen_ != id) releasePixmap();
    chosen_ = id;
}

std::vector<LinuxCapture::WindowInfo> LinuxCapture::windows() {
    std::vector<WindowInfo> found;
    if (!display_) return found;
    Display* d = (Display*)display_;

    // The whole tree rather than _NET_CLIENT_LIST: that property only exists
    // where a full EWMH window manager is running, and some compositors
    // (WSLg's, for one) do not provide it at all.
    std::vector<Window> stack{(Window)root_};
    while (!stack.empty()) {
        const Window w = stack.back();
        stack.pop_back();

        Window r = 0, parent = 0, *kids = nullptr;
        unsigned int n = 0;
        if (XQueryTree(d, w, &r, &parent, &kids, &n)) {
            for (unsigned int i = 0; i < n; i++) stack.push_back(kids[i]);
            if (kids) XFree(kids);
        }
        if (w == (Window)root_) continue;

        XWindowAttributes wa;
        if (!XGetWindowAttributes(d, w, &wa)) continue;
        if (wa.map_state != IsViewable) continue;
        if (wa.width < 200 || wa.height < 150) continue;

        WindowInfo info;
        info.id = w;
        info.width = wa.width;
        info.height = wa.height;

        char* name = nullptr;
        if (XFetchName(d, w, &name) && name) {
            info.title = name;
            XFree(name);
        }
        if (info.title.empty()) info.title = textProperty(d, w, "_NET_WM_NAME");

        XClassHint hint = {};
        if (XGetClassHint(d, w, &hint)) {
            if (hint.res_class) { info.wmClass = hint.res_class; XFree(hint.res_class); }
            if (hint.res_name) XFree(hint.res_name);
        }
        info.process = processOf(pidOf(d, w));
        // 320x240 is the same floor ScreenCapture::gameViewport() uses
        info.viable = wa.width >= 320 && wa.height >= 240;
        found.push_back(info);
    }
    return found;
}

std::optional<LinuxCapture::WindowInfo> LinuxCapture::gameWindow() {
    const std::vector<WindowInfo> all = windows();
    if (all.empty()) return std::nullopt;

    if (chosen_) {
        for (const auto& w : all)
            if (w.id == chosen_) return w;
        // the window the caller picked is gone; fall through and search again
    }

    // The process is the strongest claim: a title can be localised and a
    // class is whatever Wine felt like, but the binary is the binary.
    for (const auto& w : all)
        if (w.viable && looksLikeGame(w.process)) return w;
    for (const auto& w : all)
        if (w.viable && (looksLikeGame(w.title) || looksLikeGame(w.wmClass))) return w;

    // Nothing named itself. If exactly one window is large enough to be a
    // game, it is a fair guess - and a wrong guess here is visible at once,
    // because the reader finds no marker in it.
    std::vector<WindowInfo> viable;
    for (const auto& w : all)
        if (w.viable) viable.push_back(w);
    if (viable.size() == 1) return viable.front();

    return std::nullopt;
}

std::optional<cv::Rect> LinuxCapture::gameViewport() {
    const auto win = gameWindow();
    if (!win) return std::nullopt;
    // The window's own coordinates: the capture IS the client area, so there
    // is nothing to offset and nothing of the desktop can be in it.
    return cv::Rect(0, 0, win->width, win->height);
}

bool LinuxCapture::ensurePixmap(const WindowInfo& win) {
    Display* d = (Display*)display_;

    // A pixmap is tied to the window's size at the moment it was named, so
    // it goes stale the first time the window is resized. Cheaper to notice
    // here than to read a torn frame.
    if (pixmap_ && pixmapOf_ == win.id && pixmapW_ == win.width && pixmapH_ == win.height) {
        return true;
    }
    releasePixmap();

    // Asking for automatic redirection is harmless when a compositor already
    // redirects the window - which, on any desktop that draws shadows, it
    // does. Without one this is what puts the contents somewhere readable.
    g_errors = 0;
    XCompositeRedirectWindow(d, (Window)win.id, CompositeRedirectAutomatic);
    XSync(d, False);

    const Pixmap pm = XCompositeNameWindowPixmap(d, (Window)win.id);
    XSync(d, False);
    if (!pm || g_errors) {
        error_ = "the window has no Composite pixmap - it may have closed, or the "
                 "server may not be redirecting it";
        return false;
    }
    pixmap_ = pm;
    pixmapOf_ = win.id;
    pixmapW_ = win.width;
    pixmapH_ = win.height;
    return true;
}

cv::Mat LinuxCapture::captureWindow() {
    lastSource_ = Source::None;
    if (!ok()) return {};
    const auto win = gameWindow();
    if (!win) {
        error_ = "no game window found";
        return {};
    }
    return captureRegion(cv::Rect(0, 0, win->width, win->height));
}

cv::Mat LinuxCapture::captureRegion(const cv::Rect& region) {
    lastSource_ = Source::None;
    if (!ok()) return {};
    const auto win = gameWindow();
    if (!win) {
        error_ = "no game window found";
        return {};
    }

    cv::Rect want = region & cv::Rect(0, 0, win->width, win->height);
    if (want.width <= 0 || want.height <= 0) {
        error_ = "the requested region is outside the window";
        return {};
    }

    Display* d = (Display*)display_;
    Drawable source = 0;
    if (ensurePixmap(*win)) {
        source = (Drawable)pixmap_;
        lastSource_ = Source::Composite;
    } else {
        // Without a pixmap the window itself is still worth a try: on a
        // server that is not compositing, its contents are on screen and
        // readable. Anything in front of it lands in the picture then, which
        // is exactly the failure mode the Composite route avoids.
        source = (Drawable)win->id;
        lastSource_ = Source::Direct;
    }

    g_errors = 0;
    XImage* img = XGetImage(d, source, want.x, want.y, want.width, want.height,
                            AllPlanes, ZPixmap);
    if (!img || g_errors) {
        if (img) XDestroyImage(img);
        releasePixmap();   // stale across a resize; the next call names a fresh one
        error_ = "XGetImage failed on the window";
        lastSource_ = Source::None;
        return {};
    }

    cv::Mat out;
    if (img->bits_per_pixel == 32) {
        // BGRA in memory on every server worth the name; the alpha byte is
        // undefined for a redirected window, so it is dropped rather than
        // trusted.
        const cv::Mat wrapped(img->height, img->width, CV_8UC4, img->data,
                              (size_t)img->bytes_per_line);
        cv::cvtColor(wrapped, out, cv::COLOR_BGRA2BGR);
    } else if (img->bits_per_pixel == 24) {
        const cv::Mat wrapped(img->height, img->width, CV_8UC3, img->data,
                              (size_t)img->bytes_per_line);
        out = wrapped.clone();
    } else {
        // 16-bit and paletted servers exist but no one runs a game on one;
        // going the slow way keeps it correct rather than guessing at a mask.
        out.create(img->height, img->width, CV_8UC3);
        for (int y = 0; y < img->height; y++) {
            for (int x = 0; x < img->width; x++) {
                const unsigned long p = XGetPixel(img, x, y);
                out.at<cv::Vec3b>(y, x) = cv::Vec3b((uchar)(p & 0xFF),
                                                    (uchar)((p >> 8) & 0xFF),
                                                    (uchar)((p >> 16) & 0xFF));
            }
        }
    }
    XDestroyImage(img);
    error_.clear();
    return out;
}

}  // namespace SCMiner
