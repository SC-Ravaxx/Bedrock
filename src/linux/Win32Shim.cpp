// Win32 spoken by Qt - the windows, the messages, the drawing surfaces.
// See include/Win32Shim.h for what this is and is not.
#include "Win32ShimInternal.h"
#include "EmbeddedData.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_map>

using namespace bedrock_shim;

// The known folders the notebook asks for. The values are Windows' own, so
// a comparison against them says which folder was meant.
const GUID FOLDERID_Documents = {0xFDD39AD0, 0x238F, 0x46AF, {0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7}};
const GUID FOLDERID_Pictures = {0x33E28130, 0x4E1E, 0x4676, {0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB}};

namespace {

struct WindowClass {
    WNDPROC proc = nullptr;
    HBRUSH background = nullptr;
    HCURSOR cursor = nullptr;
    HICON icon = nullptr;
    UINT style = 0;
};

std::map<std::wstring, WindowClass>& classes() {
    static std::map<std::wstring, WindowClass> c;
    return c;
}

std::unordered_map<QWidget*, HWND>& byWidget() {
    static std::unordered_map<QWidget*, HWND> m;
    return m;
}

std::deque<MSG>& queue() {
    static std::deque<MSG> q;
    return q;
}

bool g_quitPosted = false;
int g_quitCode = 0;
DWORD g_lastError = 0;
HWND g_capture = nullptr;
HWND g_cursorTarget = nullptr;   // the window the last WM_SETCURSOR went to
QImage g_appIcon;
QElapsedTimer& clockSince() {
    static QElapsedTimer t = [] { QElapsedTimer e; e.start(); return e; }();
    return t;
}

// An owner for the timers and shortcuts, so they die with the program
QObject* shimOwner() {
    static QObject* o = new QObject();
    return o;
}

// The events kept alive for queued messages. Bounded: a message the
// application drops without dispatching leaves its event here until the
// ring moves on.
struct KeptEvent { uint64_t token; std::unique_ptr<QEvent> event; };
std::deque<KeptEvent>& keptEvents() {
    static std::deque<KeptEvent> k;
    return k;
}
uint64_t g_eventToken = 1;

QRect toQRect(const RECT& r) { return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top); }
RECT toRECT(const QRect& r) { return RECT{r.left(), r.top(), r.left() + r.width(), r.top() + r.height()}; }
QColor fromColorref(COLORREF c) { return QColor(GetRValue(c), GetGValue(c), GetBValue(c)); }

QScreen* screenAt(const QPoint& pt) {
    for (QScreen* s : QGuiApplication::screens()) {
        if (s->geometry().contains(pt)) return s;
    }
    return nullptr;
}

QScreen* nearestScreen(const QRect& r) {
    QScreen* best = QGuiApplication::primaryScreen();
    qint64 bestDistance = -1;
    for (QScreen* s : QGuiApplication::screens()) {
        const QRect g = s->geometry();
        if (g.intersects(r)) return s;
        const QPoint c = r.center();
        const int dx = std::max({g.left() - c.x(), 0, c.x() - g.right()});
        const int dy = std::max({g.top() - c.y(), 0, c.y() - g.bottom()});
        const qint64 d = (qint64)dx * dx + (qint64)dy * dy;
        if (bestDistance < 0 || d < bestDistance) { bestDistance = d; best = s; }
    }
    return best;
}

QScreen* screenOf(HWND hwnd) {
    if (hwnd && hwnd->alive()) {
        if (QScreen* s = hwnd->w()->screen()) return s;
    }
    return QGuiApplication::primaryScreen();
}

bool isTopLevel(HWND h) { return !(h->style & WS_CHILD); }

// ---- the window widget ----------------------------------------------------

class ShimWindow : public QWidget {
public:
    ShimWindow(HWND h, QWidget* parent, Qt::WindowFlags flags) : QWidget(parent, flags), h_(h) {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_DeleteOnClose, false);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!h_->alive() || !h_->proc) return;
        QPainter p(this);
        h_->paintDC = HDC__();
        h_->paintDC.device = this;
        h_->paintDC.painter = &p;
        h_->paintDC.ownsPainter = false;
        h_->paintTarget = &h_->paintDC;
        // Windows asks the window to clear itself first and clears for it
        // if it says no; every one of Bedrock's windows says yes
        if (!callProc(h_, WM_ERASEBKGND, (WPARAM)&h_->paintDC, 0)) {
            if (h_->background) p.fillRect(rect(), asBrush(h_->background)->colour);
        }
        callProc(h_, WM_PAINT, 0, 0);
        h_->paintTarget = nullptr;
        h_->paintDC.painter = nullptr;
        h_->paintDC.device = nullptr;
    }
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        reportSize();
    }
    void moveEvent(QMoveEvent* e) override {
        QWidget::moveEvent(e);
        if (h_->alive()) callProc(h_, WM_MOVE, 0, MAKELPARAM(x(), y()));
    }
    void changeEvent(QEvent* e) override {
        QWidget::changeEvent(e);
        if (e->type() == QEvent::WindowStateChange) {
            h_->lastW = -1;   // the state is part of WM_SIZE, so say it again
            reportSize();
        }
        if (e->type() == QEvent::ActivationChange && h_->alive()) {
            callProc(h_, WM_ACTIVATE, isActiveWindow() ? 1 : 0, 0);
        }
    }
    void closeEvent(QCloseEvent* e) override {
        // The procedure decides: Bedrock's windows hide or destroy
        // themselves in WM_CLOSE, so Qt is never the one to close them
        e->ignore();
        if (h_->alive()) callProc(h_, WM_CLOSE, 0, 0);
    }
    void mousePressEvent(QMouseEvent* e) override { press(e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        // Without CS_DBLCLKS a second click is a click, and no class here
        // asks for double clicks
        press(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        const UINT msg = e->button() == Qt::RightButton ? WM_RBUTTONUP
                       : e->button() == Qt::MiddleButton ? WM_MBUTTONUP : WM_LBUTTONUP;
        const QPoint at = e->position().toPoint();
        postMessage(h_, msg, mouseKeys(e->buttons(), e->modifiers()), MAKELPARAM(at.x(), at.y()));
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!h_->alive()) return;
        if (!(e->buttons() & Qt::LeftButton) || g_capture == h_ || true) {
            g_cursorTarget = h_;
            if (!callProc(h_, WM_SETCURSOR, (WPARAM)h_, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE))) {
                // DefWindowProc's answer: the class cursor
                unsetCursor();
            }
        }
        const QPoint at = e->position().toPoint();
        postMessage(h_, WM_MOUSEMOVE, mouseKeys(e->buttons(), e->modifiers()), MAKELPARAM(at.x(), at.y()));
    }
    void leaveEvent(QEvent*) override {
        if (h_->alive()) postMessage(h_, WM_MOUSELEAVE, 0, 0);
    }
    void wheelEvent(QWheelEvent* e) override {
        const QPoint global = e->globalPosition().toPoint();
        const int delta = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
        const WPARAM w = MAKEWPARAM(mouseKeys(e->buttons(), e->modifiers()), (WORD)(short)delta);
        postMessage(h_, WM_MOUSEWHEEL, w, MAKELPARAM(global.x(), global.y()));
        e->accept();
    }
    void keyPressEvent(QKeyEvent* e) override {
        const UINT vk = vkFromKey(e->key(), e->text(), e->modifiers() & Qt::KeypadModifier);
        const bool sys = (e->modifiers() & Qt::AltModifier) && e->key() != Qt::Key_Alt;
        LPARAM lp = 1 | ((LPARAM)(e->nativeScanCode() & 0xFF) << 16);
        if (e->isAutoRepeat()) lp |= (LPARAM)1 << 30;
        postMessage(h_, sys ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, lp, keepEvent(e->clone()));
        e->accept();
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        const UINT vk = vkFromKey(e->key(), e->text(), e->modifiers() & Qt::KeypadModifier);
        const bool sys = (e->modifiers() & Qt::AltModifier) && e->key() != Qt::Key_Alt;
        LPARAM lp = 1 | ((LPARAM)(e->nativeScanCode() & 0xFF) << 16) | ((LPARAM)3 << 30);
        postMessage(h_, sys ? WM_SYSKEYUP : WM_KEYUP, vk, lp);
        e->accept();
    }
    void focusInEvent(QFocusEvent* e) override {
        QWidget::focusInEvent(e);
        if (h_->alive()) callProc(h_, WM_SETFOCUS, 0, 0);
    }
    void focusOutEvent(QFocusEvent* e) override {
        QWidget::focusOutEvent(e);
        if (h_->alive()) callProc(h_, WM_KILLFOCUS, 0, 0);
    }
    bool focusNextPrevChild(bool) override { return false; }   // Tab is a key here

private:
    void press(QMouseEvent* e) {
        if (!h_->alive()) return;
        const QPoint global = e->globalPosition().toPoint();
        if (e->button() == Qt::LeftButton) {
            // The window says what the point is. In Bedrock's overlay mode
            // the whole client area answers "caption", which is how a
            // frameless window gets dragged.
            const LRESULT hit = callProc(h_, WM_NCHITTEST, 0, MAKELPARAM(global.x(), global.y()));
            if (hit == HTCAPTION) {
                if (QWindow* win = window()->windowHandle()) win->startSystemMove();
                return;
            }
        }
        const UINT msg = e->button() == Qt::RightButton ? WM_RBUTTONDOWN
                       : e->button() == Qt::MiddleButton ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
        const QPoint at = e->position().toPoint();
        postMessage(h_, msg, mouseKeys(e->buttons(), e->modifiers()), MAKELPARAM(at.x(), at.y()));
    }
    void reportSize() {
        if (!h_->alive()) return;
        if (width() == h_->lastW && height() == h_->lastH) return;
        h_->lastW = width();
        h_->lastH = height();
        const WPARAM state = isMaximized() ? SIZE_MAXIMIZED : isMinimized() ? SIZE_MINIMIZED : SIZE_RESTORED;
        callProc(h_, WM_SIZE, state, MAKELPARAM(width(), height()));
    }
    HWND h_;
};

