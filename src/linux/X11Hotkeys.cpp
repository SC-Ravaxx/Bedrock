// System-wide hotkeys on X11: what RegisterHotKey means on Windows.
//
// Ctrl+Alt+S and Ctrl+Alt+K have to reach Bedrock from inside the game -
// the one moment anyone knows the last minute is worth keeping is while
// looking at the game. On X11 that is XGrabKey on the root window: the
// server hands the key to the grabbing client whoever has the focus. The
// game under Wine or Proton is an X11 client on a Wayland desktop too
// (XWayland), and a grab from another XWayland client reaches it there as
// well; it does not reach native Wayland windows, which would need the
// portal.
//
// The key arrives on Qt's own X connection, so a native event filter sees
// it before Qt does and turns it into the WM_HOTKEY message MinerGUI's
// pump already expects.
#include "Win32ShimInternal.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QtGui/qguiapplication_platform.h>

#include <map>
#include <vector>

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>

// Xlib's macros collide with Qt's names; nothing below needs them by name
#undef Bool
#undef Status
#undef None
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef FontChange
#undef Expose
#undef Unsorted
#undef CursorShape

namespace {

struct Grab {
    int id;
    int keycode;
    unsigned int mods;
    UINT modifiers;   // the MOD_ flags, for the message
    UINT vk;
};

// NumLock and CapsLock must not stand in the way: one grab per combination
const unsigned int kIgnored[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

class HotkeyFilter : public QAbstractNativeEventFilter {
public:
    std::vector<Grab> grabs;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
        if (type != "xcb_generic_event_t") return false;
        auto* event = static_cast<xcb_generic_event_t*>(message);
        if ((event->response_type & ~0x80) != XCB_KEY_PRESS) return false;
        auto* key = reinterpret_cast<xcb_key_press_event_t*>(event);
        const unsigned int state = key->state & ~(LockMask | Mod2Mask);
        for (const Grab& g : grabs) {
            if (g.keycode == key->detail && g.mods == state) {
                bedrock_shim::postMessage(nullptr, WM_HOTKEY, (WPARAM)g.id, MAKELPARAM(g.modifiers & 0xFF, g.vk));
                return true;
            }
        }
        return false;
    }
};

HotkeyFilter* filter() {
    static HotkeyFilter* f = nullptr;
    if (!f) {
        f = new HotkeyFilter();
        QCoreApplication::instance()->installNativeEventFilter(f);
    }
    return f;
}

Display* x11Display() {
    if (QGuiApplication::platformName() != QLatin1String("xcb")) return nullptr;
    auto* x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
    return x11 ? x11->display() : nullptr;
}

KeySym keysymFor(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return XK_a + (vk - 'A');
    if (vk >= '0' && vk <= '9') return XK_0 + (vk - '0');
    if (vk >= VK_F1 && vk <= VK_F12) return XK_F1 + (vk - VK_F1);
    switch (vk) {
        case VK_SPACE: return XK_space;
        case VK_RETURN: return XK_Return;
        case VK_ESCAPE: return XK_Escape;
        case VK_SNAPSHOT: return XK_Print;
        case VK_PAUSE: return XK_Pause;
        default: return 0;
    }
}

}  // namespace

namespace bedrock_shim {

bool grabGlobalHotkey(int id, UINT modifiers, UINT vk) {
    Display* display = x11Display();
    if (!display) return false;
    const KeySym sym = keysymFor(vk);
    if (!sym) return false;
    const int keycode = XKeysymToKeycode(display, sym);
    if (!keycode) return false;
    unsigned int mods = 0;
    if (modifiers & MOD_CONTROL) mods |= ControlMask;
    if (modifiers & MOD_ALT) mods |= Mod1Mask;
    if (modifiers & MOD_SHIFT) mods |= ShiftMask;
    if (modifiers & MOD_WIN) mods |= Mod4Mask;
    ungrabGlobalHotkey(id);
    const Window root = DefaultRootWindow(display);
    for (unsigned int ignored : kIgnored) {
        XGrabKey(display, keycode, mods | ignored, root, False, GrabModeAsync, GrabModeAsync);
    }
    XFlush(display);
    filter()->grabs.push_back({id, keycode, mods, modifiers, vk});
    return true;
}

void ungrabGlobalHotkey(int id) {
    Display* display = x11Display();
    if (!display) return;
    auto& grabs = filter()->grabs;
    for (auto it = grabs.begin(); it != grabs.end();) {
        if (it->id != id) { ++it; continue; }
        const Window root = DefaultRootWindow(display);
        for (unsigned int ignored : kIgnored) XUngrabKey(display, it->keycode, it->mods | ignored, root);
        it = grabs.erase(it);
    }
    XFlush(display);
}

}  // namespace bedrock_shim
