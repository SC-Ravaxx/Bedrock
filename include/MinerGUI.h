#pragma once

#define NOMINMAX
#include <Windows.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011   // Windows 10 2004: shown, but in no capture of the screen
#endif
#include <array>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <chrono>
#include <gdiplus.h>
#include "MiningData.h"
#include "MiningAdvice.h"
#include "RockContents.h"
#include "RefineryOrders.h"
#include "TradeBook.h"
#include "UexPrices.h"
#include "UexMarket.h"
#include "NoteBook.h"
#include "Reputation.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

namespace SCMiner {

// What a window looks like under frosted glass, kept from one paint to
// the next: blurring is the dear part of painting glass, and most paints
// (the pointer moving over a sheet, the caret, the live pulse) change
// nothing under it. `key` is a fingerprint of the page a blur was made from;
// a page that comes out the same pixel for pixel is not blurred again. Two
// of them, because the page under glass often goes back and forth between
// two looks - the live pulse, a caret - and each would otherwise throw the
// other one's blur away. Given back as soon as no glass is up, and in
// ~MinerGUI while GDI+ still stands.
struct FrostCache {
    struct Slot {
        std::unique_ptr<Gdiplus::Bitmap> blur;
        std::unique_ptr<Gdiplus::TextureBrush> brush;   // the blur as paint, made when a pane needs it
        std::uint64_t key = 0;
        int w = 0, h = 0;
    };
    Slot slot[2];
    int last = 0;   // the slot used last; the other one is the one to reuse
    void clear() {
        for (Slot& s : slot) {
            s.brush.reset();
            s.blur.reset();
            s.key = 0;
            s.w = s.h = 0;
        }
    }
};

// Forward declaration
class MinerGUI;

// Global instance for the window procedure
inline MinerGUI* g_guiInstance = nullptr;

class MinerGUI {
public:
    MinerGUI();
    ~MinerGUI();

    // Creates the window
    bool initialize(HINSTANCE hInstance);

    // Shows the window
    void show();

    // Hides the window
    void hide();

    // Updates the display with new analysis results.
    // confidence: OCR confidence in percent, or < 0 for manual input.
    // `outOfPlace` says the place the log names explains none of these -
    // then they are shown as they are rather than greyed out of the way,
    // with a word about it: the tables name the ores a place is known for,
    // and a marker can hold one they do not list.
    void updateResults(int signature, const std::vector<AnalysisResult>& results,
                       double confidence = -1.0, bool outOfPlace = false);

    // The game's scan panel is up, and this is what it says. The window swaps
    // to the rock view for as long as it stays up.
    void updateRock(const RockStats& rock);
    // ...and it is gone. Back to the signature view, without anyone clicking.
    void clearRock();
    bool rockInView() const { return rockInView_; }
    // What that rock is made of, as the panel lists it under its figures
    // (RockContents.h): each part's share and its quality. Shown with the
    // rock and gone with it; a list without parts takes the last one off.
    void setRockContents(const RockContents& contents);
    // What the player bought and sold at the commodity terminals, the oldest
    // first (TradeBook): the Ore Finder lists an ore's own under its places
    void setTrades(std::vector<Trade> trades);
    // The work orders the refinery's terminal showed (RefineryOrders), the
    // soonest done first, for the Refinery window's own view of them; and
    // who is told when one is ticked off as collected
    void setRefineryOrders(std::vector<RefineryOrder> orders);
    void setRefineryOrderDoneCallback(std::function<void(const RefineryOrder&)> done) {
        onRefineryOrderDone_ = std::move(done);
    }
    // UEX's prices for the Ore Finder (UexPrices): whether it is asked at all
    // (uex= in region_config.txt), what it said, when the price list was
    // fetched (Unix seconds), and what went wrong the last time it was asked
    // ("" when nothing did). Null prices while nothing has come yet.
    void setUexPrices(bool enabled, std::shared_ptr<const UexPrices> prices, long long fetchedAt,
                      const std::string& note);
    // Whether the Ore Finder is open: UEX is only asked while it is
    bool plannerOpen() const { return planner_ != nullptr && plannerVisible_; }
    // The UEX marketplace's listings of an ore (UexMarket.h), and what went
    // wrong asking; null listings while nothing has come
    void setUexMarket(const std::string& ore, std::shared_ptr<const std::vector<UexListing>> listings,
                      long long fetchedAt, const std::string& note);
    // The ore the Ore Finder's UEX tab shows the marketplace for; "" while it shows none
    std::string uexMarketWanted() const;
    // The player's own conversations there (read with their key): a listing
    // they are talking about says so, and opens at their thread
    void setUexThreads(std::shared_ptr<const UexThreads> threads);
    // Reporting to UEX, as the Ore Finder's Report to UEX shows it
    // (Application::pushUexAccount)
    struct UexAccount {
        bool on = false;
        long long since = 0;         // when the player agreed, Unix seconds
        std::string keyEnd;          // the key's last four characters; "" without a key
        bool keyTrouble = false;     // UEX turned the key away
        std::string accountTrouble;  // ...or the account: "datarunner", "blocked", "unverified" (UexReports)
        std::string note;            // what came of the last report or press
        long long noteAt = 0;
        std::string user;            // the data runner's name UEX answered with
        bool hasToken = false;       // the build carries Bedrock's app token: without it nothing is reported
        int keyChanges = 0;          // counts every key kept, so a card waiting for one sees it come
        std::wstring lastShot;       // the screenshot that went to UEX last (debug folder); "" if none
    };
    void setUexAccount(const UexAccount& account);
    // What was pressed: 1 a key pasted (the clipboard's text), 2 the key
    // forgotten, 3 agreed - reporting on, 4 declined or switched off, 5 the
    // first start's offer made (whatever came of it)
    using UexAccountCallback = std::function<void(int action, const std::string& text)>;
    void setUexAccountCallback(UexAccountCallback callback) { onUexAccount_ = std::move(callback); }
    // At the first start, UEX offered over the main window, once it is free -
    // no other card, no news window (the application calls tickUexOffer each
    // tick)
    void offerUex() { uexOfferPending_ = true; }
    void tickUexOffer();

