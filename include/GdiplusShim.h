#pragma once
// GDI+ spoken by QPainter.
//
// MinerGUI.cpp is 10,541 lines, and the temptation is to read that as ten
// thousand lines of Windows. It is not. Counting what it actually calls:
//
//     321 DrawString      50 FillRectangle     5 FillPath
//      61 MeasureString   12 FillEllipse       4 DrawRectangle
//                         12 DrawLine          2 DrawLines / DrawImage
//
// Ten drawing operations, plus Save/Restore/SetClip and two quality hints.
// Every one of them has a QPainter counterpart that takes the same arguments
// in the same order and means the same thing. And because the app draws every
// pixel of itself - there are eight BUTTON controls and one EDIT in the whole
// program, nothing else is a native widget - porting the window is not a
// redesign of the interface. It is these ten calls.
//
// So rather than rewrite the drawing code, this header answers to its names.
// The card code, the tables, the layout arithmetic, the hit rectangles: all of
// it compiles unchanged against this.
//
// What this is not: a general GDI+ implementation. It covers what Bedrock
// draws and nothing else, and it says so where it cuts a corner.

#include "Win32Shim.h"

#include <QBrush>
#include <QLinearGradient>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QStack>
#include <QString>
#include <QTransform>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

// The one pixel format MinerGUI names, for a thumbnail with alpha
#define PixelFormat32bppPARGB 0x000E200B

namespace Gdiplus {

using REAL = float;

namespace detail {
// A wide string as Qt reads it. wchar_t is 32 bits here and 16 on Windows;
// a byte widened the C++ way (toW's fallback for text that is not UTF-8)
// comes out as 0xFFFFFFD7 here and 0xFFD7 there. Cut to what Windows
// keeps, so the same glyph stands in the same place on both sides.
inline QString toQ(const wchar_t* text, int length) {
    if (!text) return QString();
    const size_t n = length < 0 ? std::wcslen(text) : (size_t)length;
    std::u32string s;
    s.reserve(n);
    for (size_t i = 0; i < n; i++) {
        auto c = (char32_t)text[i];
        if (c > 0x10FFFF) c &= 0xFFFF;
        s.push_back(c);
    }
    return QString::fromStdU32String(s);
}
// What GDI+ puts round a line of text with the default StringFormat: a
// sixth of the em on either side. It is in every width MeasureString
// reports and every DrawString that is not centred, and MinerGUI's layout
// was measured with it in - the "by Ravaxx" beside the name sits where
// the padded width of the name puts it. Measured against Windows
// renders: 5 px at 27 px, the Windows "B" 5 px further right than a
// bare one.
inline qreal pad(const QFont& f) {
    return (f.pixelSize() > 0 ? f.pixelSize() : f.pointSizeF() * 96.0 / 72.0) / 6.0;
}
}  // namespace detail
using ARGB = std::uint32_t;

enum Status { Ok = 0, GenericError = 1 };
enum Unit { UnitPixel = 2, UnitPoint = 3 };
// Where a picture's pixels sit when it is stretched, and what lies beyond
// its edge. Qt already samples at pixel centres and clamps at the edge, which
// is what PixelOffsetModeHalf and a flipped tile ask GDI+ for, so both are
// taken and have nothing left to do here.
enum PixelOffsetMode {
    PixelOffsetModeDefault = 0,
    PixelOffsetModeHighSpeed = 1,
    PixelOffsetModeHighQuality = 2,
    PixelOffsetModeNone = 3,
    PixelOffsetModeHalf = 4,
};
enum FlushIntention { FlushIntentionFlush = 0, FlushIntentionSync = 1 };
enum WrapMode {
    WrapModeTile = 0,
    WrapModeTileFlipX = 1,
    WrapModeTileFlipY = 2,
    WrapModeTileFlipXY = 3,
    WrapModeClamp = 4,
};
enum SmoothingMode { SmoothingModeDefault = 0, SmoothingModeNone = 3, SmoothingModeAntiAlias = 4 };
enum TextRenderingHint {
    TextRenderingHintSystemDefault = 0,
    TextRenderingHintAntiAlias = 4,
    TextRenderingHintClearTypeGridFit = 5,
};
enum InterpolationMode {
    InterpolationModeDefault = 0,
    InterpolationModeBilinear = 3,
    InterpolationModeNearestNeighbor = 5,
    InterpolationModeHighQualityBilinear = 6,
    InterpolationModeHighQualityBicubic = 7,
};
enum FontStyle {
    FontStyleRegular = 0,
    FontStyleBold = 1,
    FontStyleItalic = 2,
    FontStyleBoldItalic = 3,
    FontStyleUnderline = 4,
};
enum StringAlignment { StringAlignmentNear = 0, StringAlignmentCenter = 1, StringAlignmentFar = 2 };
enum StringTrimming {
    StringTrimmingNone = 0,
    StringTrimmingCharacter = 1,
    StringTrimmingWord = 2,
    StringTrimmingEllipsisCharacter = 3,
    StringTrimmingEllipsisWord = 4,
};
enum StringFormatFlags {
    StringFormatFlagsMeasureTrailingSpaces = 0x00000800,
    StringFormatFlagsNoWrap = 0x00001000,
};
enum LinearGradientMode {
    LinearGradientModeHorizontal = 0,
    LinearGradientModeVertical = 1,
    LinearGradientModeForwardDiagonal = 2,
};
enum LineCap { LineCapFlat = 0, LineCapRound = 2 };
enum LineJoin { LineJoinMiter = 0, LineJoinRound = 2 };
enum FillMode { FillModeAlternate = 0, FillModeWinding = 1 };

using GraphicsState = int;
using PixelFormat = int;

// A bitmap's pixels, opened for reading and writing in place
enum ImageLockMode { ImageLockModeRead = 1, ImageLockModeWrite = 2, ImageLockModeUserInputBuf = 4 };
struct BitmapData {
    UINT Width = 0;
    UINT Height = 0;
    INT Stride = 0;
    int PixelFormat = 0;   // GDI+ spells the type the same as the member
    void* Scan0 = nullptr;
    UINT_PTR Reserved = 0;
};
enum EncoderParameterValueType { EncoderParameterValueTypeLong = 4 };
struct EncoderParameters;

// --------------------------------------------------------------------------

class Color {
public:
    Color() = default;
    Color(int a, int r, int g, int b) : c_(r, g, b, a) {}
    Color(int r, int g, int b) : c_(r, g, b, 255) {}
    explicit Color(ARGB argb)
        : c_((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF) {}

    static ARGB MakeARGB(int a, int r, int g, int b) {
        return ((ARGB)a << 24) | ((ARGB)r << 16) | ((ARGB)g << 8) | (ARGB)b;
    }
    int GetAlpha() const { return c_.alpha(); }
    int GetRed() const { return c_.red(); }
    int GetGreen() const { return c_.green(); }
    int GetBlue() const { return c_.blue(); }
    // GDI+ offers both spellings and MinerGUI uses the short one throughout,
    // to build a translucent version of an ore's colour
    int GetA() const { return c_.alpha(); }
    int GetR() const { return c_.red(); }
    int GetG() const { return c_.green(); }
    int GetB() const { return c_.blue(); }
    ARGB GetValue() const { return MakeARGB(c_.alpha(), c_.red(), c_.green(), c_.blue()); }

    const QColor& q() const { return c_; }

private:
    QColor c_{0, 0, 0, 255};
};

struct PointF {
    PointF() = default;
    PointF(REAL x, REAL y) : X(x), Y(y) {}
    REAL X = 0, Y = 0;
    QPointF q() const { return QPointF(X, Y); }
};

struct Point {
    Point() = default;
    Point(int x, int y) : X(x), Y(y) {}
    int X = 0, Y = 0;
};

struct RectF {
    RectF() = default;
    RectF(REAL x, REAL y, REAL w, REAL h) : X(x), Y(y), Width(w), Height(h) {}
    REAL X = 0, Y = 0, Width = 0, Height = 0;

