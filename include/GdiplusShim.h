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
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QStack>
#include <QString>
#include <QStringList>
#include <QTransform>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
// A text broken at its spaces into at most that many lines of that width,
// as GDI+ wraps it, the last of them ending in "…" where the rest would
// not fit on it
inline QString wrapAndTrim(const QString& s, const QFontMetricsF& fm, qreal width, int maxLines) {
    const QStringList words = s.split(QLatin1Char(' '));
    QStringList lines;
    QString line;
    int i = 0;
    for (; i < words.size(); ++i) {
        const QString longer = line.isEmpty() ? words[i] : line + QLatin1Char(' ') + words[i];
        if (line.isEmpty() || fm.horizontalAdvance(longer) <= width) {
            line = longer;
            continue;
        }
        if (lines.size() + 1 == maxLines) break;
        lines << line;
        line = words[i];
    }
    if (i < words.size()) {
        QString rest = line;
        for (; i < words.size(); ++i) rest += QLatin1Char(' ') + words[i];
        lines << fm.elidedText(rest, Qt::ElideRight, width);
    } else {
        lines << line;
    }
    return lines.join(QLatin1Char('\n'));
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
            exact_ = size;
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
    // The size in pixels as asked, which GDI+ draws at - Qt is given it
    // rounded to the pixel (0 for a size in points)
    REAL exactPixels() const { return exact_; }

private:
    QFont f_;
    REAL exact_ = 0;
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

// The text check's (below) hold on a picture: let go when the picture goes
// or is written to other than by drawing
namespace textcheck {
inline void forget(const QPaintDevice* device);
}

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
    ~Bitmap() override { textcheck::forget(&img_); }
    const QImage& q() const override { return img_; }
    QImage& image() { return img_; }
    // Qt's premultiplied ARGB lies in memory as GDI+'s PARGB does - blue,
    // green, red, alpha - so the pixels are handed out where they are
    Status LockBits(const Rect* rect, UINT flags, PixelFormat format, BitmapData* data) {
        if (!data || img_.isNull()) return GenericError;
        if (flags & ImageLockModeWrite) textcheck::forget(&img_);
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
    const QImage& image() const { return img_; }

private:
    QImage img_;
};

class ImageAttributes {
public:
    Status SetWrapMode(WrapMode, const Color& = Color(), BOOL = FALSE) { return Ok; }
};

// --------------------------------------------------------------------------
// The text check, for the shot run (BEDROCK_TEXT_CHECK=1): what of the texts
// a Windows user would see wrong in a picture. Every DrawString is noted on
// the picture it goes into, laid out as GDI+ lays it out at the size the
// font was asked for - the shim draws at that size rounded to the pixel -
// and when the painting of a picture ends, what is wrong in it is printed,
// one line each, among the shot run's own lines:
//   CUT      shortened to fit its box: GDI+ trims it and puts in the "..."
//   CLIPPED  it needs more than its box has (width, lines, height of the
//            ink), or it runs past the clip or the picture's edge
//   OVERLAP  two texts that can be seen lie over each other
// A text is no longer there to be seen where an opaque fill or picture
// drawn after it covers it; a picture with texts of its own drawn one to
// one brings them along (a page drawn first and the lists over it). It
// only looks: the pictures are the same with it as without it.
// scripts/text_check_report.py sorts the lines to the pictures.
namespace textcheck {

inline bool on() {
    static const bool enabled = [] {
        const char* v = std::getenv("BEDROCK_TEXT_CHECK");
        return v != nullptr && *v != '\0' && std::strcmp(v, "0") != 0;
    }();
    return enabled;
}

// A line of a text, in the picture's pixels
struct Line {
    QString words;
    QRectF box;    // across, the line's advance; up and down, its ink
    QRectF ink;    // the glyphs
    QRectF seen;   // what of box can be seen: inside the clip and the text's box, under no later fill
};

struct Text {
    QString text;            // as asked
    QString shown;           // as GDI+ shows it at the asked size, a line break where it is wrapped
    bool shotWhole = true;   // ...and whether the shot, at the size rounded, shows all of it
    bool boxed = false;      // drawn into a box (the rectangle overload), which GDI+ clips to
    QRectF where;            // the box, or for a point what the lines take
    QRectF clip;             // the picture and the painter's clip
    QString shortOf;         // what the box lacks for it, if anything
    std::vector<Line> lines;
};

struct Target {
    int open = 0;            // Graphics drawing on it
    bool kept = false;       // a Bitmap: its texts go along where it is drawn
    QRectF frame;            // the whole picture
    std::vector<Text> texts;
};

struct State {
    std::mutex lock;
    std::unordered_map<const QPaintDevice*, Target> targets;
    std::unordered_map<qint64, bool> opaque;   // pictures, by cacheKey
};

inline State& state() {
    static State s;
    return s;
}

inline bool visible(const QRectF& r) { return r.width() >= 0.5 && r.height() >= 0.5; }

// How far a reaches out of b, the most on any side
inline qreal outside(const QRectF& a, const QRectF& b) {
    return std::max({b.left() - a.left(), a.right() - b.right(), b.top() - a.top(), a.bottom() - b.bottom(), (qreal)0});
}

// The painter's clip in the picture's pixels, within the picture
inline QRectF clipOf(QPainter* p, const QRectF& frame) {
    if (!p->hasClipping()) return frame;
    return frame.intersected(p->combinedTransform().mapRect(p->clipBoundingRect()));
}

inline QString quoted(const QString& s) {
    QString out(QLatin1Char('"'));
    for (const QChar c : s) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\')) {
            out += QLatin1Char('\\');
            out += c;
        } else if (c == QLatin1Char('\n') || c == QChar::LineSeparator) {
            out += QLatin1String("\\n");
        } else {
            out += c;
        }
    }
    return out + QLatin1Char('"');
}

