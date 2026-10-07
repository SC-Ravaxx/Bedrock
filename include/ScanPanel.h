#pragma once

#define NOMINMAX
#include <opencv2/opencv.hpp>
#include <array>
#include <deque>
#include <string>
#include <vector>

#include "MiningAdvice.h"
#include "OCREngine.h"
#include "GlyphReader.h"

#include <ostream>
#include <tuple>

namespace SCMiner {

class TextLineReader;

// The game's SCAN RESULTS panel: the rock's own figures, which is everything
// the break question needs about the rock.
//
// It is not at a fixed place on the screen. The panel is anchored to the rock
// in 3D and slides with the view, exactly the way the signature marker does -
// so it is SEARCHED for, not cropped. What it is searched by is structure,
// never colour: the HUD's colour follows the ship, and a MOLE's is nothing
// like the one in the test frame.
//
// The structure that gives it away is a pair of long horizontal rules - one
// under the SCAN RESULTS heading, one over the composition bar - of the same
// width, at the same left edge, a panel's height apart. No part of a rock, a
// planet or space draws two of those.
// Known floor: the search brings every frame to 1080 tall before looking,
// and below that the rules are thinner than a pixel and do not survive the
// capture. Measured on the test frame reduced to 960x540: two rules, no pair.
// Above 1080 it scales down with INTER_AREA and the rules hold.
// One figure as the shape reader saw it in one look: what it made of it, and
// for every numeral how near it came to each of 0 to 9 - which is what lets
// several looks be read as one (see PanelSteadier).
struct PanelFigure {
    std::string text;
    std::vector<std::array<float, 10>> likeness;
    bool placed = false;       // every glyph found, and the figure has its format
};

struct ScanPanel {
    bool found = false;
    cv::Rect box;              // in the coordinates of the picture passed in
    double confidence = 0;     // 0..1, how well the two rules agreed
    // The two rules the box was built from, in the same coordinates. The
    // figures are the rows BETWEEN them: the heading sits above the first, and
    // rows taken from the top of the box would count the heading as the ore's
    // name and shift everything down one.
    int headRule = 0;
    int bodyRule = 0;
    RockStats stats;           // what was read out of it; zeroes when nothing was
    // How many of the four figures came back - the ore, the mass, the
    // resistance, the instability. Below three this is not a panel, and the
    // caller must not treat it as one: "found" alone means a pair of rules
    // was seen, and a cockpit has plenty of those.
    int figuresRead = 0;
    std::string note;          // for the tools: why it failed, or what it read
    cv::Rect oreRow;           // where the ore's name was, when the face found it
    // The shape reader's own account of the mass, the resistance and the
    // instability, filled whenever it found the labels - also when a figure
    // was too close to call in this one look
    PanelFigure figures[3];
    cv::Point labels{-1, -1};  // the shape reader's label column: left edge, MASS label's top
    // A column that was SEEN and not trusted: under its bar, with figures
    // the shapes could not place, so nothing is learned from it and it is
    // not reported as a panel's labels. The line reader is given it all the
    // same - it proves a panel by the label word, the colon and the three
    // formats, which the shapes cannot do at a column they cannot read.
    cv::Point tryLabels{-1, -1};
    double tryScale = 0;
    char tryGlyph = 0;
    double labelScale = 0;     // ...and its size against a 1440-line frame
    int ink = 0;               // ...and the channel it was found in: 0 white lettering, 4 orange
    char labelGlyph = 0;       // ...and which label column's shape it matched
    // The panel matched clearly better at another size than the one it was
    // last read at for sure: it is growing into place, and its figures are
    // not to be trusted yet
    bool resizing = false;
    // COMP. and SCU were found where the labels put them, and pinned the size
    bool pinned = false;
};

// Looks at one panel, a second and a half apart, read as one.
//
// A single look cannot always tell this font's 0 from its 8 - at eleven
// pixels a dotted zero and an eight are two dark specks in a bright block
// either way - but the glyphs of a rock averaged over its frames separate
// cleanly (0.99 against 0.95). So every numeral's likeness to each of 0 to 9
// is summed over the looks, the older ones fading, and a figure is given out
// once each of its numerals is clear of the next likeliest. A look that is
// sure of a different numeral somewhere, or reads a figure of another shape,
// is another rock: the sums start again from it.
class PanelSteadier {
public:
    // One look. True when the figures are settled; `rock` then holds them.
    bool add(const ScanPanel& look, RockStats& rock);
    void clear();
    // ...and the size the figures were last settled at, for a new session
    void forget() {
        clear();
        settledScale_ = 0;
        offScale_ = 0;
        offLooks_ = 0;
    }
    int looks() const { return looks_; }

private:
    struct Figure {
        std::string shape;                         // "DDDD", "D%", "D.DD" - numerals as D
        std::string text;                          // the last look's reading
        std::vector<std::array<float, 10>> sums;
        int looks = 0;
    };
    Figure figures_[3];
    std::deque<float> scus_;
    std::deque<std::string> ores_;
    int looks_ = 0;
    int pinnedLooks_ = 0;          // looks in the sums whose place COMP. and SCU confirmed
    double settledScale_ = 0;      // the panel's size when its figures last settled
    double stateScale_ = 0;        // the size of the looks summed since the last clear
    double offScale_ = 0;          // another size looks keep finding it at...
    int offLooks_ = 0;             // ...and for how many looks in a row
};

// Every pair of rules in the frame that could be the panel, best-matched
// first. There is more than one: a cockpit is full of long horizontal lines,
// and a pair up in the power management readout can match at 99 % and hold
// nothing. Which one is the panel is settled by reading it, not by looking
// at it - see readScanPanel.
std::vector<ScanPanel> scanPanelCandidates(const cv::Mat& frame, int most = 6);

// The best-matched candidate, for the tools and the tests.
ScanPanel findScanPanel(const cv::Mat& frame);

// ...and reads the figures out of it. The rows are not at fixed offsets: the
// panel shows one line per thing it has to say, and it has a different number
// of them on different builds - the test frame spells INSTABILITY and puts a
// word for it underneath, the 4.10 client writes INST and does not.
// So the rows are FOUND inside the box and then taken in order, which is the
// order the game writes them in and has not changed: the ore, its mass, its
// resistance, its instability, and after the second rule the volume.
//
// `ocr` has to be an initialised engine; it is left as it was found.
// `lines`, when it is given and loaded, reads the figures where the shape
// reader found the labels and could not read what stands beside them - see
// readPanelByLines.
ScanPanel readScanPanel(const cv::Mat& frame, OCREngine& ocr, GlyphReader* glyphs = nullptr,
                        TextLineReader* lines = nullptr);

// Where the panel's four written lines sit - the ore's name, the mass, the
// resistance, the instability - label and figure together, as the shape
// reader's label column places them. Empty when no labels were found.
//
// Label AND figure, because the label is what makes a line of text safe to
// believe: a reader given the figure alone has nothing to check itself
// against, and "MASS: 173" read off a row that is not the mass row does not
// happen.
std::vector<cv::Rect> panelFigureLines(const ScanPanel& panel, const cv::Size& frame);

// The panel read as four lines of text, by the second reader.
//
// The shape reader knows the numerals of two fonts at the sizes it harvested
// them, and where it does not know them it stops: in the MOLE's turret it
// finds the labels and reads none of the figures. The turret's figures are
// not faint or small - they are the biggest on the screen - they are simply
// written in a face the shapes were not cut for.
//
// The neural reader has no such gap, and it is already in the exe for the
// signature markers. Given the label with the figure it reads the turret's
// three rows at 0.86 to 0.96 certainty, and the ore's name - which no shape
// reader reads and Tesseract is not always there for - at 0.92.
//
// It is the last reader tried, not the first: it costs 80 to 100 ms a line
// against the shapes' whole look, and the shapes are sure of the numerals
// they do know.
bool readPanelByLines(const cv::Mat& frame, TextLineReader& lines, ScanPanel& panel);

// Four lines that could be a panel - the ore's name, the mass, the
// resistance, the instability - found by their shape on the screen alone,
// best first.
//
// This is readFiguresIn's own test with the one thing taken out that the
// MOLE's turret fails at: what the figures READ. That reader asks for a
// whole number, a per cent and a number to two places before it will call
// three lines a panel, so a HUD whose numerals it cannot make out is a HUD
// it cannot find either - it finds the turret's panel in 4 of 765 looks and
// the pilot seat's in almost every one.
//
// What is left is structure: three lines under one another, an even pitch
// apart, of one height, their left edges together, each with a word and
// something after it. Structure finds them; the second reader says what
// they say; and the labels it reads back - MASS, RES, INST, each with its
// colon and its figure's format - are what proves them a panel.
struct FigureLines {
    std::array<cv::Rect, 4> rows;     // the ore's name, the mass, the resistance, the instability
    std::array<cv::Rect, 3> values;   // ...and the figure half of the three, for the colour test
    double off = 0;                   // how far from an even block: smaller is better, for ranking
                                      // one region's candidates against another's
};
std::vector<FigureLines> figureLineCandidates(const cv::Mat& frame, const cv::Rect& region, int most);

// ...and those four lines read. The rows are given, not worked out from a
// label column, so this serves both the shape reader's places and the ones
// found by shape on the screen.
// `values` is the figure half of the three rows: the resistance is printed
// green or red once the laser has had its say, and that has to be told from
// the label's own colour, so the test needs the figure alone.
bool readPanelRows(const cv::Mat& frame, TextLineReader& lines, const std::array<cv::Rect, 4>& rows,
                   const std::array<cv::Rect, 3>& values, ScanPanel& panel);

// Draws what the search saw onto a copy of the frame, for tuning against real
// screenshots: every rule it considered, and the box it settled on.
cv::Mat drawScanPanelSearch(const cv::Mat& frame, const ScanPanel& panel);

// One glyph cut out of a panel row: the shape, and where it sat.
//
// The panel is drawn in the HUD's own face, and that face has a SLASHED zero
// which general OCR reads as an 8 about as often as not - 12,560 as 12,568,
// 40 % as 48, 0 % as 8. No threshold mends a digit that is recognised as a
// different digit, so the digits get read against shapes harvested from this
// very font instead, the way the signature marker is read.
struct PanelGlyph {
    cv::Mat shape;     // PANEL_GLYPH_H x PANEL_GLYPH_W, CV_32F, 0..1
    cv::Rect box;      // where it sat in the row
};
inline constexpr int PANEL_GLYPH_W = 14;
inline constexpr int PANEL_GLYPH_H = 22;

// Cuts the glyphs out of one row of a panel, left to right. `row` is in the
// frame's own coordinates.
std::vector<PanelGlyph> cutPanelGlyphs(const cv::Mat& frame, const cv::Rect& row);

// Where the figures sit inside a found panel: the ore's name first, then the
// mass, the resistance and the instability, in the order the game writes
// them. Empty when the rows cannot be made out. Only the value half of each
// row is returned - the label is of no use to anything.
std::vector<cv::Rect> panelValueRows(const cv::Mat& frame, const ScanPanel& panel);

// The rules the search found, exposed for the tools and the tests. Each is a
// run of bright pixels one row tall.
struct PanelRule {
    int y = 0;
    int x0 = 0;
    int x1 = 0;
    int width() const { return x1 - x0; }
};
std::vector<PanelRule> findHorizontalRules(const cv::Mat& frame, int minWidth, int maxWidth);

// How far each pixel is from the colour of what lies around it, 0..255.
//
// The HUD draws in thin strokes, and a stroke is a different colour from
// whatever is right behind it - white on black space, white on a bright blue
// nebula, green or orange on a sunlit rock. A median over a window wider than
// a stroke takes the strokes out and leaves the ground; the distance to it is
// the writing. Brightness alone cannot do this: over a lit rock or a nebula
// the ground is as bright as the glyphs.
cv::Mat hudStrokeMap(const cv::Mat& bgr, int radius);

// The brightness structure of one panel and the ink in it, worked out over the
// panel alone. The search works out its level over the whole picture, and a
// sunlit rock or a bright nebula anywhere in it raises that level past the
// panel's own glyphs; over the panel's own few hundred pixels the glyphs are
// always the brightest thing there is.
struct PanelInk {
    cv::Rect area;        // the part of the frame both maps cover
    cv::Mat structure;    // CV_8U: top-hat of the brightest channel
    cv::Mat ink;          // CV_8U, 0 or 255
};
PanelInk panelInk(const cv::Mat& frame, const cv::Rect& box);

// One glyph of the HUD's own face, as found in a row.
inline constexpr int FACE_W = 12;
inline constexpr int FACE_H = 18;
struct FaceGlyph {
    cv::Rect box;         // frame coordinates
    cv::Mat shape;        // FACE_H x FACE_W, CV_32F, zero mean, unit length
    bool stop = false;    // a full stop: far shorter than the numerals beside it
};

// The glyphs of one row, left to right. Pieces that stand over one another
// are one glyph - the per cent sign is three - and specks that do not share
// the numerals' line are left out.
std::vector<FaceGlyph> faceGlyphs(const PanelInk& ink, const cv::Rect& row);

// The same row as words - runs of glyphs with no wide gap - each measured by
// its own height, since a label and its figure need not share a size.
std::vector<std::vector<FaceGlyph>> faceWords(const PanelInk& ink, const cv::Rect& row);

// One row read in the face: digits, '.', '%', and '?' for a glyph that is not
// clearly any of them.
std::string readFaceRow(const PanelInk& ink, const cv::Rect& row);

// The mass, the resistance and the instability in the face, found anywhere in
// a region - all three or nothing. The formats are the gate: three lines in a
// row whose figures read as a whole number, a number with a per cent sign and
// a number to two places, standing under one another with a label before
// each. Nothing else in the HUD has that; the composition list below the
// figures pairs its rules as neatly as the figures do, and never has it.
bool readFiguresIn(const cv::Mat& frame, const cv::Rect& region, ScanPanel& panel);

// The panel read by its shapes: the label column found as a picture, the
// figures matched glyph by glyph where the labels say they are.
//
// `found` is set when the labels were found and every figure is in its place
// and format; the return value says whether this one look was also sure of
// every numeral.
bool readPanelByShape(const cv::Mat& frame, ScanPanel& panel);

// Where the last panel stood: its label column's place, size, shape and ink.
//
// For the glance. A panel comes back where it went away - in the pilot seat
// the column stays within thirty pixels of one place - so the app looks there
// on every scan, at the one shape and size, for a few milliseconds, and looks
// in full only when the glance says the column is back. The full look with
// nothing on screen costs a tenth of a second, so it runs only every second
// and a half; without the glance that interval is most of the wait for a
// rock.
struct PanelSpot {
    bool known = false;
    cv::Point labels{-1, -1};  // the column's left edge and the MASS label's top, frame coordinates
    double scale = 0;          // its size against a 1440-line frame
    char glyph = 0;            // which label column's shape
    int ink = 0;               // the channel the column stands out in (channelGrey)
};
PanelSpot lastPanelSpot();
// ...and one brought back from the last session, until a panel is read
void setPanelSpot(const PanelSpot& spot);
// The ink the next look tries first - the spot's, when the glance asks for
// a look: with nothing found the inks are tried by turns, and a look asked
// for by a glance at an orange column could go by in white
void preferPanelInk(int ink);
// The part of the frame a glance needs: the column, with room about it for
// the HUD's sway. Empty when no panel has been seen.
cv::Rect panelSpotWindow(const PanelSpot& spot, const cv::Size& frame);
// How well a column like the last panel's matches in that window - `window`
// is the frame cut to panelSpotWindow - and the score from which a full look
// is worth its cost: the column's bar less what the reading may make up.
double glanceAtPanelSpot(const PanelSpot& spot, const cv::Mat& window);
double glanceBar(const PanelSpot& spot);

// An ore's name as Tesseract read it, snapped to the nearest mineral the game
// has and written as the app writes it ("Ice"); empty when nothing is near
// enough.
std::string snapOreName(const std::string& read);

// ...and the same name read off its row by the second reader, snapped the
// same way. The download carries no Tesseract data, so on a fresh install
// that reader is the only one there is for the ore's name; this one is in
// the exe. The last name is kept while the row looks unchanged.
std::string oreByLine(const cv::Mat& frame, const cv::Rect& row, TextLineReader& lines);

// For the app, which looks every second and a half and mostly at no panel:
// the search over every size is then tried only every fourth look.
void throttlePanelSearch(bool throttled);

// For the tools: where the label column is - the labels' left edge and the
// MASS label's top - at what size, and how well it matched.
bool findPanelLabels(const cv::Mat& frame, cv::Point& labels, double& scale, double& score);

// A shape of the label-column kind (PanelShapes.h) looked for over a region
// of the frame in one of the panel reader's inks (0 darkest, 5 whiteness,
// ...): where its first word's left edge and top stand, at which size, and
// how well it matched (-1: nowhere to look). For the loadout reader, whose
// laser table is such a column too.
struct ShapeFound {
    cv::Point at;
    double scale = 0;
    double score = -1;
};
struct PanelShape;
ShapeFound findShapeIn(const cv::Mat& frame, const cv::Rect& region, int ink, const PanelShape& shape,
                       const std::vector<double>& scales);

// For the tools: every label column's shape looked for in every ink, with the
// best score, place and size of each
void probePanelInks(const cv::Mat& frame, std::ostream& out);

// For the bench: what each stage of a look cost since the last report, per
// look (SCMINER_PANELSTAGES=1)
void reportPanelStages(std::ostream& out, int looks);

// For the tools: a label column's averaged shape grown from one cut (see the
// definition); `found` lists file, score, size, x, y of every frame taken in
cv::Mat bootstrapLabelShape(const cv::Mat& seed, const std::vector<std::string>& files, int ink, double minScore,
                            std::vector<std::tuple<std::string, double, double, int, int>>& found, int passes);

// For the tools: the three figures as the shape reader places them, and a
// grey cut of every glyph it placed (numerals and the per cent sign, not the
// full stops) at the shapes' own size - each with its neighbours a pixel or
// `jitter` pixels off, the unmoved one in the middle.
bool shapeSamplesOf(const cv::Mat& frame, std::string texts[3], std::vector<std::vector<cv::Mat>> patches[3],
                    int jitter);

// Where the figures may be: around every pair of rules and every long rule.
std::vector<cv::Rect> figureRegions(const cv::Mat& frame);

// For the tools: every line of a region as the face reader cuts it, each word
// with what it read and the shape of every glyph (empty for a full stop).
struct FaceTokenInfo {
    std::string text;
    cv::Rect box;
    std::vector<cv::Mat> shapes;
    std::vector<cv::Rect> boxes;
};
struct FaceLineInfo {
    cv::Rect rect;
    std::vector<FaceTokenInfo> tokens;
};
std::vector<FaceLineInfo> faceLinesDetailed(const cv::Mat& frame, const cv::Rect& region);

// For the tools: every text line of a region and what the face makes of it.
std::vector<std::pair<cv::Rect, std::string>> faceLinesIn(const cv::Mat& frame, const cv::Rect& region);

}  // namespace SCMiner
