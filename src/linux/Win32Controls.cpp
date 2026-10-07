// The three native controls Bedrock uses, on Qt: the owner-drawn BUTTON, the
// EDIT that names a note, and the rich edit the note is written in.
//
// The button paints nothing itself - it asks its parent through WM_DRAWITEM,
// exactly as Windows does, so MinerGUI::drawOwnerButton draws it. The edit is
// a QLineEdit, the note a QTextEdit answering the EM_* messages MinerGUI
// sends it: about fifteen, and the hairiest part of the shim, because a
// character index in a rich edit counts a paragraph break as one character
// and MinerGUI's suggestion code depends on that being so.
#include "Win32ShimInternal.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QIntValidator>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QWheelEvent>

#include <algorithm>

using namespace bedrock_shim;

namespace {

// Format properties the shim keeps on a run of text: whether it is a link
// (EN_LINK fires on it) and whether it is hidden (drawn out of sight)
constexpr int LinkProperty = QTextFormat::UserProperty + 1;
constexpr int HiddenProperty = QTextFormat::UserProperty + 2;

QColor fromColorref(COLORREF c) { return QColor(GetRValue(c), GetGValue(c), GetBValue(c)); }

void registerWidget(HWND h, QWidget* w);

// ---- BUTTON ---------------------------------------------------------------

class ShimButton : public QWidget {
public:
    ShimButton(HWND h, QWidget* parent) : QWidget(parent), h_(h) {
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
    }
    bool pressed() const { return pressed_; }
    void drawInto(QPainter* p) {
        HDC__ dc;
        dc.device = p->device();
        dc.painter = p;
        DRAWITEMSTRUCT dis{};
        dis.CtlType = ODT_BUTTON;
        dis.CtlID = (UINT)h_->id;
        dis.itemAction = ODA_DRAWENTIRE;
        dis.itemState = pressed_ ? ODS_SELECTED : 0;
        dis.hwndItem = h_;
        dis.hDC = &dc;
        dis.rcItem = {0, 0, width(), height()};
        if (h_->parent && h_->parent->alive()) {
            callProc(h_->parent, WM_DRAWITEM, (WPARAM)h_->id, (LPARAM)&dis);
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        drawInto(&p);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        pressed_ = true;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const bool inside = rect().contains(e->position().toPoint());
        pressed_ = false;
        update();
        if (inside && h_->parent && h_->parent->alive()) {
            callProc(h_->parent, WM_COMMAND, MAKEWPARAM(h_->id, BN_CLICKED), (LPARAM)h_);
        }
    }

private:
    HWND h_;
    bool pressed_ = false;
};

// ---- EDIT -----------------------------------------------------------------

class ShimEdit : public QLineEdit {
public:
    ShimEdit(HWND h, QWidget* parent) : QLineEdit(parent), h_(h) {
        setFrame(false);
        connect(this, &QLineEdit::textChanged, this, [this] {
            if (h_->parent && h_->parent->alive()) {
                callProc(h_->parent, WM_COMMAND, MAKEWPARAM(h_->id, EN_CHANGE), (LPARAM)h_);
            }
        });
    }
    void deliver(QEvent* e) {
        if (e->type() == QEvent::KeyPress) QLineEdit::keyPressEvent(static_cast<QKeyEvent*>(e));
        else if (e->type() == QEvent::Wheel) QLineEdit::wheelEvent(static_cast<QWheelEvent*>(e));
    }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        const UINT vk = vkFromKey(e->key(), e->text(), e->modifiers() & Qt::KeypadModifier);
        LPARAM lp = 1 | ((LPARAM)(e->nativeScanCode() & 0xFF) << 16);
        if (e->isAutoRepeat()) lp |= (LPARAM)1 << 30;
        postMessage(h_, WM_KEYDOWN, vk, lp, keepEvent(e->clone()));
        e->accept();
    }
    void showEvent(QShowEvent* e) override {
        QLineEdit::showEvent(e);
        applyEditColours(h_);
    }

private:
    HWND h_;
};

// ---- RICHEDIT -------------------------------------------------------------

class ShimRichEdit : public QTextEdit {
public:
    ShimRichEdit(HWND h, QWidget* parent) : QTextEdit(parent), h_(h) {
        setFrameStyle(QFrame::NoFrame);
        setAcceptRichText(false);
        setLineWrapMode(QTextEdit::WidgetWidth);
        setUndoRedoEnabled(true);
        viewport()->setAutoFillBackground(true);
        connect(document(), &QTextDocument::contentsChanged, this, [this] {
            if ((eventMask_ & ENM_CHANGE) && h_->parent && h_->parent->alive()) {
                callProc(h_->parent, WM_COMMAND, MAKEWPARAM(h_->id, EN_CHANGE), (LPARAM)h_);
            }
        });
    }