inline QString xywh(const QRectF& r) {
    return QStringLiteral("%1,%2,%3,%4")
        .arg(std::lround(r.x()))
        .arg(std::lround(r.y()))
        .arg(std::lround(r.width()))
        .arg(std::lround(r.height()));
}

inline QString px(qreal v) { return QString::number(v, 'f', 1); }

// The words, wherever the lines break
inline QString flat(const QString& s) {
    QString f = s;
    f.replace(QLatin1Char('\n'), QLatin1Char(' '));
    f.replace(QChar::LineSeparator, QLatin1Char(' '));
    return f.simplified();
}

inline QString withoutTrailingSpace(const QString& s) {
    int n = (int)s.size();
    while (n > 0 && s[n - 1].isSpace()) n--;
    return s.left(n);
}

inline bool opaqueLocked(const QImage& img) {
    if (img.isNull()) return false;
    if (!img.hasAlphaChannel()) return true;
    auto& known = state().opaque;
    const auto it = known.find(img.cacheKey());
    if (it != known.end()) return it->second;
    const bool argb = img.format() == QImage::Format_ARGB32 || img.format() == QImage::Format_ARGB32_Premultiplied;
    const QImage pixels = argb ? img : img.convertToFormat(QImage::Format_ARGB32);
    bool all = true;
    for (int y = 0; all && y < pixels.height(); y++) {
        const auto* row = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width(); x++) {
            if (qAlpha(row[x]) != 255) {
                all = false;
                break;
            }
        }
    }
    if (known.size() > 4096) known.clear();
    known[img.cacheKey()] = all;
    return all;
}

inline bool opaqueLocked(const Brush* brush) {
    if (!brush) return false;
    if (const auto* texture = dynamic_cast<const TextureBrush*>(brush)) return opaqueLocked(texture->image());
    return brush->q().isOpaque();
}

inline Target* targetLocked(const QPaintDevice* device) {
    auto& targets = state().targets;
    const auto it = targets.find(device);
    return it == targets.end() ? nullptr : &it->second;
}