    REAL GetLeft() const { return X; }
    REAL GetTop() const { return Y; }
    REAL GetRight() const { return X + Width; }
    REAL GetBottom() const { return Y + Height; }
    bool Contains(REAL x, REAL y) const {
        return x >= X && x < X + Width && y >= Y && y < Y + Height;
    }
    bool Contains(const PointF& p) const { return Contains(p.X, p.Y); }
    void Inflate(REAL dx, REAL dy) { X -= dx; Y -= dy; Width += 2 * dx; Height += 2 * dy; }
    QRectF q() const { return QRectF(X, Y, Width, Height); }
};

struct Rect {
    Rect() = default;
    Rect(int x, int y, int w, int h) : X(x), Y(y), Width(w), Height(h) {}
    int X = 0, Y = 0, Width = 0, Height = 0;
};

// --------------------------------------------------------------------------

class FontFamily {
public:
    FontFamily() = default;
    explicit FontFamily(const wchar_t* name) : name_(QString::fromWCharArray(name)) {
        // "available" means a real face answered to the name, not that Qt
        // found something to substitute. QFontDatabase would say families();
        // asking the resolved font what it actually is says the same thing in
        // one call and matches what GDI+ means by it.
        //
        // ...with one exception, on purpose: a face registered as a stand-in
        // for the name (QFont::insertSubstitution, see the fonts the Linux
        // package carries - docs/linux-port.md) counts as the name. The
        // layout was measured in Segoe UI and Bahnschrift; a metric twin
        // under another name is the same face as far as the layout knows.
        QFont probe(name_);
        const QString resolved = QFontInfo(probe).family();
        available_ = resolved.compare(name_, Qt::CaseInsensitive) == 0;
        if (!available_) {
            for (const QString& stand : QFont::substitutes(name_)) {
                if (resolved.compare(stand, Qt::CaseInsensitive) == 0) {
                    available_ = true;
                    name_ = stand;
                    break;
                }
            }
        }
    }
    bool IsAvailable() const { return available_; }
    const QString& name() const { return name_; }