    // The text in the control's own character space: a paragraph break is
    // one character, and it is a CR, as the rich edit control reports it
    std::wstring textCR() const {
        std::wstring out;
        for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
            if (b.blockNumber() > 0) out.push_back(L'\r');
            out += toW(b.text());
        }
        return out;
    }
    int length() const { return std::max(0, document()->characterCount() - 1); }

    void setTextFromWindows(const std::wstring& text) {
        // CR+LF, lone CR and lone LF are all one paragraph break
        QString s = fromW(text);
        s.replace("\r\n", "\n");
        s.replace('\r', '\n');
        setPlainText(s);
    }
    std::wstring textCRLF() const {
        QString s = toPlainText();
        s.replace('\n', "\r\n");
        return toW(s);
    }

    void select(LONG from, LONG to) {
        const int n = length();
        int a = from < 0 ? n : std::min((int)from, n);
        int b = to < 0 ? n : std::min((int)to, n);
        QTextCursor c = textCursor();
        c.setPosition(a);
        c.setPosition(b, QTextCursor::KeepAnchor);
        setTextCursor(c);
    }
    void selection(LONG* from, LONG* to) const {
        const QTextCursor c = textCursor();
        const int a = std::min(c.anchor(), c.position());
        const int b = std::max(c.anchor(), c.position());
        if (from) *from = a;
        if (to) *to = b;
    }

    void setCharFormat(WPARAM scope, const CHARFORMAT2W* cf) {
        if (!cf) return;
        QTextCharFormat format;
        if (cf->dwMask & CFM_FACE) format.setFontFamilies({fromW(cf->szFaceName)});
        if (cf->dwMask & CFM_SIZE) format.setFontPointSize(cf->yHeight / 20.0);
        if (cf->dwMask & CFM_COLOR) format.setForeground(fromColorref(cf->crTextColor));
        if (cf->dwMask & CFM_BOLD) format.setFontWeight((cf->dwEffects & CFE_BOLD) ? QFont::Bold : QFont::Normal);
        if (cf->dwMask & CFM_ITALIC) format.setFontItalic((cf->dwEffects & CFE_ITALIC) != 0);
        if (cf->dwMask & CFM_UNDERLINE) format.setFontUnderline((cf->dwEffects & CFE_UNDERLINE) != 0);
        if (cf->dwMask & CFM_LINK) format.setProperty(LinkProperty, (cf->dwEffects & CFE_LINK) != 0);
        if (cf->dwMask & CFM_HIDDEN) {
            // Qt has no hidden text. Out of sight is the next best thing:
            // the colour of the paper and no width to speak of.
            const bool hidden = (cf->dwEffects & CFE_HIDDEN) != 0;
            format.setProperty(HiddenProperty, hidden);
            if (hidden) {
                format.setForeground(viewport()->palette().color(QPalette::Base));
                format.setFontPointSize(0.5);
                format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
                format.setFontLetterSpacing(0);
            } else if (scope & SCF_SELECTION) {
                format.setFontPointSize(document()->defaultFont().pointSizeF());
            }
        }
        if (scope & SCF_ALL) {
            // The face and colour of ordinary text: the document's default,
            // and everything already in it
            QFont f = document()->defaultFont();
            if (cf->dwMask & CFM_FACE) f.setFamily(fromW(cf->szFaceName));
            if (cf->dwMask & CFM_SIZE) f.setPointSizeF(cf->yHeight / 20.0);
            if (cf->dwMask & CFM_BOLD) f.setBold((cf->dwEffects & CFE_BOLD) != 0);
            if (cf->dwMask & CFM_ITALIC) f.setItalic((cf->dwEffects & CFE_ITALIC) != 0);
            document()->setDefaultFont(f);
            setFont(f);
            if (cf->dwMask & CFM_COLOR) {
                QPalette p = palette();
                p.setColor(QPalette::Text, fromColorref(cf->crTextColor));
                setPalette(p);
                setTextColor(fromColorref(cf->crTextColor));
            }
            QTextCursor all(document());
            all.select(QTextCursor::Document);
            all.mergeCharFormat(format);
        } else {
            QTextCursor c = textCursor();
            c.mergeCharFormat(format);
            setCurrentCharFormat(c.charFormat());
        }
    }