// r less the part of it that is gone, where that takes a side off it; a
// hole in the middle leaves the wider side, a corner leaves it whole
inline QRectF without(const QRectF& r, const QRectF& gone) {
    if (gone.top() <= r.top() + 0.5 && gone.bottom() >= r.bottom() - 0.5) {
        const QRectF left(r.left(), r.top(), gone.left() - r.left(), r.height());
        const QRectF right(gone.right(), r.top(), r.right() - gone.right(), r.height());
        return left.width() >= right.width() ? left : right;
    }
    if (gone.left() <= r.left() + 0.5 && gone.right() >= r.right() - 0.5) {
        const QRectF above(r.left(), r.top(), r.width(), gone.top() - r.top());
        const QRectF below(r.left(), gone.bottom(), r.width(), r.bottom() - gone.bottom());
        return above.height() >= below.height() ? above : below;
    }
    return r;
}

// An opaque shape painted over what is there (in the painter's units)
inline void coverLocked(Target& t, QPainter* p, const QPainterPath& shape, bool rectangle) {
    if (t.texts.empty()) return;
    const QTransform m = p->combinedTransform();
    const QRectF bounds = m.mapRect(shape.boundingRect()).intersected(clipOf(p, t.frame));
    if (!visible(bounds)) return;
    const bool plain = rectangle && m.type() <= QTransform::TxScale;
    QPainterPath onPicture;
    bool mapped = false;
    auto covers = [&](const QRectF& r) {
        if (plain) return true;
        if (!mapped) {
            onPicture = m.map(shape);
            mapped = true;
        }
        return onPicture.contains(r.adjusted(0.25, 0.25, -0.25, -0.25));
    };
    for (Text& x : t.texts) {
        for (Line& l : x.lines) {
            if (!visible(l.seen) || !bounds.intersects(l.seen)) continue;
            const QRectF part = l.seen.intersected(bounds);
            if (covers(part)) {
                l.seen = without(l.seen, part);
                continue;
            }
            // A shape with round corners over a part of the line: the longest
            // run of columns in which it covers the line's whole height
            const bool tall = part.top() <= l.seen.top() + 0.5 && part.bottom() >= l.seen.bottom() - 0.5;
            if (plain || !tall) continue;
            qreal start = 0, bestStart = 0, bestLength = 0;
            bool running = false;
            for (qreal cx = part.left(); cx < part.right(); cx += 1.0) {
                const qreal step = std::min<qreal>(1.0, part.right() - cx);
                const bool covered = covers(QRectF(cx, part.top(), step, part.height()));
                if (covered && !running) {
                    start = cx;
                    running = true;
                }
                const bool last = cx + 1.0 >= part.right();
                if (running && (!covered || last)) {
                    const qreal stop = covered ? cx + step : cx;
                    if (stop - start > bestLength) {
                        bestLength = stop - start;
                        bestStart = start;
                    }
                    running = false;
                }
            }
            if (bestLength >= 1.0) l.seen = without(l.seen, QRectF(bestStart, part.top(), bestLength, part.height()));
        }
    }
    t.texts.erase(std::remove_if(t.texts.begin(), t.texts.end(),
                                 [](const Text& x) {
                                     return std::none_of(x.lines.begin(), x.lines.end(),
                                                         [](const Line& l) { return visible(l.seen); });
                                 }),
                  t.texts.end());
}