    // What the ship carries. Set once; the rock view can do nothing without it.
    void setLoadout(const MiningLoadout& fit) { loadout_ = fit; }
    void setFittedGadget(const MiningGadget* gadget) { fittedGadget_ = gadget; }
    // One of the player's ships as they fit it: a name of their own, the
    // kind of ship, its heads. A kind can have several fits, such as one
    // Prospector for heavy rocks and one for fiddly ones.
    struct ShipFit {
        std::string name;                        // never empty once read: the ship's own by default
        const MiningShip* ship = nullptr;
        std::vector<MiningLoadout> heads;
        // The game's ids of the hulls it was flown in, newest last (see
        // GameState::hull): sitting down in one of them picks this fit
        std::vector<std::string> hulls;
        bool last = false;                       // the one of its kind flown last
        // Its hull was destroyed with the player in it: the next hull of its
        // kind that no fit knows is taken for the claim that replaced it
        bool lost = false;
        // What a crafted laser this ship carried before prints on itself:
        // put aside when the ship's HUD shows another laser fitted, back on
        // the head when it shows that one again (fitTakes)
        std::vector<std::pair<const MiningLaser*, float>> crafted;
    };
    static constexpr int MAX_FITS = 12;
    static constexpr int MAX_HULLS = 8;           // per fit: a claim makes a new id every so often
    static constexpr int MAX_CRAFTED_KEPT = 4;    // per fit: lasers put aside with a figure of their own
    // loadout.txt, read and written without a window or a file - public for
    // the self-test. Each [ship] section is one fit. The files of before
    // read as fits named after their ship: one section per kind of ship,
    // and before that one ship's heads with no section at all.
    struct LoadoutFile {
        const MiningShip* ship = nullptr;        // ship=: the chosen fit's kind, all an older Bedrock reads
        std::string fit;                         // fit=: the chosen fit, by its name
        const MiningGadget* gadget = nullptr;
        std::vector<ShipFit> fits;
    };
    static LoadoutFile readLoadoutFile(std::istream& in);
    static void writeLoadoutFile(std::ostream& out, const LoadoutFile& file);
    // Trims heads to what a ship can fly: its number of hardpoints, heads
    // that fit them, no more modules than slots
    static void fitHeads(const MiningShip& ship, std::vector<MiningLoadout>& heads);
    // `wanted`, or - when another fit (not `except`) has that name - it
    // with " 2", " 3" ... after it
    static std::string uniqueFitName(const std::vector<ShipFit>& fits, const std::string& wanted, int except = -1);
    // A hull destroyed with the player in it: taken off the fit that flew it,
    // which is marked lost. The fit's index, or -1 when no fit knew the hull.
    static int fitLosingHull(std::vector<ShipFit>& fits, const std::string& hull);
    // A hull no fit knows, of `kind`: when exactly one fit of that kind is
    // lost, it is the replacement - given to that fit, which is lost no more.
    // The fit's index, or -1 (known already, no lost fit, or two of them).
    static int takeReplacement(std::vector<ShipFit>& fits, const MiningShip* kind, const std::string& hull);
    // The fit of `kind` with this laser and these modules on its first head:
    // `current` when that is one, else the one of them flown last. -1 when
    // no fit has them.
    static int fitForSeen(const std::vector<ShipFit>& fits, int current, const MiningShip* kind,
                          const MiningLaser* laser, const std::vector<const MiningModule*>& modules);
    // The fit of `kind` that is the ship flown, where none has what its HUD
    // shows: the one the hull the log names belongs to; else the one fit of
    // that kind there is. -1 for a ship of its own - no fit of the kind yet,
    // or a hull none of several has flown - and FIT_UNSURE where there are
    // several and the log has not named the hull yet. It is not guessed: a
    // wrong guess overwrites the other ship's modules, and its hull, once
    // named, goes to the fit the guess made.
    static constexpr int FIT_UNSURE = -2;
    static int fitToAdapt(const std::vector<ShipFit>& fits, int current, const MiningShip* kind,
                          const std::string& hull, const MiningLaser* laser,
                          const std::vector<const MiningModule*>& modules);
    // The fit of `kind` a hull belongs to, or -1
    static int fitOfHull(const std::vector<ShipFit>& fits, const MiningShip* kind, const std::string& hull);
    // How many fits of `kind` have this laser and these modules on their
    // first head
    static int fitsWithSeen(const std::vector<ShipFit>& fits, const MiningShip* kind, const MiningLaser* laser,
                            const std::vector<const MiningModule*>& modules);
    // Whether a ship of this kind can carry that: its own laser where one is
    // built in, a head its mount takes, no more modules than the head has
    // slots - and one head, the only kind of ship whose HUD shows its own
    static bool kindCarries(const MiningShip& kind, const MiningLaser& laser,
                            const std::vector<const MiningModule*>& modules);
    // ...and a fit given what the HUD shows: the laser and the modules on its
    // one head. A crafted laser's bonus stays with its laser - put aside
    // when another is fitted, back when that one returns. False, and
    // nothing changed, when a ship of its kind cannot carry that.
    static bool fitTakes(ShipFit& fit, const MiningLaser& laser, const std::vector<const MiningModule*>& modules);
    // A laser put on a head - by the HUD or picked on the card: the figure a
    // crafted one prints on itself goes aside with the laser that leaves,
    // into `kept`, and comes back with one that returns
    static void putLaser(std::vector<std::pair<const MiningLaser*, float>>& kept, MiningLoadout& head,
                         const MiningLaser* laser);
    // --screenshot only: put the window into a state worth a picture
    void pickShipForShot(const char* id) {
        if (const MiningShip* kind = shipById(id)) {
            pickShipKind(*kind, true);
            everRead_ = true;
        }
    }
    // ...a second fit of a kind, under a name of its own
    void addFitForShot(const char* id, const char* name) {
        const MiningShip* kind = shipById(id);
        const int added = kind ? addFit(*kind) : -1;
        if (added < 0) return;
        fits_[added].name = uniqueFitName(fits_, name, added);
        pickFit(added, true);
    }
    // ...and the title being typed over, as a new fit opens its card
    void renameFitForShot(const wchar_t* typed) {
        nameEditing_ = true;
        nameEntry_ = typed;
    }
    void stopRenameForShot() { endNameEditing(false); }   // ...and let go, the name as it was
    void showLoadoutForShot(bool open) { showLoadout(open); }
    // Types into the crafted-% field the way a keyboard does: real key
    // messages posted to the window, so they travel the whole path the
    // shortcuts and IsDialogMessage sit on. WM_CHAR delivered straight to
    // the procedure would skip that path.
    void typeCraftedForShot(const wchar_t* text) {
        craftedEditing_ = true;
        SetFocus(hwnd_);
        for (const wchar_t* c = text; *c; c++) {
            const UINT vk = (*c == L'.') ? VK_OEM_PERIOD : (UINT)*c;
            const LPARAM lp = 1 | ((LPARAM)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
            PostMessageW(hwnd_, WM_KEYDOWN, vk, lp);
            PostMessageW(hwnd_, WM_KEYUP, vk, lp | 0xC0000000);
        }
    }
    float craftedForShot() const { return loadout_.craftedPower; }
    // Back to the state a new install opens in: the borderless notice still
    // up, nothing read yet.
    void firstRunForShot() {
        everRead_ = false;
        layoutControls();
    }
    void showShipPickerForShot(bool open) { setShipPickerOpen(open); }
    void showPlacePickerForShot(bool open) { setPlacePickerOpen(open); }
    void setHeadForShot(int index, const MiningLoadout& fit) {
        if (index >= 0 && index < (int)heads_.size()) heads_[index] = fit;
        if (index == headShown_) loadout_ = fit;
    }

    // Sets the current star system
    void setCurrentSystem(const std::string& system);
    // The ship Game.log names, shown in the header; "" clears it. hull: which
    // one of its kind, when the log has said (GameState::hull), "" when not
    void setShip(const std::string& label, bool inSeat, const std::string& hull = "");
    // A hull the log says was destroyed with the player in it (GameState::lostHulls)
    void shipLost(const std::string& hull);
    // What the mining HUD of a one-seat ship shows fitted (LoadoutReader):
    // the fit of that kind with this laser and these modules is chosen, and
    // the hull the log names is its own from now on. Where no fit has them
    // and `own` says the reading is sure enough to change one, the ship
    // flown takes them for its own (fitToAdapt, fitTakes): a module swapped
    // on the ship is swapped here.
    // The fit's name - empty, and nothing changed, when none has them and
    // none may or can take them; *changed says a fit was given them.
    std::string setSeenLoadout(const MiningShip& kind, const MiningLaser& laser,
                               const std::vector<const MiningModule*>& modules, bool own = false,
                               bool* changed = nullptr);
    // Where Game.log puts the player ("Pyro - Monox"), shown in the header
    // as "You are at: ...", and the ore location the results are narrowed
    // to ("Pyro II (Monox)", "" when they are not) for the "not at" notes
    // The two rules that keep the quality slider's marks reachable - the
    // ends of the track and 500, where the item comes out exactly as the
    // shop version. Free-hand only; a snapped slider lands on the eight
    // values the material is mined at instead. Public because the
    // self-test asks them directly: they are arithmetic, and arithmetic
    // that lives in a window procedure never gets checked.
    //
    // magnetQuality: what a pointer `reach` quality-units from a mark reads
    // as. stepQuality: where one press of an arrow key lands, which is
    // never PAST a mark it would cross.
    static int magnetQuality(int quality, int qMin, int qMax, int reach);
    static int stepQuality(int quality, int qMin, int qMax, bool up, bool big);

    // `inSpace`: the ship is in open space, where no ground deposit can be -
    // the note on such a row says so rather than "not in NYX"
    void setPlace(const std::string& line, const std::string& oreLocation, bool inSpace = false);

    // Pumps Windows messages (non-blocking); returns false once the window closed
    bool processMessages();

    // Checks whether the window is still open
    bool isOpen() const;

    // Live mode status (updates the toggle button and the status pill)
    void setLiveModeActive(bool active);
    // At a commodity terminal with a picture owed to UEX: the overlay is kept
    // out of the pictures meanwhile, as while the scan runs
    void setTradeWatch(bool on);

    // Callback when the system selection changes
    using SystemChangedCallback = std::function<void(const std::string&)>;
    void setSystemChangedCallback(SystemChangedCallback callback);

    // The place, set by hand. "" hands it back to the game log. The log is
    // right nearly always and wrong in one way that matters: a hop to
    // something its map cannot name - a marker, a party member, one's own
    // parked ship - leaves the old place standing, and the ore filter with
    // it.
    using PlaceOverrideCallback = std::function<void(const std::string&)>;
    void setPlaceOverrideCallback(PlaceOverrideCallback callback) { onPlaceOverride_ = std::move(callback); }
    void setPlaceOverride(const std::string& placeName) {
        placeOverride_ = placeName;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    }
    const std::string& placeOverride() const { return placeOverride_; }

    // The box in the Blueprints window that puts Bedrock's marks into the
    // game's own contract texts (GameTexts), and what the last write said
    using GameTextsCallback = std::function<void(bool on)>;
    void setGameTextsCallback(GameTextsCallback callback) { onGameTexts_ = std::move(callback); }
    void setGameTexts(bool on, const std::string& note) {
        gameTextsOn_ = on;
        gameTextsNote_ = note;
        if (crafting_) InvalidateRect(crafting_, nullptr, TRUE);
    }

    // Callback when live mode is toggled
    using LiveModeCallback = std::function<void(bool)>;
    void setLiveModeCallback(LiveModeCallback callback);

    // Callback for manual analysis
    using ManualAnalyzeCallback = std::function<void(int)>;
    void setManualAnalyzeCallback(ManualAnalyzeCallback callback);

    // Callback for a single scan request (SCAN button / S key)
    using ScanRequestedCallback = std::function<void()>;
    void setScanRequestedCallback(ScanRequestedCallback callback);

    // Callback when auto-tracking is toggled
    using TrackModeCallback = std::function<void(bool)>;
    void setTrackModeCallback(TrackModeCallback callback);

    // Reports whether tracking is enabled and whether the last scan actually
    // found the label, plus where it sits (virtual screen coordinates)
    void setTrackingStatus(bool enabled, bool locked, int x, int y);

    // Callback for the global "keep the recording" hotkey (Ctrl+Alt+K).
    // Registered system-wide on purpose: it has to be reachable from inside
    // the game, which is the only moment anyone knows the last minute is
    // worth keeping.
    using KeepRecordingCallback = std::function<void()>;
    void setKeepRecordingCallback(KeepRecordingCallback callback) {
        keepRecordingCallback_ = callback;
    }
    // Gives the system-wide Ctrl+Alt+K back (no recording, nothing to keep)
    void releaseKeepHotkey();
    // Whether the system-wide keys are actually held right now. A failed
    // registration is tried again (ensureHotkeys), since another app may hold
    // Ctrl+Alt+S only for a moment; this says how it stands so a report can
    // carry it.
    bool snapshotHotkeyHeld() const { return snapshotHotkey_; }
    void ensureHotkeys();

    // Callback for the global snapshot hotkey (Ctrl+Alt+S): the game picture
    // as the scan sees it, for a report from a machine that reads nothing
    using SnapshotCallback = std::function<void()>;
    void setSnapshotCallback(SnapshotCallback callback) { snapshotCallback_ = callback; }

    // The player's blueprint library: one stamp per blueprint ("" = not
    // owned) and a note on where that knowledge comes from
    void setOwnedBlueprints(std::vector<std::string> stamps, const std::string& note);
    // The player's rank with each outfit, out of their own logs (see
    // Reputation), for the Grind Planner. `known` false while the logs are
    // still being read, or with none found: nothing is marked open or locked
    // then. `fromVersion` is the game the counting starts at, LIVE's last wipe.
    void setRanks(std::vector<Standing> standings, bool known, int fromVersion);
    // A rank picked in the planner: outfit, ladder, rank; rank -1 goes back
    // to what the logs say
    using RankSetCallback = std::function<void(int, int, int)>;
    void setRankSetCallback(RankSetCallback callback) { rankSetCallback_ = callback; }
    void grindOpenOnlyForShot(bool on);   // --screenshot only: "Open to me" pressed
    // The contracts of the moment, off the game's log: the ones taken and not
    // over - the player's own, or shared with them - and the ones of their
    // own finished lately. A gathering order among the running ones (Shubin's
    // "Purchase Order: Ship Mined Ore") puts its ores among the ones to look
    // for; the Grind Planner marks a mission taken, and says when one just
    // finished, which its outfit blocks for a while, is back.
    struct ContractNow {
        const RepContract* contract = nullptr;
        bool running = false;
        bool shared = false;
        long long finishedAt = 0;   // of one finished: seconds since 1970, UTC
    };
    void setContractsNow(std::vector<ContractNow> list);
    // The clock moving on under such a wait: the planner is drawn again
    void tickContracts();
    // A mining gadget the log saw stuck on a rock - of the kind it names,
    // "MineableRock_AsteroidLegendary_Savrilium", that many seconds ago - or,
    // with null, taken off again. See GadgetOnRock.
    void setGadgetPlaced(const MiningGadget* gadget, const std::string& rockClass, long long ageSeconds);
    // What the updater is doing, pushed in from the Application. 0 idle,
    // 1 checking, 2 up to date, 3 available, 4 downloading, 5 ready,
    // 6 failed - the same order as Updater::State.
    void setUpdateStatus(int state, const std::string& version, const std::string& note,
                         int percent);
    using UpdateActionCallback = std::function<void()>;
    void setUpdateActionCallback(UpdateActionCallback cb) { updateAction_ = cb; }
    // --screenshot only: where the overlay's update chip ended up, so the
    // harness can press it. Whether a click reaches it is the whole point
    // of it - this mode drags the window from anywhere else.
    Gdiplus::RectF updateChipForShot() const { return updateHit_; }

    // Puts up the "what's new" card - once, after an update, decided by the
    // Application (see maybeShowWhatsNew). Not a question the window can
    // answer: it does not know which version was last seen.
    void showWhatsNew();
    void scrollWhatsNewForShot(float by);   // --screenshot only: part way down
    // The news, a window of its own, once after an update: 2.5 on its first
    // page, 2.0 a page on - see showNews
    void showNews();
    void showNewsForShot();
    void newsPageForShot(int page);   // --screenshot only: the other page
    void hideNewsForShot();
    HWND newsHandle() const { return news_; }
    void paintNewsTo(HDC hdc);
    // A blueprint ticked or unticked by hand (index, now owned)
    using OwnedToggleCallback = std::function<void(int, bool)>;
    void setOwnedToggleCallback(OwnedToggleCallback callback) { ownedToggleCallback_ = callback; }
    // A backup was put back: what the Application holds of the player's
    // data is read again from the files
    using DataRestoredCallback = std::function<void()>;
    void setDataRestoredCallback(DataRestoredCallback callback) { dataRestoredCallback_ = callback; }

    // Callback when the capture region is applied
    using RegionChangedCallback = std::function<void(int, int, int, int)>;
    void setRegionChangedCallback(RegionChangedCallback callback) { regionChangedCallback_ = callback; }
    void setRegionValues(int x, int y, int w, int h);
    // AUTO button: the box follows the game's viewport again
    using RegionAutoCallback = std::function<void()>;
    void setRegionAutoCallback(RegionAutoCallback callback) { regionAutoCallback_ = callback; }
    // Whether the box shown is the automatic one (label on the card)
    void setRegionAuto(bool automatic);

    // Compact always-on-top overlay mode (for use while the game is running)
    void toggleOverlayMode();
    bool isOverlayMode() const { return overlayMode_; }
    // The overlay out of screen captures while live mode is on or a trade's
    // picture is owed (setTradeWatch), in them otherwise
    void keepOutOfCaptures();
    // A rock's ores in the overlay: one to a row (true, the overlay two rows
    // taller) or all on one line; kept in window.txt
    void setOverlayOreRows(bool rows);
    bool overlayOreRows() const { return overlayOreRows_; }
    // Shows, places and names the overlay's switch for it
    void placeOreRowsButton();
    // The overlay's size after what it shows (MinerGUI.cpp, keepOverlayHeight)
    void measureOverlay();
    void fitOverlay();
    int overlayMaxWidth() const;
    int overlayMaxHeight() const;
    void wantWidth(float right) { if (measuring_ && right > fitWantW_) fitWantW_ = right; }
    void wantHeight(float bottom) { if (measuring_ && bottom > fitWantH_) fitWantH_ = bottom; }

    // The ore planner: a second window to pick an ore BEFORE the trip and see
    // where its rocks are (F, or the ORE FINDER button)
    void togglePlanner();

    // The refinery table: tick what is in the hold and the stations sort
    // themselves by what they pay for it (R, or the REFINERY button)
    void toggleRefinery();
    // Paints the Ore Finder into any DC - the headless screenshot uses it,
    // because DWM does not always compose a window parked off every monitor
    void paintPlannerTo(HDC hdc);

    // The Blueprints window: every crafting recipe in the build, and for a
    // picked material what it goes into. Mining and crafting are one trip -
    // the ore in the hold is only worth what it builds.
    void toggleCrafting();
    // The grind planner: which reward set is worth working through, which
    // mission feeds it, and how many runs it takes. Opened from the button in
    // the Blueprints window.
    void toggleGrind();

    // The notebook: folders and notes of your own, in their own window (N)
    // What a note points at. Nothing is marked up: the names of ores, mining
    // places and recipes are recognised in the text as it stands.
    struct NoteLink {
        std::string name;   // as the table spells it
        int kind = 4;       // 0 ore, 1 blueprint, 2 place, 3 system
        int index = -1;     // into CRAFT_MATERIALS or CRAFT_BLUEPRINTS
        size_t at = 0;      // where it sits in the note, in characters
        size_t len = 0;
    };
    // Pure, and the reason both of these are testable:
    // what the writer might mean by what they have just typed...
    static std::vector<NoteLink> suggestionsAt(const std::wstring& text, int caret);

    // "" when every [[link]] in the text still names something this build
    // knows; otherwise a sentence saying which one does not. An import is
    // refused on that, because a link that names nothing is not a link any
    // more - it is a pair of brackets in the middle of a sentence.
    static std::wstring brokenLink(const std::wstring& text);

    // A row can name more than one thing: "Hephaestanite + FPS Minable x6"
    // is one find with two deposits in it. Picking Hephaestanite has to
    // light that up as surely as a row that says nothing else, so the name
    // is taken apart and each piece asked on its own - with the count and
    // the ground family's surface wording taken back off.
    static bool anyPart(const std::string& name,
                        const std::function<bool(const std::string&)>& wanted);
    // ...and the links they went on to confirm, which the note carries as
    // [[Name]] - written by the suggestion, never typed by hand
    static std::vector<NoteLink> bracketLinksIn(const std::wstring& text);
    // The note as the file wants it: every line break as CR+LF. The edit
    // control hands over lone CRs, a file from elsewhere may hold lone LFs.
    static std::wstring toFileText(const std::wstring& s);

    // A window may never ask for more room than the screen has. Pure.
    // scale(640) at 175 % is 1120 px, which a 1080p screen cannot show: the
    // bottom edge, borders and all, would end up under the taskbar.
    static int fitToWork(int want, int work) { return (work > 0 && want > work) ? work : want; }

    // The three groups the filter list is written in. A heading is a filter
    // of its own: every type under it at once.
    struct TypeGroup { const wchar_t* title; const char* const* names; int count; };
    static const TypeGroup* typeGroups();      // three of them
    static bool typeInGroup(int type, int group);
    // Which ores a system actually has, so the finder's list can hold
    // only those when one is picked
    static bool oreInSystem(const std::string& mineral, const std::string& system);
    static bool familyInSystem(int family, const std::string& system);

    // The zoom: read from the settings at start, changed with
    // Ctrl+plus and Ctrl+minus, and handed back so it can be saved.
    void setUiScale(int percent);
    int uiScale() const { return uiScale_; }
    using UiScaleCallback = std::function<void(int)>;
    void setUiScaleCallback(UiScaleCallback callback) { uiScaleCallback_ = callback; }

    void toggleNotes();
    void paintNotesTo(HDC hdc);
    // --screenshot only: opens the first note there is, so the picture
    // shows the thing rather than an empty pane
    void openOreForShot(const char* ore = "Laranite");     // --screenshot only: an ore worth looking at
    void openPlaceForShot();     // ...and the same list turned round
    void suggestForShot();       // ...and the suggestion list, which only
    HWND suggestHandle() const { return notesSuggest_; }   // lives while typing
    // --screenshot only: `typed` added at the end of the open note, and the
    // list its last letters bring up, under the caret as while typing
    void suggestInNoteForShot(const std::wstring& typed);
    bool typingForShot_ = false;   // ...while it does: noteText() reads the editor
    // ...the list moved under the end of the note as the hidden run draws
    // it (its editor is not on screen, the text is drawn instead), and the
    // highlighted name taken, the way Enter takes it
    void placeSuggestForShot();
    void acceptSuggestForShot();
    Gdiplus::PointF noteEndShown_{0.0f, 0.0f};   // where the drawn note's last line ends
    void openNoteForShot();
    // --screenshot only: a picture put on the open note's media rail,
    // so the one part of the notebook that needs content to be worth
    // looking at has some
    void addMediaForShot(const std::wstring& file);
    void foldMediaForShot(bool open);   // ...and the rail folded away
    void startNamingForShot();
    void dragNoteForShot(bool on);   // ...and a note frozen in mid-drag
    void cartForShot();              // ...and a shopping list with things on it
    void cartOnlyForShot(const char* blueprint);   // ...or with one recipe, and no window opened
    void widestFilterForShot();      // ...and the filter bar at its most crowded,
                                     // outfits unfolded and all
    void narrowByContractForShot();  // ...and the list cut to one system and one outfit   // --screenshot only: the naming box open
    void openTrashForShot();     // ...and the trash unfolded, with a note in it
    HWND notesHandle() const { return notes_; }
    void paintCraftingTo(HDC hdc);
    void paintGrindTo(HDC hdc);
    HWND craftingHandle() const { return crafting_; }
    HWND grindHandle() const { return grind_; }
    HWND shotHandle() const { return shot_; }
    void paintShotTo(HDC hdc);
    void openShotForShot();      // --screenshot only: the first picture, opened
    // --screenshot only: opens a recipe so the slot view can be photographed
    void openBlueprintForShot();
    void showComponentsForShot();
    void openContractForShot(bool unfold = true);   // the contracts that drop a recipe; the first one opened, or not
    // With a contract card open, a real click where another contract's row
    // lies under the card: what the card is left showing, -1 if it closed.
    // The shot run holds the card to that - see paintContractCard.
    int clickUnderCardForShot();
    // A named recipe opened with the card of its first contract whose title
    // starts so and runs in `system`: false when there is none
    bool openCardForShot(const std::string& blueprint, const std::string& titleStart, const std::string& system);
    // How many recipes the Blueprints window's search keeps for `query`
    int craftSearchCountForShot(const std::wstring& query);
    void openGrindForShot();     // ...and the grind planner, with a set unfolded
    void grindSearchForShot(const std::wstring& query);   // the finder filled in, for a picture
    // The planner opened on a name - a recipe's, from the Blueprints window
    void openGrindFor(const std::wstring& query);
    // ...or on the one set a contract pays out of, from its card
    void openGrindForSet(int source);
    // ...or on a set by its place in poolGroups(), named after `source`
    void openGrindForGroup(int group, int source);
    void grindWarningForShot(bool show);   // ...and the notice over it
    void openRecoilBlueprintForShot();

    // Headless: every window stays outside all monitors, nothing is
    // activated - for --screenshot, which photographs the windows from their
    // own surfaces without ever showing them on the desktop
    void setHeadless(bool on) { headless_ = on; }
    HWND windowHandle() const { return hwnd_; }
    HWND plannerHandle() const { return planner_; }
    HWND refineryHandle() const { return refinery_; }
    void openAboutForShot();     // ...and the ? card, which has no key
    void openBackupForShot();    // ...and the backup card, with a line in it
    void paintRefineryTo(HDC hdc);
    void pickForShot();
    void showRefineryOrdersForShot(bool on);   // ...and its work orders' view
    void openLookingForShot();   // --screenshot only: the ore pick, open
    void setPlanPulseForShot(bool on);   // ...and the Ore Finder's blink held on or off
    void openUexCardForShot(int step);  // ...and its UEX card: 0 shut, 1 the account, 3 report prices?, 2 the pictures
    void openUexWelcomeForShot(int step);   // ...and the first start's offer over the main window, the same steps
    // ...and its UEX tab on an ore - or on a ground family, at one of its
    // gems; nullptr leaves it
    void openUexTabForShot(const char* ore, const char* gem = nullptr);
    void setLookingOpenForShot(bool open);   // ...and shut again
    void setPulseForShot(bool on);           // ...the red blink, held on (and the timer off)
    // ...and an ore ticked in "I'm looking for", or not
    void lookForShot(const char* ore, bool on) {
        if (lookingForOre(ore) != on) toggleLookingFor(ore);
    }
    void importForShot(const std::wstring& file);   // ...an import, refused or not, for real
    void openMethodForShot();                // ...the refining method dropped down
    void openCartPickForShot(int kind);      // ...the station (30) or method (31) picker over the list
    // Shot only: opens one amount for typing and feeds it digits the way
    // a click and a keyboard do - by finding the field the window itself
    // last drew and sending the messages there. The picture then proves
    // the real path rather than a variable somebody set.
    bool typeAmountForShot(int material, const wchar_t* digits);          // ...and a hold with something in it

private:
    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    bool isOpen_ = false;
    bool liveModeActive_ = false;
    bool tradeWatch_ = false;
    bool pulseOn_ = false;      // Blinking state of the LIVE indicator
    bool overlayMode_ = false;  // compact topmost overlay
    bool overlayOreRows_ = true;   // a rock's ores one to a row in the overlay
    // ...its size: what the unseen paint wanted, and since when it wants less
    static constexpr int FIT_SHRINK_MS = 2500;
    bool measuring_ = false;
    bool fitPosted_ = false;
    float fitWantW_ = 0.0f;
    float fitWantH_ = 0.0f;
    std::chrono::steady_clock::time_point fitSmallerSince_{};
    bool headless_ = false;     // see setHeadless
    RECT restoreRect_ = {};     // window rect to restore when leaving overlay mode
    // Where the overlay itself was put last, in screen coordinates - kept in
    // window.txt, so it reopens where it was dragged to rather than at the
    // full window's corner.
    POINT overlayPlace_ = {};
    bool overlayPlaceKnown_ = false;
    void keepOverlayPlace();    // overlayPlace_ <- where the overlay is now
    bool overlayPlaceOnScreen(const POINT& at, int width) const;
    // ...the overlay whole on the screen its top edge is on, and its height
    // kept to whether there is a ship to name (see overlayHeight)
    POINT overlayOnMonitor(POINT at, int width, int height) const;
    void keepOverlayHeight();

    // GDI+ token
    ULONG_PTR gdiplusToken_ = 0;

    // DPI scaling
    UINT dpi_ = 96;
    // A zoom of the app's own, on top of whatever Windows says the DPI is.
    // 100 means "as designed"; on a 4K screen at 100 % system scaling that is
    // half the size it should be, which is why this exists.
    int uiScale_ = 100;
    int scale(int value) const { return MulDiv(value, (int)dpi_ * uiScale_, 9600); }
    float scaleF(float value) const { return value * dpi_ * uiScale_ / 9600.0f; }

    // Cached GDI objects (created once, freed in the destructor)
    HBRUSH editBrush_ = nullptr;
    HFONT controlFont_ = nullptr;
    void createFonts();

    // Current data
    // Empty until Game.log names one: there is no way to pick a system by
    // hand, so assuming Stanton would mark half the ores "not in STANTON" on
    // the strength of nothing.
    std::string currentSystem_;
    int currentSignature_ = 0;
    double currentConfidence_ = -1.0;   // < 0 = manual input / unknown
    std::vector<AnalysisResult> currentResults_;

    // --- the rock in front of you -----------------------------------------
    // When the game's scan panel is on screen, the window shows the rock
    // instead of the signature: they answer different questions and are never
    // both the question at once. Reading a signature asks "is this worth
    // stopping for"; having a rock scanned asks "can I break it". The moment
    // the panel goes away the window goes back, so nothing has to be dismissed
    // by hand.
    bool rockInView_ = false;
    RockStats rock_;
    RockContents rockContents_;   // what it holds; no parts until its list was read
    std::vector<Trade> trades_;   // see setTrades
    // What the ship carries. It does not change between rocks, so it is set
    // once rather than read off the screen on every scan.
    //
    // One entry per head: a Prospector has one, a MOLE three, and the MOLE
    // fits each separately - which is the point of the ship. `loadout_` is
    // whichever head is being looked at, kept as its own field because every
    // reckoning below works on one head at a time.
    std::vector<MiningLoadout> heads_;
    int headShown_ = 0;                            // which one the card edits
    // Which heads are firing at this rock. A MOLE's three fire together and
    // their fracture power adds up - that is the point of the ship. The
    // rock view switches them on and off; the loadout card still edits one
    // at a time, which is a different question.
    std::vector<char> firing_;
    void setFiringAll();
    LaserOutput firingOutput() const;
    // One head as it meets the rock in view: atRange is what gets through its
    // resistance, fracture what leaves the head. See resistanceLetsThrough.
    LaserOutput onRock(int head) const;
    // The rock's own resistance with the fitted gadget, per cent - the R the
    // break formula wants, whichever colour the panel printed it in
    float rockResistanceForBreak() const;
    float rockOwnResistanceBare() const;            // ...before any gadget
    float powerThroughOwn(float ownResistancePercent) const;
    float powerThroughSolo(float ownResistancePercent) const;   // ...the one head a miner alone flies
    int leastOverpoweredHead() const;
    int firingCount() const;
    // The one head that could hold this rock on its own, and the SAFEST of
    // them where several could - not the strongest. -1 when none can.
    int safestSingleHead() const;
    // Every head that can hold this rock by itself, safest first. The normal
    // case: a MOLE needs a person per laser, and most of the time only one is
    // aboard. Empty when no single head can; only then is more than one the
    // answer.
    std::vector<int> headsThatHoldAlone() const;
    // The fewest heads that can hold it together, for when none can alone.
    std::vector<int> fewestHeadsNeeded() const;
    // What one head would have to be throttled to for this rock, as a share
    // of full: above 1 it cannot hold it alone at all.
    float shareForHead(int head) const;
    // What to call a head: the laser on it, not its number on the ship.
    // "HEAD 2" says nothing about what it does.
    std::wstring headLabel(int head) const;
    // ...and where it sits, which is how you reach it from the seat: a MOLE's
    // three arms are left, middle and right. Empty for a ship with one head,
    // where there is nothing to tell apart.
    std::wstring headPlace(int head) const;
    MiningLoadout loadout_;                        // ...and its contents
    const MiningGadget* fittedGadget_ = nullptr;   // one only - more explodes
    // ...and the one the game's log saw go on a rock. While the rock on the
    // panel is that rock it IS the fitted one, and the panel's figures, which
    // the game prints with the gadget, are taken back to the rock's own.
    GadgetOnRock placed_;
    std::chrono::steady_clock::time_point placedSince_{};   // when it went on
    std::chrono::steady_clock::time_point rockReadAt_{};    // when the panel was last read
    void pickGadget(const MiningGadget* gadget);            // a gadget's button, clicked
    const MiningShip* ship_ = nullptr;             // decides which lasers fit: the chosen fit's kind
    // The player's ships, each as it was last fitted, and which one is
    // chosen. One loadout per ship: every fit keeps its own heads, so
    // choosing the Prospector leaves the MOLE's loadout as it was. heads_ is
    // the live copy of the chosen fit's heads.
    std::vector<ShipFit> fits_;
    int fit_ = -1;
    // The chosen fit is known to be the one flown - picked by hand, or found
    // by its hull - rather than guessed from the kind of ship the log names.
    // Only then is a hull the log names taught to it (see setShip).
    bool fitByHand_ = false;
    std::string hull_;                              // the hull the log names, "" when it has not
    const MiningShip* hullKind_ = nullptr;          // ...and its kind, when it is one Bedrock knows
    void storeFit();                                // heads_ back into the chosen fit
    void pickFit(int index, bool byHand);
    // The fit of that kind flown last, or a new one when there is none
    void pickShipKind(const MiningShip& kind, bool byHand);
    int addFit(const MiningShip& kind);             // its index, or -1 at MAX_FITS
    void removeFit(int index);
    void rememberHull();                            // hull_ is the chosen fit's from now on
    std::wstring fitTitle() const;                  // the chosen fit's name, as it is shown
    // The card's title is its fit's name, typed over in place (see
    // drawLoadoutCard): what is typed, and whether it is being typed
    bool nameEditing_ = false;
    std::wstring nameEntry_;
    void endNameEditing(bool keep);
    bool removeArmed_ = false;                      // Remove pressed once; the second press removes

    // heads_ <-> loadout_, so the card can edit one without the others
    // drifting out of step
    void useHead(int index);
    void storeHead();

    // The borderless-window notice earns its place only until the app has
    // actually read something. Once a signature has come through, the capture
    // demonstrably works, and a permanent instruction about a setting the
    // player has evidently got right is just a strip of window nobody reads.
    // The space goes to the ship and its loadout instead.
    bool everRead_ = false;
    // The borderless notice, which stands until the app has read its first
    // signature. Its height is a constant rather than a number in the
    // painter because the row below has to clear it: two separate numbers
    // drift apart and put the PLACE and SHIP headings inside the notice.
    static constexpr int NOTICE_H = 36;
    // How much further down everything below the header sits while the
    // notice is still up: the notice itself, room for the headings that sit
    // 12 above their fields, and a little air. The ship row has to be
    // reachable from the first minute - a loadout is what the rock view runs
    // on, and making it appear only after a successful read would mean
    // nobody can set one up before they need it. So both are shown, and the
    // rest of the window moves down until the notice retires.
    int extraTop() const { return everRead_ ? 0 : scale(NOTICE_H + 22); }
    bool shipPickerOpen_ = false;
    Gdiplus::RectF shipField_{};        // the ship dropdown
    Gdiplus::RectF loadoutButton_{};    // ...and the button beside it
    struct ShipHit { Gdiplus::RectF rect; int index; };
    std::vector<ShipHit> rockHeadHits_;   // the head tabs in the rock view
    // The gadget goes on the ROCK, so it is picked where the rock is, not on
    // the loadout card, which is about the ship and seldom opened.
    std::vector<ShipHit> rockGadgetHits_;
    std::vector<ShipHit> shipHits_;

    // The match whose locations are shown. A click on a probable-match row
    // picks it; a fresh result list keeps the pick if it is still listed and
    // otherwise falls back to the best match.
    std::string selectedMineral_;
    struct ResultHit { Gdiplus::RectF rect; std::string name; };
    std::vector<ResultHit> resultHits_;   // the rows as last painted, for hit-testing

    SystemChangedCallback systemChangedCallback_;
    LiveModeCallback liveModeCallback_;
    ManualAnalyzeCallback manualAnalyzeCallback_;
    ScanRequestedCallback scanRequestedCallback_;
    TrackModeCallback trackModeCallback_;
    RegionChangedCallback regionChangedCallback_;
    RegionAutoCallback regionAutoCallback_;
    bool regionAuto_ = true;
    bool snapshotHotkey_ = false;
    bool keepHotkey_ = false;
    bool keepHotkeyWanted_ = true;   // false while recording is off
    std::chrono::steady_clock::time_point lastHotkeyTry_{};
    std::wstring shipLabel_;      // from Game.log, "" when unknown
    bool shipInSeat_ = false;
    std::wstring placeLine_;      // "Pyro - Monox" from Game.log, "" when unknown
    std::wstring placeOre_;       // the location the results are narrowed to, "" when none
    bool placeInSpace_ = false;   // out between the bodies: no ground deposits here
    bool resultsOutOfPlace_ = false;   // ...and it explains none of the matches
    // "not in NYX" or "not at Pyro II (Monox)": what a dimmed result is outside of
    std::wstring outOfScopeNote(const AnalysisResult& result) const;
    KeepRecordingCallback keepRecordingCallback_;
    SnapshotCallback snapshotCallback_;

    // Auto-tracking state, mirrored from the application for display
    bool trackEnabled_ = true;
    bool trackLocked_ = false;
    int trackX_ = 0;
    int trackY_ = 0;

    // Window procedure
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // Painting
    void onPaint(HDC hdc);
    void drawHeader(Gdiplus::Graphics& g, int clientW);
    void drawSignatureCard(Gdiplus::Graphics& g, int clientW);
    int  drawResults(Gdiplus::Graphics& g, int clientW, int yPos);
    int  drawDetailCard(Gdiplus::Graphics& g, int clientW, int yPos);

    // The rock view, which takes the signature view's place while the game's
    // scan panel is up. Same three bands as the signature side - the reading,
    // what it means, what to do about it - so the window does not feel like a
    // different program when it switches.
    void drawShipPicker(Gdiplus::Graphics& g, int clientW);
    void setShipPickerOpen(bool open);

    // The place, set by hand: the header line is the control, and the panel
    // it opens lists everything the ore tables have anything to say about.
    void drawPlacePicker(Gdiplus::Graphics& g, int clientW, int clientH);
    void setPlacePickerOpen(bool open);
    void choosePlace(const std::string& placeName);
    PlaceOverrideCallback onPlaceOverride_;
    GameTextsCallback onGameTexts_;
    std::string placeOverride_;            // "" while the log has it
    bool placePickerOpen_ = false;
    Gdiplus::RectF placeField_{};          // the header line, as last painted
    struct PlaceHit { Gdiplus::RectF rect; std::string name; };
    std::vector<PlaceHit> placeHits_;
    void saveLoadout();
    void loadLoadout();

    // The loadout window: the head, what a crafted one prints on itself, the
    // modules it has room for, and the gadget on the rock.
    void showLoadout(bool open);
    // One sheet or dropped list at a time: the glass lets one under another
    // show through it, and the one underneath would take the clicks.
    // `opening` is the flag of the one about to open; every other closes.
    void closeOtherSheets(const bool& opening);
    void drawLoadoutCard(Gdiplus::Graphics& g, int clientW, int clientH);
    bool loadoutOpen_ = false;
    std::string craftedEntry_;          // what is being typed into the % field
    bool craftedEditing_ = false;
    struct LoadoutHit { Gdiplus::RectF rect; int kind; int value; };
    // kind 0 laser, 1 module slot, 2 gadget, 3 the crafted field, 4 close
    std::vector<LoadoutHit> loadoutHits_;

    void drawRockCard(Gdiplus::Graphics& g, int clientW);
    // The strip the rock card grows by when the rock's parts are known. Not
    // under the first-run notice: the tallest rock view fills the window to
    // the footer there, and the notice goes with the first signature read.
    float rockStripHeight() const;
    int  drawRockOutput(Gdiplus::Graphics& g, int clientW, int yPos);
    int  drawRockAdvice(Gdiplus::Graphics& g, int clientW, int yPos);
    int  drawHeadSuggestion(Gdiplus::Graphics& g, float x, float w, float y);
    void drawCompactRock(Gdiplus::Graphics& g, int clientW, int clientH);
    // "SHIP Prospector Hofstede · Hofstede-S1 · Deluge" on the overlay's last
    // line, where nothing drawn down to `above` runs into it
    void drawCompactShip(Gdiplus::Graphics& g, int clientW, int clientH, float above);
    // Fixed, and as wide as the overlay sized to its content ever gets
    // (330 and 40 % over it): a width that changes from rock to rock is
    // too restless.
    static constexpr int OVERLAY_WIDTH = 462;
    int overlayHeight() const;                      // unscaled: taller where there is a ship to name

    // The rock view's answer, worked out once and drawn by the full window
    // and the compact overlay alike - see rockVerdict()
    struct RockVerdict {
        enum Kind { NoLoadout, Unread, HeadsOff, OutOfRange, Solo, SoloOverpowered, Crew, AllHeads, WontHold };
        Kind kind = NoLoadout;
        std::wstring headline;     // "YES, ON YOUR OWN"
        std::wstring who;          // the head it names - "RIGHT Helix II" - empty on a single head
        std::wstring instruction;  // "61 % laser power holds it steady, more makes it climb"
        std::wstring note;         // a second line, or empty
        int head = -1;             // the head it is about, on a MOLE
        int crew = 0;              // how many heads together, when it takes more than one
        float hold = 0;            // per cent laser power at which the charge holds
        float sway = 0;            // per cent either side that the rock's instability moves it
        float floor = 0;           // the laser's minimum, per cent
        bool good = false;         // comfortable, rather than at the edge
        // The active modules the figures count as fired - "Surge", "Surge x2",
        // "Surge + Stampede" - and what the laser needs with none of them
        // running: per cent, or -1 when it will not hold then. Empty and 0
        // where no active module counts.
        std::wstring fired;
        float holdAlone = 0;
    };
    RockVerdict rockVerdict() const;
    // The active modules counted on these heads, as words - empty when none
    std::wstring firedWords(const std::vector<const MiningLoadout*>& fits) const;
    // The laser power share these heads need with no active module running, or -1
    float holdWithoutActives(const std::vector<const MiningLoadout*>& fits) const;
    // ...and the two together into a verdict: v.fired for the instruction's
    // "with Surge fired" (holdWords), the note what the laser does alone
    void sayFired(RockVerdict& v, const std::vector<const MiningLoadout*>& fits, std::wstring& note) const;
    // What a head row says about its head on its own
    struct HeadState { std::wstring words; int standing = 0; };   // 0 cannot, 1 borderline, 2 holds
    HeadState headState(int head) const;
    // The gadget's line: on the rock, needed, a help, or optional
    struct GadgetWords {
        const MiningGadget* gadget = nullptr;
        std::wstring tag, line, note;
        Gdiplus::Color tone;
    };
    GadgetWords gadgetWords() const;
    // The head off the shelf that would break a rock nothing on board holds
    struct Suggestion { std::wstring line, note; bool good = false; };
    Suggestion headSuggestion() const;
    void drawCompactUpdateChip(Gdiplus::Graphics& g, int clientW, float leftOf);
    int  drawLocationCard(Gdiplus::Graphics& g, int clientW, int yPos, int clientH);
    void drawFooter(Gdiplus::Graphics& g, int clientW, int clientH);
    void drawCompact(Gdiplus::Graphics& g, int clientW, int clientH);
    void drawOwnerButton(const DRAWITEMSTRUCT* dis);

    // Helpers
    Gdiplus::Color getMineralColor(const std::string& mineral);
    Gdiplus::Color getRarityColor(Rarity rarity);
    // A rock part's quality in the colour of the rarity its band is as rare as
    Gdiplus::Color qualityColor(int quality);
    void createControls();
    void layoutControls();

    // Control handles
    // The system comes from Game.log, so there are no system buttons; the
    // ores to watch for are in lookingFor_.
    HWND btnLiveMode_ = nullptr;
    HWND btnOverlay_ = nullptr;
    HWND btnOreRows_ = nullptr;   // the overlay's Rows/Line switch, lower right
    HWND btnPlan_ = nullptr;
    HWND btnCraft_ = nullptr;
    HWND btnNotes_ = nullptr;
    HWND btnKeys_ = nullptr;
    HWND btnRefinery_ = nullptr;
    HWND btnAbout_ = nullptr;
    // The keys, on a card over the window.
    bool keysOpen_ = false;
    // The card that asks whether to back the player's data up or put a
    // backup back. It stays open after either, because it is the one place
    // with room to say how it went.
    bool backupOpen_ = false;
    std::wstring backupNotice_;
    bool backupNoticeBad_ = false;
    Gdiplus::RectF backupHit_ = {0, 0, 0, 0};      // the button in the foot
    Gdiplus::RectF backupDoHit_ = {0, 0, 0, 0};    // BACK UP, on the card
    Gdiplus::RectF restoreDoHit_ = {0, 0, 0, 0};   // RESTORE, on the card
    void drawBackupCard(Gdiplus::Graphics& g, int cw, int ch);
    void showBackup(bool on);   // ...and the buttons it has to take turns with
    void backUpData();
    void restoreData();
    void drawKeysCard(Gdiplus::Graphics& g, int cw, int ch);

    // The ? card: who to tell when it reads wrong, and where to put money
    // if it earned some. Same trick as the keys card - drawn by the parent,
    // so the buttons step aside while it is up.
    bool aboutOpen_ = false;
    void showAbout(bool on);
    void drawAboutCard(Gdiplus::Graphics& g, int cw, int ch);
    Gdiplus::RectF aboutKofiHit_ = {0, 0, 0, 0};
    // ...and the same address along the foot of the window, where it is
    // seen without opening the ? card
    Gdiplus::RectF footerKofiHit_ = {0, 0, 0, 0};
    Gdiplus::RectF aboutDiscordHit_ = {0, 0, 0, 0};
    bool aboutCopied_ = false;   // the handle was put on the clipboard
    // The Blueprints window's box for the game's texts, and its last word
    bool gameTextsOn_ = false;
    std::string gameTextsNote_;

    // The card that goes up by itself once, on the first start after an
    // update: what changed, and a word of thanks. Same shape as the ? card -
    // drawn by the parent, dismissed by a click anywhere, buttons step aside
    // while it is up.
    bool whatsNewOpen_ = false;
    // How far the list of changes is scrolled, in pixels, and how tall that
    // list came out the last time it was drawn. It scrolls because it grows
    // with every release. The height is measured while drawing rather than
    // guessed, so nothing has to be kept in step with the text.
    float whatsNewScroll_ = 0.0f;
    float whatsNewContent_ = 0.0f;
    float whatsNewView_ = 0.0f;
    void scrollWhatsNew(float by);
    void showWhatsNew(bool on);
    void drawWhatsNewCard(Gdiplus::Graphics& g, int cw, int ch);

    // The buttons are child windows and are painted after their parent,
    // so they cannot be drawn over - they have to step aside.
    void showKeys(bool on);

    // Ore Finder window ("planner" in the code)
    HWND planner_ = nullptr;
    bool plannerVisible_ = false;
    std::string plannerMineral_ = "Quantainium";   // whose places are listed
    std::string plannerSystem_ = "ALL";           // ALL / STANTON / PYRO / NYX
    // Set from a [[place]] link: the finder then lists what is mined HERE
    std::string plannerPlace_;
    void showPlaceFinderFor(const std::string& place);
    void showSystemFinderFor(const std::string& system);
    // kind 0 ore, 1 system, 2 family, 3 tick box, 4 a link, 5 Report to UEX,
    // 6 the UEX tab, 7 a listing (its page), 8 a gem in the UEX tab, 9 the
    // listings' order ("price", "newest")
    struct PlanHit { Gdiplus::RectF rect; int kind; std::string value; };
    std::vector<PlanHit> planHits_;
    static LRESULT CALLBACK PlannerProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handlePlannerMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool createPlanner();
    void placePlanner();
    void paintPlanner(HDC hdc);
    static constexpr int PLANNER_WIDTH = 620;
    static constexpr int PLANNER_HEIGHT = 880;   // 840 cut the last ground row off
    // UEX's prices (setUexPrices): the WHERE TO SELL card, and the ore that
    // fetches most refined, whose row in the list blinks red
    bool uexEnabled_ = false;
    std::shared_ptr<const UexPrices> uexPrices_;
    long long uexFetchedAt_ = 0;
    std::string uexNote_;
    bool planPulse_ = false;          // the blink's beat
    // The Ore Finder's UEX tab: what players on the UEX marketplace buy and
    // sell the picked ore for, and at what quality; for the ground's two
    // families, the gem picked in it
    bool plannerUex_ = false;
    std::string plannerGem_;
    // the listings by price (the default), or the most lately renewed first:
    // the one sign of life the API gives (UEX has no feedback to sort by)
    bool marketByRecent_ = false;
    struct MarketEntry {
        std::shared_ptr<const std::vector<UexListing>> listings;
        long long fetchedAt = 0;
        std::string note;
    };
    std::map<std::string, MarketEntry> uexMarket_;   // by ore
    std::shared_ptr<const UexThreads> uexThreads_;
    std::string marketOre() const;                    // the ore or gem the UEX tab is on
    void paintMarketColumn(Gdiplus::Graphics& g, int cw, int ch, float cardX, float top);
    // The rock view's line: what a player on UEX pays for the best part of
    // quality 900 or more; "" where there is none to say
    std::wstring rockMarketLine() const;
    // A UEX card: over the Ore Finder (its foot's link), or offered once over
    // the main window at the first start. Its steps:
    // 1 the account - what it gives, the key; once a key is in, 3 asks
    // whether to report prices: No keeps the account without reports, Yes
    // leads to 2, the screenshots of the first 90 days - Agree, and reporting
    // is on; Decline, and it is off, the account kept. 0 is shut.
    UexAccount uexAccount_;
    UexAccountCallback onUexAccount_;
    struct CardHit { Gdiplus::RectF rect; int action; };
    struct UexCardView {
        int step = 0;
        bool welcome = false;        // the first start's offer, over the main window
        int keyChangesAtPaste = -1;  // Paste key pressed: on to the question once a key is kept
        std::vector<CardHit> hits;
        Gdiplus::RectF rect;
    };
    UexCardView uexCard_;            // over the Ore Finder
    UexCardView uexWelcome_;         // over the main window
    bool uexOfferPending_ = false;
    void drawUexCard(Gdiplus::Graphics& g, int cw, int ch, UexCardView& view, POINT hover);
    void pressUexCard(int action, UexCardView& view, HWND owner);
    void showUexWelcome(int step);   // ...and the main window's buttons, which take turns with it
    void closeUexWelcome();          // ...and tells the application the offer was made
    Gdiplus::RectF planBlinkRow_;     // where the blinking row was drawn; empty where none was
    void keepPlanPulse();             // the beat runs while the Ore Finder is open and has a row to blink

    // Refinery window: the station table
    HWND refinery_ = nullptr;
    bool refineryVisible_ = false;
    std::string refinerySystem_ = "ALL";   // ALL / STANTON / PYRO / NYX
    // What is in the hold: indices into REFINERY_MATERIALS. Empty means
    // nothing ticked, and then the list shows every station's specialities
    // rather than pretending to rank them.
    std::vector<int> refineryPicked_;
    bool refineryPicked(int material) const;
    // How much of each, in SCU, indexed like REFINERY_MATERIALS. A hold is
    // not a list of ores, it is a list of amounts - and the yield, the bill
    // and the wait all scale with them.
    std::vector<float> refineryAmount_;
    float refineryAmountOf(int material) const;
    void refineryStep(int material, float by);
    void refinerySet(int material, float v);
    // A hold is almost never a round number of SCU - 1.47 is the normal
    // case and 2 is the rare one - and no ladder of steps reaches 1.47
    // without a lot of clicking. So the figure can be typed: click it,
    // type what the cargo screen says, Enter. -1 when nothing is being
    // typed; the buffer holds the digits until they are committed, so a
    // half-typed "1." never reaches the calculation.
    int refineryTyping_ = -1;
    std::wstring refineryTypeBuf_;
    void refineryTypeStart(int material);
    void refineryTypeCommit();
    void refineryTypeCancel();
    // Pyrometric Chromalysis, not the first row of the table. It keeps the
    // whole yield and takes twice the base time; Dinyx keeps just as much
    // but takes twelve times as long, and nobody waits that out. The usual
    // pick is the costliest method, with high yield and high speed.
    int refineryMethod_ = 2;    // index into REFINING_METHODS
    // The method is picked from a dropped list (a row of nine chips reads
    // as a caption, not as something to click).
    // True while the list is down: every click then closes it, one on an
    // entry picks first; the arrows walk it, Enter and Esc close it.
    bool refineryMethodOpen_ = false;
    // The shopping list needs ONE station, not a ranking: it is answering
    // "how much do I have to mine", and that depends on where it is refined.
    int cartStation_ = 0;       // index into REFINERIES
    // A crafting material against the refinery's list, or -1. The two name
    // things differently - "Titanium" against "Titanium (Ore)" - so the
    // match is on the part before the bracket.
    static int refineryIndexOfCraftMaterial(int craftMaterial);
    // kind 0 material, 1 system, 2 clear the picks, 3 the method field,
    // 4 less of a material, 5 more of it, 6 its amount (typed in),
    // 7 an entry of the dropped method list, 8 the work orders' tab,
    // 9 an order's "Collected" (value: its place in refineryOrders_)
    struct RefHit { Gdiplus::RectF rect; int kind; int value; };
    std::vector<RefHit> refHits_;
    static LRESULT CALLBACK RefineryProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleRefineryMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool createRefinery();
    void placeRefinery();
    void paintRefinery(HDC hdc);
    // The work orders' view, under the tabs where the calculator otherwise is
    std::vector<RefineryOrder> refineryOrders_;
    bool refineryOrdersView_ = false;
    std::function<void(const RefineryOrder&)> onRefineryOrderDone_;
    void paintRefineryOrders(Gdiplus::Graphics& g, int cw, int ch, float top);
    // The finished ones, shown as a badge on the main window's Refinery
    // button; counted again every quarter minute while one is still running
    int refineryDoneCount() const;
    void keepRefineryBadge();
    int refineryDoneShown_ = -1;
    static constexpr UINT_PTR REFINERY_TICK = 1;   // the window's timer: a minute counts down
    // Wider since the right column carries a yield, a bill and a wait
    static constexpr int REFINERY_WIDTH = 760;
    static constexpr int REFINERY_HEIGHT = 800;

    // Grind planner window
    HWND grind_ = nullptr;
    bool grindVisible_ = false;
    int grindSystem_ = 0;      // 0 where the log says I am, 1 Stanton, 2 Pyro, 3 Nyx, 4 everywhere
    int grindScroll_ = 0;      // first set shown
    int grindOpen_ = -1;       // the set unfolded, -1 none
    POINT hoverGrind_ = {-1, -1};
    // 1 system chip, 2 a set, 3 the notice, 4 the finder, 5 a set the same
    // runs pay into, 6 a rank picked, 7 "Open to me", 8 back to the logs,
    // 9 an outfit in the rank line
    struct GrindHit { Gdiplus::RectF rect; int kind; int value; };
    std::vector<GrindHit> grindHits_;
    // The player's ranks, see setRanks
    std::vector<Standing> ranks_;
    bool ranksKnown_ = false;
    int ranksFrom_ = 0;
    RankSetCallback rankSetCallback_;
    // "Open to me": only the sets with a mission the player is offered at
    // their rank with its outfit
    bool grindOpenOnly_ = false;
    // The player's standing on an outfit's ladder - what the logs say, or
    // the bottom of it with nothing behind it
    Standing standingOn(int faction, int ladder) const;
    // Whether the game offers the player a contract (a CRAFT_SOURCES row) at
    // their rank: 1 it does, -1 not yet (it asks for more), -2 no more (it
    // stops below their rank), 0 not known
    int rungOpen(int source) const;
    // The finder: any part of a mission's name, a blueprint's name or an
    // outfit's, typed into the box beside the system chips. The list keeps the
    // sets it is true of, and a mission it names leads its row.
    std::wstring grindQuery_;
    // Where the open contract card's own hits begin in craftHits_ - see
    // paintContractCard and the crafting window's click handler
    size_t craftCardHitsFrom_ = 0;
    bool grindTyping_ = false;   // the box has the keys; letters go to it, not to the shortcuts
    // ...and held to one set: the planner opened from a contract card on the
    // pool that contract pays out of. The finder shows the contract's name;
    // a new word typed into it lets go of the set.
    int grindGroup_ = -1;
    // One reward pool, and what it would take to empty it. See grindSets()
    // for where the numbers come from.
    struct GrindSet {
        int group;         // which pool, in poolGroups()
        int best;          // the CRAFT_SOURCES row worth running
        int poolFirst, poolCount;
        int poolSize;      // the true size, entries the files do not name included
        int missing;       // named entries not in the library
        int chance;        // % the contract pays out a blueprint at all
        int respawn;       // minutes after a run until it is offered again
        int missions;      // how many contracts feed the set
        int inSystem;      // ...of them, how many run where the planner is looking
        unsigned systems;  // where it can be ground: 1 Stanton, 2 Pyro, 4 Nyx
        double runs;       // runs to clear what is still missing
        // The missions that feed the set, one row per distinct name, the
        // lowest standing first: an outfit's ladder. `lead` is the one the
        // row is headed by - the one the finder named, else the highest the
        // player is offered, else the lowest.
        std::vector<int> rungs;
        int lead;
        int open = -1;     // rungs the player is offered at their rank, -1 not known
        // The other sets the same missions pay out of - a contract with the
        // game's "Pool 2" feeds two sets with every run. Indices into
        // poolGroups(), each with the missions of this set that feed it.
        struct Also {
            int group;
            std::vector<std::wstring> by;   // the missions, by name
        };
        std::vector<Also> also;
    };
    // The one thing that can make every number in this window wrong: the
    // library is learnt from the game's own log, and a log only goes back so
    // far. Said once, and it has to be clicked away rather than clicked
    // past, because somebody who never reads it grinds for things they own.
    bool grindWarned_ = false;
    Gdiplus::RectF grindWarnHit_ = {0, 0, 0, 0};
    void loadGrindWarned();
    void drawGrindWarning(Gdiplus::Graphics& g, int cw, int ch);

    // `filtered` false: every set still holding something, whatever the
    // finder says - for the count the header measures the finder against
    std::vector<GrindSet> grindSets(bool filtered = true) const;
    unsigned grindMask() const;
    int systemIAmIn() const;   // 1 Stanton, 2 Pyro, 3 Nyx, 0 nothing known
    bool createGrind();
    void placeGrind();
    void paintGrind(HDC hdc);
    void paintGrindList(Gdiplus::Graphics& g, int cw, int ch);
    static LRESULT CALLBACK GrindProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleGrindMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static constexpr int GRIND_WIDTH = 900;
    static constexpr int GRIND_HEIGHT = 780;

    // Blueprints window
    HWND crafting_ = nullptr;
    bool craftingVisible_ = false;
    int craftMaterial_ = -1;         // index into CRAFT_MATERIALS, -1 = every blueprint
    int craftType_ = -1;             // index into CRAFT_TYPES, -1 = every component
    char craftGrade_ = 0;            // 'A'..'D', 0 = any grade
    int craftSize_ = -1;             // component size, -1 = any
    int craftClass_ = -1;            // index into CRAFT_CLASSES, -1 = any
    // Where a recipe can be earned, and from whom. Both read the contracts
    // that hand it out: 0 any system, else 1 Stanton, 2 Pyro, 3 Nyx; "" any
    // outfit. One contract has to answer BOTH - "in Pyro, from Shubin" is
    // one question, not two - and a recipe no contract drops answers
    // neither, which is what makes these the sharpest filters in the bar.
    int craftSystem_ = 0;
    // More than one outfit at a time: "Shubin or Rayari" is a question
    // somebody actually has, and the alternative - one at a time - makes
    // them run the search twice and hold the two lists in their head.
    // Empty means any of them.
    std::vector<std::string> craftFactions_;
    std::vector<std::string> factionHits_;   // what the faction chips stand for
    // Twenty-one factions will not fit beside a heading, so the FROM row
    // holds two chips and the list of them drops below it when asked for.
    bool craftFactionPick_ = false;
    bool craftByComponent_ = false;  // which list the left column shows
    int craftScroll_ = 0;            // first blueprint row shown
    std::wstring craftSearch_;       // typed filter: item, type, material, outfit or contract
    // kind 0 material filter, 1 blueprint row, 2 quality slider, 3 material
    // in the open blueprint (jumps to the ore finder), 4 back, 5 preset
    struct CraftHit { Gdiplus::RectF rect; int kind; int value; };
    std::vector<CraftHit> craftHits_;
    bool craftMaterialPicked_ = false;  // the user chose; stop following the scan
    int craftOpen_ = -1;             // blueprint shown with its slots, -1 = the list
    static constexpr int CRAFT_MAX_SLOTS = 8;
    int craftQuality_[CRAFT_MAX_SLOTS] = {500, 500, 500, 500, 500, 500, 500, 500};
    int craftDrag_ = -1;             // slider being dragged, -1 = none
    // Where a drag began, and whether it began ON a mark - the 500 stock
    // mark or a tooth of the comb. A click there sets the value outright,
    // and the pointer has to travel before the drag may move it again:
    // otherwise the mouse-move that arrives with every click drags the
    // value straight off the mark that was just clicked.
    float craftDragX_ = 0.0f;
    bool craftDragOnMark_ = false;
    int craftSlider_ = 0;            // slot the arrow keys move
    int craftSourceOpen_ = -1;       // contract row whose briefing is unfolded
    bool craftCardApart_ = false;    // the recipe is being drawn under that card, without it
    int craftSourceScroll_ = 0;      // first contract row shown
    std::string craftSourceSystem_;  // "", "Stanton", "Pyro", "Nyx"
    std::vector<std::string> systemHits_;   // what the filter chips stand for
    void paintContractCard(Gdiplus::Graphics& g, int cw, int ch, const struct CraftSource& src);
    bool craftSnap_ = true;          // sliders step instead of running free
    bool caretOn_ = true;            // the search caret, blinking on a timer
    std::vector<std::string> owned_; // per blueprint: when it was received, "" = not owned
    std::string ownedNote_;          // where the library knowledge comes from
    int craftOwned_ = 0;             // 0 every blueprint, 1 owned only, 2 missing only
    // 0 every blueprint, 1 only the ones a contract hands out. A recipe the
    // player does not own is worth knowing about only when there is a way
    // to earn it; one switch, since "any" is it being off.
    int craftContract_ = 0;
    OwnedToggleCallback ownedToggleCallback_;
    DataRestoredCallback dataRestoredCallback_;
    UiScaleCallback uiScaleCallback_;
    void stepUiScale(int direction);   // one notch bigger or smaller
    void resizeForScale();             // the windows follow the zoom
    bool isOwned(int blueprint) const {
        return blueprint >= 0 && blueprint < (int)owned_.size() && !owned_[blueprint].empty();
    }
    int ownedCount() const {
        int n = 0;
        for (const auto& s : owned_) if (!s.empty()) n++;
        return n;
    }

    // ---- the shopping list ----
    // Several different things you mean to build, and what they come to in
    // material altogether. Kept in craft_list.txt beside the exe and written
    // by NAME, so regenerating the tables cannot turn it into nonsense.
    struct CartLine {
        int blueprint = -1;
        int count = 0;
    };
    static constexpr const char* CART_FILE = "craft_list.txt";
    std::vector<CartLine> cart_;
    // Does anything on the shopping list want this ore? If so, its match
    // blinks red. The scan names an ore the way MiningData spells it, the
    // recipes the way CraftingData does - "Aluminium" against "Aluminum" -
    // so both spellings of a material are tried. Walked rather than cached:
    // the list holds a handful of recipes with eight slots at most, and a
    // repaint asks five times.
    bool wantedByCart(const std::string& mineral) const;
    // The least quality a recipe on the list asks of that material - the
    // bench refuses anything under it; 0 where none of them asks for one
    int cartQuality(const std::string& mineral) const;
public:
    // A part of a rock: how likely each quality band is, and how the Ore
    // Finder says its quality ("553-1000 · mostly 553 or 644", "quality 264,
    // rarely 553"). Public for the self-test.
    static std::array<double, 8> qualityOdds(const RockPart& part);
    static std::wstring rockPartQuality(const RockPart& part);
private:
    bool anyWantedResult() const;   // ...for any match on screen, so the blink repaints
    // Whether a part of the rock on view is one the player is after: its
    // ore ticked in "I'm looking for" or asked for by an order taken in the
    // game, at whatever quality; or taken by a recipe on the shopping list,
    // where the part has the quality the recipe asks for
    bool rockPartWanted(const RockPartRead& part) const;
    // The red beat of a thing looked for, behind a figure: a frame that is
    // always there and a wash while the pulse is on, as a wanted match's row
    // has them
    void drawWantedBeat(Gdiplus::Graphics& g, const Gdiplus::RectF& behind, float radius);
    bool blinkFor(const AnalysisResult& result, bool* fromCart = nullptr, bool* fromOrder = nullptr) const;
    // ...or a gathering order taken in the game wants it (see ContractNow).
    // A gem of hand or ground mining is wanted as the deposit that can hold
    // it: the scan names the kind of deposit, the place decides the gem.
    bool wantedByOrder(const std::string& mineral) const;
    std::wstring orderWords() const;   // "16 SCU Torite, 11 SCU Bexalite", of the orders taken; "" with none
    std::vector<ContractNow> contractsNow_;
    std::chrono::steady_clock::time_point contractsDrawn_{};
    // What the log says of a mission of the planner right now: "accepted",
    // "back in ~43 min" while its outfit blocks the one just finished, or ""
    std::wstring contractMoment(const struct CraftSource& src) const;

    // "I'M LOOKING FOR": ores picked by hand, which blink exactly like the
    // ones a recipe on the shopping list wants. The list it drops is
    // searchable because typing a name beats hunting for it in twenty-nine.
    // Kept in looking_for.txt beside the player data, so it survives a
    // restart the way the shopping list does.
    static constexpr const char* LOOKING_FILE = "looking_for.txt";
    static constexpr const char* LOADOUT_FILE = "loadout.txt";
    // Where the window was when it was closed - see saveWindowPlace
    static constexpr const char* WINDOW_FILE = "window.txt";
    void saveWindowPlace() const;
    // Puts it back there if that is still on a screen; returns how to show it
    int restoreWindowPlace();
    bool placeRestored_ = false;
    std::vector<std::string> lookingFor_;   // ore names as MiningData spells them
    bool lookingOpen_ = false;              // the panel is down over the window
    std::wstring lookingSearch_;            // what has been typed into it
    Gdiplus::RectF lookingField_;           // the field it hangs from, as last painted
    struct LookHit { Gdiplus::RectF rect; int kind; std::string name; };   // 0 an ore, 1 clear
    std::vector<LookHit> lookHits_;
    void loadLookingFor();
    void saveLookingFor();
    bool lookingForOre(const std::string& mineral) const;
    void toggleLookingFor(const std::string& mineral);
    void setLookingOpen(bool open);
    // Every name a scan can ever hand back - the ores and the three ground
    // families - in alphabetical order. Picking anything else would be
    // picking something that can never light up.
    static std::vector<std::string> detectableOres();
    // ...as the I'M LOOKING FOR panel shows them: in groups, by rarity
    struct LookingGroup {
        std::wstring heading;
        Gdiplus::Color colour;
        std::vector<std::string> names;
    };
    std::vector<LookingGroup> lookingGroups();
    // The ore finder spells the two ground families its own way ("ROC
    // deposits", "Hand minables"); a scan says "ROC Deposit" and "FPS
    // Minable". Watching for one has to use the name the scan will say.
    static std::string watchName(const std::string& shown);
    void drawLookingField(Gdiplus::Graphics& g, int cw);
    void drawLookingPanel(Gdiplus::Graphics& g, int cw, int ch);
    bool cartOpen_ = false;          // the list, instead of the recipes
    // The two pickers over the shopping list - the station it is refined
    // at, the method - are dropped lists. 30 while the station list is
    // down, 31 the method list, 0 neither; the field the open list hangs
    // from is remembered for the painter. Hit kinds: 30/31 the fields,
    // 33 a station entry, 34 a method entry.
    int cartPickOpen_ = 0;
    Gdiplus::RectF cartPickAnchor_;
    void paintCartPickList(Gdiplus::Graphics& g, int cw, int ch);
    int cartCountOf(int blueprint) const {
        for (const auto& line : cart_) {
            if (line.blueprint == blueprint) return line.count;
        }
        return 0;
    }
    int cartTotal() const {
        int n = 0;
        for (const auto& line : cart_) n += line.count;
        return n;
    }
    void cartAdd(int blueprint, int delta);
    // The minus / number / plus, drawn the same in the recipe list and on
    // the shopping list itself
    // A small trolley, drawn rather than typed: the glyph for one is not
    // in the UI font and an emoji font would come out a different size
    // and colour than everything around it.
    void drawTrolley(Gdiplus::Graphics& g, Gdiplus::RectF box, const Gdiplus::Color& ink);
    void cartStepper(Gdiplus::Graphics& g, int blueprint, float x, float y, bool offer,
                     Gdiplus::Font& face, Gdiplus::Font& digits, Gdiplus::StringFormat& middle);
    void loadCart();
    void saveCart();

    int craftGroup_ = -1;            // index into typeGroups(), -1 = every group
    void openBlueprint(int index);   // switches the window to one recipe
    void showOreFinderFor(const std::string& material);
    void paintCraftingList(Gdiplus::Graphics& g, int cw, int ch);
    // kind 6 = the material/component switch, 7 = a component filter
    void paintCraftingDetail(Gdiplus::Graphics& g, int cw, int ch);
    static LRESULT CALLBACK CraftingProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleCraftingMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    bool createCrafting();
    void placeCrafting();
    void paintCrafting(HDC hdc);
    std::vector<int> craftRows() const;   // the blueprints the filters leave
    // ---- the notebook ----
    // Notes are plain .txt files under notes\ - the model is in NoteBook,
    // what lives here is the window around it.
    HWND notes_ = nullptr;
    // ---- pictures kept with a note ------------------------------------
    // A rail down the right of the open note, which folds away. Bedrock takes
    // no picture itself: by the time the player opens the notebook the game is
    // behind them, and the Snipping Tool has already done it.
    bool notesMediaOpen_ = true;
    struct MediaItem {
        std::filesystem::path path;
        std::wstring when;
        std::shared_ptr<Gdiplus::Bitmap> thumb;   // made once, not per paint
    };
    std::vector<MediaItem> media_;
    int mediaFolder_ = -2;           // the note media_ was built for; -2 = none yet
    int mediaNote_ = -2;
    std::wstring mediaNotice_;       // what the last one did, said on the rail
    // Throwing a picture away asks first, the way DELETE on a note does: the
    // cross is small and sits under the pointer while reading, so one stray
    // click must not be enough. -1 = nothing asked, otherwise the index waiting
    // for a second click.
    int mediaConfirm_ = -1;
    void refreshMedia(bool force = false);
    float mediaRailWidth() const;    // 0 when no note is open
    void drawMediaRail(Gdiplus::Graphics& g, float x, float top, float bottom);
    std::filesystem::path nextMediaPath(const char* extension);
    void mediaSays(const std::wstring& text, bool reread);
    bool pasteIntoOpenNote();
    bool addFilesToOpenNote(const std::vector<std::filesystem::path>& files);
    bool pickFilesForOpenNote();
    // A picture, in a window of Bedrock's own rather than handed to whatever
    // Windows opens pictures with.
    HWND shot_ = nullptr;
    bool shotVisible_ = false;
    // The news window: 2.5 first, 2.0 a page on, a tab for each
    HWND news_ = nullptr;
    bool newsVisible_ = false;
    Gdiplus::RectF newsButton_;
    int newsPage_ = 0;                            // 0: 2.5, 1: 2.0
    Gdiplus::RectF newsTabs_[2];
    std::unique_ptr<Gdiplus::Bitmap> newsShot_;   // the 2.5 page's picture, once decoded
    Gdiplus::Bitmap* newsPicture();
    // UEX's badge for the Ore Finder, made at the height it is drawn at
    std::unique_ptr<Gdiplus::Bitmap> uexBadge_;
    int uexBadgeHeight_ = 0;
    Gdiplus::Bitmap* uexBadge(int height);
    bool createNews();
    void paintNews(HDC hdc);
    void drawRockDemo(Gdiplus::Graphics& g, float x, float y, int width);
    static LRESULT CALLBACK NewsProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleNewsMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static constexpr int NEWS_WIDTH = 1080;   // wide enough for the 2.5 page's picture
    static constexpr int NEWS_HEIGHT = 720;
    int shotIndex_ = -1;                          // into media_
    std::shared_ptr<Gdiplus::Bitmap> shotImage_;  // the one being looked at, full size
    bool createShot();
    void loadShot();
    void showShot(int index);
    void stepShot(int by);
    void paintShot(HDC hdc);
    static LRESULT CALLBACK ShotProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleShotMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static constexpr int SHOT_WIDTH = 1100;
    static constexpr int SHOT_HEIGHT = 760;

    static constexpr int NOTES_MEDIA_WIDTH = 206;   // open
    static constexpr int NOTES_MEDIA_SHUT = 26;     // ...and folded away

    HWND notesEdit_ = nullptr;       // the note being written
    HWND notesName_ = nullptr;       // the box that names and renames
    HFONT notesFont_ = nullptr;      // fixed pitch: notes are full of numbers
    bool notesVisible_ = false;
    NoteBook book_;
    int noteFolder_ = -1;            // folder shown, -1 = none picked
    int noteIndex_ = -1;             // note open in it, -1 = none
    bool noteDirty_ = false;         // typed since the last write
    // A rich edit control reports the text being LOADED as a change, and
    // a note that counts as edited the moment it is opened gets rewritten
    // and jumps to the top of the list for having been read.
    bool loadingNote_ = false;
    // Likewise while the links are being styled: the control reports a
    // format change as a change, which would mark every saved note unsaved
    // again and keep it saving and restyling every second and a half
    bool styling_ = false;
    int nameMode_ = 0;               // 0 none, 1 new folder, 2 new note, 3/4 rename
    // Where the mouse last was, per window, in that window's own client
    // coordinates. -1 means it is not over the window at all.
    POINT hoverMain_ = {-1, -1};
    FrostCache frostMain_, frostCraft_, frostGrind_, frostRef_;
    // While a list drops over a window: the page under it, blurred, as paint
    // for the list's pane of glass (see onPaint). Null otherwise.
    const Gdiplus::Brush* frost_ = nullptr;
    POINT hoverPlan_ = {-1, -1};
    POINT hoverRef_ = {-1, -1};
    POINT hoverCraft_ = {-1, -1};
    // True when the point falls in the row, and worth a lift
    static bool isHot(const POINT& at, const Gdiplus::RectF& row) {
        return at.x >= 0 && row.Contains((float)at.x, (float)at.y);
    }
    // Follows the mouse and asks for a repaint only when it has moved
    // far enough to be a different row - a repaint per pixel would be
    // a lot of painting for a highlight.
    bool trackHover(HWND hwnd, POINT& slot, LPARAM lParam);
    void clearHover(HWND hwnd, POINT& slot);

    std::vector<CraftHit> noteHits_;
    // DELETE asks once: the first click arms it, the second within a few
    // seconds does it, anything else disarms it. 1 = a note, 2 = a folder.
    int confirmDelete_ = 0;
    // Dragging a note from one folder into another. The press remembers
    // what was under it; only a few pixels of travel turn that into a
    // drag, so an ordinary click still just opens the note.
    int dragFolder_ = -1, dragNote_ = -1;
    bool dragging_ = false;
    POINT dragFrom_ = {0, 0};
    POINT dragAt_ = {0, 0};          // where the pointer is, so the name can follow it
    int dropFolder_ = -1;            // the folder under the cursor, -1 = none
    bool trashOpen_ = false;             // the list of thrown-away things is unfolded
    std::vector<NoteBook::Trashed> trashList_;   // what it showed, for the RESTORE clicks
    // The autocorrect-style list under the caret
    HWND notesSuggest_ = nullptr;
    std::vector<NoteLink> suggestions_;
    // The update strip along the foot: drawn only when it has something
    // to say, and it is the only thing in the app that a click can make
    // reach the network.
    int updateState_ = 0;
    int updatePercent_ = 0;
    std::string updateVersion_, updateNote_;
    Gdiplus::RectF updateHit_ = {0, 0, 0, 0};
    UpdateActionCallback updateAction_;
    bool updateAsked_ = false;       // somebody clicked; failures may be said out loud
    // Is there anything to show? 3 waiting, 4 fetching, 5 ready to go in,
    // and 6 only when the player asked for it themselves. Idle, looking
    // and up-to-date are not news, and a button that cannot do anything
    // is furniture.
    bool updateWorthShowing() const {
        return updateState_ == 3 || updateState_ == 4 || updateState_ == 5 ||
               (updateState_ == 6 && updateAsked_);
    }
    int suggestChoice_ = 0;
    int suggestTop_ = 0;                     // first row drawn, when not all fit
    static constexpr int SUGGEST_ROW = 22;
    static constexpr int SUGGEST_ROWS = 8;   // shown at once; 24 may be found
    void updateSuggestions(bool forShot = false);
    void hideSuggestions();
    // Moves the choice and drags the drawn window along with it
    void moveSuggestion(int delta, bool wrap);
    // One colour per kind, so a link in the text and its chip below can
    // never drift apart: an ore in its own mineral colour, a recipe in
    // the accent, a place plain
    Gdiplus::Color linkColour(const NoteLink& link);
    // Hides the brackets and marks the name inside them as a link
    void styleNoteLinks();
    // False once we find that hidden text does not come back out of the
    // control - then the brackets stay visible rather than being lost
    bool hideBrackets_ = true;
    void openLinkRange(long from, long to);
    void acceptSuggestion();
    bool suggesting() const { return !suggestions_.empty(); }
    static LRESULT CALLBACK SuggestProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleSuggestMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void openLink(const NoteLink& link);
    void openLinkAt(int character);     // ...the one around this character
    std::wstring noteText() const;      // what is in the editor, in its own character space
    static const std::map<std::wstring, NoteLink>& linkTable();

    static constexpr int NOTES_WIDTH = 980;
    static constexpr int NOTES_HEIGHT = 720;
    static constexpr int NOTES_TREE_WIDTH = 300;
    bool createNotes();
    void placeNotes();
    void layoutNotes();
    void paintNotes(HDC hdc);
    void drawNotes(Gdiplus::Graphics& g, int cw, int ch);
    void openNote(int folder, int note);
    void saveNote();
    // Notes out to a file of your choosing and back in again, so nobody has
    // to go digging in %LOCALAPPDATA% to send a route to a friend. -1, -1
    // is the whole book; a folder and a note is that one note.
    void exportNotes(int folder, int note);
    void importNotes();
    // The import itself, once the file is known: what the button does after
    // the dialog, and what the screenshot run does without one. True when
    // something landed.
    bool importNotesFrom(const std::filesystem::path& file);
    std::wstring noteNotice_;        // the line at the foot of the notes window
    bool noteNoticeBad_ = false;     // ...red rather than green
    // Typing on an open notebook with nothing open starts a note rather
    // than going nowhere. Returns false when there was nowhere to put it.
    bool startTypedNote();
    void startNaming(int mode, const std::wstring& initial);
    void commitNaming();
    static LRESULT CALLBACK NotesProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT handleNotesMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static std::string fromW(const std::wstring& s);

    static constexpr int CRAFTING_WIDTH = 880;
    static constexpr int CRAFTING_HEIGHT = 1000;

    // Capture region controls
    // The aim box is always the automatic one, so it is shown, not typed
    int regionX_ = 0, regionY_ = 0, regionW_ = 0, regionH_ = 0;

    // Logical layout constants (in DIPs, scaled by the current DPI at runtime)
    static constexpr int WINDOW_WIDTH = 480;    // initial client width
    static constexpr int WINDOW_HEIGHT = 888;   // initial client height
    static constexpr int MARGIN = 16;
    // The rows down the window, top to bottom, with the air between them
    // spelled out. Nothing here may end where the next thing begins: the
    // notice strip ends at LY_NOTICE + 36, above the ore pick's heading.
    static constexpr int LY_NOTICE = 72;        // the place/ship row, 26 tall -> 98; its headings at 60
    static constexpr int LY_SYSTEM = 136;       // the ore pick, 28 tall -> 164; its heading at 118
    static constexpr int LY_CONTROLS = 180;     // the action row, 28 tall -> 208
    // The aim box has no card: it is worked out from the game window, and a
    // box nobody can move needs no display.
    static constexpr int LY_SIGCARD = 224;      // big signature readout card, 76 tall -> 300
    static constexpr int LY_RESULTS = 310;      // matches list
};

} // namespace SCMiner
