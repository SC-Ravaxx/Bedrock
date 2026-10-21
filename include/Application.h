#pragma once

#include <set>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include "ScreenCapture.h"
#include "OCREngine.h"
#include "MiningAdvice.h"
#include "ScanPanel.h"
#include <deque>
#include <map>
#include "SignatureAnalyzer.h"
#include "MinerGUI.h"
#include "BlueprintLibrary.h"
#include "GameTexts.h"
#include <future>
#include "GameState.h"
#include "Updater.h"
#include "GlyphReader.h"
#include "TextLineReader.h"
#include "LoadoutReader.h"
#include "RockContents.h"
#include "RefineryOrders.h"
#include "FrameRecorder.h"
#include "UexPrices.h"
#include "UexReports.h"
#include "RefineryShare.h"
#include "UexMarket.h"
#include <memory>

namespace SCMiner {

class Application {
public:
    Application();
    ~Application();

    // Initializes all components
    bool initialize();
    // The aim box for a game drawn in this viewport, in the proportions of
    // the setup it was measured on (see the definition). margin gets the
    // search margin that goes with it.
    static CaptureRegion defaultAimBox(const cv::Rect& viewport, int* margin = nullptr);
    // Re-derives the box when it is automatic and the viewport changed;
    // true when it did change
    bool refreshAutoAim(bool announce);

    // "Bedrock.exe --snapshot": the files Ctrl+Alt+S writes, from the
    // console and without the window - debug/snapshot_*.png|txt and
    // debug/session.txt with the capture path filled in
    void snapshotFromConsole();

    // Runs the console main loop
    void run();

    // Runs with the GUI
    void runWithGUI(HINSTANCE hInstance);

    // Stops the application
    void stop();

    // Callback for new analysis results
    using ResultCallback = std::function<void(const std::vector<AnalysisResult>&)>;
    void setResultCallback(ResultCallback callback);

    // Settings
    void setCaptureInterval(int milliseconds);
    void setHotkey(int vkCode);

    // Manual scan (for hotkey triggers)
    void triggerScan();

    // Toggle live mode on/off
    void toggleLiveMode();
    bool isLiveModeActive() const;

    // Auto-tracking of the signature label (for head tracking / Tobii)
    void setAutoTrack(bool enabled);
    bool isAutoTrackActive() const;

private:
    ScreenCapture screenCapture_;
    OCREngine ocrEngine_;
    bool ocrAvailable_ = false;   // tessdata found at start
    GlyphReader glyphReader_;
    // The second reader: a neural recogniser run on the row the glyph reader
    // cut - a second opinion on its value, and a reader of its own where the
    // glyph reader gives up. See TextLineReader.
    TextLineReader textLines_;
    // The value both readers last agreed on, and where: the scans after that
    // one do not ask again while the same value stands in the same place -
    // holding a rock in the sights would otherwise pay for the second reader
    // on every scan to hear the same answer
    int agreedValue_ = 0;
    cv::Rect agreedRect_;
    // Rows the second reader has already looked at and found no
    // signature in: where each sat on the screen and when it was
    // tried. While it is resting these are left alone - the same
    // pixels give the same answer - and a row that is NOT among them
    // is read at once, resting or not. See the idle pace in the scan.
    struct LineLooked {
        cv::Rect where;
        std::chrono::steady_clock::time_point when;
    };
    std::vector<LineLooked> lineLooked_;
    bool lineLookedAt(const cv::Rect& where);
    void rememberLineLook(const cv::Rect& where);
    // Rolling record of what the scan looked at, so a missed reading can be
    // replayed later instead of reconstructed from a screenshot taken too late
    FrameRecorder recorder_;
    // Recording is OFF unless region_config.txt says record=1: a hand-out
    // must neither film anyone's screen nor need ffmpeg. With it off there is
    // no ring, no Ctrl+Alt+K, and no debug pictures either.
    bool recordScans_ = false;
    // How many Ctrl+Alt+K copies of the recording ring may pile up, and how
    // many GB they may take together - region_config.txt: keeps=, keepgb=.
    // The hand-out defaults are small; a machine that collects footage on
    // purpose sets them higher.
    int maxKeeps_ = 3;
    int maxKeepGb_ = 2;
    // region_config.txt: live=0 starts idle. A fresh install scans right
    // away - the one thing the app is for, and one button less to miss
    bool startLive_ = true;
    bool inScan_ = false;          // a scan is running; messages pumped inside it must not start another
    // Scans in a row that produced nothing. After a streak the search widens
    // to the whole monitor once, to pick the marker up again wherever it went.
    mutable int missStreak_ = 0;
    SignatureAnalyzer analyzer_;
    std::unique_ptr<MinerGUI> gui_;
    // What the player owns, off the game's log - see BlueprintLibrary
    BlueprintLibrary library_;
    // Where the player is and what they fly, off the same Game.log - see
    // GameState. The log sets the system; a click by hand holds until the
    // log names another one.
    GameState gameState_;
    std::string logSystem_;      // the system the log last set
    std::string logShipLabel_;   // what was last announced, so changes are announced once
    std::string placeLine_;      // "Pyro - Monox", as shown; "" when unknown
    std::string placeOre_;       // the ore tables' location behind it, "" when none
    // A place the player set by hand; "" while the game log has it. It
    // outlives the session, because a player who had to set it once will be
    // in the same place when they come back, and because forgetting it
    // silently would put them back on a stale place without saying so. The
    // log takes it back the moment it proves the ship elsewhere - see
    // applyGameState - which is why the time it was set is kept with it, in
    // the log's own UTC spelling ("2026-09-27T09:27:53"): only a line the
    // game wrote AFTER the setting can overrule it.
    std::string placeOverride_;
    std::string placeOverrideStamp_;