Qt::WindowFlags flagsFor(HWND h) {
    if (!isTopLevel(h)) return Qt::Widget;
    Qt::WindowFlags f = Qt::Window;
    if (!(h->style & WS_CAPTION)) f |= Qt::FramelessWindowHint;
    if (h->exStyle & WS_EX_TOOLWINDOW) f |= Qt::Tool;
    if (h->topMost || (h->exStyle & WS_EX_TOPMOST)) f |= Qt::WindowStaysOnTopHint;
    if (h->exStyle & WS_EX_NOACTIVATE) f |= Qt::WindowDoesNotAcceptFocus;
    if ((h->style & WS_CAPTION) && !(h->style & WS_SYSMENU)) f |= Qt::CustomizeWindowHint;
    return f;
}

void applyFlags(HWND h) {
    if (!h->alive() || !isTopLevel(h)) return;
    QWidget* w = h->w();
    const Qt::WindowFlags want = flagsFor(h);
    if (w->windowFlags() != want) {
        const bool vis = w->isVisible();
        const QPoint at = w->pos();
        w->setWindowFlags(want);
        w->move(at);
        if (vis) w->show();
    }
    w->setAttribute(Qt::WA_ShowWithoutActivating, (h->exStyle & WS_EX_NOACTIVATE) != 0);
    if (h->exStyle & WS_EX_NOACTIVATE) w->setFocusPolicy(Qt::NoFocus);
}

void sendMinMax(HWND h) {
    if (!h->alive() || !isTopLevel(h) || !h->proc) return;
    MINMAXINFO mmi{};
    mmi.ptMaxTrackSize = {30000, 30000};
    mmi.ptMinTrackSize = {0, 0};
    callProc(h, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);
    const Frame f = frameOf(h->style, currentDpi());
    const int minW = std::max(0, (int)mmi.ptMinTrackSize.x - f.left - f.right);
    const int minH = std::max(0, (int)mmi.ptMinTrackSize.y - f.top - f.bottom);
    if (minW > 0 || minH > 0) h->w()->setMinimumSize(minW, minH);
}

void sendMinMax(HWND h);

void placeWindow(HWND h, int x, int y, int w, int hgt, bool move, bool size) {
    QWidget* wid = h->w();
    if (isTopLevel(h)) {
        const Frame f = frameOf(h->style, currentDpi());
        if (size) {
            // Windows asks the window for its limits before every resize;
            // Bedrock's answer changes with the mode (the overlay is allowed
            // to be small), so the limits are asked for again here
            sendMinMax(h);
            const int cw = std::max(1, w - f.left - f.right);
            const int ch = std::max(1, hgt - f.top - f.bottom);
            if (!(h->style & WS_THICKFRAME) && (h->style & WS_CAPTION)) {
                wid->setMinimumSize(0, 0);
                wid->setMaximumSize(16777215, 16777215);
            }
            wid->resize(cw, ch);
            if (!(h->style & WS_THICKFRAME) && (h->style & WS_CAPTION)) wid->setFixedSize(cw, ch);
        }
        if (move) wid->move(x, y);
    } else {
        QRect g = wid->geometry();
        if (move) g.moveTo(x, y);
        if (size) g.setSize(QSize(std::max(0, w), std::max(0, hgt)));
        wid->setGeometry(g);
    }
    if (size && h->kind == HWND__::Window && !wid->isVisible()) {
        // A hidden widget is told about its size when shown; the procedure
        // wants it now, the way Windows tells it
        if (wid->width() != h->lastW || wid->height() != h->lastH) {
            h->lastW = wid->width();
            h->lastH = wid->height();
            callProc(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(wid->width(), wid->height()));
        }
    }
}

std::vector<HWND> childrenOf(HWND h) {
    std::vector<HWND> out;
    if (!h->alive()) return out;
    for (QObject* o : h->w()->children()) {
        if (auto* w = qobject_cast<QWidget*>(o)) {
            if (HWND c = handleOf(w)) {
                if (!(c->style & WS_CHILD)) continue;
                out.push_back(c);
            }
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
namespace bedrock_shim {

Frame frameOf(DWORD style, UINT dpi) {
    Frame f;
    if (style & WS_CAPTION) {
        f.left = f.right = f.bottom = MulDiv(8, (int)dpi, 96);
        f.top = MulDiv(31, (int)dpi, 96);
    }
    return f;
}

HWND handleOf(QWidget* widget) {
    auto it = byWidget().find(widget);
    if (it != byWidget().end()) return it->second;
    return controlHandleOf(widget);
}

LRESULT callProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!hwnd || hwnd->destroyed) return 0;
    if (hwnd->kind != HWND__::Window) return controlMessage(hwnd, msg, wParam, lParam);
    if (!hwnd->proc) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return hwnd->proc(hwnd, msg, wParam, lParam);
}

void postMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, void* qtEvent) {
    MSG m{};
    m.hwnd = hwnd;
    m.message = msg;
    m.wParam = wParam;
    m.lParam = lParam;
    m.time = GetTickCount();
    const QPoint c = QCursor::pos();
    m.pt = {c.x(), c.y()};
    m.qt_ = qtEvent;
    queue().push_back(m);
}

void* keepEvent(QEvent* event) {
    auto& kept = keptEvents();
    const uint64_t token = g_eventToken++;
    kept.push_back({token, std::unique_ptr<QEvent>(event)});
    while (kept.size() > 256) kept.pop_front();
    return (void*)(uintptr_t)token;
}

QEvent* takeEvent(void* token) {
    if (!token) return nullptr;
    auto& kept = keptEvents();
    const uint64_t t = (uint64_t)(uintptr_t)token;
    for (auto it = kept.begin(); it != kept.end(); ++it) {
        if (it->token == t) return it->event.get();
    }
    return nullptr;
}

UINT vkFromKey(int key, const QString& text, bool keypad) {
    switch (key) {
        case Qt::Key_Backspace: return VK_BACK;
        case Qt::Key_Tab: case Qt::Key_Backtab: return VK_TAB;
        case Qt::Key_Return: case Qt::Key_Enter: return VK_RETURN;
        case Qt::Key_Shift: return VK_SHIFT;
        case Qt::Key_Control: return VK_CONTROL;
        case Qt::Key_Alt: return VK_MENU;
        case Qt::Key_Escape: return VK_ESCAPE;
        case Qt::Key_Space: return VK_SPACE;
        case Qt::Key_PageUp: return VK_PRIOR;
        case Qt::Key_PageDown: return VK_NEXT;
        case Qt::Key_End: return VK_END;
        case Qt::Key_Home: return VK_HOME;
        case Qt::Key_Left: return VK_LEFT;
        case Qt::Key_Up: return VK_UP;
        case Qt::Key_Right: return VK_RIGHT;
        case Qt::Key_Down: return VK_DOWN;
        case Qt::Key_Insert: return VK_INSERT;
        case Qt::Key_Delete: return VK_DELETE;
        case Qt::Key_Plus: return keypad ? VK_ADD : VK_OEM_PLUS;
        case Qt::Key_Equal: return VK_OEM_PLUS;
        case Qt::Key_Minus: return keypad ? VK_SUBTRACT : VK_OEM_MINUS;
        case Qt::Key_Period: return keypad ? VK_DECIMAL : VK_OEM_PERIOD;
        case Qt::Key_Comma: return VK_OEM_COMMA;
        case Qt::Key_Asterisk: return keypad ? VK_MULTIPLY : '8';
        case Qt::Key_Slash: return keypad ? VK_DIVIDE : 0xBF;
        default: break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) return VK_F1 + (key - Qt::Key_F1);
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return keypad ? VK_NUMPAD0 + (key - Qt::Key_0) : '0' + (key - Qt::Key_0);
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return 'A' + (key - Qt::Key_A);
    // A letter typed through a layout Qt names otherwise: what it printed
    if (text.size() == 1) {
        const QChar c = text.at(0).toUpper();
        if (c.unicode() >= 'A' && c.unicode() <= 'Z') return c.unicode();
        if (c.unicode() >= '0' && c.unicode() <= '9') return c.unicode();
    }
    return key & 0xFF;
}

WPARAM mouseKeys(Qt::MouseButtons buttons, Qt::KeyboardModifiers mods) {
    WPARAM k = 0;
    if (buttons & Qt::LeftButton) k |= MK_LBUTTON;
    if (buttons & Qt::RightButton) k |= MK_RBUTTON;
    if (buttons & Qt::MiddleButton) k |= MK_MBUTTON;
    if (mods & Qt::ShiftModifier) k |= MK_SHIFT;
    if (mods & Qt::ControlModifier) k |= MK_CONTROL;
    return k;
}

QString fromW(const wchar_t* s) { return s ? QString::fromWCharArray(s) : QString(); }
QString fromW(const std::wstring& s) { return QString::fromWCharArray(s.data(), (int)s.size()); }
std::wstring toW(const QString& s) { return s.toStdWString(); }

UINT currentDpi() { return GetDpiForSystem(); }

QPainter* painterFor(HDC hdc) {
    if (!hdc) return nullptr;
    if (hdc->painter && hdc->painter->isActive()) return hdc->painter;
    hdc->endOwnPainter();
    QPaintDevice* device = hdc->device;
    if (hdc->memory && hdc->bitmap) device = &asBitmap(hdc->bitmap)->image;
    if (!device) {
        if (hdc->scratch.isNull()) hdc->scratch = QImage(4, 4, QImage::Format_ARGB32_Premultiplied);
        device = &hdc->scratch;
    }
    hdc->painter = new QPainter(device);
    hdc->ownsPainter = true;
    return hdc->painter;
}

QImage* imageFor(HDC hdc) {
    if (!hdc || !hdc->bitmap) return nullptr;
    return &asBitmap(hdc->bitmap)->image;
}

QImage imageFromDib(const BITMAPINFOHEADER* header, const void* bits) {
    if (!header || !bits) return QImage();
    const int w = header->biWidth;
    const int h = std::abs(header->biHeight);
    const bool bottomUp = header->biHeight > 0;
    if (w <= 0 || h <= 0) return QImage();
    QImage out(w, h, QImage::Format_ARGB32);
    const auto* src = static_cast<const unsigned char*>(bits);
    if (header->biBitCount == 32) {
        const int stride = w * 4;
        for (int y = 0; y < h; y++) {
            const unsigned char* row = src + (size_t)(bottomUp ? h - 1 - y : y) * stride;
            auto* dst = reinterpret_cast<uint32_t*>(out.scanLine(y));
            for (int x = 0; x < w; x++) {
                const unsigned char* p = row + x * 4;
                dst[x] = 0xFF000000u | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            }
        }
        return out;
    }
    if (header->biBitCount == 24) {
        const int stride = ((w * 3 + 3) / 4) * 4;
        for (int y = 0; y < h; y++) {
            const unsigned char* row = src + (size_t)(bottomUp ? h - 1 - y : y) * stride;
            auto* dst = reinterpret_cast<uint32_t*>(out.scanLine(y));
            for (int x = 0; x < w; x++) {
                const unsigned char* p = row + x * 3;
                dst[x] = 0xFF000000u | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            }
        }
        return out;
    }
    return QImage();
}

QWidget* widgetFor(HWND hwnd) { return hwnd && hwnd->alive() ? hwnd->w() : nullptr; }

void setApplicationIcon(const QImage& icon) { g_appIcon = icon; }

int registerFonts() {
    int loaded = 0;
    for (const char* name : {"FONT_SELAWIK", "FONT_SELAWIK_BOLD", "FONT_SELAWIK_SEMIBOLD", "FONT_SELAWIK_LIGHT",
                             "FONT_DDIN", "FONT_DDIN_BOLD", "FONT_DDIN_CONDENSED", "FONT_DDIN_CONDENSED_BOLD"}) {
        const unsigned char* data = nullptr;
        size_t size = 0;
        if (!SCMiner::embeddedResource(name, &data, &size)) continue;
        // The bytes belong to the program for as long as it runs, so the
        // database can read them in place
        const QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(data), (qsizetype)size);
        if (QFontDatabase::addApplicationFontFromData(bytes) >= 0) loaded++;
    }
    // The names MinerGUI asks for, answered by what is here. Selawik is
    // Segoe UI's metric twin; its weights are families of their own, as
    // Segoe's are on Windows. D-DIN stands in for Bahnschrift.
    QFont::insertSubstitution(QStringLiteral("Segoe UI"), QStringLiteral("Selawik"));
    QFont::insertSubstitution(QStringLiteral("Segoe UI Semibold"), QStringLiteral("Selawik Semibold"));
    QFont::insertSubstitution(QStringLiteral("Segoe UI Light"), QStringLiteral("Selawik Light"));
    QFont::insertSubstitution(QStringLiteral("Segoe UI Variable"), QStringLiteral("Selawik"));
    QFont::insertSubstitution(QStringLiteral("Bahnschrift"), QStringLiteral("D-DIN"));
    // ...and the notebook's fixed pitch: whichever of the usual monospaced
    // faces the machine has
    const QStringList families = QFontDatabase::families();
    for (const char* mono : {"Cascadia Mono", "DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Monospace"}) {
        if (families.contains(QLatin1String(mono), Qt::CaseInsensitive)) {
            QFont::insertSubstitution(QStringLiteral("Consolas"), QLatin1String(mono));
            break;
        }
    }
    return loaded;
}

}  // namespace bedrock_shim

