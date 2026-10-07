#pragma once
// What a refinery's REFINEMENT CENTER shows, read off the screen.
//
// Placing a work order writes nothing to Game.log: an order set up, quoted
// and confirmed leaves not a line. The terminal itself shows everything a
// tracker needs, in a layout of fixed panels:
//
//   WORK ORDER                                  <- a light grey bar over every panel
//   SETUP                       WORK ORDER 0    <- a band: dark red while it is set up
//   01 // RAW MATERIALS   IN MANIFEST 720   TO REFINE 484
//   02 // PROCESSING SELECTION, YIELD AND COSTS
//   PYROMETRIC CHROMALYSIS                     <- the method
//   MATERIALS SELECTED   QUALITY  QTY  YIELD  REFINE
//   IRON (ORE)               874  484    235   [on]
//   TOTAL COST 194.00 aUEC     PROCESSING TIME 7m 22s
//
//   WORK ORDER
//   PROCESSING                  WORK ORDER 1    <- amber once it runs
//   MATERIALS YIELDED (CSCU) QUALITY  YIELD  TO DO  DONE
//   IRON                         874    235    228     7
//   YIELD 7.77 cSCU            TIME REMAINING 7m 7s
//
// The panels stand side by side, each under its own bar, and the bar's
// width is the panel's: everything in it sits at a fixed place measured in
// that width. So the bars are found first - cheaply, which is all that runs
// while the player is merely near a refinery - and only then is anything
// read, field by field, with the second reader (TextLineReader).

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace SCMiner {

class TextLineReader;

// One panel's header bar, and what the band beneath it says it is
struct RefineryBar {
    // Where the panel stands, in the measures the layout is kept in: Levski's
    // grey bar over the panel, 446 wide in 1440 lines. Pyro's terminals have
    // no such bar; their hatched block is turned into the bar it stands for.
    cv::Rect bar;            // in the picture's pixels
    enum class Band { None, Setup, Processing, Completed, Other } band = Band::None;
    // The terminal's dress: Levski's (a grey bar with a coloured band under
    // it) or Pyro's rest stops' (a hatched block, red or orange, at the
    // panel's top left) - the same panels, the same fields, nearly the same
    // places
    enum class Theme { Levski, Pyro } theme = Theme::Levski;
    cv::Scalar bandColour;   // BGR, for the tools
};

// The cheap look: the work-order bars in a picture, left to right. Only bars
// with a SETUP or PROCESSING band beneath them count; empty when none.
std::vector<RefineryBar> findRefineryBars(const cv::Mat& bgr, bool everyBar = false);

struct RefineryRow {
    std::string material;    // as the refinery tables spell it ("Iron (Ore)"); "" when not made out
    std::string text;        // as the reader gave it
    bool inert = false;
    int quality = -1;
    int quantity = -1;       // SETUP: cSCU in the hold; -1 in PROCESSING
    int yield = -1;          // cSCU that comes out; -1 for "--"
    int toDo = -1, done = -1;   // PROCESSING
    // Whether a SETUP row is refined is not read off its switch: once the
    // order is quoted, a row has a yield exactly when it is
};

struct RefineryPanel {
    RefineryBar::Band kind = RefineryBar::Band::None;
    cv::Rect bar;
    int order = -1;          // "WORK ORDER n"
    std::string method;      // SETUP: as REFINING_METHODS names it; "" when not made out
    std::string methodText;
    int manifest = -1, toRefine = -1;   // SETUP, cSCU
    double cost = -1;        // SETUP: aUEC
    int seconds = -1;        // SETUP: processing time; PROCESSING: time remaining
    double yieldSoFar = -1;  // PROCESSING: cSCU
    std::vector<RefineryRow> rows;
    std::string note;        // for the tools: what was not made out
    // Every field the panel's kind has was read and makes sense
    bool whole() const;
};

struct RefineryScreen {
    std::vector<RefineryPanel> panels;   // left to right
    // The station's name over the panels, as read - "LEVSKI". The log does
    // not always say where the player is (logged in at Levski it gives "Nyx"
    // and no place), and the terminal names its station itself.
    std::string stationText;
    // "WORK ORDER DELIVERED" on the terminal (looked for only when asked): an
    // order was just collected, and its panel is gone
    bool delivered = false;
    double ms = 0;
};

// Reads every work-order panel the bars point at - and with notice, whether
// "WORK ORDER DELIVERED" stands anywhere on the picture, panels or none
RefineryScreen readRefineryScreen(const cv::Mat& bgr, const std::vector<RefineryBar>& bars, TextLineReader& reader,
                                  bool notice = false);
// ...that notice alone: its green line of lettering found, then read
bool findDeliveredNotice(const cv::Mat& bgr, TextLineReader& reader);

// The pieces, for the self-test
// "7m 22s" -> 442, "1h 2m" -> 3720, "45s" -> 45; -1 for anything else
int parseRefineryTime(const std::string& text);
// "194.00" -> 194.0; -1 when no number
double parseRefineryAmount(const std::string& text);
// "874" -> 874 with O/I/S/B put right; -1 for "--" and anything else
int parseRefineryCount(const std::string& text);
// The method a read names, as REFINING_METHODS spells it; "" when none is near
std::string refiningMethodIn(const std::string& text);
// The material a read names, as REFINERY_MATERIALS spells it ("IRON (ORE)" and
// the refined "IRON" both give "Iron (Ore)"); "" when none is near
std::string refineryMaterialIn(const std::string& text, bool* inert = nullptr);
// Whether a quality is one a material comes in: one of its ore's eight
// values (ORE_BANDS) - the terminal prints the raw ore's, refined or not.
// True for a material the bands do not know (salvage, rubble).
bool refineryQualityFits(const std::string& material, int quality);

}  // namespace SCMiner