    // Looking for the game's SCAN RESULTS panel, which is what puts the
    // window into its rock view. It is anchored to the rock in 3D, so the
    // whole viewport has to be searched rather than the small area the
    // signature reader watches - and that is dear enough at 5120x1440 to be
    // on a clock of its own rather than on every scan.
    void lookForRock();
    bool shipCanMine() const;   // the ship the log names can mine, or none is named yet
    // One line per look at a scan panel into debug\rock_readings.csv - what
    // was read, and what the median made of it. steady is null for a look that
    // did not name enough figures to count.
    void logRockReading(const ScanPanel& panel, const RockStats* steady);
    // Whether the rock view is fed at all; see the definition
    static bool rockViewWanted();
    std::chrono::steady_clock::time_point lastRockLook_{};
    int rockMisses_ = 0;          // panels missed in a row; the view waits
    OCREngine rockOcr_;
    bool rockOcrTried_ = false;
    bool rockOcrReady_ = false;
    bool rockSeen_ = false;       // the last look had one, for the log line
    float shownMass_ = 0;         // the mass of the rock on view, 0 when none is
    std::string shownOre_;        // ...and its ore, as the panel's heading read
    float otherMass_ = 0;         // a mass read with no figures beside it that is not that rock's
    int otherLooks_ = 0;          // ...and how many looks in a row read it
    // The last few readings of the same rock. Every look reads the panel
    // again, and OCR does not read the same pixels the same way twice. What
    // is shown is the middle of the recent ones, which one bad reading cannot
    // move.
    std::deque<RockStats> rockReadings_;
    int rockMissShots_ = 0;      // pictures kept of a panel the shapes missed
    static RockStats steadyRock(const std::deque<RockStats>& readings);
    // The shape reader's looks, read as one - see PanelSteadier. The median
    // above is left for the face reader, which stands in when the shape
    // reader cannot find the panel's labels at all.
    PanelSteadier rockSteadier_;
    int rockUnsettled_ = 0;       // looks in a row that found a panel and settled nothing
    bool rockLabelsOnly_ = false; // the last look found the labels and no figures
    int rockQuickLooks_ = 0;      // ...and how many looks in a row came straight after
    bool rockSettling_ = false;   // the last look found a panel whose figures have not settled
    int rockSettlingLooks_ = 0;   // ...and how many looks in a row came straight after
    int glanceQuiet_ = 0;         // scans left without a glance, after one that led to nothing
    // The last panel's place, kept across sessions - see loadPanelSpot
    static constexpr const char* PANEL_SPOT_FILE = "panel_spot.txt";
    void loadPanelSpot(const cv::Size& viewport);
    void keepPanelSpot(const cv::Size& viewport);
    PanelSpot keptSpot_;          // what panel_spot.txt holds, so it is written only on a change
    // The loadout a one-seat ship's mining HUD shows (LoadoutReader), read
    // now and then on the looks for a rock - beside its panel, or with none
    // up in the Prospector or Golem the log has the player sitting in - and
    // handed to the window once two readings agree: the fit with that laser
    // and those modules is chosen.
    // No fit has them: on a third reading that says the same, the ship flown
    // takes them for its own.
    static constexpr int OWN_READINGS = 3;
    void readLoadoutOnHud(const cv::Mat& frame, const ScanPanel& panel);
    void takeSeenLoadout(const SeenLoadout& seen, const MiningShip& kind);
    std::future<SeenLoadout> loadoutJob_;   // a reading under way, off the window's thread
    std::string loadoutJobSeat_;            // ...and the seat it was asked for in
    const MiningShip* loadoutJobKind_ = nullptr;   // ...and whose HUD it was
    std::chrono::steady_clock::time_point lastLoadoutRead_{};
    std::string loadoutSeat_;           // the ship and hull the readings below were made in
    std::string loadoutSeen_;           // the last reading, as laser and modules by id
    std::string loadoutSaid_;           // ...and the one the window was last told, so it is told once
    int loadoutAgreeing_ = 0;           // readings in a row that said the same
    int loadoutUnsure_ = 0;             // readings in a row that came back unsure
    int loadoutDisagreeing_ = 0;        // whole readings in a row that said something else than the last
    bool loadoutNoHud_ = false;         // the last reading found no mining HUD in the picture
    bool loadoutSettled_ = false;       // handed to the window for this seat
    std::string loadoutLogSeat_;        // the ship, hull, seat and boarding the log last said
    // The next look reads the loadout at once: live mode switched on, a scan
    // asked for, the log saying the player sat down or got up
    void loadoutLookAgain();
    // ...and a new rock on view, when the last reading is older than this:
    // the verdict about to be drawn rests on the loadout
    void loadoutCheckForRock(float mass);
    static constexpr int LOADOUT_ROCK_CHECK_S = 30;
    bool loadoutRockCheck_ = false;     // a new rock asked for a reading...
    std::chrono::steady_clock::time_point loadoutRockCheckSince_{};   // ...at this moment
    // What the rock on view holds (RockContents): the composition list under
    // the panel's figures, read off the window's thread while the view has
    // none for this rock, and handed to it once two readings say the same.
    void readContentsOnHud(const cv::Mat& frame, const ScanPanel& panel, const RockStats& rock);
    void takeRockContents(const RockContents& read, const RockStats& rock);
    void clearRockView();                    // the rock off the view, and its list with it
    std::future<RockContents> contentsJob_;  // a reading under way
    float contentsJobMass_ = 0;              // ...and the rock it was asked for
    float contentsFor_ = 0;                  // the mass of the rock the view's list is of; 0: it has none
    std::string contentsOre_;                // ...and its ore
    std::string contentsShown_;              // ...and the list itself, as one line
    float contentsWanted_ = 0;               // the rock a list is being read for
    std::string contentsWantedOre_;
    std::string contentsSeen_;               // the last reading that held together, of whichever rock
    int contentsTries_ = 0;                  // readings asked for it so far
    std::chrono::steady_clock::time_point lastContentsRead_{};
    int tradesSeen_ = -1;                    // the trade book's changes the window has been told of
    // The refinery's terminal (RefineryTerminal, RefineryOrders). Placing a
    // work order writes nothing to the game's log, so the REFINEMENT CENTER
    // is read off the screen - and only there: at a station the tables know
    // a refinery at, the picture is looked at once a second for a work
    // order's panel (some 10 ms), and only while one is up is it read, off
    // the window's thread. What two readings in a row agree on goes into
    // the book, which the Refinery window shows.
    void lookForRefinery();
    // Each work order read whole goes to the UEX report once
    void handRefineryOrdersToUex();
    // What the order book made of each quote and order, and what came of
    // each job at UEX, a line each with its time: debug\refinery_jobs.txt,
    // to see why a job did not go
    void keepRefineryNotes(const std::vector<std::string>& lines);
    std::chrono::steady_clock::time_point refineryHandCheck_{};
    void takeRefineryScreen(const RefineryScreen& read, const std::string& station, long long taken);
    void pushRefineryOrders();
    RefineryOrderBook refineryOrders_;
    std::future<RefineryScreen> refineryJob_;   // a reading under way
    std::string refineryJobStation_;            // ...where
    long long refineryJobTaken_ = 0;            // ...and when its picture was taken (UTC)
    std::chrono::steady_clock::time_point lastRefineryLook_{};
    bool refineryUp_ = false;                   // the last look found a work order's panel
    std::string refineryLastStation_;           // the station of the last reading with panels on it...
    std::chrono::steady_clock::time_point refineryLastPanels_{};   // ...and when
    // Refinery workloads (RefineryShare): what the terminals show of them,
    // read by the player - shown in the Refinery window, and sent unless the
    // player takes the tick off in it (sharerefinery=0) where the build
    // carries the service's address (BEDROCK_REFINERY_URL), LIVE's refineries
    // only - and what other players read, fetched while the Refinery window
    // is open
    bool refineryShare_ = true;
    struct WorkloadReading {
        int capacity = -1, workload = -1;
        long long at = 0;    // UTC seconds
    };
    std::map<std::string, WorkloadReading> workloadLast_;   // the last read per station, for the next to agree with
    std::map<std::string, WorkloadReading> workloadMine_;   // ...what two in a row agreed on
    std::map<std::string, WorkloadReading> workloadSent_;
    std::string quoteLast_;                                 // a quote read, for the next reading to agree with
    std::set<std::string> quotesSent_;
    std::deque<std::string> shareQueue_;                    // reports waiting to go
    std::string shareSending_;
    std::future<int> shareJob_;                             // one going: the status it was answered with
    std::chrono::steady_clock::time_point shareRetryAt_{};
    std::future<std::pair<std::string, std::map<std::string, RefineryShare::StationState>>> stationsJob_;
    std::chrono::steady_clock::time_point stationsAsked_{};
    std::map<std::string, RefineryShare::StationState> stationsShared_;
    void shareRefineryScreen(const RefineryScreen& read, const std::string& station, long long taken);
    bool queueRefineryShare(const RefineryShare::Reading& reading);   // false where it does not go
    void serviceRefineryShare();
    void pushRefineryWorkloads();
    void setRefineryShare(bool on);
public:
    // The service's address in this build ("" for none) - a workshop build
    // can be pointed elsewhere with SCMINER_REFINERY_URL
    static std::string refineryServiceUrl();
private:
    // When a rock's scan panel was last on screen: the player is mining, and
    // no terminal is in reach - in a MOLE's turret the log has nobody in a
    // seat, and its cockpit's orange light can take a terminal's shape
    std::chrono::steady_clock::time_point lastRockPanel_{};
    static constexpr const char* REFINERY_ORDERS_FILE = "refinery_orders.txt";
    static constexpr const char* PLACE_OVERRIDE_FILE = "place.txt";
    void loadPlaceOverride();
    void setPlaceOverride(const std::string& placeName);
    void setGameTextsWanted(bool wanted);   // the box in the Blueprints window
    void applyGameState();
    std::string gameLogOverride_;    // region_config.txt: gamelog=<path to Game.log>
    // region_config.txt: update=0 stops the app ever looking for a newer
    // version, updateurl= points it somewhere else. Looking sends nothing
    // but the request itself, and installs nothing without being asked.
    bool updateCheck_ = true;
    // stats=0: the update check and the refinery workloads go without the
    // install's own number (Updater::installId), and only the version and
    // platform are sent
    bool statsWanted_ = true;
    // ...the number itself, "" with stats=0: sent with the update check and
    // with the refinery workloads (X-Bedrock-Install)
    std::string installId_;
    std::vector<std::string> installHeader() const;
    std::string updateUrl_;
    Updater updater_;
    // Asked at start and once an hour after that: a companion window can stay
    // up for days, and the next start may be that far away.
    std::chrono::steady_clock::time_point lastUpdateCheck_{};
    static constexpr int UPDATE_RECHECK_MINUTES = 60;
    std::chrono::steady_clock::time_point lastLibraryPoll_;
    size_t lostHullsPassed_ = 0;     // GameState::lostHulls() already given to the window
    int libraryLooks_ = 0;           // polls without a log, for looking again
    bool libraryPushed_ = false;     // the window has seen the library once
    bool ranksPushed_ = false;       // ...and the ranks
    bool ranksKnown_ = false;        // the logs read to the end, so the ranks mean something
    // The contracts running in the game and the ones finished lately, as the
    // window was last told (see pushContracts)
    bool contractsPushed_ = false;
    int runningSeen_ = -1;
    std::set<std::string> ordersSaid_;   // gathering orders the log line was written for, by MissionId
    int gadgetChangesSeen_ = 0;          // GameState::gadgetChanges(), as last acted on
    bool sessionQuiet_ = false;          // the log has not been written to for a quarter of an hour
    // The box in the Blueprints window: Bedrock's marks in the game's own
    // contract texts, rewritten when the library changes - see GameTexts.
    // gametexts= in region_config.txt. Off until asked for: it writes into
    // the game's folder.
    bool gameTextsWanted_ = false;
    bool settingsHeld_ = false;      // a backup was put back: region_config.txt is its until the next start
    bool gameTextsDue_ = true;       // a write is owed - once per run, and after each change
    std::chrono::steady_clock::time_point gameTextsDueAt_{};
    std::future<GameTexts::Outcome> gameTextsJob_;   // the write in flight, off the UI thread
    void keepGameTextsFresh();
    std::chrono::steady_clock::time_point lastGameTextsCheck_{};
    std::map<std::string, long long> gameTextsAnswered_;   // per channel: the patch already written or cleared for
    std::string gameTextsNote_;      // what the last one said, for the window
    // The Ore Finder's prices from UEX (uexcorp.space, see UexPrices). Asked
    // for only while the Ore Finder is open: the price list once the one
    // kept is half an hour old, the terminal list once a week; both kept in
    // the data folder's uex\ between runs. What goes along is the version
    // and the platform in the User-Agent, and Bedrock's app token where the
    // build has one (CMakeLists.txt) - nothing of the player. uex=0 in
    // region_config.txt and it is never asked.
public:
    struct UexOutcome {
        std::shared_ptr<const UexPrices> prices;   // null where there is nothing to show
        long long fetchedAt = 0;                   // when the price list was fetched, Unix seconds
        std::string note;                          // why the price list is not fresh; "" when it is
        std::string placesNote;                    // why the offers have no places (the terminal list)
        bool tokenRefused = false;                 // UEX turned Bedrock's app token away, and was asked without it
    };
    // The two lists, kept in `folder` and fetched where they are too old -
    // waits for UEX, so never on the UI thread (--uexprobe calls it as well)
    static UexOutcome loadUex(const std::filesystem::path& folder);
    // Whether the build carries an app token, for the self-test (the token
    // itself goes no further)
    static std::string uexTokenForProbe();
    // ...and the marketplace's listings of one ore, told as lines - no
    // player's name in them
    static std::string uexMarketForProbe(const std::filesystem::path& folder, const std::string& ore);
private:
    bool uexWanted_ = true;
    bool uexPushed_ = false;                       // the window has been told whether it is on
    std::future<UexOutcome> uexJob_;               // the asking in flight, off the UI thread
    std::shared_ptr<const UexPrices> uexPrices_;
    long long uexFetchedAt_ = 0;
    std::string uexNote_;
    bool uexTried_ = false;
    std::chrono::steady_clock::time_point uexTriedAt_{};
    static constexpr int UEX_PRICES_MINUTES = 30;
    // ...and after a failure, again only after this long, and only while
    // the Ore Finder is still open
    static constexpr int UEX_RETRY_MINUTES = 10;
    // Reporting to UEX (UexReports.h), opt-in: uexreport= in
    // region_config.txt is 0, or the Unix second the player agreed. What goes
    // is the player's refinery jobs, to their own account there. The key is
    // in uex_key.dat.
    struct UexPending {
        std::string key;                   // what makes it once-only
        UexRefineryJob job;
    };
    struct UexSent {
        std::string key, note, user;
        bool keyTrouble = false;           // UEX turned the key away
        std::string accountTrouble;        // ...or the account behind it (UexReports::accountTrouble)
        bool answered = false;             // UEX answered at all - not sent, it says nothing of the account
    };
    long long uexReportSince_ = 0;
    bool uexOffered_ = false;              // uexoffered= : the first start's offer was made
    int uexKeyChanges_ = 0;                // every key kept, for the window
    std::string uexKey_;                   // never logged, never in the config
    bool uexKeyTrouble_ = false;           // ...until the key changes, nothing more is sent
    std::string uexAccountTrouble_;        // what UEX said of the account at the last report, for the window
    std::deque<UexPending> uexQueue_;
    std::set<std::string> uexReported_;    // done, and kept in uex\reported.txt
    bool uexReportedRead_ = false;
    std::future<UexSent> uexSendJob_;
    std::future<std::shared_ptr<UexDirectory>> uexDirectoryJob_;
    std::shared_ptr<const UexDirectory> uexDirectory_;
    std::chrono::steady_clock::time_point uexDirectoryTried_{};
    bool uexDirectoryAsked_ = false;
    std::string uexReportNote_;            // what came of the last report or press, for the window
    long long uexReportNoteAt_ = 0;
    std::string uexUser_;                  // the name UEX answered with
    // The UEX marketplace (UexMarket.h): the listings of the ore the Ore
    // Finder's UEX tab shows, and of a rock's part of quality 900 or more
    // (the rock view says what a player pays for it); each ore's kept ten
    // minutes. Public and anonymous - no key, no opt-in, uex=0 stops it.
    struct UexMarketAnswer {
        std::string ore;
        std::shared_ptr<std::vector<UexListing>> listings;   // null where there was no answer
        std::shared_ptr<std::map<std::string, int>> items;   // the marketplace's items, where they were read now
        std::string note;
    };
    static UexMarketAnswer loadUexMarket(const std::filesystem::path& folder, const std::string& ore,
                                         std::shared_ptr<const std::map<std::string, int>> items);
    std::future<UexMarketAnswer> uexMarketJob_;
    std::shared_ptr<const std::map<std::string, int>> uexMarketItems_;
    std::map<std::string, std::chrono::steady_clock::time_point> uexMarketAsked_;
    std::vector<std::string> uexRockOres_;     // the rock on view's ores of quality 900 or more
    void serviceUexMarket();
    // ...and, with the player's key, their own conversations there - so a
    // listing they are talking about opens at their thread - every two
    // minutes while the UEX tab is up
    std::future<std::shared_ptr<UexThreads>> uexThreadsJob_;
    std::chrono::steady_clock::time_point uexThreadsAsked_{};
    bool uexThreadsEver_ = false;
    static std::shared_ptr<UexThreads> loadUexThreads(const std::string& key);
    static constexpr int UEX_MARKET_MINUTES = 10;
    static constexpr int UEX_MARKET_QUALITY = 900;
    static std::shared_ptr<UexDirectory> loadUexDirectory(const std::filesystem::path& folder);
    cv::Mat grabGamePicture();
    void serviceUexReports();
    void readUexReported();
    void rememberUexReported(const std::string& key);
    void pushUexAccount();
    void uexAccountAction(int action, const std::string& text);
    std::string currentSystem_;   // "" until Game.log names one - see MinerGUI