// ---------------------------------------------------------------------------
// Window classes and windows
// ---------------------------------------------------------------------------

BOOL RegisterClassExW(const WNDCLASSEXW* wc) {
    if (!wc || !wc->lpszClassName) return FALSE;
    WindowClass c;
    c.proc = wc->lpfnWndProc;
    c.background = wc->hbrBackground;
    c.cursor = wc->hCursor;
    c.icon = wc->hIcon;
    c.style = wc->style;
    classes()[wc->lpszClassName] = c;
    return TRUE;
}

BOOL RegisterClassExA(const WNDCLASSEXA* wc) {
    if (!wc || !wc->lpszClassName) return FALSE;
    WNDCLASSEXW wide{};
    wide.cbSize = sizeof(wide);
    wide.style = wc->style;
    wide.lpfnWndProc = wc->lpfnWndProc;
    wide.hInstance = wc->hInstance;
    wide.hIcon = wc->hIcon;
    wide.hCursor = wc->hCursor;
    wide.hbrBackground = wc->hbrBackground;
    const std::wstring name = toW(QString::fromLatin1(wc->lpszClassName));
    wide.lpszClassName = name.c_str();
    wide.hIconSm = wc->hIconSm;
    return RegisterClassExW(&wide);
}

BOOL UnregisterClassW(LPCWSTR className, HINSTANCE) {
    if (!className) return FALSE;
    return classes().erase(className) > 0 ? TRUE : FALSE;
}

HWND CreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR title, DWORD style, int x, int y,
                     int w, int h, HWND parent, HMENU menu, HINSTANCE, LPVOID) {
    if (!className) return nullptr;
    const std::wstring cls(className);
    const int id = (int)(intptr_t)menu;
    const QRect at(x == CW_USEDEFAULT ? 0 : x, y == CW_USEDEFAULT ? 0 : y,
                   w == CW_USEDEFAULT ? 640 : w, h == CW_USEDEFAULT ? 480 : h);
    std::wstring upper = cls;
    for (wchar_t& c : upper) c = (wchar_t)towupper(c);
    if (upper == L"BUTTON") return makeButton(parent, title ? title : L"", style, id, at);
    if (upper == L"EDIT") return makeEdit(parent, title ? title : L"", style, id, at);
    if (upper == L"RICHEDIT50W" || upper == L"RICHEDIT20W" || upper == L"RICHEDIT") {
        return makeRichEdit(parent, style, id, at);
    }
    auto it = classes().find(cls);
    if (it == classes().end()) {
        g_lastError = 1407;   // ERROR_CANNOT_FIND_WND_CLASS
        return nullptr;
    }

    HWND hwnd = new HWND__();
    hwnd->kind = HWND__::Window;
    hwnd->proc = it->second.proc;
    hwnd->className = cls;
    hwnd->text = title ? title : L"";
    hwnd->style = style;
    hwnd->exStyle = exStyle;
    hwnd->parent = parent;
    hwnd->id = id;
    hwnd->background = it->second.background;
    hwnd->bigIcon = it->second.icon;
    hwnd->topMost = (exStyle & WS_EX_TOPMOST) != 0;

    QWidget* parentWidget = parent && parent->alive() ? parent->w() : nullptr;
    auto* widget = new ShimWindow(hwnd, parentWidget, flagsFor(hwnd));
    hwnd->widget = widget;
    byWidget()[widget] = hwnd;
    QObject::connect(widget, &QObject::destroyed, [hwnd, widget] {
        hwnd->destroyed = true;
        byWidget().erase(widget);
    });
    widget->setWindowTitle(fromW(hwnd->text));
    if (it->second.icon) widget->setWindowIcon(asIcon(it->second.icon)->icon);
    if (exStyle & WS_EX_NOACTIVATE) {
        widget->setAttribute(Qt::WA_ShowWithoutActivating, true);
        widget->setFocusPolicy(Qt::NoFocus);
    }
    if (isTopLevel(hwnd) && x == CW_USEDEFAULT) {
        // Where a new window goes: Qt's choice, like Windows' own
        placeWindow(hwnd, 0, 0, at.width(), at.height(), false, true);
    } else {
        placeWindow(hwnd, at.x(), at.y(), at.width(), at.height(), true, true);
    }
    callProc(hwnd, WM_CREATE, 0, 0);
    sendMinMax(hwnd);
    if (hwnd->lastW < 0) {
        hwnd->lastW = widget->width();
        hwnd->lastH = widget->height();
        callProc(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(widget->width(), widget->height()));
    }
    callProc(hwnd, WM_MOVE, 0, MAKELPARAM(widget->x(), widget->y()));
    if (style & WS_VISIBLE) widget->show();
    return hwnd;
}

HWND CreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y,
                     int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    const std::wstring cls = className ? toW(QString::fromLatin1(className)) : std::wstring();
    const std::wstring t = title ? toW(QString::fromUtf8(title)) : std::wstring();
    return CreateWindowExW(exStyle, cls.c_str(), t.c_str(), style, x, y, w, h, parent, menu, inst, param);
}

BOOL DestroyWindow(HWND hwnd) {
    if (!hwnd || hwnd->destroyed) return FALSE;
    for (HWND child : childrenOf(hwnd)) DestroyWindow(child);
    if (hwnd->kind == HWND__::Window) callProc(hwnd, WM_DESTROY, 0, 0);
    hwnd->destroyed = true;
    for (auto& t : hwnd->timers) delete t.second;
    hwnd->timers.clear();
    for (auto& s : hwnd->hotkeyById) delete s.second;
    hwnd->hotkeyById.clear();
    if (g_capture == hwnd) g_capture = nullptr;
    if (QWidget* w = hwnd->w()) {
        byWidget().erase(w);
        w->hide();
        w->deleteLater();
    }
    // Messages still queued for it are dropped when they come up
    return TRUE;
}

BOOL ShowWindow(HWND hwnd, int cmd) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    QWidget* w = hwnd->w();
    const BOOL was = w->isVisible();
    switch (cmd) {
        case SW_HIDE:
            w->hide();
            break;
        case SW_SHOWMAXIMIZED:
            w->showMaximized();
            break;
        case SW_SHOWMINIMIZED:
        case SW_MINIMIZE:
            w->showMinimized();
            break;
        case SW_SHOWNOACTIVATE:
        case SW_SHOWNA:
        case SW_SHOWMINNOACTIVE: {
            const bool before = w->testAttribute(Qt::WA_ShowWithoutActivating);
            w->setAttribute(Qt::WA_ShowWithoutActivating, true);
            w->show();
            w->setAttribute(Qt::WA_ShowWithoutActivating, before || (hwnd->exStyle & WS_EX_NOACTIVATE));
            break;
        }
        case SW_SHOWNORMAL:
        case SW_RESTORE:
            w->showNormal();
            break;
        default:
            w->show();
            break;
    }
    return was;
}

BOOL UpdateWindow(HWND hwnd) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    if (hwnd->w()->isVisible()) hwnd->w()->repaint();
    return TRUE;
}

BOOL InvalidateRect(HWND hwnd, const RECT* rect, BOOL) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    if (rect) hwnd->w()->update(toQRect(*rect));
    else hwnd->w()->update();
    return TRUE;
}

BOOL RedrawWindow(HWND hwnd, const RECT* rect, HRGN, UINT flags) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    if (flags & RDW_UPDATENOW) {
        if (hwnd->w()->isVisible()) hwnd->w()->repaint();
    } else {
        InvalidateRect(hwnd, rect, (flags & RDW_ERASE) != 0);
    }
    return TRUE;
}