    // The link run under a character position: [from, to) or false
    bool linkAt(int pos, int* from, int* to) const {
        const QTextBlock block = document()->findBlock(pos);
        if (!block.isValid()) return false;
        int start = -1, end = -1;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid()) continue;
            const bool link = f.charFormat().property(LinkProperty).toBool();
            if (!link) {
                if (start >= 0 && end > start && pos >= start && pos < end) break;
                start = end = -1;
                continue;
            }
            if (start < 0) start = f.position();
            end = f.position() + f.length();
        }
        if (start >= 0 && pos >= start && pos < end) {
            *from = start;
            *to = end;
            return true;
        }
        return false;
    }

    QPoint positionOfChar(int index) {
        QTextCursor c(document());
        c.setPosition(std::min(std::max(0, index), length()));
        const QRect r = cursorRect(c);
        return viewport()->mapToParent(r.topLeft());
    }

    void deliver(QEvent* e) {
        if (e->type() == QEvent::KeyPress) QTextEdit::keyPressEvent(static_cast<QKeyEvent*>(e));
        else if (e->type() == QEvent::Wheel) QTextEdit::wheelEvent(static_cast<QWheelEvent*>(e));
    }
    void setEventMask(DWORD mask) { eventMask_ = mask; }
    DWORD eventMask() const { return eventMask_; }

protected:
    void keyPressEvent(QKeyEvent* e) override {
        const UINT vk = vkFromKey(e->key(), e->text(), e->modifiers() & Qt::KeypadModifier);
        LPARAM lp = 1 | ((LPARAM)(e->nativeScanCode() & 0xFF) << 16);
        if (e->isAutoRepeat()) lp |= (LPARAM)1 << 30;
        postMessage(h_, WM_KEYDOWN, vk, lp, keepEvent(e->clone()));
        e->accept();
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && (eventMask_ & ENM_LINK) && h_->parent && h_->parent->alive()) {
            const int pos = cursorForPosition(e->position().toPoint()).position();
            int from = 0, to = 0;
            if (linkAt(pos, &from, &to)) {
                ENLINK link{};
                link.nmhdr.hwndFrom = h_;
                link.nmhdr.idFrom = (UINT_PTR)h_->id;
                link.nmhdr.code = EN_LINK;
                link.msg = WM_LBUTTONDOWN;
                link.wParam = MK_LBUTTON;
                const QPoint at = e->position().toPoint();
                link.lParam = MAKELPARAM(at.x(), at.y());
                link.chrg = {from, to};
                if (callProc(h_->parent, WM_NOTIFY, (WPARAM)h_->id, (LPARAM)&link)) return;
            }
        }
        QTextEdit::mousePressEvent(e);
    }

private:
    HWND h_;
    DWORD eventMask_ = 0;
};

// ---- shared -----------------------------------------------------------------

HWND makeHandle(HWND__::Kind kind, HWND parent, DWORD style, int id, const std::wstring& text) {
    HWND h = new HWND__();
    h->kind = kind;
    h->parent = parent;
    h->style = style | WS_CHILD;
    h->id = id;
    h->text = text;
    return h;
}