    std::atomic<bool> running_ = false;
    std::atomic<bool> scanRequested_ = false;
    std::atomic<bool> liveModeActive_ = false;
    // Live scan interval. Kept short because signatures can flash up for only
    // a moment (in-game bug), and a missed frame means a missed reading.
    int captureIntervalMs_ = 300;
    // What the scan is allowed to cost when nothing is coming back. A
    // scan is expensive - it takes most of a sixteen-core machine, and on
    // a smaller one it takes the lot. None of this ever STOPS the scan:
    // every judgement here about what the player is doing is a guess, and a
    // wrong guess has to cost time, never a reading. ease=0 in
    // region_config.txt turns it off.
    bool easeIdle_ = true;
    // How many Ctrl+Alt+S pictures to keep. Ten for a report; a few
    // hundred while a new cockpit's markers are being collected - seventy
    // of them at ten megabytes each is what a template set is built from,
    // and a cap of ten throws away all but the last handful.
    int snapshotKeep_ = 50;   // a tester's snapshots are the data that comes back
    int scanThreads_ = 0;           // 0 = worked out from the machine
    int effectiveIntervalMs() const;
    int hotkeyVkCode_ = VK_F9;      // default: F9

    // Live mode: last recognized signature (to avoid duplicate updates)
    int lastRecognizedSignature_ = 0;
    std::chrono::steady_clock::time_point lastScanTime_;

