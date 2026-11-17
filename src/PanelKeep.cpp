#include "PanelKeep.h"

#include "PanelFive.h"
#include "ScanPanel.h"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace SCMiner {

namespace {

// The 4.10 panel about its label column, in the column's units (its size
// against a 1,440-line frame), from the labels' left edge and the MASS
// label's top. Measured on three snapshots of the MOLE pilot seat at 5,120 x
// 1,440 (17 November 2026, LIVE 4.10.3, size 1.0): the SCAN RESULTS heading
// begins 60 above, the hook at the end of the rule under it 222 right, the
// figures end at 160, the volume at 212, and the five lines of the list end
// 248 down. Every seat sets the list alike (RockContents.cpp: from 26 left
// of the labels, as a turret's list leans, to 236 right, six lines in 300).
// Room round it of 15 above and 10 below, 40 left for the turret's lean.
constexpr double CUT_LEFT = -40.0;
constexpr double CUT_TOP = -75.0;
constexpr double CUT_RIGHT = 250.0;
constexpr double CUT_BOTTOM = 310.0;

bool isRockMass(const std::string& mass) {
    return !mass.empty() && mass[0] != '0' &&
           std::all_of(mass.begin(), mass.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

bool PanelKeep::sameRock(const std::string& a, const std::string& b) {
    // right-aligned, as numeralsOff counts them
    const size_t n = (std::max)(a.size(), b.size());
    int off = 0;
    for (size_t i = 0; i < n; i++) {
        const char x = i < a.size() ? a[a.size() - 1 - i] : 0;
        const char y = i < b.size() ? b[b.size() - 1 - i] : 0;
        const bool zeroEight = (x == '0' || x == '8') && (y == '0' || y == '8');
        if (x != y && !zeroEight) off++;
    }
    return off <= 1;
}

std::string PanelKeep::massOf(const ScanPanel& panel) {
    // 5.0's reader gives the text where it found the mass's word
    if (panel.labelGlyph == FIVE_PANEL || panel.figures[0].placed) return panel.figures[0].text;
    if (panel.found && panel.stats.mass > 0) return std::to_string((long long)std::llround(panel.stats.mass));
    return std::string();
}

const char* PanelKeep::ring(const ScanPanel& panel) {
    return panel.labelGlyph == FIVE_PANEL ? "panel5" : "panel";
}

std::string PanelKeep::hudOf(char labelGlyph) {
    // the label columns' shapes (PanelShapes.h) and the ships they are of
    // (LoadoutReader.cpp, panelShipOf)
    switch (labelGlyph) {
        case 'L': return "Prospector";
        case 'M': return "Prospector, long labels";
        case 'K': return "MOLE pilot seat";
        case 'P': return "MOLE pilot seat, long labels";
        case 'Q': return "MOLE pilot seat, 4.10.3 labels";
        case 'T': return "MOLE turret";
        case 'W': return "MOLE turret, long labels";
        case 'G': return "Golem";
        case 'H': return "Golem, long labels";
        case FIVE_PANEL: return "5.0";
        default: return "unknown";
    }
}

cv::Rect PanelKeep::cut(const ScanPanel& panel, const cv::Size& frame) {
    if (panel.labelGlyph == FIVE_PANEL) return fivePanelCut(panel, frame);
    if (panel.labels.x < 0 || panel.labelScale <= 0) return cv::Rect();
    const double s = panel.labelScale;
    auto X = [&](double u) { return (int)std::lround(panel.labels.x + u * s); };
    auto Y = [&](double u) { return (int)std::lround(panel.labels.y + u * s); };
    return cv::Rect(cv::Point(X(CUT_LEFT), Y(CUT_TOP)), cv::Point(X(CUT_RIGHT), Y(CUT_BOTTOM))) &
           cv::Rect(0, 0, frame.width, frame.height);
}

PanelKeep::Kept PanelKeep::look(const ScanPanel& panel, const cv::Size& frame, double seconds) {
    if (!panel.found && panel.labels.x < 0) return Kept::None;   // no panel; how long, seenAt_ tells
    // back after a while: another stay, as RockViewHold has it - nobody
    // scans a new rock inside twelve seconds
    if (seconds - seenAt_ >= AWAY) {
        unreadFrom_ = -1;
        unreadKept_ = false;
    }
    seenAt_ = seconds;
    const std::string mass = massOf(panel);
    const bool rock = isRockMass(mass);
    const bool kept = rock && std::any_of(rocks_.begin(), rocks_.end(),
                                          [&](const std::string& other) { return sameRock(mass, other); });
    if (panel.found) {
        unreadFrom_ = -1;
        if (!rock || kept || cut(panel, frame).area() <= 0) return Kept::None;
        rocks_.push_back(mass);
        while ((int)rocks_.size() > RING) rocks_.pop_front();
        return Kept::Rock;
    }
    // the labels, and no figures: the game's first look at a rock it is
    // still scanning is that for a second or two, and a MOLE turret's panel
    // most of the time between the looks the second reader reads
    if (unreadFrom_ < 0) unreadFrom_ = seconds;
    if (kept || unreadKept_ || seconds - unreadFrom_ < UNREAD_AFTER || seconds - unreadAt_ < UNREAD_EVERY ||
        cut(panel, frame).area() <= 0) {
        return Kept::None;
    }
    unreadKept_ = true;
    unreadAt_ = seconds;
    return Kept::Unread;
}

int PanelKeep::nextNumber(const std::string& dir, const std::string& ring) {
    int& last = ring == "panel5" ? lastFive_ : lastPanel_;
    if (last == 0) {
        // the newest file of the ring, as an earlier start left it
        std::filesystem::file_time_type when{};
        std::error_code ec;
        for (int n = 1; n <= RING; n++) {
            const std::filesystem::path file = std::filesystem::path(dir) / (ring + "_" + std::to_string(n) + ".png");
            if (!std::filesystem::exists(file, ec)) continue;
            const auto t = std::filesystem::last_write_time(file, ec);
            if (ec) continue;
            if (last == 0 || t > when) {
                last = n;
                when = t;
            }
        }
    }
    last = last % RING + 1;
    return last;
}

bool PanelKeep::write(const std::string& dir, const cv::Mat& frame, const ScanPanel& panel, Kept kept,
                      const About& about, std::string* written) {
    const cv::Rect part = cut(panel, frame.size());
    if (kept == Kept::None || part.area() <= 0) return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string name = std::string(ring(panel)) + "_" + std::to_string(nextNumber(dir, ring(panel)));
    const std::filesystem::path base = std::filesystem::path(dir) / name;
    bool saved = false;
    try {
        saved = cv::imwrite((base.string() + ".png"), frame(part));
    } catch (const cv::Exception&) {
        saved = false;
    }
    if (!saved) return false;
    // The first five lines as the PTU's panel5 files had them, which the
    // tools read; what it is after them
    std::ofstream out(base.string() + ".txt", std::ios::trunc);
    out << "frame " << frame.cols << "x" << frame.rows << "\ncut " << part.x << "," << part.y << " " << part.width << "x"
        << part.height << "\ntone mapped " << (about.toneMapped ? 1 : 0) << "\nlabels " << panel.labels.x << ","
        << panel.labels.y << " x" << panel.labelScale << "\nread " << panel.note << "\n";
    out << "kept " << (kept == Kept::Rock ? "rock " + massOf(panel) : std::string("unread")) << "\n";
    out << "panel " << (panel.labelGlyph == FIVE_PANEL ? std::string("5.0") : "4.10, " + hudOf(panel.labelGlyph)) << "\n";
    if (panel.found) {
        char figures[96];
        std::snprintf(figures, sizeof(figures), "%.0f kg, %.0f %%, %.2f, %.2f SCU", panel.stats.mass,
                      panel.stats.resistance, panel.stats.instability, panel.stats.scu);
        out << "figures " << figures << (panel.stats.ore.empty() ? "" : ", " + panel.stats.ore) << "\n";
    }
    out << "ship " << (about.ship.empty() ? "not named by the game's log" : about.ship)
        << (about.ship.empty() ? "" : about.seated ? ", seated" : ", not seated") << "\n";
    if (!about.channel.empty()) out << "channel " << about.channel << "\n";
    if (written) *written = name;
    return true;
}

}  // namespace SCMiner
