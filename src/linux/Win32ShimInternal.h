#pragma once
// What lies behind the handles - shared by the shim's own sources only.
#include "Win32Shim.h"

#include <QColor>
#include <QCursor>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QTextEdit>
#include <QWidget>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ---- GDI objects ---------------------------------------------------------

struct ShimBitmap : HBITMAP__ {
    ShimBitmap(int w, int h) : image(w, h, QImage::Format_ARGB32_Premultiplied) { image.fill(Qt::black); }
    QImage image;
};

struct ShimBrush : HBRUSH__ {
    explicit ShimBrush(QColor c) : colour(c) {}
    QColor colour;
    bool hollow = false;
};

struct ShimFont : HFONT__ {
    explicit ShimFont(QFont f) : font(std::move(f)) {}
    QFont font;
};

struct ShimPen : HPEN__ {
    ShimPen(QColor c, int w) : colour(c), width(w) {}
    QColor colour;
    int width;
};

struct ShimIcon : HICON__ {
    explicit ShimIcon(QIcon i) : icon(std::move(i)) {}
    explicit ShimIcon(Qt::CursorShape s) : shape(s) { kind = Cursor; }
    QIcon icon;
    Qt::CursorShape shape = Qt::ArrowCursor;
};

// Every handle of a kind was made here as the class above, so the cast is safe
inline ShimBitmap* asBitmap(HBITMAP h) { return static_cast<ShimBitmap*>(h); }
inline ShimBrush* asBrush(HBRUSH h) { return static_cast<ShimBrush*>(h); }
inline ShimFont* asFont(HFONT h) { return static_cast<ShimFont*>(h); }
inline ShimPen* asPen(HPEN h) { return static_cast<ShimPen*>(h); }
inline ShimIcon* asIcon(HICON h) { return static_cast<ShimIcon*>(h); }

// ---- Device contexts -----------------------------------------------------
//
// A DC is where drawing goes. Three kinds:
//   - the window's, handed out by BeginPaint while the widget paints: its
//     painter is the paintEvent's, and it lives exactly that long
//   - a memory DC (CreateCompatibleDC) with a bitmap selected into it: the
//     painter is the shim's own and stays open until the bitmap is read
//     (BitBlt, GetDIBits) or swapped out
//   - one with nothing behind it (GetDC(nullptr)): measuring only, on a
//     scratch pixel
struct HDC__ {
    QPaintDevice* device = nullptr;
    QPainter* painter = nullptr;
    bool ownsPainter = false;
    bool memory = false;
    HBITMAP bitmap = nullptr;
    HFONT font = nullptr;
    HBRUSH brush = nullptr;
    HPEN pen = nullptr;
    COLORREF textColour = 0;
    COLORREF bkColour = 0xFFFFFF;
    int bkMode = OPAQUE;
    QImage scratch;

    ~HDC__() { endOwnPainter(); }
    void endOwnPainter() {
        if (ownsPainter && painter) {
            if (painter->isActive()) painter->end();
            delete painter;
        }
        if (ownsPainter) painter = nullptr;
        ownsPainter = false;
    }
};

// ---- Windows -------------------------------------------------------------

struct HWND__ {
    enum Kind { Window, Button, Edit, RichEdit };
    Kind kind = Window;
    QPointer<QWidget> widget;
    WNDPROC proc = nullptr;
    std::wstring className;
    std::wstring text;           // the title, or a button's label
    DWORD style = 0;
    DWORD exStyle = 0;
    HWND parent = nullptr;
    int id = 0;
    HFONT font = nullptr;
    HBRUSH background = nullptr;
    HICON bigIcon = nullptr;
    bool destroyed = false;
    bool trackLeave = false;
    // While the widget paints, or while PrintWindow renders it into a DC of
    // the caller's: what BeginPaint hands back
    HDC paintTarget = nullptr;
    HDC__ paintDC;
    // The last size WM_SIZE reported, so a resize is told once
    int lastW = -1, lastH = -1;
    // What the frame the window manager draws is taken to be - see frameOf()
    bool topMost = false;
    BYTE alpha = 255;
    std::map<UINT_PTR, class QTimer*> timers;
    std::vector<class QShortcut*> hotkeys;
    std::map<int, class QShortcut*> hotkeyById;

    QWidget* w() const { return widget.data(); }
    bool alive() const { return !destroyed && !widget.isNull(); }
};

namespace bedrock_shim {

// The frame the window manager puts round a window of this style: what
// AdjustWindowRectEx adds and what every window rect includes. Nominal -
// the real one is the compositor's business - but used consistently, so a
// rect that went out through GetWindowRect comes back in through
// SetWindowPos as the same window.
struct Frame { int left = 0, top = 0, right = 0, bottom = 0; };
Frame frameOf(DWORD style, UINT dpi);

HWND handleOf(QWidget* widget);
HWND controlHandleOf(QWidget* widget);   // the controls keep a table of their own
LRESULT callProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void postMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, void* qtEvent = nullptr);
// Keeps a copy of a Qt event alive until the message made from it has been
// dispatched or dropped; returns the token that goes into MSG::qt_
void* keepEvent(class QEvent* event);
class QEvent* takeEvent(void* token);
UINT vkFromKey(int qtKey, const QString& text, bool keypad);
WPARAM mouseKeys(Qt::MouseButtons buttons, Qt::KeyboardModifiers mods);
// The colours a parent gives its edit control through WM_CTLCOLOREDIT
void applyEditColours(HWND control);
// The control's own answer to a message (WM_SETFONT, EM_*, ...)
LRESULT controlMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
// The queued key or wheel event of a control, handed to the widget at last
void deliverControlEvent(HWND hwnd, class QEvent* event);
// Paints a control into a DC of the caller's, for PrintWindow
void renderControl(HWND hwnd, QPainter* painter);
QString fromW(const wchar_t* s);
QString fromW(const std::wstring& s);
std::wstring toW(const QString& s);
UINT currentDpi();

// System-wide hotkeys on X11, in X11Hotkeys.cpp: false where there is no
// X display to grab on (Wayland, offscreen)
bool grabGlobalHotkey(int id, UINT modifiers, UINT vk);
void ungrabGlobalHotkey(int id);

// The controls' factories, in Win32Controls.cpp
HWND makeButton(HWND parent, const std::wstring& text, DWORD style, int id, const QRect& at);
HWND makeEdit(HWND parent, const std::wstring& text, DWORD style, int id, const QRect& at);
HWND makeRichEdit(HWND parent, DWORD style, int id, const QRect& at);

}  // namespace bedrock_shim