    CaptureRegion signatureRegion_;
    bool regionDetected_ = false;
    // The aim box follows the game: derived from its viewport - the monitor
    // the game is drawn on, scaled to its size - unless the user typed one
    bool aimAuto_ = true;
    cv::Rect aimViewport_;           // the viewport the automatic box was made for
    // viewport height over the reader's reference height - see
    // GlyphReader::REFERENCE_VIEWPORT_HEIGHT; re-read before every scan
    double hudScale_ = 1.0;
    // region_config.txt: hudscale=100 leaves every picture as captured. On
    // 1440p footage shown at 1.5x (4K) and 0.75x (1080p) it read 164 / 94
    // frames right, against 153 / 78 with the locator brought to the
    // reference scale and 146 / 75 with locator and reader both. 0 follows
    // the picture (1.5 on 4K), another number forces that factor; both are
    // for measuring on a real 4K capture.
    int hudScalePercent_ = 100;
    // region_config.txt: uiscale=, the app's own zoom. 0 means the
    // window has never been told, so it keeps whatever it worked out
    // from the screen.
    int uiScalePercent_ = 0;
    std::string captureStats_;       // size and light of the first capture, for the notes
    bool snapshotPending_ = false;   // Ctrl+Alt+S arrived while a scan ran

    // Auto-tracking: with head tracking (Tobii) the marker is anchored to the
    // rock in 3D, so it slides across the screen as the view moves. Instead of
    // cropping fixed coordinates, search a generous area around the configured
    // region and follow the label itself.
    std::atomic<bool> autoTrack_ = true;
    int trackMargin_ = 250;          // how far around the region to search, px (see defaultAimBox)
    cv::Rect lastLabelRect_;         // last located label, virtual screen coords
    bool hasLastLabel_ = false;
    // Scans since the locked marker was last confirmed. The lock survives a
    // run of them instead of being dropped by the first empty frame.
    int lockMisses_ = 0;
    // The marker as it last read. Holding a rectangle only helps while the
    // marker stays inside it, and it does not: the label is anchored to a rock
    // in 3D, so looking around slides it across the screen. Matching this
    // picture inside the search window drags the lock along with it, through
    // the runs of frames where the value itself cannot be read.
    cv::Mat lockPatch_;
    // Detecting a lock that has settled on cockpit furniture: the same value
    // at the same pixel, scan after scan
    // Scans the lock has been carried by picture matching alone, without a
    // reading confirming it, and whether the last match failed
    int followOnly_ = 0;
    bool followMissed_ = false;
    int lockLost_ = 0;               // scans in a row the follow found it nowhere - see LOCK_LOST_AFTER
    bool followedNow_ = false;       // this scan's follow found the marker: the locator may look near it only
    // The previous scan of the allowed area, greyscale, for measuring how far
    // the view turned between two scans
    cv::Mat previousView_;
    int stuckValue_ = 0;
    cv::Rect stuckRect_;
    int stuckCount_ = 0;
    cv::Rect blacklistRect_;    // ignored while blacklistLeft_ counts down
    int blacklistLeft_ = 0;
    bool lastScanTracked_ = false;   // was the last capture taken from a found label?