// A DrawString: where GDI+ puts the lines at the asked size. `drawn` is what
// the shim drew, `layout` the box (null for the point overloads)
inline void textLocked(Target& t, QPainter* p, const Font& font, const QString& s, const QString& drawn,
                       const RectF* layout, const PointF& at, int flags, StringTrimming trimming) {
    if (s.trimmed().isEmpty()) return;
    const QFont& f = font.q();
    const QFontMetricsF fm(f);
    const qreal rounded = f.pixelSize() > 0 ? (qreal)f.pixelSize() : f.pointSizeF() * 96.0 / 72.0;
    const qreal asked = font.exactPixels() > 0 ? (qreal)font.exactPixels() : rounded;
    // widths and heights scale with the size; Qt's are the rounded size's
    const qreal k = rounded > 0 ? asked / rounded : 1.0;
    const qreal pad = asked / 6.0;
    const qreal ascent = fm.ascent() * k;
    const qreal spacing = std::max<qreal>(1.0, fm.lineSpacing() * k);
    auto advance = [&](const QString& w) { return fm.horizontalAdvance(w) * k; };
    const QTransform m = p->combinedTransform();
    const qreal sx = std::hypot(m.m11(), m.m12());

    Text x;
    x.text = s;
    x.shotWhole = flat(drawn) == flat(s);
    x.boxed = layout != nullptr;
    QStringList lines;
    QRectF box;
    qreal room = 0;   // how wide a line may be, inside the padding
    int holds = 1;    // how many lines the box has room for
    if (layout) {
        box = layout->q();
        room = box.width() - 2 * pad > 0 ? box.width() - 2 * pad : box.width();
        holds = std::max(1, (int)((box.height() + 0.01) / spacing));
        const bool single = (flags & Qt::TextSingleLine) != 0;
        QString one = s;
        one.replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (trimming != StringTrimmingNone) {
            const int most = single ? 1 : holds;
            x.shown = most > 1 ? detail::wrapAndTrim(s, fm, room / k, most) : fm.elidedText(s, Qt::ElideRight, room / k);
            lines = x.shown.split(QLatin1Char('\n'));
        } else {
            x.shown = s;
            QStringList wrapped;
            if (!single) {
                for (const QString& para : s.split(QLatin1Char('\n')))
                    wrapped += detail::wrapAndTrim(para, fm, room / k, INT_MAX).split(QLatin1Char('\n'));
            }
            if (single || (wrapped.size() > 1 && holds == 1)) {
                // one line however long: GDI+ starts it at the box's near
                // side, aligned as it may be, and the box cuts it off
                lines = QStringList{one};
                const qreal need = advance(withoutTrailingSpace(one));
                if (need > room + 0.5) {
                    x.shortOf = QStringLiteral("width: needs %1 px, has %2").arg(px(need * sx), px(room * sx));
                }
            } else {
                lines = wrapped;
                if (lines.size() > holds) {
                    x.shortOf = QStringLiteral("lines: needs %1, has room for %2").arg(lines.size()).arg(holds);
                }
                qreal widest = 0;
                for (const QString& w : lines) widest = std::max(widest, advance(withoutTrailingSpace(w)));
                if (widest > room + 0.5 && x.shortOf.isEmpty()) {
                    x.shortOf = QStringLiteral("width: a word needs %1 px, has %2").arg(px(widest * sx), px(room * sx));
                }
            }
        }
    } else {
        x.shown = s;
        lines = s.split(QLatin1Char('\n'));
    }

    const int across = flags & (Qt::AlignLeft | Qt::AlignRight | Qt::AlignHCenter);
    const int down = flags & (Qt::AlignTop | Qt::AlignBottom | Qt::AlignVCenter);
    const qreal block = lines.size() * spacing;
    qreal top = layout ? box.y() : at.Y;
    if (layout && (down & Qt::AlignVCenter)) top += (box.height() - block) / 2;
    else if (layout && (down & Qt::AlignBottom)) top += box.height() - block;
    x.clip = clipOf(p, t.frame);
    const QRectF boxOnPicture = layout ? m.mapRect(box) : QRectF();
    const QRectF can = layout ? x.clip.intersected(boxOnPicture) : x.clip;
    qreal past = 0;   // the ink out of the box, above or below
    QRectF extent;
    for (int i = 0; i < (int)lines.size(); i++) {
        const QString& w = lines[i];
        int lead = 0;
        while (lead < (int)w.size() && w[lead].isSpace()) lead++;
        const QString body = withoutTrailingSpace(w.mid(lead));
        if (body.isEmpty()) continue;
        const qreal width = advance(withoutTrailingSpace(w));
        qreal left = (layout ? box.x() : at.X) + pad;
        if (layout && width <= room + 0.5) {
            if (across & Qt::AlignRight) left = box.right() - pad - width;
            else if (across & Qt::AlignHCenter) left = box.x() + (box.width() - width) / 2;
        }
        const qreal start = left + advance(w.left(lead));
        const qreal base = top + i * spacing + ascent;
        const QRectF tight = fm.tightBoundingRect(body);
        const QRectF ink(start + tight.left() * k, base + tight.top() * k, tight.width() * k, tight.height() * k);
        if (layout && i < holds) past = std::max({past, box.top() - ink.top(), ink.bottom() - box.bottom()});
        Line l;
        l.words = body;
        l.ink = m.mapRect(ink);
        l.box = m.mapRect(QRectF(start, ink.top(), advance(body), ink.height()));
        l.seen = l.box.intersected(can);
        extent = extent.isNull() ? l.box : extent.united(l.box);
        x.lines.push_back(l);
    }
    if (layout && past > 1.0) {
        const QString high = QStringLiteral("height: ink %1 px out of the box").arg(px(past * std::hypot(m.m21(), m.m22())));
        x.shortOf = x.shortOf.isEmpty() ? high : x.shortOf + QStringLiteral("; ") + high;
    }
    x.where = layout ? boxOnPicture : extent;
    if (std::none_of(x.lines.begin(), x.lines.end(), [](const Line& l) { return visible(l.seen); })) return;
    t.texts.push_back(std::move(x));
}