    // The face's measures as GDI+ has them: an em, and how much of it stands
    // over the baseline - so that size * GetCellAscent / GetEmHeight is how
    // far under a line's top its baseline is. Asked of a Font's family
    // (Font::GetFamily) it is that very font's ascent, at its own size, which
    // is what DrawString sets the line by; asked of a family by its name, the
    // face's at a size hinting does not round.
    unsigned short GetEmHeight(int /*style*/) const { return EM; }
    unsigned short GetCellAscent(int style) const {
        if (ascentOfEm_ > 0) return (unsigned short)std::lround(ascentOfEm_ * EM);
        QFont f(name_);
        f.setPixelSize(256);
        f.setBold((style & 1) != 0);
        return (unsigned short)std::lround(QFontMetricsF(f).ascent() / 256.0 * EM);
    }

    // The generic families cannot be missing, which is what callers use them for
    static const FontFamily* GenericSansSerif() {
        static FontFamily* f = [] {
            auto* g = new FontFamily();
            g->name_ = QFont().defaultFamily();
            if (g->name_.isEmpty()) g->name_ = QStringLiteral("sans-serif");
            g->available_ = true;
            return g;
        }();
        return f;
    }

private:
    friend class Font;
    static constexpr unsigned short EM = 16384;
    QString name_;
    bool available_ = false;
    double ascentOfEm_ = 0;   // of the font this family was asked of, by its size
};

class Font {
public:
    Font(const FontFamily* family, REAL size, int style = FontStyleRegular,
         Unit unit = UnitPixel) {
        f_.setFamily(family ? family->name() : FontFamily::GenericSansSerif()->name());
        // Bedrock asks in pixels everywhere, which is the honest unit for a
        // program that lays itself out in pixels.
        if (unit == UnitPixel) {
            f_.setPixelSize((int)std::lround(size));
        } else {
            f_.setPointSizeF(size);
        }
        f_.setBold((style & FontStyleBold) != 0);
        f_.setItalic((style & FontStyleItalic) != 0);
        f_.setUnderline((style & FontStyleUnderline) != 0);
    }
    const QFont& q() const { return f_; }

    // What the font was made with, for its measures (see FontFamily's):
    // the size is the one set, in whole pixels where pixels were asked for
    REAL GetSize() const { return f_.pixelSize() > 0 ? (REAL)f_.pixelSize() : (REAL)f_.pointSizeF(); }
    int GetStyle() const {
        return (f_.bold() ? FontStyleBold : 0) | (f_.italic() ? FontStyleItalic : 0) |
               (f_.underline() ? FontStyleUnderline : 0);
    }
    Status GetFamily(FontFamily* family) const {
        if (!family) return GenericError;
        family->name_ = f_.family();
        family->available_ = true;
        const REAL size = GetSize();
        family->ascentOfEm_ = size > 0 ? QFontMetricsF(f_).ascent() / size : 0.0;
        return Ok;
    }

private:
    QFont f_;
};

class StringFormat {
public:
    void SetAlignment(StringAlignment a) { h_ = a; }
    void SetLineAlignment(StringAlignment a) { v_ = a; }
    void SetFormatFlags(int flags) { flags_ = flags; }
    void SetTrimming(StringTrimming t) { trim_ = t; }