    // Overlay window for visualizing the capture region
    HWND overlayHwnd_ = nullptr;
    // The cyan frame on screen, off by default: the box is worked out from
    // the game window, so there is nothing to drag into place, and a frame
    // that only shows while LIVE is off tells the player nothing.
    // overlay=1 in region_config.txt brings it back for a look.
    bool overlayEnabled_ = false;
    void createOverlayWindow(HINSTANCE hInstance);
    void updateOverlayPosition();
    void destroyOverlay();
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    // Persisting the capture region
    void saveRegionConfig();
    void loadRegionConfig();
    // The "what's new" card, once per version - see the definition for why
    // a first-ever run is not an update
    void maybeShowWhatsNew();
    // Game.log: what the user pinned, else the running game's own folder,
    // else the folder the launcher last started from; empty when none
    std::filesystem::path locateGameLog() const;
    void pollLibrary();              // a step of log reading, now and then
    void pushLibrary();              // hands the window what is owned
    void pushRanks();                // ...and the player's rank with each outfit
    void pushContracts();            // ...and the contracts of the moment
    void applyGadget();              // a gadget stuck on a rock, or taken off it
    void serviceGameTexts();         // starts an owed write, collects a finished one
    void serviceUex();               // asks UEX when the Ore Finder wants prices, collects the answer

public:
    // Callback for overlay movement (called from OverlayProc)
    void onOverlayMoved(int x, int y, int w, int h);
    // A refinery job read off the REFINEMENT CENTER - the terminal reader
    // hands each one over once it has its method, cost, time and every
    // ore's quantity and yield: reported to the player's UEX account when
    // they have opted in, once a job
    void reportRefineryJob(const UexRefineryJob& job);

private:
    ResultCallback resultCallback_;