BOOL MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    placeWindow(hwnd, x, y, w, h, true, true);
    return TRUE;
}

BOOL SetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    QWidget* wid = hwnd->w();
    if (!(flags & SWP_NOZORDER) && isTopLevel(hwnd)) {
        if (after == HWND_TOPMOST && !hwnd->topMost) {
            hwnd->topMost = true;
            applyFlags(hwnd);
        } else if (after == HWND_NOTOPMOST && hwnd->topMost) {
            hwnd->topMost = false;
            applyFlags(hwnd);
        } else if (after == HWND_BOTTOM) {
            wid->lower();
        } else if (after == HWND_TOP) {
            wid->raise();
        }
    }
    if (flags & SWP_FRAMECHANGED) applyFlags(hwnd);
    placeWindow(hwnd, x, y, w, h, !(flags & SWP_NOMOVE), !(flags & SWP_NOSIZE));
    if (flags & SWP_SHOWWINDOW) {
        if (flags & SWP_NOACTIVATE) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        else wid->show();
    }
    if (flags & SWP_HIDEWINDOW) wid->hide();
    return TRUE;
}

BOOL GetWindowRect(HWND hwnd, RECT* rect) {
    if (!hwnd || !hwnd->alive() || !rect) return FALSE;
    QWidget* w = hwnd->w();
    if (isTopLevel(hwnd)) {
        const Frame f = frameOf(hwnd->style, currentDpi());
        const QRect g = w->geometry();
        *rect = {g.left() - f.left, g.top() - f.top, g.left() + g.width() + f.right,
                 g.top() + g.height() + f.bottom};
    } else {
        const QPoint at = w->mapToGlobal(QPoint(0, 0));
        *rect = {at.x(), at.y(), at.x() + w->width(), at.y() + w->height()};
    }
    return TRUE;
}

BOOL GetClientRect(HWND hwnd, RECT* rect) {
    if (!hwnd || !hwnd->alive() || !rect) return FALSE;
    *rect = {0, 0, hwnd->w()->width(), hwnd->w()->height()};
    return TRUE;
}

BOOL ClientToScreen(HWND hwnd, POINT* pt) {
    if (!hwnd || !hwnd->alive() || !pt) return FALSE;
    const QPoint g = hwnd->w()->mapToGlobal(QPoint(pt->x, pt->y));
    *pt = {g.x(), g.y()};
    return TRUE;
}

BOOL ScreenToClient(HWND hwnd, POINT* pt) {
    if (!hwnd || !hwnd->alive() || !pt) return FALSE;
    const QPoint c = hwnd->w()->mapFromGlobal(QPoint(pt->x, pt->y));
    *pt = {c.x(), c.y()};
    return TRUE;
}

BOOL AdjustWindowRectExForDpi(RECT* rect, DWORD style, BOOL, DWORD, UINT dpi) {
    if (!rect) return FALSE;
    const Frame f = frameOf(style, dpi);
    rect->left -= f.left;
    rect->top -= f.top;
    rect->right += f.right;
    rect->bottom += f.bottom;
    return TRUE;
}

BOOL AdjustWindowRectEx(RECT* rect, DWORD style, BOOL menu, DWORD exStyle) {
    return AdjustWindowRectExForDpi(rect, style, menu, exStyle, currentDpi());
}

BOOL SetWindowTextW(HWND hwnd, LPCWSTR text) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    if (hwnd->kind != HWND__::Window) return controlMessage(hwnd, WM_SETTEXT, 0, (LPARAM)text) != 0;
    hwnd->text = text ? text : L"";
    hwnd->w()->setWindowTitle(fromW(hwnd->text));
    return TRUE;
}

int GetWindowTextW(HWND hwnd, LPWSTR out, int max) {
    if (!hwnd || !out || max <= 0) return 0;
    if (hwnd->kind != HWND__::Window) return (int)controlMessage(hwnd, WM_GETTEXT, (WPARAM)max, (LPARAM)out);
    const int n = (int)std::min<size_t>(hwnd->text.size(), (size_t)max - 1);
    std::wmemcpy(out, hwnd->text.data(), (size_t)n);
    out[n] = 0;
    return n;
}

int GetWindowTextLengthW(HWND hwnd) {
    if (!hwnd) return 0;
    if (hwnd->kind != HWND__::Window) return (int)controlMessage(hwnd, WM_GETTEXTLENGTH, 0, 0);
    return (int)hwnd->text.size();
}

LONG_PTR GetWindowLongPtrW(HWND hwnd, int index) {
    if (!hwnd) return 0;
    switch (index) {
        case GWL_STYLE: return (LONG_PTR)(LONG)hwnd->style;
        case GWL_EXSTYLE: return (LONG_PTR)(LONG)hwnd->exStyle;
        case GWLP_ID: return hwnd->id;
        case GWLP_HWNDPARENT: return (LONG_PTR)hwnd->parent;
        default: return 0;
    }
}

LONG_PTR SetWindowLongPtrW(HWND hwnd, int index, LONG_PTR value) {
    if (!hwnd) return 0;
    const LONG_PTR before = GetWindowLongPtrW(hwnd, index);
    switch (index) {
        case GWL_STYLE:
            hwnd->style = (DWORD)value;
            break;
        case GWL_EXSTYLE:
            hwnd->exStyle = (DWORD)value;
            if (!(hwnd->exStyle & WS_EX_LAYERED) && hwnd->alive()) hwnd->w()->setWindowOpacity(1.0);
            break;
        default:
            break;
    }
    return before;
}

BOOL GetWindowPlacement(HWND hwnd, WINDOWPLACEMENT* wp) {
    if (!hwnd || !hwnd->alive() || !wp) return FALSE;
    QWidget* w = hwnd->w();
    RECT r{};
    GetWindowRect(hwnd, &r);
    if (w->isMaximized()) {
        const QRect n = w->normalGeometry();
        const Frame f = frameOf(hwnd->style, currentDpi());
        r = {n.left() - f.left, n.top() - f.top, n.left() + n.width() + f.right, n.top() + n.height() + f.bottom};
    }
    const QRect work = screenOf(hwnd)->availableGeometry();
    OffsetRect(&r, -work.left(), -work.top());
    wp->length = sizeof(*wp);
    wp->flags = 0;
    wp->showCmd = w->isMaximized() ? SW_SHOWMAXIMIZED : w->isMinimized() ? SW_SHOWMINIMIZED
                : w->isVisible() ? SW_SHOWNORMAL : SW_HIDE;
    wp->rcNormalPosition = r;
    wp->ptMinPosition = {-1, -1};
    wp->ptMaxPosition = {-1, -1};
    return TRUE;
}

BOOL SetWindowPlacement(HWND hwnd, const WINDOWPLACEMENT* wp) {
    if (!hwnd || !hwnd->alive() || !wp) return FALSE;
    const QRect work = QGuiApplication::primaryScreen()->availableGeometry();
    RECT r = wp->rcNormalPosition;
    OffsetRect(&r, work.left(), work.top());
    placeWindow(hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, true, true);
    if (wp->showCmd == SW_SHOWMAXIMIZED) hwnd->w()->showMaximized();
    else if (wp->showCmd == SW_SHOWNORMAL || wp->showCmd == SW_SHOW) hwnd->w()->showNormal();
    return TRUE;
}

BOOL SetLayeredWindowAttributes(HWND hwnd, COLORREF, BYTE alpha, DWORD flags) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    if (flags & LWA_ALPHA) {
        hwnd->alpha = alpha;
        hwnd->w()->setWindowOpacity(alpha / 255.0);
    }
    return TRUE;
}

HWND SetFocus(HWND hwnd) {
    HWND before = GetFocus();
    if (!hwnd || !hwnd->alive()) return before;
    QWidget* w = hwnd->w();
    if (w->focusPolicy() == Qt::NoFocus && hwnd->kind == HWND__::Window) {
        w->setFocusPolicy(Qt::StrongFocus);
    }
    w->setFocus(Qt::OtherFocusReason);
    return before;
}

HWND GetFocus() {
    QWidget* f = QApplication::focusWidget();
    return f ? handleOf(f) : nullptr;
}

BOOL SetForegroundWindow(HWND hwnd) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    QWidget* w = hwnd->w()->window();
    w->raise();
    w->activateWindow();
    return TRUE;
}

HWND GetForegroundWindow() {
    QWidget* a = QApplication::activeWindow();
    return a ? handleOf(a) : nullptr;
}

HWND GetActiveWindow() { return GetForegroundWindow(); }

BOOL IsWindowVisible(HWND hwnd) { return hwnd && hwnd->alive() && hwnd->w()->isVisible(); }
BOOL IsWindow(HWND hwnd) { return hwnd && hwnd->alive(); }
BOOL IsIconic(HWND hwnd) { return hwnd && hwnd->alive() && hwnd->w()->isMinimized(); }
BOOL IsZoomed(HWND hwnd) { return hwnd && hwnd->alive() && hwnd->w()->isMaximized(); }
HWND GetParent(HWND hwnd) { return hwnd ? hwnd->parent : nullptr; }

int GetClassNameW(HWND hwnd, LPWSTR out, int max) {
    if (!hwnd || !out || max <= 0) return 0;
    std::wstring name = hwnd->className;
    if (hwnd->kind == HWND__::Button) name = L"Button";
    else if (hwnd->kind == HWND__::Edit) name = L"Edit";
    else if (hwnd->kind == HWND__::RichEdit) name = L"RICHEDIT50W";
    const int n = (int)std::min<size_t>(name.size(), (size_t)max - 1);
    std::wmemcpy(out, name.data(), (size_t)n);
    out[n] = 0;
    return n;
}

int GetDlgCtrlID(HWND hwnd) { return hwnd ? hwnd->id : 0; }

BOOL EnableWindow(HWND hwnd, BOOL enable) {
    if (!hwnd || !hwnd->alive()) return FALSE;
    const BOOL was = !hwnd->w()->isEnabled();
    hwnd->w()->setEnabled(enable != 0);
    return was;
}

HWND WindowFromPoint(POINT pt) {
    QWidget* w = QApplication::widgetAt(QPoint(pt.x, pt.y));
    while (w) {
        if (HWND h = handleOf(w)) return h;
        w = w->parentWidget();
    }
    return nullptr;
}