    int qtFlags() const {
        int f = 0;
        f |= (h_ == StringAlignmentCenter) ? Qt::AlignHCenter
             : (h_ == StringAlignmentFar)  ? Qt::AlignRight
                                           : Qt::AlignLeft;
        f |= (v_ == StringAlignmentCenter) ? Qt::AlignVCenter
             : (v_ == StringAlignmentFar)  ? Qt::AlignBottom
                                           : Qt::AlignTop;
        f |= (flags_ & StringFormatFlagsNoWrap) ? Qt::TextSingleLine : Qt::TextWordWrap;
        return f;
    }
    StringTrimming trimming() const { return trim_; }

private:
    StringAlignment h_ = StringAlignmentNear;
    StringAlignment v_ = StringAlignmentNear;
    int flags_ = 0;
    StringTrimming trim_ = StringTrimmingNone;
};

class Brush {
public:
    virtual ~Brush() = default;
    virtual QBrush q() const = 0;
};

class SolidBrush : public Brush {
public:
    explicit SolidBrush(const Color& c) : c_(c) {}
    QBrush q() const override { return QBrush(c_.q()); }
    const Color& color() const { return c_; }

private:
    Color c_;
};

// Used once in the whole program, and it is the once that matters most on the
// main window: every probable match is a card with a fill from the left whose
// width is the probability, shading out towards the right. Without it the rows
// are flat boxes and the reading loses the thing that makes it scannable.
class LinearGradientBrush : public Brush {
public:
    LinearGradientBrush(const RectF& area, const Color& from, const Color& to,
                        LinearGradientMode mode) {
        QLinearGradient g;
        switch (mode) {
            case LinearGradientModeVertical:
                g = QLinearGradient(area.X, area.Y, area.X, area.Y + area.Height);
                break;
            case LinearGradientModeForwardDiagonal:
                g = QLinearGradient(area.X, area.Y, area.X + area.Width, area.Y + area.Height);
                break;
            default:
                g = QLinearGradient(area.X, area.Y, area.X + area.Width, area.Y);
                break;
        }
        g.setColorAt(0.0, from.q());
        g.setColorAt(1.0, to.q());
        g_ = g;
    }
    QBrush q() const override { return QBrush(g_); }

private:
    QLinearGradient g_;
};

class Pen {
public:
    Pen(const Color& c, REAL width = 1.0f) : c_(c), w_(width) {}
    void SetLineJoin(LineJoin j) { join_ = j; }
    void SetStartCap(LineCap c) { cap_ = c; }
    void SetEndCap(LineCap c) { cap_ = c; }
    void SetWidth(REAL w) { w_ = w; }