void finish(HWND h, QWidget* w, const QRect& at) {
    h->widget = w;
    registerWidget(h, w);
    w->setGeometry(at);
    if (h->style & WS_VISIBLE) w->show();
}

std::unordered_map<QWidget*, HWND>& controlsByWidget() {
    static std::unordered_map<QWidget*, HWND> m;
    return m;
}

void registerWidget(HWND h, QWidget* w) {
    controlsByWidget()[w] = h;
    QObject::connect(w, &QObject::destroyed, [h, w] {
        h->destroyed = true;
        controlsByWidget().erase(w);
    });
}

ShimButton* button(HWND h) { return h->kind == HWND__::Button && h->alive() ? static_cast<ShimButton*>(h->w()) : nullptr; }
ShimEdit* edit(HWND h) { return h->kind == HWND__::Edit && h->alive() ? static_cast<ShimEdit*>(h->w()) : nullptr; }
ShimRichEdit* rich(HWND h) { return h->kind == HWND__::RichEdit && h->alive() ? static_cast<ShimRichEdit*>(h->w()) : nullptr; }

int copyOut(const std::wstring& s, LPWSTR out, int max) {
    if (!out || max <= 0) return 0;
    const int n = (int)std::min<size_t>(s.size(), (size_t)max - 1);
    std::wmemcpy(out, s.data(), (size_t)n);
    out[n] = 0;
    return n;
}

}  // namespace