BOOL EnumWindows(WNDENUMPROC proc, LPARAM lParam) {
    if (!proc) return FALSE;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (HWND h = handleOf(w)) {
            if (!proc(h, lParam)) break;
        }
    }
    return TRUE;
}

DWORD GetWindowThreadProcessId(HWND, DWORD* pid) {
    if (pid) *pid = GetCurrentProcessId();
    return 1;
}

BOOL PrintWindow(HWND hwnd, HDC hdc, UINT) {
    if (!hwnd || !hwnd->alive() || !hdc) return FALSE;
    QPainter* p = painterFor(hdc);
    if (!p) return FALSE;
    if (hwnd->kind == HWND__::Window) {
        // The window paints as it would on screen, into the caller's DC
        hwnd->paintTarget = hdc;
        p->save();
        p->resetTransform();
        p->setClipRect(QRect(0, 0, hwnd->w()->width(), hwnd->w()->height()));
        if (!callProc(hwnd, WM_ERASEBKGND, (WPARAM)hdc, 0) && hwnd->background) {
            p->fillRect(hwnd->w()->rect(), asBrush(hwnd->background)->colour);
        }
        callProc(hwnd, WM_PAINT, 0, 0);
        p = painterFor(hdc);
        p->restore();
        hwnd->paintTarget = nullptr;
        // ...and its controls where they sit
        for (HWND child : childrenOf(hwnd)) {
            if (!child->alive() || !child->w()->isVisibleTo(hwnd->w())) continue;
            p = painterFor(hdc);
            p->save();
            p->resetTransform();
            p->translate(child->w()->pos());
            p->setClipRect(QRect(QPoint(0, 0), child->w()->size()));
            renderControl(child, p);
            p = painterFor(hdc);
            p->restore();
        }
    } else {
        p->save();
        p->resetTransform();
        renderControl(hwnd, p);
        painterFor(hdc)->restore();
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

LRESULT DefWindowProcW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_SETCURSOR:
            return 0;
        case WM_GETTEXT:
            return GetWindowTextW(hwnd, (LPWSTR)lParam, (int)wParam);
        case WM_SETTEXT:
            return SetWindowTextW(hwnd, (LPCWSTR)lParam);
        case WM_SETICON:
            if (hwnd && hwnd->alive() && lParam) {
                auto* icon = asIcon(reinterpret_cast<HICON>(lParam));
                if (wParam == ICON_BIG) hwnd->w()->setWindowIcon(icon->icon);
            }
            return 0;
        default:
            return 0;
    }
}

LRESULT DefWindowProcA(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SendMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!hwnd || hwnd->destroyed) return 0;
    if (hwnd->kind == HWND__::Window) {
        switch (msg) {
            case WM_SETICON:
            case WM_GETTEXT:
            case WM_SETTEXT:
                return DefWindowProcW(hwnd, msg, wParam, lParam);
            default:
                break;
        }
    }
    return callProc(hwnd, msg, wParam, lParam);
}

LRESULT SendMessageA(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return SendMessageW(hwnd, msg, wParam, lParam);
}

BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    postMessage(hwnd, msg, wParam, lParam);
    return TRUE;
}

BOOL PostMessageA(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return PostMessageW(hwnd, msg, wParam, lParam);
}

void PostQuitMessage(int code) {
    g_quitPosted = true;
    g_quitCode = code;
}

BOOL PeekMessageW(MSG* msg, HWND, UINT, UINT, UINT remove) {
    if (!msg) return FALSE;
    if (queue().empty()) {
        // Qt's events become messages here: paint and resize are answered
        // on the spot, the input goes onto the queue for the loop that
        // asked, so it can look at every key before the window does
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents();
    }
    if (queue().empty()) {
        if (g_quitPosted) {
            g_quitPosted = false;
            *msg = MSG{};
            msg->message = WM_QUIT;
            msg->wParam = (WPARAM)g_quitCode;
            return TRUE;
        }
        return FALSE;
    }
    *msg = queue().front();
    if (remove & PM_REMOVE) queue().pop_front();
    return TRUE;
}

BOOL GetMessageW(MSG* msg, HWND hwnd, UINT first, UINT last) {
    while (!PeekMessageW(msg, hwnd, first, last, PM_REMOVE)) {
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    }
    return msg->message != WM_QUIT;
}

// The character a virtual key stands for when no keyboard event is behind
// the message - a key the application posted itself (typeCraftedForShot)
static wchar_t charOfVk(UINT vk) {
    if (vk >= '0' && vk <= '9') return (wchar_t)vk;
    if (vk >= 'A' && vk <= 'Z') return (wchar_t)vk;
    switch (vk) {
        case VK_BACK: return 8;
        case VK_TAB: return 9;
        case VK_RETURN: return 13;
        case VK_ESCAPE: return 27;
        case VK_SPACE: return L' ';
        case VK_OEM_PERIOD: return L'.';
        case VK_DECIMAL: return L'.';
        case VK_OEM_COMMA: return L',';
        case VK_OEM_MINUS: return L'-';
        case VK_SUBTRACT: return L'-';
        case VK_OEM_PLUS: return L'=';
        case VK_ADD: return L'+';
        default: return 0;
    }
}

BOOL TranslateMessage(const MSG* msg) {
    if (!msg || !msg->hwnd) return FALSE;
    if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) return FALSE;
    if (msg->hwnd->kind != HWND__::Window) return FALSE;   // a control reads its own event
    std::u32string chars;
    if (QEvent* e = takeEvent(msg->qt_)) {
        if (e->type() == QEvent::KeyPress) {
            const QString text = static_cast<QKeyEvent*>(e)->text();
            chars = text.toStdU32String();
        }
    } else if (const wchar_t c = charOfVk((UINT)msg->wParam)) {
        chars.push_back((char32_t)c);
    }
    for (char32_t c : chars) {
        if (c == 0) continue;
        postMessage(msg->hwnd, msg->message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, (WPARAM)c, msg->lParam);
    }
    return chars.empty() ? FALSE : TRUE;
}

LRESULT DispatchMessageW(const MSG* msg) {
    if (!msg || !msg->hwnd || msg->hwnd->destroyed) return 0;
    HWND h = msg->hwnd;
    if (h->kind != HWND__::Window) {
        if (QEvent* e = takeEvent(msg->qt_)) {
            deliverControlEvent(h, e);
            return 0;
        }
        return controlMessage(h, msg->message, msg->wParam, msg->lParam);
    }
    return callProc(h, msg->message, msg->wParam, msg->lParam);
}

BOOL IsDialogMessageW(HWND, MSG*) {
    // Tab order and mnemonics are Windows' dialog manager; Bedrock's windows
    // have no tab order worth the name, and the keys it would eat (Tab on a
    // plain window) reach the procedure as keys instead
    return FALSE;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

HDC BeginPaint(HWND hwnd, PAINTSTRUCT* ps) {
    if (!hwnd || !ps) return nullptr;
    HDC dc = hwnd->paintTarget;
    if (!dc) {
        // Asked outside a paint: a surface of its own, so nothing crashes
        // and the drawing goes nowhere - which is what Windows does too
        dc = &hwnd->paintDC;
        dc->device = nullptr;
        dc->painter = nullptr;
    }
    ps->hdc = dc;
    ps->fErase = FALSE;
    GetClientRect(hwnd, &ps->rcPaint);
    return dc;
}

BOOL EndPaint(HWND, const PAINTSTRUCT*) { return TRUE; }

HDC GetDC(HWND hwnd) {
    HDC dc = new HDC__();
    (void)hwnd;   // measuring only; drawing on a window outside its paint is not done here
    return dc;
}

int ReleaseDC(HWND, HDC hdc) {
    delete hdc;
    return 1;
}

HDC CreateCompatibleDC(HDC) {
    HDC dc = new HDC__();
    dc->memory = true;
    return dc;
}

BOOL DeleteDC(HDC hdc) {
    delete hdc;
    return TRUE;
}

HBITMAP CreateCompatibleBitmap(HDC, int w, int h) {
    return new ShimBitmap(std::max(1, w), std::max(1, h));
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj) {
    if (!hdc || !obj) return nullptr;
    switch (obj->kind) {
        case HGDIOBJ__::Bitmap: {
            HGDIOBJ before = hdc->bitmap;
            hdc->endOwnPainter();
            hdc->bitmap = static_cast<HBITMAP>(obj);
            return before ? before : GetStockObject(-1);
        }
        case HGDIOBJ__::Brush:
        case HGDIOBJ__::Stock: {
            HGDIOBJ before = hdc->brush;
            hdc->brush = obj->kind == HGDIOBJ__::Brush ? static_cast<HBRUSH>(obj) : nullptr;
            return before ? before : GetStockObject(NULL_BRUSH);
        }
        case HGDIOBJ__::Font: {
            HGDIOBJ before = hdc->font;
            hdc->font = static_cast<HFONT>(obj);
            return before;
        }
        case HGDIOBJ__::Pen: {
            HGDIOBJ before = hdc->pen;
            hdc->pen = static_cast<HPEN>(obj);
            return before;
        }
        default:
            return nullptr;
    }
}

BOOL DeleteObject(HGDIOBJ obj) {
    if (!obj || obj->kind == HGDIOBJ__::Stock) return FALSE;
    delete obj;
    return TRUE;
}

HGDIOBJ GetStockObject(int which) {
    static ShimBrush nullBrush(Qt::transparent);
    static ShimBrush blackBrush(Qt::black);
    static ShimBrush whiteBrush(Qt::white);
    static HGDIOBJ__ nothing(HGDIOBJ__::Stock);
    nullBrush.hollow = true;
    switch (which) {
        case NULL_BRUSH: return &nullBrush;
        case BLACK_BRUSH: return &blackBrush;
        case WHITE_BRUSH: return &whiteBrush;
        default: return &nothing;
    }
}

BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD) {
    if (!dst || !src) return FALSE;
    QImage* image = imageFor(src);
    if (!image) return FALSE;
    src->endOwnPainter();   // so the picture is whole before it is read
    QPainter* p = painterFor(dst);
    if (!p) return FALSE;
    p->save();
    p->setCompositionMode(QPainter::CompositionMode_Source);
    p->drawImage(QPoint(x, y), *image, QRect(sx, sy, w, h));
    p->restore();
    return TRUE;
}