    QPen q() const {
        QPen p(c_.q());
        p.setWidthF(w_);
        p.setJoinStyle(join_ == LineJoinRound ? Qt::RoundJoin : Qt::MiterJoin);
        p.setCapStyle(cap_ == LineCapRound ? Qt::RoundCap : Qt::FlatCap);
        return p;
    }

private:
    Color c_;
    REAL w_ = 1.0f;
    LineJoin join_ = LineJoinMiter;
    LineCap cap_ = LineCapFlat;
};

enum MatrixOrder { MatrixOrderPrepend = 0, MatrixOrderAppend = 1 };

// What a path is moved by - the refinery button's count, centred on its
// ink, uses GraphicsPath::AddString, GetBounds, Transform and Matrix
class Matrix {
public:
    void Translate(REAL dx, REAL dy, MatrixOrder order = MatrixOrderPrepend) {
        const QTransform by = QTransform::fromTranslate(dx, dy);
        // GDI+ and Qt both apply a matrix to row vectors: prepended goes first
        t_ = order == MatrixOrderPrepend ? by * t_ : t_ * by;
    }
    const QTransform& q() const { return t_; }

private:
    QTransform t_;
};

class GraphicsPath {
public:
    // A string's outlines, laid as GDI+ lays them: `origin` is the top left
    // of the line, where Qt puts the baseline at the point it is given
    Status AddString(const wchar_t* text, int length, const FontFamily* family, int style, REAL emSize,
                     const PointF& origin, const StringFormat* format) {
        (void)format;
        if (!text) return GenericError;
        const Font font(family, emSize, style, UnitPixel);
        const QString s = QString::fromWCharArray(text, length);
        path_.addText(QPointF(origin.X, origin.Y + QFontMetricsF(font.q()).ascent()), font.q(), s);
        return Ok;
    }
    Status GetBounds(RectF* bounds, const Matrix* matrix = nullptr, const Pen* pen = nullptr) const {
        (void)pen;
        if (!bounds) return GenericError;
        const QRectF r = matrix ? matrix->q().map(path_).boundingRect() : path_.boundingRect();
        *bounds = RectF((REAL)r.x(), (REAL)r.y(), (REAL)r.width(), (REAL)r.height());
        return Ok;
    }
    Status Transform(const Matrix* matrix) {
        if (matrix) path_ = matrix->q().map(path_);
        return Ok;
    }
    void AddArc(REAL x, REAL y, REAL w, REAL h, REAL startDeg, REAL sweepDeg) {
        const QRectF box(x, y, w, h);
        // GDI+ measures angles clockwise from three o'clock; Qt measures them
        // anticlockwise in sixteenths of a degree. Same arc, other convention.
        if (path_.elementCount() == 0) {
            path_.arcMoveTo(box, -startDeg);
        }
        path_.arcTo(box, -startDeg, -sweepDeg);
    }
    void AddLine(REAL x1, REAL y1, REAL x2, REAL y2) {
        if (path_.elementCount() == 0) path_.moveTo(x1, y1);
        else path_.lineTo(x1, y1);
        path_.lineTo(x2, y2);
    }
    void AddRectangle(const RectF& r) { path_.addRect(r.q()); }
    void AddEllipse(REAL x, REAL y, REAL w, REAL h) { path_.addEllipse(QRectF(x, y, w, h)); }
    void CloseFigure() { path_.closeSubpath(); }
    void Reset() { path_ = QPainterPath(); }
    void SetFillMode(FillMode m) {
        path_.setFillRule(m == FillModeWinding ? Qt::WindingFill : Qt::OddEvenFill);
    }
    const QPainterPath& q() const { return path_; }

private:
    QPainterPath path_;
};

class Image {
public:
    virtual ~Image() = default;
    virtual const QImage& q() const = 0;
    REAL GetWidth() const { return q().width(); }
    REAL GetHeight() const { return q().height(); }
};

class Bitmap : public Image {
public:
    explicit Bitmap(const wchar_t* file) {
        if (!img_.load(QString::fromWCharArray(file))) status_ = GenericError;
        else img_ = img_.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    explicit Bitmap(const QImage& img) : img_(img) {}
    Bitmap(int w, int h, PixelFormat = PixelFormat32bppPARGB)
        : img_(w, h, QImage::Format_ARGB32_Premultiplied) { img_.fill(0); }
    // A packed DIB, as the clipboard hands one over
    Bitmap(const BITMAPINFO* info, void* bits) {
        img_ = bedrock_shim::imageFromDib(info ? &info->bmiHeader : nullptr, bits);
        if (img_.isNull()) status_ = GenericError;
        else img_ = img_.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    const QImage& q() const override { return img_; }
    QImage& image() { return img_; }
    // Qt's premultiplied ARGB lies in memory as GDI+'s PARGB does - blue,
    // green, red, alpha - so the pixels are handed out where they are
    Status LockBits(const Rect* rect, UINT, PixelFormat format, BitmapData* data) {
        if (!data || img_.isNull()) return GenericError;
        if (img_.format() != QImage::Format_ARGB32_Premultiplied)
            img_ = img_.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        const Rect all(0, 0, img_.width(), img_.height());
        const Rect r = rect ? *rect : all;
        data->Width = (UINT)r.Width;
        data->Height = (UINT)r.Height;
        data->Stride = (INT)img_.bytesPerLine();
        data->PixelFormat = format;
        data->Scan0 = img_.bits() + (qsizetype)r.Y * img_.bytesPerLine() + (qsizetype)r.X * 4;
        return Ok;
    }
    Status UnlockBits(BitmapData*) { return Ok; }
    Status GetLastStatus() const { return status_; }
    // GDI+ wants the encoder's CLSID and a quality parameter; Qt wants the
    // file's extension and a number. The CLSID names the format below.
    Status Save(const wchar_t* file, const CLSID* clsid, const EncoderParameters* params);

private:
    QImage img_;
    Status status_ = Ok;
};

// A picture as paint, laid from the origin of the page: filling a shape with
// it shows the part of the picture that lies under the shape
class TextureBrush : public Brush {
public:
    explicit TextureBrush(Image* image) : img_(image ? image->q() : QImage()) {}
    QBrush q() const override { return QBrush(img_); }

private:
    QImage img_;
};

class ImageAttributes {
public:
    Status SetWrapMode(WrapMode, const Color& = Color(), BOOL = FALSE) { return Ok; }
};

// --------------------------------------------------------------------------

class Graphics {
public:
    explicit Graphics(QPainter* p) : p_(p) {}
    // On a DC: the window's painter while it paints, the bitmap's for a
    // memory DC. The painter's state is put back when this goes.
    // Whatever transform and clip the painter already carries is the DC's
    // own - PrintWindow shifts it to where a child control sits - and stays;
    // what this Graphics adds is taken back when it goes.
    explicit Graphics(HDC hdc) : p_(bedrock_shim::painterFor(hdc)) {
        if (p_) {
            p_->save();
            restoreOnExit_ = true;
        }
    }
    // On a picture of its own
    explicit Graphics(Image* image) {
        if (auto* bitmap = dynamic_cast<Bitmap*>(image)) {
            own_ = new QPainter(&bitmap->image());
            p_ = own_;
        }
    }
    ~Graphics() {
        if (!p_) return;
        while (depth_ > 0) { p_->restore(); depth_--; }
        if (restoreOnExit_) p_->restore();
        if (own_) { own_->end(); delete own_; }
    }
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;

    SmoothingMode GetSmoothingMode() const {
        return p_->testRenderHint(QPainter::Antialiasing) ? SmoothingModeAntiAlias : SmoothingModeNone;
    }
    Status TranslateTransform(REAL dx, REAL dy) {
        p_->translate(dx, dy);
        return Ok;
    }
    Status RotateTransform(REAL degrees) {
        p_->rotate(degrees);
        return Ok;
    }
    Status ScaleTransform(REAL sx, REAL sy) {
        p_->scale(sx, sy);
        return Ok;
    }
    Status ResetTransform() {
        p_->resetTransform();
        return Ok;
    }

    Status SetSmoothingMode(SmoothingMode m) {
        p_->setRenderHint(QPainter::Antialiasing, m == SmoothingModeAntiAlias);
        return Ok;
    }
    Status SetTextRenderingHint(TextRenderingHint h) {
        p_->setRenderHint(QPainter::TextAntialiasing, h != TextRenderingHintSystemDefault);
        return Ok;
    }
    // Taken and given back as asked, and otherwise left alone: Qt's
    // antialiased painter already puts pixel centres at the half, which is
    // what PixelOffsetModeHalf asks of GDI+ (the app asks for it everywhere)
    Status SetPixelOffsetMode(PixelOffsetMode m) { pom_ = m; return Ok; }
    PixelOffsetMode GetPixelOffsetMode() const { return pom_; }
    InterpolationMode GetInterpolationMode() const { return interp_; }
    // Qt's raster painter draws as it is told; there is nothing queued
    Status Flush(FlushIntention = FlushIntentionFlush) { return Ok; }
    Status SetInterpolationMode(InterpolationMode m) {
        interp_ = m;
        p_->setRenderHint(QPainter::SmoothPixmapTransform,
                          m == InterpolationModeHighQualityBicubic ||
                          m == InterpolationModeHighQualityBilinear || m == InterpolationModeBilinear);
        return Ok;
    }

    Status Clear(const Color& c) {
        p_->fillRect(p_->window(), c.q());
        return Ok;
    }

    Status FillRectangle(const Brush* brush, const RectF& r) {
        p_->fillRect(r.q(), brush->q());
        return Ok;
    }
    Status FillRectangle(const Brush* brush, REAL x, REAL y, REAL w, REAL h) {
        return FillRectangle(brush, RectF(x, y, w, h));
    }
    Status DrawRectangle(const Pen* pen, REAL x, REAL y, REAL w, REAL h) {
        return DrawRectangle(pen, RectF(x, y, w, h));
    }
    Status DrawRectangle(const Pen* pen, const RectF& r) {
        p_->save();
        p_->setPen(pen->q());
        p_->setBrush(Qt::NoBrush);
        p_->drawRect(r.q());
        p_->restore();
        return Ok;
    }
    Status FillEllipse(const Brush* brush, const RectF& r) {
        p_->save();
        p_->setPen(Qt::NoPen);
        p_->setBrush(brush->q());
        p_->drawEllipse(r.q());
        p_->restore();
        return Ok;
    }
    Status FillEllipse(const Brush* brush, REAL x, REAL y, REAL w, REAL h) {
        return FillEllipse(brush, RectF(x, y, w, h));
    }
    Status DrawLine(const Pen* pen, REAL x1, REAL y1, REAL x2, REAL y2) {
        p_->save();
        p_->setPen(pen->q());
        p_->drawLine(QPointF(x1, y1), QPointF(x2, y2));
        p_->restore();
        return Ok;
    }
    Status DrawPolygon(const Pen* pen, const PointF* points, int count) {
        if (count < 2) return GenericError;
        QPolygonF poly;
        for (int i = 0; i < count; i++) poly << points[i].q();
        p_->save();
        p_->setPen(pen->q());
        p_->setBrush(Qt::NoBrush);
        p_->drawPolygon(poly);
        p_->restore();
        return Ok;
    }
    Status DrawEllipse(const Pen* pen, const RectF& r) {
        p_->save();
        p_->setPen(pen->q());
        p_->setBrush(Qt::NoBrush);
        p_->drawEllipse(r.q());
        p_->restore();
        return Ok;
    }
    Status DrawEllipse(const Pen* pen, REAL x, REAL y, REAL w, REAL h) {
        return DrawEllipse(pen, RectF(x, y, w, h));
    }
    Status DrawLines(const Pen* pen, const PointF* points, int count) {
        if (count < 2) return GenericError;
        QPolygonF poly;
        for (int i = 0; i < count; i++) poly << points[i].q();
        p_->save();
        p_->setPen(pen->q());
        p_->drawPolyline(poly);
        p_->restore();
        return Ok;
    }
    Status FillPath(const Brush* brush, const GraphicsPath* path) {
        p_->fillPath(path->q(), brush->q());
        return Ok;
    }
    Status DrawPath(const Pen* pen, const GraphicsPath* path) {
        p_->save();
        p_->setPen(pen->q());
        p_->setBrush(Qt::NoBrush);
        p_->drawPath(path->q());
        p_->restore();
        return Ok;
    }
    Status DrawImage(Image* img, const RectF& dest) {
        p_->drawImage(dest.q(), img->q());
        return Ok;
    }
    // A part of the picture (in its own pixels) into a rectangle of the page
    Status DrawImage(Image* img, const RectF& dest, REAL sx, REAL sy, REAL sw, REAL sh, Unit,
                     const ImageAttributes* = nullptr) {
        p_->drawImage(dest.q(), img->q(), QRectF(sx, sy, sw, sh));
        return Ok;
    }
    Status DrawImage(Image* img, REAL x, REAL y, REAL w, REAL h) {
        return DrawImage(img, RectF(x, y, w, h));
    }
    Status DrawImage(Image* img, const Rect& dest) {
        return DrawImage(img, RectF((REAL)dest.X, (REAL)dest.Y, (REAL)dest.Width, (REAL)dest.Height));
    }

    // GDI+ counts the string's length itself when handed -1, and takes the
    // layout rectangle's origin as the top left. Qt wants a QString and a
    // rectangle, which is the same thing said differently.
    Status DrawString(const wchar_t* text, int length, const Font* font,
                      const RectF& layout, const StringFormat* format, const Brush* brush) {
        // A rectangle with no extent is a point in GDI+ and a clip that admits
        // nothing in Qt, so it is sent the other way rather than drawn empty.
        if (layout.Width <= 0 || layout.Height <= 0) {
            return DrawString(text, length, font, PointF(layout.X, layout.Y), brush);
        }
        const QString s = detail::toQ(text, length);
        p_->save();
        p_->setFont(font->q());
        if (const auto* solid = dynamic_cast<const SolidBrush*>(brush)) {
            p_->setPen(solid->color().q());
        }
        // no format at all is GDI+'s default one, and that one wraps
        const int flags = format ? format->qtFlags() : (Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap);
        // GDI+ keeps a sixth of the em clear at both ends of the rectangle:
        // text aligned near starts that far in, text aligned far ends that
        // far short, and a line can only be as long as what is left
        const qreal pad = detail::pad(font->q());
        QRectF box = layout.q().adjusted(pad, 0, -pad, 0);
        if (box.width() <= 0) box = layout.q();
        QString shown = s;
        if (format && format->trimming() != StringTrimmingNone && layout.Width > 0) {
            const QFontMetricsF fm(font->q());
            shown = fm.elidedText(s, Qt::ElideRight, box.width());
        }
        p_->drawText(box, flags, shown);
        p_->restore();
        return Ok;
    }
    // The point overload is NOT the rectangle overload with a zero-sized
    // rectangle, and treating it as one draws nothing at all: Qt clips to the
    // rectangle it is given. Two things differ from GDI+ here and both bite:
    // the text is unbounded, and the point means the TOP LEFT of the line,
    // where Qt's drawText(QPointF) means the baseline. The ascent is the
    // difference between them.
    Status DrawString(const wchar_t* text, int length, const Font* font,
                      const PointF& at, const Brush* brush) {
        const QString s = detail::toQ(text, length);
        p_->save();
        p_->setFont(font->q());
        if (const auto* solid = dynamic_cast<const SolidBrush*>(brush)) {
            p_->setPen(solid->color().q());
        }
        const QFontMetricsF fm(font->q());
        p_->drawText(QPointF(at.X + detail::pad(font->q()), at.Y + fm.ascent()), s);
        p_->restore();
        return Ok;
    }

    // The point overload with a format: the origin is the top left, the
    // format only says how to align, which changes nothing about the size
    Status DrawString(const wchar_t* text, int length, const Font* font,
                      const PointF& at, const StringFormat* format, const Brush* brush) {
        (void)format;
        return DrawString(text, length, font, at, brush);
    }
    Status MeasureString(const wchar_t* text, int length, const Font* font,
                         const RectF& layout, const StringFormat* format, RectF* out) const {
        const QString s = detail::toQ(text, length);
        const QFontMetricsF fm(font->q());
        int flags = Qt::TextWordWrap;
        if (format) flags = (format->qtFlags() & Qt::TextSingleLine) ? Qt::TextSingleLine : Qt::TextWordWrap;
        // The padding is in the answer, as it is in GDI+'s; a wrapped text
        // wraps inside what the padding leaves of the rectangle
        const qreal pad = detail::pad(font->q());
        if (layout.Width > 0 && !(flags & Qt::TextSingleLine)) {
            const QRectF inside(layout.X, layout.Y, std::max<qreal>(1.0, layout.Width - 2 * pad), 100000.0);
            const QRectF box = fm.boundingRect(inside, flags, s);
            *out = RectF(layout.X, layout.Y, (REAL)(box.width() + 2 * pad), (REAL)box.height());
        } else {
            *out = RectF(layout.X, layout.Y, (REAL)(fm.horizontalAdvance(s) + 2 * pad), (REAL)fm.height());
        }
        return Ok;
    }
    Status MeasureString(const wchar_t* text, int length, const Font* font,
                         const PointF& at, const StringFormat* format, RectF* out) const {
        return MeasureString(text, length, font, RectF(at.X, at.Y, 0, 0), format, out);
    }
    Status MeasureString(const wchar_t* text, int length, const Font* font,
                         const RectF& layout, RectF* out) const {
        return MeasureString(text, length, font, layout, nullptr, out);
    }
    Status MeasureString(const wchar_t* text, int length, const Font* font,
                         const PointF& at, RectF* out) const {
        return MeasureString(text, length, font, RectF(at.X, at.Y, 0, 0), out);
    }

    Status SetClip(const RectF& r) {
        p_->setClipRect(r.q());
        return Ok;
    }
    Status ResetClip() {
        p_->setClipping(false);
        return Ok;
    }

    GraphicsState Save() {
        p_->save();
        return ++depth_;
    }
    Status Restore(GraphicsState state) {
        while (depth_ >= state && depth_ > 0) {
            p_->restore();
            depth_--;
        }
        return Ok;
    }

    QPainter* painter() const { return p_; }

private:
    QPainter* p_ = nullptr;
    QPainter* own_ = nullptr;
    PixelOffsetMode pom_ = PixelOffsetModeDefault;
    InterpolationMode interp_ = InterpolationModeDefault;
    bool restoreOnExit_ = false;
    GraphicsState depth_ = 0;
};

// --------------------------------------------------------------------------
// Encoders. GDI+ lists its codecs and MinerGUI picks the JPEG one by MIME
// type; here the list has exactly the entries Qt writes, and the CLSID is
// made up but stable, so it can be handed back to Save.
struct ImageCodecInfo {
    CLSID Clsid;
    GUID FormatID;
    const wchar_t* CodecName;
    const wchar_t* DllName;
    const wchar_t* FormatDescription;
    const wchar_t* FilenameExtension;
    const wchar_t* MimeType;
    DWORD Flags;
    DWORD Version;
    DWORD SigCount;
    DWORD SigSize;
    const BYTE* SigPattern;
    const BYTE* SigMask;
};

struct EncoderParameter {
    GUID Guid;
    ULONG NumberOfValues;
    ULONG Type;
    void* Value;
};

struct EncoderParameters {
    UINT Count;
    EncoderParameter Parameter[1];
};

inline const GUID EncoderQuality = {0x1D5BE4B5, 0xFA4A, 0x452D, {0x9C, 0xDD, 0x5D, 0xB3, 0x51, 0x05, 0xE7, 0xEB}};

namespace detail {
inline const ImageCodecInfo* codecs(UINT* count) {
    static const ImageCodecInfo table[] = {
        {{0x557CF401, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}}, {}, L"JPEG", L"",
         L"JPEG", L"*.jpg;*.jpeg", L"image/jpeg", 0, 1, 0, 0, nullptr, nullptr},
        {{0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}}, {}, L"PNG", L"",
         L"PNG", L"*.png", L"image/png", 0, 1, 0, 0, nullptr, nullptr},
    };
    *count = 2;
    return table;
}
}  // namespace detail

inline Status GetImageEncodersSize(UINT* count, UINT* bytes) {
    UINT n = 0;
    detail::codecs(&n);
    if (count) *count = n;
    if (bytes) *bytes = n * (UINT)sizeof(ImageCodecInfo);
    return Ok;
}

inline Status GetImageEncoders(UINT count, UINT bytes, ImageCodecInfo* out) {
    UINT n = 0;
    const ImageCodecInfo* table = detail::codecs(&n);
    if (!out || count < n || bytes < n * sizeof(ImageCodecInfo)) return GenericError;
    std::memcpy(out, table, n * sizeof(ImageCodecInfo));
    return Ok;
}

inline Status Bitmap::Save(const wchar_t* file, const CLSID* clsid, const EncoderParameters* params) {
    if (!file) return GenericError;
    UINT n = 0;
    const ImageCodecInfo* table = detail::codecs(&n);
    const char* format = nullptr;
    for (UINT i = 0; i < n && clsid; i++) {
        if (std::memcmp(&table[i].Clsid, clsid, sizeof(CLSID)) == 0) {
            format = std::wcscmp(table[i].MimeType, L"image/jpeg") == 0 ? "JPEG" : "PNG";
        }
    }
    int quality = -1;
    if (params) {
        for (UINT i = 0; i < params->Count; i++) {
            const EncoderParameter& p = params->Parameter[i];
            if (std::memcmp(&p.Guid, &EncoderQuality, sizeof(GUID)) == 0 && p.Value) {
                quality = (int)*static_cast<const ULONG*>(p.Value);
            }
        }
    }
    return img_.save(QString::fromWCharArray(file), format, quality) ? Ok : GenericError;
}

// GDI+ has to be started and stopped; Qt does not. The calls stay, so the
// application code does not have to care which it is talking to.
struct GdiplusStartupInput {
    std::uint32_t GdiplusVersion = 1;
    void* DebugEventCallback = nullptr;
    bool SuppressBackgroundThread = false;
    bool SuppressExternalCodecs = false;
};
inline Status GdiplusStartup(std::uintptr_t* token, const GdiplusStartupInput*, void*) {
    if (token) *token = 1;
    return Ok;
}
inline void GdiplusShutdown(std::uintptr_t) {}

}  // namespace Gdiplus
