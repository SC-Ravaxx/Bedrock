// Does MinerGUI's drawing code run on QPainter unchanged?
//
// Everything between the two marker comments below is lifted WORD FOR WORD out
// of MinerGUI.cpp - the palette, the ore colours, addRoundRect, fillRoundRect,
// strokeRoundRect, hairline, and the block that draws a probable-match row.
// Not adapted, not rewritten: copied, and compiled against GdiplusShim.h
// instead of gdiplus.h.
//
// The figures are not invented either: the demo asks the real
// SignatureAnalyzer what a reading stands for and draws what comes back, bars
// and all. If the picture looks like Bedrock, the shim carries the real
// drawing code.
#include "GdiplusShim.h"
#include "SignatureAnalyzer.h"
#include "MiningData.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <iostream>
#include <string>

// ===================== lifted from MinerGUI.cpp, verbatim =================

const Gdiplus::Color COL_BG(21, 22, 24);
const Gdiplus::Color COL_CARD(29, 31, 35);        // a surface
const Gdiplus::Color COL_CARD_HI(38, 41, 46);     // a raised surface
const Gdiplus::Color COL_BORDER(48, 51, 57);      // the hairline
const Gdiplus::Color COL_TEXT(236, 236, 234);
const Gdiplus::Color COL_TEXT_DIM(158, 162, 170);
const Gdiplus::Color COL_TEXT_FAINT(104, 109, 118);
const Gdiplus::Color COL_ACCENT(226, 170, 62);
const Gdiplus::Color COL_GREEN(108, 191, 122);
const Gdiplus::Color COL_RED(220, 86, 80);

void addRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void fillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                   const Gdiplus::Color& fill) {
    Gdiplus::GraphicsPath path;
    addRoundRect(path, r, radius);
    Gdiplus::SolidBrush brush(fill);
    g.FillPath(&brush, &path);
}

void strokeRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                     const Gdiplus::Color& stroke, float width = 1.0f) {
    Gdiplus::GraphicsPath path;
    addRoundRect(path, r, radius);
    Gdiplus::Pen pen(stroke, width);
    g.DrawPath(&pen, &path);
}

void hairline(Gdiplus::Graphics& g, float x1, float y, float x2) {
    Gdiplus::Pen pen(COL_BORDER, 1.0f);
    g.DrawLine(&pen, x1, y, x2, y);
}

Gdiplus::Color getMineralColor(const std::string& mineral) {
    auto it = SCMiner::MINERAL_COLORS.find(mineral);
    if (it != SCMiner::MINERAL_COLORS.end()) {
        uint32_t color = it->second;
        return Gdiplus::Color(
            (color >> 24) & 0xFF,
            (color >> 16) & 0xFF,
            (color >> 8) & 0xFF,
            color & 0xFF
        );
    }
    return Gdiplus::Color(200, 200, 200);  // default: gray
}

// ===================== end of the lifted code =============================