int GetDIBits(HDC hdc, HBITMAP bmp, UINT start, UINT lines, LPVOID bits, BITMAPINFO* info, UINT) {
    if (!bmp || !info) return 0;
    if (hdc) hdc->endOwnPainter();
    const QImage image = asBitmap(bmp)->image.convertToFormat(QImage::Format_ARGB32);
    if (!bits) {
        info->bmiHeader.biWidth = image.width();
        info->bmiHeader.biHeight = -image.height();
        info->bmiHeader.biBitCount = 32;
        info->bmiHeader.biPlanes = 1;
        return image.height();
    }
    const int w = info->bmiHeader.biWidth > 0 ? info->bmiHeader.biWidth : image.width();
    const int h = std::abs(info->bmiHeader.biHeight);
    const bool topDown = info->bmiHeader.biHeight < 0;
    auto* out = static_cast<unsigned char*>(bits);
    int copied = 0;
    for (UINT i = 0; i < lines; i++) {
        const int srcRow = (int)(start + i);
        if (srcRow >= image.height() || srcRow >= h) break;
        const int dstRow = topDown ? srcRow : (h - 1 - srcRow);
        const auto* row = image.constScanLine(srcRow);
        std::memcpy(out + (size_t)dstRow * w * 4, row, (size_t)std::min(w, image.width()) * 4);
        copied++;
    }
    return copied;
}

int GetObjectW(HGDIOBJ obj, int size, LPVOID out) {
    (void)obj; (void)size; (void)out;
    return 0;
}

HBRUSH CreateSolidBrush(COLORREF colour) { return new ShimBrush(fromColorref(colour)); }
HPEN CreatePen(int, int width, COLORREF colour) { return new ShimPen(fromColorref(colour), width); }

HFONT CreateFontW(int height, int, int, int, int weight, DWORD italic, DWORD underline, DWORD,
                  DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR face) {
    QFont f(fromW(face));
    if (height < 0) f.setPixelSize(-height);
    else if (height > 0) f.setPixelSize(height);
    f.setWeight(weight >= FW_BOLD ? QFont::Bold : weight >= FW_SEMIBOLD ? QFont::DemiBold : QFont::Normal);
    f.setItalic(italic != 0);
    f.setUnderline(underline != 0);
    return new ShimFont(f);
}

HFONT CreateFontA(int height, int width, int escapement, int orientation, int weight, DWORD italic,
                  DWORD underline, DWORD strikeOut, DWORD charSet, DWORD outPrecision,
                  DWORD clipPrecision, DWORD quality, DWORD pitchAndFamily, LPCSTR face) {
    const std::wstring wide = toW(QString::fromLatin1(face ? face : ""));
    return CreateFontW(height, width, escapement, orientation, weight, italic, underline, strikeOut,
                       charSet, outPrecision, clipPrecision, quality, pitchAndFamily, wide.c_str());
}

COLORREF SetTextColor(HDC hdc, COLORREF colour) {
    if (!hdc) return 0;
    const COLORREF before = hdc->textColour;
    hdc->textColour = colour;
    return before;
}

COLORREF SetBkColor(HDC hdc, COLORREF colour) {
    if (!hdc) return 0;
    const COLORREF before = hdc->bkColour;
    hdc->bkColour = colour;
    return before;
}

int SetBkMode(HDC hdc, int mode) {
    if (!hdc) return 0;
    const int before = hdc->bkMode;
    hdc->bkMode = mode;
    return before;
}

int FillRect(HDC hdc, const RECT* rect, HBRUSH brush) {
    if (!hdc || !rect || !brush) return 0;
    QPainter* p = painterFor(hdc);
    if (!p) return 0;
    p->fillRect(toQRect(*rect), asBrush(brush)->colour);
    return 1;
}

BOOL TextOutA(HDC hdc, int x, int y, LPCSTR text, int length) {
    if (!hdc || !text) return FALSE;
    QPainter* p = painterFor(hdc);
    if (!p) return FALSE;
    p->save();
    if (hdc->font) p->setFont(asFont(hdc->font)->font);
    p->setPen(fromColorref(hdc->textColour));
    const QString s = QString::fromUtf8(text, length);
    p->drawText(QPoint(x, y + p->fontMetrics().ascent()), s);
    p->restore();
    return TRUE;
}

BOOL Rectangle(HDC hdc, int left, int top, int right, int bottom) {
    if (!hdc) return FALSE;
    QPainter* p = painterFor(hdc);
    if (!p) return FALSE;
    p->save();
    if (hdc->pen) p->setPen(QPen(asPen(hdc->pen)->colour, asPen(hdc->pen)->width));
    if (hdc->brush && !asBrush(hdc->brush)->hollow) p->setBrush(asBrush(hdc->brush)->colour);
    else p->setBrush(Qt::NoBrush);
    p->drawRect(QRect(left, top, right - left - 1, bottom - top - 1));
    p->restore();
    return TRUE;
}

// ---------------------------------------------------------------------------
// Rectangles and points
// ---------------------------------------------------------------------------

BOOL PtInRect(const RECT* r, POINT pt) {
    return r && pt.x >= r->left && pt.x < r->right && pt.y >= r->top && pt.y < r->bottom;
}

BOOL OffsetRect(RECT* r, int dx, int dy) {
    if (!r) return FALSE;
    r->left += dx; r->right += dx; r->top += dy; r->bottom += dy;
    return TRUE;
}

BOOL SetRect(RECT* r, int left, int top, int right, int bottom) {
    if (!r) return FALSE;
    *r = {left, top, right, bottom};
    return TRUE;
}

BOOL IsRectEmpty(const RECT* r) { return !r || r->right <= r->left || r->bottom <= r->top; }

BOOL IntersectRect(RECT* out, const RECT* a, const RECT* b) {
    if (!out || !a || !b) return FALSE;
    RECT r = {std::max(a->left, b->left), std::max(a->top, b->top), std::min(a->right, b->right),
              std::min(a->bottom, b->bottom)};
    if (r.right <= r.left || r.bottom <= r.top) { *out = {0, 0, 0, 0}; return FALSE; }
    *out = r;
    return TRUE;
}

BOOL EqualRect(const RECT* a, const RECT* b) {
    return a && b && a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

// ---------------------------------------------------------------------------
// Mouse, keyboard, timers, hotkeys
// ---------------------------------------------------------------------------

HWND SetCapture(HWND hwnd) {
    HWND before = g_capture;
    if (hwnd && hwnd->alive()) {
        g_capture = hwnd;
        hwnd->w()->grabMouse();
    }
    return before;
}

BOOL ReleaseCapture() {
    HWND had = g_capture;
    g_capture = nullptr;
    if (had && had->alive()) {
        had->w()->releaseMouse();
        callProc(had, WM_CAPTURECHANGED, 0, 0);
    }
    return TRUE;
}

HWND GetCapture() { return g_capture; }

BOOL TrackMouseEvent(TRACKMOUSEEVENT* tme) {
    if (!tme || !tme->hwndTrack) return FALSE;
    tme->hwndTrack->trackLeave = (tme->dwFlags & TME_LEAVE) != 0;
    return TRUE;
}

BOOL GetCursorPos(POINT* pt) {
    if (!pt) return FALSE;
    const QPoint c = QCursor::pos();
    *pt = {c.x(), c.y()};
    return TRUE;
}

HCURSOR SetCursor(HCURSOR cursor) {
    static HCURSOR current = nullptr;
    HCURSOR before = current;
    current = cursor;
    if (g_cursorTarget && g_cursorTarget->alive()) {
        if (cursor) g_cursorTarget->w()->setCursor(QCursor(asIcon(cursor)->shape));
        else g_cursorTarget->w()->unsetCursor();
    }
    return before;
}

static HCURSOR stockCursor(uintptr_t id) {
    static std::map<uintptr_t, HCURSOR> made;
    auto it = made.find(id);
    if (it != made.end()) return it->second;
    Qt::CursorShape shape = Qt::ArrowCursor;
    switch (id) {
        case 32513: shape = Qt::IBeamCursor; break;
        case 32514: shape = Qt::WaitCursor; break;
        case 32515: shape = Qt::CrossCursor; break;
        case 32642: shape = Qt::SizeFDiagCursor; break;
        case 32646: shape = Qt::SizeAllCursor; break;
        case 32648: shape = Qt::ForbiddenCursor; break;
        case 32649: shape = Qt::PointingHandCursor; break;
        default: break;
    }
    HCURSOR c = new ShimIcon(shape);
    made[id] = c;
    return c;
}

HCURSOR LoadCursorW(HINSTANCE, LPCWSTR name) { return stockCursor((uintptr_t)name); }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR name) { return stockCursor((uintptr_t)name); }

static HICON appIcon() {
    static HICON icon = nullptr;
    if (!icon) {
        QIcon i;
        if (!g_appIcon.isNull()) i = QIcon(QPixmap::fromImage(g_appIcon));
        icon = new ShimIcon(i);
    }
    return icon;
}

HICON LoadIconW(HINSTANCE, LPCWSTR) { return appIcon(); }
HICON LoadIconA(HINSTANCE, LPCSTR) { return appIcon(); }
HANDLE LoadImageW(HINSTANCE, LPCWSTR, UINT type, int, int, UINT) {
    if (type == IMAGE_ICON) return appIcon();
    return nullptr;
}

SHORT GetKeyState(int vk) {
    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
    const Qt::MouseButtons buttons = QGuiApplication::mouseButtons();
    bool down = false;
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: down = mods & Qt::ShiftModifier; break;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: down = mods & Qt::ControlModifier; break;
        case VK_MENU: case VK_LMENU: case VK_RMENU: down = mods & Qt::AltModifier; break;
        case VK_LBUTTON: down = buttons & Qt::LeftButton; break;
        case VK_RBUTTON: down = buttons & Qt::RightButton; break;
        case VK_MBUTTON: down = buttons & Qt::MiddleButton; break;
        default: break;
    }
    return down ? (SHORT)0x8000 : 0;
}

SHORT GetAsyncKeyState(int vk) { return GetKeyState(vk); }
UINT MapVirtualKeyW(UINT code, UINT) { return code; }
UINT GetCaretBlinkTime() { return (UINT)std::max(1, QApplication::cursorFlashTime() / 2); }

UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT ms, void*) {
    if (!hwnd || !hwnd->alive()) return 0;
    KillTimer(hwnd, id);
    auto* t = new QTimer(shimOwner());
    t->setInterval((int)std::max<UINT>(1, ms));
    QObject::connect(t, &QTimer::timeout, [hwnd, id] {
        if (hwnd->alive()) callProc(hwnd, WM_TIMER, (WPARAM)id, 0);
    });
    t->start();
    hwnd->timers[id] = t;
    return id;
}

BOOL KillTimer(HWND hwnd, UINT_PTR id) {
    if (!hwnd) return FALSE;
    auto it = hwnd->timers.find(id);
    if (it == hwnd->timers.end()) return FALSE;
    delete it->second;
    hwnd->timers.erase(it);
    return TRUE;
}

BOOL RegisterHotKey(HWND hwnd, int id, UINT modifiers, UINT vk) {
    // System-wide where X11 allows it (X11Hotkeys.cpp), and as a shortcut
    // inside the app's own windows everywhere - so a key pressed over
    // Bedrock itself works on Wayland too, where only the portal reaches
    // further (docs/linux-port.md).
    const bool grabbed = grabGlobalHotkey(id, modifiers, vk);
    QWidget* owner = hwnd && hwnd->alive() ? hwnd->w() : QApplication::activeWindow();
    if (!owner) {
        const auto tops = QApplication::topLevelWidgets();
        if (tops.isEmpty()) return FALSE;
        owner = tops.first();
    }
    Qt::KeyboardModifiers mods;
    if (modifiers & MOD_CONTROL) mods |= Qt::ControlModifier;
    if (modifiers & MOD_ALT) mods |= Qt::AltModifier;
    if (modifiers & MOD_SHIFT) mods |= Qt::ShiftModifier;
    int key = 0;
    if (vk >= 'A' && vk <= 'Z') key = Qt::Key_A + (vk - 'A');
    else if (vk >= '0' && vk <= '9') key = Qt::Key_0 + (vk - '0');
    else if (vk >= VK_F1 && vk <= VK_F12) key = Qt::Key_F1 + (vk - VK_F1);
    else return FALSE;
    auto* s = new QShortcut(QKeySequence(QKeyCombination(mods, (Qt::Key)key)), owner);
    s->setContext(Qt::ApplicationShortcut);
    QObject::connect(s, &QShortcut::activated, [id, modifiers, vk] {
        postMessage(nullptr, WM_HOTKEY, (WPARAM)id, MAKELPARAM(modifiers & 0xFF, vk));
    });
    HWND keeper = hwnd ? hwnd : nullptr;
    if (keeper) {
        auto it = keeper->hotkeyById.find(id);
        if (it != keeper->hotkeyById.end()) {
            delete it->second;
            keeper->hotkeyById.erase(it);
        }
        keeper->hotkeyById[id] = s;
    }
    (void)grabbed;
    return TRUE;
}

BOOL UnregisterHotKey(HWND hwnd, int id) {
    ungrabGlobalHotkey(id);
    if (!hwnd) return FALSE;
    auto it = hwnd->hotkeyById.find(id);
    if (it == hwnd->hotkeyById.end()) return FALSE;
    delete it->second;
    hwnd->hotkeyById.erase(it);
    return TRUE;
}

DWORD GetTickCount() { return (DWORD)clockSince().elapsed(); }
uint64_t GetTickCount64() { return (uint64_t)clockSince().elapsed(); }
void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
BOOL MessageBeep(UINT) { QApplication::beep(); return TRUE; }
BOOL Beep(DWORD, DWORD) { QApplication::beep(); return TRUE; }

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

static QRect virtualScreen() {
    QRect r;
    for (QScreen* s : QGuiApplication::screens()) r = r.united(s->geometry());
    return r;
}

int GetSystemMetrics(int index) {
    QScreen* primary = QGuiApplication::primaryScreen();
    const QRect g = primary ? primary->geometry() : QRect(0, 0, 1920, 1080);
    switch (index) {
        case SM_CXSCREEN: return g.width();
        case SM_CYSCREEN: return g.height();
        case SM_CXICON: case SM_CYICON: return 32;
        case SM_CXSMICON: case SM_CYSMICON: return 16;
        case SM_CXCURSOR: case SM_CYCURSOR: return 32;
        case SM_CXVSCROLL: case SM_CYHSCROLL: return 17;
        case SM_CYCAPTION: return 23;
        case SM_CXFRAME: case SM_CYFRAME: return 8;
        case SM_XVIRTUALSCREEN: return virtualScreen().left();
        case SM_YVIRTUALSCREEN: return virtualScreen().top();
        case SM_CXVIRTUALSCREEN: return virtualScreen().width();
        case SM_CYVIRTUALSCREEN: return virtualScreen().height();
        case SM_CMONITORS: return (int)QGuiApplication::screens().size();
        default: return 0;
    }
}

BOOL SystemParametersInfoW(UINT action, UINT, LPVOID out, UINT) {
    if (action == SPI_GETWORKAREA && out) {
        QScreen* primary = QGuiApplication::primaryScreen();
        *static_cast<RECT*>(out) = toRECT(primary ? primary->availableGeometry() : QRect(0, 0, 1920, 1080));
        return TRUE;
    }
    return FALSE;
}

HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags) {
    if (hwnd && hwnd->alive()) {
        RECT r{};
        GetWindowRect(hwnd, &r);
        if (QScreen* s = nearestScreen(toQRect(r))) return s;
    }
    if (flags == MONITOR_DEFAULTTONULL) return nullptr;
    return QGuiApplication::primaryScreen();
}

HMONITOR MonitorFromPoint(POINT pt, DWORD flags) {
    if (QScreen* s = screenAt(QPoint(pt.x, pt.y))) return s;
    if (flags == MONITOR_DEFAULTTONULL) return nullptr;
    if (flags == MONITOR_DEFAULTTONEAREST) return nearestScreen(QRect(pt.x, pt.y, 1, 1));
    return QGuiApplication::primaryScreen();
}

BOOL GetMonitorInfoW(HMONITOR monitor, MONITORINFO* info) {
    if (!monitor || !info) return FALSE;
    auto* s = static_cast<QScreen*>(monitor);
    info->rcMonitor = toRECT(s->geometry());
    info->rcWork = toRECT(s->availableGeometry());
    info->dwFlags = s == QGuiApplication::primaryScreen() ? MONITORINFOF_PRIMARY : 0;
    return TRUE;
}

BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO* info) {
    if (!GetMonitorInfoW(monitor, info)) return FALSE;
    if (info->cbSize >= sizeof(MONITORINFOEXA)) {
        auto* ex = static_cast<MONITORINFOEXA*>(info);
        const QByteArray name = static_cast<QScreen*>(monitor)->name().toLatin1();
        std::strncpy(ex->szDevice, name.constData(), sizeof(ex->szDevice) - 1);
        ex->szDevice[sizeof(ex->szDevice) - 1] = 0;
    }
    return TRUE;
}

BOOL EnumDisplayMonitors(HDC, const RECT*, MONITORENUMPROC proc, LPARAM lParam) {
    if (!proc) return FALSE;
    for (QScreen* s : QGuiApplication::screens()) {
        RECT r = toRECT(s->geometry());
        if (!proc(s, nullptr, &r, lParam)) break;
    }
    return TRUE;
}

UINT GetDpiForSystem() {
    // 96 unless the environment says otherwise: Bedrock zooms itself by the
    // screen's height when the system reports 96 (MinerGUI::initialize),
    // and Qt's own scaling already sizes the pixels. BEDROCK_DPI overrides,
    // for a picture taken at another machine's setting.
    if (const char* env = std::getenv("BEDROCK_DPI")) {
        const int v = std::atoi(env);
        if (v >= 48 && v <= 480) return (UINT)v;
    }
    return 96;
}

UINT GetDpiForWindow(HWND) { return GetDpiForSystem(); }
BOOL SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT) { return TRUE; }

int MulDiv(int number, int numerator, int denominator) {
    if (denominator == 0) return -1;
    const int64_t product = (int64_t)number * numerator;
    const int64_t half = std::abs((int64_t)denominator) / 2;
    int64_t result;
    if ((product < 0) != (denominator < 0)) result = (product - half) / denominator;
    else result = (product + half) / denominator;
    if (result > INT32_MAX || result < INT32_MIN) return -1;
    return (int)result;
}

// ---------------------------------------------------------------------------
// Shell, dialogs, clipboard
// ---------------------------------------------------------------------------

HINSTANCE ShellExecuteW(HWND, LPCWSTR, LPCWSTR file, LPCWSTR, LPCWSTR, int) {
    if (!file) return (HINSTANCE)2;
    const QString target = fromW(file);
    const QUrl url = target.contains("://") ? QUrl(target) : QUrl::fromLocalFile(target);
    return QDesktopServices::openUrl(url) ? (HINSTANCE)42 : (HINSTANCE)2;
}

int MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR title, UINT type) {
    QWidget* parent = hwnd && hwnd->alive() ? hwnd->w() : nullptr;
    if (type & MB_ICONHAND) QMessageBox::critical(parent, fromW(title), fromW(text));
    else if (type & MB_ICONWARNING) QMessageBox::warning(parent, fromW(title), fromW(text));
    else QMessageBox::information(parent, fromW(title), fromW(text));
    return IDOK;
}

int MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR title, UINT type) {
    const std::wstring t = toW(QString::fromUtf8(text ? text : ""));
    const std::wstring c = toW(QString::fromUtf8(title ? title : ""));
    return MessageBoxW(hwnd, t.c_str(), c.c_str(), type);
}

// "Text files\0*.txt\0All files\0*.*\0\0" -> "Text files (*.txt);;All files (*.*)"
static QString qtFilter(LPCWSTR filter) {
    QStringList parts;
    if (!filter) return QString();
    const wchar_t* p = filter;
    while (*p) {
        const QString name = fromW(p);
        p += wcslen(p) + 1;
        if (!*p) break;
        const QString pattern = fromW(p).replace(';', ' ');
        p += wcslen(p) + 1;
        parts << QString("%1 (%2)").arg(name, pattern);
    }
    return parts.join(";;");
}