// The end of a picture's painting: what is wrong in it
inline void reportLocked(const Target& t) {
    const QString size = QStringLiteral("%1x%2").arg(std::lround(t.frame.width())).arg(std::lround(t.frame.height()));
    QString out = QStringLiteral("TEXTCHECK PASS %1 texts=%2\n").arg(size).arg(t.texts.size());
    for (const Text& x : t.texts) {
        if (flat(x.shown) != flat(x.text)) {
            out += QStringLiteral("TEXTCHECK CUT %1 %2 %3 -> %4%5\n")
                       .arg(size, xywh(x.where), quoted(x.text), quoted(x.shown),
                            x.shotWhole ? QStringLiteral(" (the shot shows it whole)") : QString());
        }
        QStringList why;
        if (!x.shortOf.isEmpty()) why << x.shortOf;
        qreal edge = 0, clip = 0;
        for (const Line& l : x.lines) {
            const QRectF ink = x.boxed ? l.ink.intersected(x.where) : l.ink;
            if (!visible(ink)) continue;
            edge = std::max(edge, outside(ink, t.frame));
            clip = std::max(clip, outside(ink, x.clip));
        }
        if (edge > 1.0) why << QStringLiteral("edge: %1 px past the picture").arg(px(edge));
        else if (clip > 1.0) why << QStringLiteral("clip: %1 px outside the clip").arg(px(clip));
        if (!why.isEmpty()) {
            out += QStringLiteral("TEXTCHECK CLIPPED %1 %2 %3 %4\n").arg(size, xywh(x.where), quoted(x.text), why.join(QStringLiteral("; ")));
        }
    }
    for (size_t i = 0; i < t.texts.size(); i++) {
        for (size_t j = i + 1; j < t.texts.size(); j++) {
            bool found = false;
            for (const Line& a : t.texts[i].lines) {
                if (found || !visible(a.seen)) continue;
                for (const Line& b : t.texts[j].lines) {
                    if (!visible(b.seen)) continue;
                    const QRectF both = a.seen.intersected(b.seen);
                    if (both.width() <= 1.0 || both.height() <= 1.0) continue;
                    // the same words a hair off themselves: a shadow, a second coat
                    if (a.words == b.words && std::abs(a.seen.center().x() - b.seen.center().x()) <= 2.5 &&
                        std::abs(a.seen.center().y() - b.seen.center().y()) <= 2.5) {
                        continue;
                    }
                    out += QStringLiteral("TEXTCHECK OVERLAP %1 %2 %3 vs %4 %5 by %6x%7\n")
                               .arg(size, xywh(a.seen), quoted(a.words), xywh(b.seen), quoted(b.words), px(both.width()),
                                    px(both.height()));
                    found = true;
                    break;
                }
            }
        }
    }
    std::cout << out.toStdString() << std::flush;
}

