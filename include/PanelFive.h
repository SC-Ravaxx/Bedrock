#pragma once
// The rock panel as Star Citizen 5.0 draws it.
//
//     SCAN RESULTS
//     SAVRILIUM (ORE)
//     MASS:                        2322
//     RESISTANCE:          +25%     88%
//     INSTABILITY:         -35%  107.82
//     (          IMPOSSIBLE          )
//     COMPOSITION             15.46 SCU
//     [||||||||||||||||||||||||||||||||]
//     15.30%  SAVRILIUM (ORE)       580
//     38.80%  SAVRILIUM (ORE)       322
//      6.54%  GOLD (ORE)            553
//     39.34%  INERT MATERIALS         0
//
// 4.10's long panel has the same rows. What 5.0 changed: the figures stand
// right-aligned at the panel's right edge in a thinner face; the resistance
// and the instability carry the laser's modifier in front of them, and the
// figures themselves already have it in them; the colour of a figure says
// how severe it is, not whether a laser is in it; a rock under one SCU has
// its volume with an SI prefix (754.38m SCU is 0.75438 SCU).

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace SCMiner {

struct ScanPanel;
class TextLineReader;

// ScanPanel::labelGlyph of a 5.0 panel
inline constexpr char FIVE_PANEL = '5';

// A 5.0 panel on the screen. False when there is none, and the 4.10 readers
// then look as before. True when the panel was seen: `labels` and
// `labelScale` place it, and `found` says whether its figures were read.
bool readPanelFive(const cv::Mat& frame, ScanPanel& panel, TextLineReader* lines);

// The figures as the panel writes them: "2322", "88%", "+25%" (the laser's
// modifier), "107.82", and the volume "15.46" or "754.38m". False for
// anything else.
bool fiveWhole(const std::string& text, float& value);
bool fivePercent(const std::string& text, float& value);
bool fiveDelta(const std::string& text, float& value);
bool fiveTwoPlaces(const std::string& text, float& value);
bool fiveVolume(const std::string& text, float& scu);

// For the bench: the frame the next look is of (SCMINER_FIVE_HARVEST)
void fiveHarvestFile(const std::string& file);

// The composition list of a 5.0 panel: where it stands, and its lines as
// printed, top down, for readRockContents to make a RockContents of.
// A share or quality is given only where every glyph of it is sure.
struct FiveListLine {
    std::string shareText, qualityText;
    float share = 0;          // per cent
    int quality = -1;         // -1 not read; 0 the inert rest's
    bool shareSure = false, qualitySure = false;
    double bright = 0;        // the share's ink
    bool dim = false;         // drawn faint, as the inert rest's line is
    cv::Rect name;            // where its name stands
};
cv::Rect fiveContentsRegion(const ScanPanel& panel, const cv::Size& frame);
std::vector<FiveListLine> readFiveList(const cv::Mat& frame, const ScanPanel& panel);
bool fiveShare(const std::string& text, float& value);   // "15.30%"

}  // namespace SCMiner