// The controls' handles live in the same lookup the windows use: the shim
// asks handleOf() for any widget, so both tables answer
namespace bedrock_shim {

HWND controlHandleOf(QWidget* w) {
    auto it = controlsByWidget().find(w);
    return it == controlsByWidget().end() ? nullptr : it->second;
}

HWND makeButton(HWND parent, const std::wstring& text, DWORD style, int id, const QRect& at) {
    if (!parent || !parent->alive()) return nullptr;
    HWND h = makeHandle(HWND__::Button, parent, style, id, text);
    auto* w = new ShimButton(h, parent->w());
    finish(h, w, at);
    return h;
}

HWND makeEdit(HWND parent, const std::wstring& text, DWORD style, int id, const QRect& at) {
    if (!parent || !parent->alive()) return nullptr;
    HWND h = makeHandle(HWND__::Edit, parent, style, id, text);
    auto* w = new ShimEdit(h, parent->w());
    if (style & ES_CENTER) w->setAlignment(Qt::AlignCenter);
    else if (style & ES_RIGHT) w->setAlignment(Qt::AlignRight);
    if (style & ES_NUMBER) w->setValidator(new QIntValidator(w));
    if (style & ES_READONLY) w->setReadOnly(true);
    w->setText(fromW(text));
    finish(h, w, at);
    return h;
}

HWND makeRichEdit(HWND parent, DWORD style, int id, const QRect& at) {
    if (!parent || !parent->alive()) return nullptr;
    HWND h = makeHandle(HWND__::RichEdit, parent, style, id, L"");
    auto* w = new ShimRichEdit(h, parent->w());
    w->setVerticalScrollBarPolicy((style & WS_VSCROLL) ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    w->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    if (style & ES_READONLY) w->setReadOnly(true);
    finish(h, w, at);
    return h;
}

void applyEditColours(HWND control) {
    ShimEdit* e = edit(control);
    if (!e || !control->parent || !control->parent->alive()) return;
    HDC__ probe;
    probe.textColour = 0;
    probe.bkColour = 0xFFFFFF;
    const LRESULT answer = callProc(control->parent, WM_CTLCOLOREDIT, (WPARAM)&probe, (LPARAM)control);
    QPalette p = e->palette();
    p.setColor(QPalette::Text, fromColorref(probe.textColour));
    QColor base = fromColorref(probe.bkColour);
    if (answer) base = asBrush(reinterpret_cast<HBRUSH>(answer))->colour;
    p.setColor(QPalette::Base, base);
    e->setPalette(p);
}

LRESULT controlMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!hwnd || !hwnd->alive()) return 0;
    QWidget* w = hwnd->w();
    switch (msg) {
        case WM_SETFONT:
            hwnd->font = reinterpret_cast<HFONT>(wParam);
            if (hwnd->font) w->setFont(asFont(hwnd->font)->font);
            return 0;
        case WM_GETFONT:
            return (LRESULT)hwnd->font;
        case WM_SETREDRAW:
            w->setUpdatesEnabled(wParam != 0);
            if (wParam) w->update();
            return 0;
        case WM_SETTEXT: {
            const std::wstring text = lParam ? reinterpret_cast<LPCWSTR>(lParam) : L"";
            if (auto* b = button(hwnd)) { hwnd->text = text; b->update(); }
            else if (auto* e = edit(hwnd)) e->setText(fromW(text));
            else if (auto* r = rich(hwnd)) r->setTextFromWindows(text);
            return TRUE;
        }
        case WM_GETTEXT: {
            std::wstring text;
            if (button(hwnd)) text = hwnd->text;
            else if (auto* e = edit(hwnd)) text = toW(e->text());
            else if (auto* r = rich(hwnd)) text = r->textCRLF();
            return copyOut(text, reinterpret_cast<LPWSTR>(lParam), (int)wParam);
        }
        case WM_GETTEXTLENGTH: {
            if (button(hwnd)) return (LRESULT)hwnd->text.size();
            if (auto* e = edit(hwnd)) return e->text().size();
            if (auto* r = rich(hwnd)) return (LRESULT)r->textCRLF().size();
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
            // A key the application made up rather than one that was
            // pressed: typed into the control as text
            if (msg == WM_CHAR) {
                const QString s = QString::fromUcs4(reinterpret_cast<const char32_t*>(&wParam), 1);
                if (auto* e = edit(hwnd)) e->insert(s);
                else if (auto* r = rich(hwnd)) r->insertPlainText(s);
            }
            return 0;
        case EM_SETSEL:
            if (auto* e = edit(hwnd)) {
                if ((int)wParam == 0 && (int)lParam == -1) e->selectAll();
                else e->setSelection((int)wParam, (int)lParam - (int)wParam);
            } else if (auto* r = rich(hwnd)) {
                r->select((LONG)wParam, (LONG)lParam);
            }
            return 0;
        case EM_GETSEL: {
            LONG from = 0, to = 0;
            if (auto* e = edit(hwnd)) {
                from = e->hasSelectedText() ? e->selectionStart() : e->cursorPosition();
                to = e->hasSelectedText() ? e->selectionEnd() : e->cursorPosition();
            } else if (auto* r = rich(hwnd)) {
                r->selection(&from, &to);
            }
            if (wParam) *reinterpret_cast<DWORD*>(wParam) = (DWORD)from;
            if (lParam) *reinterpret_cast<DWORD*>(lParam) = (DWORD)to;
            return MAKELRESULT(from, to);
        }
        case EM_EXSETSEL:
            if (auto* r = rich(hwnd)) {
                const auto* range = reinterpret_cast<const CHARRANGE*>(lParam);
                if (range) r->select(range->cpMin, range->cpMax);
            }
            return 0;
        case EM_EXGETSEL:
            if (auto* r = rich(hwnd)) {
                auto* range = reinterpret_cast<CHARRANGE*>(lParam);
                if (range) r->selection(&range->cpMin, &range->cpMax);
            }
            return 0;
        case EM_SETMARGINS:
            if (auto* e = edit(hwnd)) {
                const int left = (wParam & EC_LEFTMARGIN) ? LOWORD(lParam) : e->textMargins().left();
                const int right = (wParam & EC_RIGHTMARGIN) ? HIWORD(lParam) : e->textMargins().right();
                e->setTextMargins(left, 0, right, 0);
            }
            return 0;
        case EM_REPLACESEL:
            if (auto* r = rich(hwnd)) {
                QString s = fromW(reinterpret_cast<LPCWSTR>(lParam));
                s.replace("\r\n", "\n");
                s.replace('\r', '\n');
                QTextCursor c = r->textCursor();
                c.insertText(s);
                r->setTextCursor(c);
            } else if (auto* e = edit(hwnd)) {
                e->insert(fromW(reinterpret_cast<LPCWSTR>(lParam)));
            }
            return 0;
        case EM_GETTEXTRANGE:
            if (auto* r = rich(hwnd)) {
                auto* range = reinterpret_cast<TEXTRANGEW*>(lParam);
                if (!range || !range->lpstrText) return 0;
                const std::wstring all = r->textCR();
                const size_t from = (size_t)std::max(0L, (long)range->chrg.cpMin);
                const size_t to = range->chrg.cpMax < 0 ? all.size() : std::min(all.size(), (size_t)range->chrg.cpMax);
                if (from >= to) { range->lpstrText[0] = 0; return 0; }
                std::wmemcpy(range->lpstrText, all.data() + from, to - from);
                range->lpstrText[to - from] = 0;
                return (LRESULT)(to - from);
            }
            return 0;
        case EM_GETTEXTLENGTHEX:
            if (auto* r = rich(hwnd)) {
                const auto* how = reinterpret_cast<const GETTEXTLENGTHEX*>(wParam);
                if (how && (how->flags & GTL_USECRLF)) return (LRESULT)r->textCRLF().size();
                return r->length();
            }
            return 0;
        case EM_GETTEXTEX:
            if (auto* r = rich(hwnd)) {
                const auto* how = reinterpret_cast<const GETTEXTEX*>(wParam);
                auto* out = reinterpret_cast<LPWSTR>(lParam);
                if (!how || !out) return 0;
                const std::wstring text = (how->flags & GT_USECRLF) ? r->textCRLF() : r->textCR();
                const int max = (int)(how->cb / sizeof(wchar_t));
                return copyOut(text, out, max);
            }
            return 0;
        case EM_POSFROMCHAR:
            if (auto* r = rich(hwnd)) {
                auto* out = reinterpret_cast<POINTL*>(wParam);
                const QPoint at = r->positionOfChar((int)lParam);
                if (out) *out = {at.x(), at.y()};
                return MAKELRESULT(at.x(), at.y());
            }
            return 0;
        case EM_SETCHARFORMAT:
            if (auto* r = rich(hwnd)) r->setCharFormat(wParam, reinterpret_cast<const CHARFORMAT2W*>(lParam));
            return TRUE;
        case EM_SETBKGNDCOLOR:
            if (auto* r = rich(hwnd)) {
                QPalette p = r->palette();
                p.setColor(QPalette::Base, fromColorref((COLORREF)lParam));
                r->setPalette(p);
                r->viewport()->setPalette(p);
            }
            return 0;
        case EM_SETEVENTMASK:
            if (auto* r = rich(hwnd)) {
                const DWORD before = r->eventMask();
                r->setEventMask((DWORD)lParam);
                return before;
            }
            return 0;
        case EM_GETEVENTMASK:
            if (auto* r = rich(hwnd)) return r->eventMask();
            return 0;
        case EM_EXLIMITTEXT:
        case EM_LIMITTEXT:
        case EM_AUTOURLDETECT:
        case EM_SETRECT:
            return 0;
        default:
            return 0;
    }
}

void deliverControlEvent(HWND hwnd, QEvent* event) {
    if (!hwnd || !hwnd->alive() || !event) return;
    if (auto* e = edit(hwnd)) e->deliver(event);
    else if (auto* r = rich(hwnd)) r->deliver(event);
}

void renderControl(HWND hwnd, QPainter* painter) {
    if (!hwnd || !hwnd->alive() || !painter) return;
    if (auto* b = button(hwnd)) {
        b->drawInto(painter);
        return;
    }
    hwnd->w()->render(painter, QPoint(0, 0), QRegion(), QWidget::DrawChildren);
}

}  // namespace bedrock_shim