static BOOL fileDialog(OPENFILENAMEW* ofn, bool saving) {
    if (!ofn || !ofn->lpstrFile) return FALSE;
    QWidget* parent = ofn->hwndOwner && ofn->hwndOwner->alive() ? ofn->hwndOwner->w() : nullptr;
    QString start = fromW(ofn->lpstrFile);
    if (start.isEmpty() && ofn->lpstrInitialDir) start = fromW(ofn->lpstrInitialDir);
    else if (!start.contains('/') && ofn->lpstrInitialDir) start = fromW(ofn->lpstrInitialDir) + "/" + start;
    const QString title = fromW(ofn->lpstrTitle);
    const QString filter = qtFilter(ofn->lpstrFilter);
    QString chosen = saving ? QFileDialog::getSaveFileName(parent, title, start, filter)
                            : QFileDialog::getOpenFileName(parent, title, start, filter);
    if (chosen.isEmpty()) return FALSE;
    // A dot in the name typed, not anywhere in the path: under /home/j.doe
    // every name would otherwise stay bare
    const QString typed = chosen.mid(chosen.lastIndexOf('/') + 1);
    if (saving && ofn->lpstrDefExt && !typed.contains('.')) chosen += "." + fromW(ofn->lpstrDefExt);
    const std::wstring wide = toW(chosen);
    const size_t n = std::min(wide.size(), (size_t)ofn->nMaxFile - 1);
    std::wmemcpy(ofn->lpstrFile, wide.data(), n);
    ofn->lpstrFile[n] = 0;
    return TRUE;
}

BOOL GetOpenFileNameW(OPENFILENAMEW* ofn) { return fileDialog(ofn, false); }
BOOL GetSaveFileNameW(OPENFILENAMEW* ofn) { return fileDialog(ofn, true); }

HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD, HANDLE, PWSTR* out) {
    if (!out) return E_FAIL;
    QStandardPaths::StandardLocation where = QStandardPaths::DocumentsLocation;
    if (std::memcmp(&id, &FOLDERID_Pictures, sizeof(GUID)) == 0) where = QStandardPaths::PicturesLocation;
    const QString path = QStandardPaths::writableLocation(where);
    if (path.isEmpty()) return E_FAIL;
    const std::wstring wide = toW(path);
    auto* buffer = static_cast<wchar_t*>(CoTaskMemAlloc((wide.size() + 1) * sizeof(wchar_t)));
    std::wmemcpy(buffer, wide.data(), wide.size());
    buffer[wide.size()] = 0;
    *out = buffer;
    return S_OK;
}

void* CoTaskMemAlloc(size_t bytes) { return std::malloc(bytes); }
void CoTaskMemFree(void* p) { std::free(p); }

namespace {
// What GetClipboardData hands out lives until CloseClipboard
struct ClipboardHold {
    std::vector<std::wstring> files;
    HGLOBAL dib = nullptr;
    HGLOBAL text = nullptr;
};
ClipboardHold& clipboardHold() {
    static ClipboardHold h;
    return h;
}
struct GlobalBlock { std::vector<unsigned char> bytes; };
}  // namespace

BOOL OpenClipboard(HWND) { return TRUE; }

BOOL CloseClipboard() {
    auto& hold = clipboardHold();
    hold.files.clear();
    if (hold.dib) { GlobalFree(hold.dib); hold.dib = nullptr; }
    if (hold.text) { GlobalFree(hold.text); hold.text = nullptr; }
    return TRUE;
}

BOOL EmptyClipboard() {
    QGuiApplication::clipboard()->clear();
    return TRUE;
}

BOOL IsClipboardFormatAvailable(UINT format) {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime) return FALSE;
    switch (format) {
        case CF_UNICODETEXT: case CF_TEXT: return mime->hasText() && !mime->hasUrls();
        case CF_DIB: case CF_BITMAP: return mime->hasImage();
        case CF_HDROP: return mime->hasUrls();
        default: return FALSE;
    }
}

HANDLE GetClipboardData(UINT format) {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (!mime) return nullptr;
    auto& hold = clipboardHold();
    if (format == CF_HDROP) {
        if (!mime->hasUrls()) return nullptr;
        hold.files.clear();
        for (const QUrl& u : mime->urls()) {
            if (u.isLocalFile()) hold.files.push_back(toW(u.toLocalFile()));
        }
        return hold.files.empty() ? nullptr : (HANDLE)&hold.files;
    }
    if (format == CF_DIB) {
        if (!mime->hasImage()) return nullptr;
        const QImage image = qvariant_cast<QImage>(mime->imageData()).convertToFormat(QImage::Format_ARGB32);
        if (image.isNull()) return nullptr;
        if (hold.dib) GlobalFree(hold.dib);
        const size_t pixels = (size_t)image.width() * image.height() * 4;
        hold.dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels);
        auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(hold.dib));
        std::memset(header, 0, sizeof(*header));
        header->biSize = sizeof(BITMAPINFOHEADER);
        header->biWidth = image.width();
        header->biHeight = -image.height();   // top-down
        header->biPlanes = 1;
        header->biBitCount = 32;
        header->biCompression = BI_RGB;
        header->biSizeImage = (DWORD)pixels;
        auto* bits = reinterpret_cast<unsigned char*>(header) + sizeof(BITMAPINFOHEADER);
        for (int y = 0; y < image.height(); y++) {
            std::memcpy(bits + (size_t)y * image.width() * 4, image.constScanLine(y), (size_t)image.width() * 4);
        }
        GlobalUnlock(hold.dib);
        return hold.dib;
    }
    if (format == CF_UNICODETEXT) {
        if (!mime->hasText()) return nullptr;
        const std::wstring wide = toW(mime->text());
        if (hold.text) GlobalFree(hold.text);
        hold.text = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
        auto* out = static_cast<wchar_t*>(GlobalLock(hold.text));
        std::wmemcpy(out, wide.data(), wide.size());
        out[wide.size()] = 0;
        GlobalUnlock(hold.text);
        return hold.text;
    }
    return nullptr;
}

HANDLE SetClipboardData(UINT format, HANDLE data) {
    if (format != CF_UNICODETEXT || !data) return nullptr;
    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
    QGuiApplication::clipboard()->setText(fromW(text));
    GlobalUnlock(data);
    GlobalFree(data);   // the clipboard owns what was set, and this is how it ends here
    return data;
}

UINT DragQueryFileW(HDROP drop, UINT index, LPWSTR out, UINT max) {
    if (!drop) return 0;
    const auto* files = static_cast<const std::vector<std::wstring>*>(drop);
    if (index == 0xFFFFFFFF) return (UINT)files->size();
    if (index >= files->size()) return 0;
    const std::wstring& f = (*files)[index];
    if (!out || max == 0) return (UINT)f.size();
    const size_t n = std::min(f.size(), (size_t)max - 1);
    std::wmemcpy(out, f.data(), n);
    out[n] = 0;
    return (UINT)n;
}

HGLOBAL GlobalAlloc(UINT flags, size_t bytes) {
    auto* block = new GlobalBlock();
    block->bytes.resize(bytes, 0);
    (void)flags;
    return block;
}

LPVOID GlobalLock(HGLOBAL mem) {
    if (!mem) return nullptr;
    return static_cast<GlobalBlock*>(mem)->bytes.data();
}

BOOL GlobalUnlock(HGLOBAL) { return TRUE; }

HGLOBAL GlobalFree(HGLOBAL mem) {
    delete static_cast<GlobalBlock*>(mem);
    return nullptr;
}

size_t GlobalSize(HGLOBAL mem) { return mem ? static_cast<GlobalBlock*>(mem)->bytes.size() : 0; }

// ---------------------------------------------------------------------------
// Modules, errors, strings
// ---------------------------------------------------------------------------

HMODULE GetModuleHandleW(LPCWSTR) { return (HMODULE)1; }
HMODULE GetModuleHandleA(LPCSTR) { return (HMODULE)1; }
HMODULE LoadLibraryW(LPCWSTR) { return (HMODULE)1; }
BOOL FreeLibrary(HMODULE) { return TRUE; }
DWORD GetLastError() { return g_lastError; }
void SetLastError(DWORD error) { g_lastError = error; }
DWORD GetCurrentProcessId() { return (DWORD)QCoreApplication::applicationPid(); }
HRESULT DwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD) { return S_OK; }

int MultiByteToWideChar(UINT, DWORD flags, LPCSTR src, int srcLen, LPWSTR dst, int dstLen) {
    if (!src) return 0;
    const bool counted = srcLen < 0;
    const size_t n = counted ? std::strlen(src) + 1 : (size_t)srcLen;
    // MB_ERR_INVALID_CHARS: a byte that is not UTF-8 fails the call rather
    // than becoming U+FFFD. MinerGUI's toW() relies on that failure to fall
    // back to byte-wise widening, as Windows does.
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder(QByteArrayView(src, (qsizetype)(counted ? n - 1 : n)));
    if ((flags & MB_ERR_INVALID_CHARS) && decoder.hasError()) {
        g_lastError = 1113;   // ERROR_NO_UNICODE_TRANSLATION
        return 0;
    }
    const std::u32string wide = text.toStdU32String();
    const size_t out = wide.size() + (counted ? 1 : 0);
    if (dstLen == 0) return (int)out;
    if ((size_t)dstLen < out) { g_lastError = 122; return 0; }   // ERROR_INSUFFICIENT_BUFFER
    for (size_t i = 0; i < wide.size(); i++) dst[i] = (wchar_t)wide[i];
    if (counted) dst[wide.size()] = 0;
    return (int)out;
}

int WideCharToMultiByte(UINT, DWORD, LPCWSTR src, int srcLen, LPSTR dst, int dstLen, LPCSTR, LPBOOL) {
    if (!src) return 0;
    const bool counted = srcLen < 0;
    const size_t n = counted ? std::wcslen(src) : (size_t)srcLen;
    const QByteArray utf8 = QString::fromWCharArray(src, (int)n).toUtf8();
    const size_t out = (size_t)utf8.size() + (counted ? 1 : 0);
    if (dstLen == 0) return (int)out;
    if ((size_t)dstLen < out) { g_lastError = 122; return 0; }
    std::memcpy(dst, utf8.constData(), (size_t)utf8.size());
    if (counted) dst[utf8.size()] = 0;
    return (int)out;
}
