#pragma once
// The app's own pictures of the rock panel, for a bug report. A player who
// finds a reading wrong says so minutes later, and by then the panel is gone;
// so Bedrock keeps a cut of it in the debug folder: one per rock, and one of
// a panel whose figures it could not read. Only the panel with its heading
// and its composition list is cut, never the rest of the screen - the chat
// and the names in it stay out.
//
// Two rings of twenty files, the newest taking the oldest's place:
// panel_<n>.png for Star Citizen 4.10's panels, panel5_<n>.png for 5.0's (the
// PTU tools read them by that name), each with a .txt of what it is. Written
// on every channel, ship and seat, whatever the recording switch says.
// Nothing of it leaves the PC but in the zip of a Feedback message with its
// debug folder attached, which carries the ten newest (DebugZip.h).

#include <opencv2/core.hpp>

#include <deque>
#include <string>

namespace SCMiner {

struct ScanPanel;

class PanelKeep {
public:
    static constexpr int RING = 20;                // files of each name
    static constexpr double UNREAD_AFTER = 5.0;    // seconds a panel stays unread, running, before it is kept
    static constexpr double UNREAD_EVERY = 60.0;   // seconds between two pictures of unread panels at least
    static constexpr double AWAY = 12.0;           // seconds without a panel after which one is another

    enum class Kept { None, Rock, Unread };

    // One look and what was read of it, at `seconds` (a steady clock):
    //   Rock    the panel read, and its mass more than one numeral off the
    //           mass of each of the last RING rocks kept - a slashed zero
    //           and an eight count as one numeral, the HUD's face mixes
    //           them up. A mass that begins with 0 is the panel of a rock
    //           the game is still scanning (000), no rock.
    //   Unread  the panel's labels found and its figures not, for
    //           UNREAD_AFTER seconds running; once while the panel stays
    //           (no gap of AWAY seconds), one in UNREAD_EVERY seconds at
    //           most, and not where its mass is a rock's kept already.
    // None for anything else, and where no label column places the cut.
    Kept look(const ScanPanel& panel, const cv::Size& frame, double seconds);

    // The part of the frame kept: the panel with its heading and its
    // composition list. Empty where no label column places it.
    static cv::Rect cut(const ScanPanel& panel, const cv::Size& frame);
    // The ring a panel goes to: "panel5" for 5.0's, "panel" for 4.10's
    static const char* ring(const ScanPanel& panel);
    // The mass as the look read it: the figure's own text where the reader
    // placed it, else the mass read; "" for none
    static std::string massOf(const ScanPanel& panel);
    // Whether two masses are one rock's (see Rock above)
    static bool sameRock(const std::string& a, const std::string& b);
    // The HUD a label column's shape is of: "MOLE turret", "Golem", "5.0"...
    static std::string hudOf(char labelGlyph);

    // What the .txt says besides what the panel tells
    struct About {
        std::string channel;     // LIVE, PTU, ...; "" when not known
        std::string ship;        // the ship the game's log names; "" when none
        bool seated = false;     // ...with the player in a seat
        bool toneMapped = false; // the frame came off an HDR desktop, tone mapped
    };
    // <dir>/<ring>_<n>.png and .txt for a look look() kept. `written`, when
    // given, gets the name without the folder (panel_7). False where the
    // picture could not be written.
    bool write(const std::string& dir, const cv::Mat& frame, const ScanPanel& panel, Kept kept, const About& about,
               std::string* written = nullptr);
    // The number of the next file of `ring` in `dir`, 1 to RING: the first
    // time the one after the newest <ring>_<n>.png there by its file's time,
    // 1 when there is none; after that the one after the last written
    int nextNumber(const std::string& dir, const std::string& ring);

private:
    std::deque<std::string> rocks_;   // the masses of the rocks kept, the newest last
    double seenAt_ = -1e9;            // the last look that saw a panel
    double unreadFrom_ = -1;          // since when the panel is seen unread, running; -1 not
    double unreadAt_ = -1e9;          // the last picture of an unread panel
    bool unreadKept_ = false;         // ...taken while the panel stays
    int lastPanel_ = 0, lastFive_ = 0;   // the number last written of each ring; 0 none yet
};

}  // namespace SCMiner