// A Graphics begins on a picture: the first of them begins its painting,
// and what was noted on it before is over
inline const QPaintDevice* begin(QPainter* p, bool bitmap) {
    if (!on() || !p || !p->isActive()) return nullptr;
    const QPaintDevice* device = p->device();
    // (the one-pixel pictures text is measured on are not looked at)
    if (!device || device->width() < 16 || device->height() < 16) return nullptr;
    std::lock_guard<std::mutex> hold(state().lock);
    Target& t = state().targets[device];
    if (t.open++ == 0) {
        t.texts.clear();
        t.kept = bitmap;
        t.frame = QRectF(0, 0, device->width(), device->height());
    }
    return device;
}

inline void end(const QPaintDevice* device) {
    std::lock_guard<std::mutex> hold(state().lock);
    auto& targets = state().targets;
    const auto it = targets.find(device);
    if (it == targets.end() || it->second.open <= 0) return;
    if (--it->second.open > 0) return;
    reportLocked(it->second);
    if (!it->second.kept) targets.erase(it);
}

inline void forget(const QPaintDevice* device) {
    if (!on()) return;
    std::lock_guard<std::mutex> hold(state().lock);
    auto& targets = state().targets;
    const auto it = targets.find(device);
    if (it != targets.end() && it->second.open == 0) targets.erase(it);
}

inline void text(const QPaintDevice* device, QPainter* p, const Font& font, const QString& s, const QString& drawn,
                 const RectF* layout, const PointF& at, int flags, StringTrimming trimming) {
    std::lock_guard<std::mutex> hold(state().lock);
    if (Target* t = targetLocked(device)) textLocked(*t, p, font, s, drawn, layout, at, flags, trimming);
}

inline void fill(const QPaintDevice* device, QPainter* p, const Brush* brush, const QPainterPath& shape, bool rectangle) {
    std::lock_guard<std::mutex> hold(state().lock);
    Target* t = targetLocked(device);
    if (!t || t->texts.empty() || !opaqueLocked(brush)) return;
    coverLocked(*t, p, shape, rectangle);
}

inline void fill(const QPaintDevice* device, QPainter* p, const Brush* brush, const QRectF& r) {
    QPainterPath shape;
    shape.addRect(r);
    fill(device, p, brush, shape, true);
}

inline void clear(const QPaintDevice* device, QPainter* p, const QColor& c) {
    if (c.alpha() != 255) return;
    std::lock_guard<std::mutex> hold(state().lock);
    Target* t = targetLocked(device);
    if (!t || t->texts.empty()) return;
    QPainterPath shape;
    shape.addRect(QRectF(p->window()));
    coverLocked(*t, p, shape, true);
}

// A picture drawn into dest (painter units) from source (its own pixels,
// null for all of it)
inline void image(const QPaintDevice* device, QPainter* p, const QImage& picture, const QRectF& dest,
                  const QRectF& source) {
    std::lock_guard<std::mutex> hold(state().lock);
    Target* t = targetLocked(device);
    if (!t) return;
    if (!t->texts.empty() && opaqueLocked(picture)) {
        QPainterPath shape;
        shape.addRect(dest);
        coverLocked(*t, p, shape, true);
    }
    const Target* from = targetLocked(static_cast<const QPaintDevice*>(&picture));
    if (!from || from == t || from->texts.empty()) return;
    const QTransform m = p->combinedTransform();
    const QRectF to = m.mapRect(dest);
    const QRectF part = source.isNull() ? QRectF(0, 0, picture.width(), picture.height()) : source;
    if (m.type() > QTransform::TxScale || std::abs(to.width() - part.width()) > 0.5 ||
        std::abs(to.height() - part.height()) > 0.5) {
        return;   // stretched: its words are not read off it
    }
    const QPointF by = to.topLeft() - part.topLeft();
    const QRectF within = clipOf(p, t->frame).intersected(to);
    for (const Text& x : from->texts) {
        Text c = x;
        c.where.translate(by);
        c.clip = c.clip.translated(by).intersected(within);
        for (Line& l : c.lines) {
            l.box.translate(by);
            l.ink.translate(by);
            l.seen = l.seen.translated(by).intersected(within);
        }
        if (std::any_of(c.lines.begin(), c.lines.end(), [](const Line& l) { return visible(l.seen); })) {
            t->texts.push_back(std::move(c));
        }
    }
}

}  // namespace textcheck