    // Domain knowledge for the OCR consensus vote: repair misread digits
    // against the signature table and reward exact hits
    OCREngine::ReadingPolicy readingPolicy();

    // One possible signature label: the cropped image and where it came from
    struct LabelCandidate {
        cv::Mat image;
        cv::Rect screenRect;
        cv::Rect row;   // the digit row inside `image` as the locator saw it; empty if unknown
        // The picture `image` was cut from, and where its corner sat in it -
        // shared, not copied. The second reader cuts its line from here, so
        // the room around the digits does not stop at the candidate's edge.
        cv::Mat source;
        cv::Point origin;
    };

    // Grabs everything that could be the signature label, best guess first.
    // Other warm HUD elements can look like markers, so the scan tries them
    // in order and keeps the one that actually reads as a table value.
    std::vector<LabelCandidate> captureSignatureCandidates(int maxCandidates = 3);

    // Grabs the image the OCR should read: either the located signature label
    // (auto-tracking) or the configured region. outScreenRect receives where
    // it was taken from, in virtual screen coordinates.
    cv::Mat captureSignatureImage(cv::Rect* outScreenRect = nullptr);

    // Switches live scanning (and with it the rolling recording) on or off.
    // Shared by the LIVE button and by the restart that resumes the previous
    // state, so the two can never drift apart.
    void applyLiveMode(bool active);