namespace {

std::wstring toW(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// This is the match row out of paintMain, with the members it reads passed in
// rather than taken off the object - the drawing itself is untouched.
void drawMatchRow(Gdiplus::Graphics& g, const SCMiner::AnalysisResult& result,
                  const Gdiplus::RectF& row, bool best, bool dimmed,
                  const Gdiplus::Font& nameFont, float M, float rowW, float rowH, float y) {
    fillRoundRect(g, row, 6, COL_CARD);

    Gdiplus::Color chipColor = getMineralColor(result.name);
    const float fillW = rowW * (float)(result.probability / 100.0);
    if (fillW > 4) {
        Gdiplus::RectF fillRect(M, y, (std::max)(fillW, 12.0f), rowH);
        Gdiplus::Color c1, c2;
        if (dimmed) {
            c1 = Gdiplus::Color(32, 158, 162, 170);
            c2 = Gdiplus::Color(12, 158, 162, 170);
        } else if (best) {
            c1 = Gdiplus::Color(105, 108, 191, 122);
            c2 = Gdiplus::Color(40, 108, 191, 122);
        } else {
            c1 = Gdiplus::Color(70, chipColor.GetR(), chipColor.GetG(), chipColor.GetB());
            c2 = Gdiplus::Color(22, chipColor.GetR(), chipColor.GetG(), chipColor.GetB());
        }
        Gdiplus::LinearGradientBrush grad(fillRect, c1, c2, Gdiplus::LinearGradientModeHorizontal);
        Gdiplus::GraphicsPath path;
        addRoundRect(path, fillRect, 6);
        g.FillPath(&grad, &path);
    }

    strokeRoundRect(g, row, 6, best ? COL_GREEN : COL_BORDER, best ? 1.5f : 1.0f);

    if (dimmed) chipColor = Gdiplus::Color(120, chipColor.GetR(), chipColor.GetG(), chipColor.GetB());
    Gdiplus::SolidBrush chipBrush(chipColor);
    g.FillEllipse(&chipBrush, M + 12, y + rowH / 2 - 5, 10, 10);

    std::wstring label = toW(result.name);
    if (result.clusterSize > 1) label += L"  ×" + std::to_wstring(result.clusterSize);
    Gdiplus::SolidBrush nameBrush(dimmed ? COL_TEXT_FAINT : COL_TEXT);
    g.DrawString(label.c_str(), -1, &nameFont, Gdiplus::PointF(M + 30, y + 5), &nameBrush);
}

void drawWindow(Gdiplus::Graphics& g, float W, float H, int signature,
                const std::vector<SCMiner::AnalysisResult>& results) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.Clear(COL_BG);

    Gdiplus::FontFamily text(L"DejaVu Sans");
    Gdiplus::Font wordmark(&text, 25, Gdiplus::FontStyleBold);
    Gdiplus::Font by(&text, 12);
    Gdiplus::Font sub(&text, 12);
    Gdiplus::Font label(&text, 11, Gdiplus::FontStyleBold);
    Gdiplus::Font button(&text, 12);
    Gdiplus::Font value(&text, 30, Gdiplus::FontStyleBold);
    Gdiplus::Font nameFont(&text, 15);
    Gdiplus::Font smallFont(&text, 12);

    Gdiplus::SolidBrush white(COL_TEXT);
    Gdiplus::SolidBrush dim(COL_TEXT_DIM);
    Gdiplus::SolidBrush faint(COL_TEXT_FAINT);
    Gdiplus::SolidBrush amber(COL_ACCENT);
    Gdiplus::SolidBrush green(COL_GREEN);
    Gdiplus::SolidBrush red(COL_RED);

    const float M = 20;

    // --- the header, the way the main window carries it
    g.FillRectangle(&amber, Gdiplus::RectF(M, 24, 9, 9));
    g.DrawString(L"Bedrock", -1, &wordmark, Gdiplus::PointF(M + 18, 12), &white);
    // Where "by Ravaxx" goes depends on how wide the wordmark came out, and
    // that differs between GDI+ and Qt - so it is measured, not guessed. This
    // is the whole of what porting the layout amounts to: 61 of these.
    Gdiplus::RectF markSize;
    g.MeasureString(L"Bedrock", -1, &wordmark, Gdiplus::PointF(0, 0), &markSize);
    g.DrawString(L"by Ravaxx", -1, &by, Gdiplus::PointF(M + 24 + markSize.Width, 22), &faint);
    g.DrawString(L"Mining companion  ·  Star Citizen 4.10", -1, &sub,
                 Gdiplus::PointF(M + 18, 42), &faint);

    // the buttons along the top right
    Gdiplus::StringFormat centre;
    centre.SetAlignment(Gdiplus::StringAlignmentCenter);
    centre.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.FillEllipse(&red, Gdiplus::RectF(W - 248, 26, 6, 6));
    const struct { const wchar_t* text; float w; bool live; } top[] = {
        {L"LIVE ON", 62, true}, {L"KEYS", 48, false}, {L"OVERLAY", 66, false}, {L"?", 22, false}};
    float tx = W - 236;
    for (const auto& b : top) {
        const Gdiplus::RectF r(tx, 18, b.w, 24);
        fillRoundRect(g, r, 3, COL_CARD_HI);
        strokeRoundRect(g, r, 3, b.live ? COL_GREEN : COL_BORDER);
        g.DrawString(b.text, -1, &smallFont, r, &centre, b.live ? &green : &dim);
        tx += b.w + 6;
    }

    // --- the notice, amber rule down its left edge
    const Gdiplus::RectF notice(M, 62, W - 2 * M, 38);
    fillRoundRect(g, notice, 3, COL_CARD_HI);
    g.FillRectangle(&amber, Gdiplus::RectF(M, 62, 3, 38));
    g.DrawString(L"STAR CITIZEN MUST RUN IN BORDERLESS WINDOW MODE", -1, &label,
                 Gdiplus::PointF(M + 14, 68), &amber);
    g.DrawString(L"Options › Graphics › Window Mode. Fullscreen leaves the capture black.",
                 -1, &smallFont, Gdiplus::PointF(M + 14, 82), &dim);

    hairline(g, M, 112, W - M);

    // --- what to watch for
    g.DrawString(L"I'M LOOKING FOR", -1, &label, Gdiplus::PointF(M, 122), &faint);
    const Gdiplus::RectF drop(M, 138, W - 2 * M, 30);
    fillRoundRect(g, drop, 4, COL_CARD);
    strokeRoundRect(g, drop, 4, COL_BORDER);
    g.DrawString(L"nothing yet - pick the ores that should blink", -1, &nameFont,
                 Gdiplus::PointF(M + 12, drop.Y + 6), &faint);
    Gdiplus::PointF chevron[3] = {Gdiplus::PointF(W - M - 24, drop.Y + 12),
                                  Gdiplus::PointF(W - M - 18, drop.Y + 18),
                                  Gdiplus::PointF(W - M - 12, drop.Y + 12)};
    Gdiplus::Pen chevronPen(COL_TEXT_DIM, 1.6f);
    chevronPen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawLines(&chevronPen, chevron, 3);

    // --- the four ways in
    const wchar_t* tabs[] = {L"REFINERY", L"ORE FINDER", L"BLUEPRINTS", L"NOTES"};
    const float bw = (W - 2 * M - 3 * 8) / 4;
    float bx = M;
    for (const wchar_t* caption : tabs) {
        const Gdiplus::RectF r(bx, 180, bw, 30);
        fillRoundRect(g, r, 4, COL_CARD_HI);
        strokeRoundRect(g, r, 4, COL_BORDER);
        g.DrawString(caption, -1, &button, r, &centre, &dim);
        bx += bw + 8;
    }

    // --- the reading
    const Gdiplus::RectF panel(M, 226, W - 2 * M, 78);
    fillRoundRect(g, panel, 6, COL_CARD);
    strokeRoundRect(g, panel, 6, COL_BORDER);
    g.DrawString(L"SCANNED SIGNATURE", -1, &label, Gdiplus::PointF(M + 14, 238), &faint);
    wchar_t shown[32];
    std::swprintf(shown, 32, L"%d,%03d", signature / 1000, signature % 1000);
    g.DrawString(shown, -1, &value, Gdiplus::PointF(M + 14, 258), &white);
    g.DrawString(L"pin 93%", -1, &smallFont, Gdiplus::PointF(M + 150, 272), &green);

    // --- and what it stands for, bars and all
    g.DrawString(L"PROBABLE MATCHES", -1, &label, Gdiplus::PointF(M, 320), &amber);

    const float rowW = W - 2 * M;
    const float rowH = 40;
    float y = 342;
    int count = 0;
    for (const auto& result : results) {
        if (count >= 5) break;
        const bool dimmed = !result.inSelectedSystem;
        const bool best = (count == 0 && !dimmed);
        drawMatchRow(g, result, Gdiplus::RectF(M, y, rowW, rowH), best, dimmed,
                     nameFont, M, rowW, rowH, y);

        // the probability, right-aligned - as the app puts it
        Gdiplus::StringFormat right;
        right.SetAlignment(Gdiplus::StringAlignmentFar);
        right.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        wchar_t pct[24];
        std::swprintf(pct, 24, L"%.0f%%", result.probability);
        g.DrawString(pct, -1, &smallFont, Gdiplus::RectF(M, y, rowW - 14, rowH), &right,
                     best ? &green : &faint);

        y += rowH + 6;
        count++;
    }

    // --- the foot
    hairline(g, M, H - 44, W - M);
    const Gdiplus::RectF backup(M, H - 34, 176, 24);
    fillRoundRect(g, backup, 4, COL_CARD_HI);
    strokeRoundRect(g, backup, 4, COL_BORDER);
    g.DrawString(L"BACKUP / RESTORE DATA", -1, &label, backup, &centre, &dim);
    Gdiplus::StringFormat rightFoot;
    rightFoot.SetAlignment(Gdiplus::StringAlignmentFar);
    g.DrawString(L"Like the app? Support the dev:   ko-fi.com/ravaxx", -1, &smallFont,
                 Gdiplus::RectF(M, H - 28, W - 2 * M, 20), &rightFoot, &faint);
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    // A real reading, run through the real analyzer. 12,680 is a good one to
    // draw: three ores of different likelihood, which is what the bars are
    // there for - the best one carries a green fill and a green frame, the
    // others are shaded in their own ore colour.
    const int signature = argc > 2 ? std::atoi(argv[2]) : 12680;
    SCMiner::SignatureAnalyzer analyzer;
    std::vector<SCMiner::AnalysisResult> results = analyzer.analyzeSignature(signature);
    if (results.size() > 5) results.resize(5);
    std::cout << signature << " -> " << results.size() << " match(es) from the analyzer\n";
    for (const auto& r : results) {
        std::cout << "   " << r.name << "  x" << r.clusterSize << "  " << r.probability << "%\n";
    }

    const int W = 600, H = 600;
    QImage canvas(W, H, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&canvas);
    Gdiplus::Graphics g(&painter);
    drawWindow(g, W, H, signature, results);
    painter.end();

    const char* out = argc > 1 ? argv[1] : "shim_card.png";
    if (!canvas.save(out)) {
        std::cerr << "could not write " << out << "\n";
        return 1;
    }
    std::cout << "wrote " << out << "  (" << W << "x" << H << ")\n";
    return 0;
}