// --------------------------------------------------------------------------

class Graphics {
public:
    explicit Graphics(QPainter* p) : p_(p) { check_ = textcheck::begin(p_, false); }
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
        check_ = textcheck::begin(p_, false);
    }
    // On a picture of its own
    explicit Graphics(Image* image) {
        if (auto* bitmap = dynamic_cast<Bitmap*>(image)) {
            own_ = new QPainter(&bitmap->image());
            p_ = own_;
        }
        check_ = textcheck::begin(p_, true);
    }
    ~Graphics() {
        if (check_) textcheck::end(check_);
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
        if (check_) textcheck::clear(check_, p_, c.q());
        return Ok;
    }

    Status FillRectangle(const Brush* brush, const RectF& r) {
        p_->fillRect(r.q(), brush->q());
        if (check_) textcheck::fill(check_, p_, brush, r.q());
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
        if (check_) {
            QPainterPath shape;
            shape.addEllipse(r.q());
            textcheck::fill(check_, p_, brush, shape, false);
        }
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
        if (check_) textcheck::fill(check_, p_, brush, path->q(), false);
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
        if (check_) textcheck::image(check_, p_, img->q(), dest.q(), QRectF());
        return Ok;
    }
    // A part of the picture (in its own pixels) into a rectangle of the page
    Status DrawImage(Image* img, const RectF& dest, REAL sx, REAL sy, REAL sw, REAL sh, Unit,
                     const ImageAttributes* = nullptr) {
        p_->drawImage(dest.q(), img->q(), QRectF(sx, sy, sw, sh));
        if (check_) textcheck::image(check_, p_, img->q(), dest.q(), QRectF(sx, sy, sw, sh));
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
        int drawn = flags;
        if (format && format->trimming() != StringTrimmingNone && layout.Width > 0) {
            // GDI+ trims the last line that fits, not the first: a rectangle
            // with room for more lines than one wraps there and ends in the
            // ellipsis only where the text goes on past the last of them.
            // The lines are broken here, and Qt is not to break them again -
            // a line that just fits by its advance can be a hair too wide
            // for Qt's own layout, and would put its last word on a line of
            // its own
            const QFontMetricsF fm(font->q());
            const int lines = (flags & Qt::TextSingleLine)
                                  ? 1
                                  : std::max(1, (int)((box.height() + 0.01) / std::max<qreal>(1.0, fm.lineSpacing())));
            if (lines > 1) {
                shown = detail::wrapAndTrim(s, fm, box.width(), lines);
                drawn &= ~Qt::TextWordWrap;
            } else {
                shown = fm.elidedText(s, Qt::ElideRight, box.width());
            }
        }
        p_->drawText(box, drawn, shown);
        p_->restore();
        if (check_) {
            textcheck::text(check_, p_, *font, s, shown, &layout, PointF(), flags,
                            format ? format->trimming() : StringTrimmingNone);
        }
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
        if (check_) textcheck::text(check_, p_, *font, s, s, nullptr, at, Qt::AlignLeft | Qt::AlignTop, StringTrimmingNone);
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
    const QPaintDevice* check_ = nullptr;   // the picture the text check notes this Graphics' texts on
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