    // Copies the rolling recording aside so the ring cannot overwrite it.
    // Bound to a global hotkey: a missed reading is only ever noticed while
    // playing, and by the time it has been reported the clips holding it are
    // long gone.
    void keepRecording();

    // Sets the reader's HUD scale from the picture the game is drawn in:
    // its window when it is running, the monitor under the aim box until
    // then. True when it changed.
    bool refreshHudScale();

    // Ctrl+Alt+S: the game picture as it is captured, with a note of
    // everything the scan works from, to debug/snapshot_*.png|txt - the
    // material for a report of "it reads nothing here". snapshotKeep_ at most.
    void requestSnapshot();
    void saveSnapshot();

    // debug/session.txt: what this run works with (screen, viewport, aim
    // box, scale, capture path). One file, rewritten, never growing.
    void writeSessionInfo();
    void describeSetup(std::ostream& out) const;
    // A copy of region_config.txt in debug/, so that folder alone is the
    // whole report: settings, session, log and snapshots in one go
    void mirrorConfigToDebug() const;
    void noteCapture(const cv::Mat& image);

    // The hard boundary the scan may never look outside: the region the user
    // marked, grown by the tracking margin, clipped to the game picture.
    cv::Rect allowedArea() const;

    // How far the view turned since the last scan, in pixels, measured from
    // the picture itself. Moves the lock with it.
    void trackCameraMotion(const cv::Mat& view);

    // The area searched for labels when tracking, in virtual screen coords
    CaptureRegion trackingSearchArea() const;

    // Moves the lock onto wherever the marker has drifted to, by matching the
    // last picture of it inside the freshly captured search window. Called
    // when a scan read nothing: the plate is usually still on screen and the
    // view has only turned.
    void followLockedMarker(const cv::Mat& searchImage, const CaptureRegion& search);

    // Processes a single scan cycle (console mode, auto-detects the region)
    void processScan();

    // Live mode scan (quiet, only reports changes)
    void processScanLive();

    // Scan with GUI update
    void processScanWithGUI();

    // Prints the results to the console
    void displayResults(const std::vector<AnalysisResult>& results, int scannedValue);

    // Compact one-line output for live mode
    void displayResultsCompact(const std::vector<AnalysisResult>& results, int scannedValue);

    // Hotkey handling
    void setupHotkey();
#ifdef _WIN32
    static LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
#endif
};

} // namespace SCMiner
