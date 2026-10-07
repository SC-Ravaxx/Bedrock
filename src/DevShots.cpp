// The screenshot run: every window of the app photographed into a folder,
// on both platforms.
//
// Shared so the Linux build takes the same pictures through the same code:
// the Windows pictures are the golden set, and a Linux picture that matches
// one is the proof that the shim carries the window (docs/linux-port.md).
#include "DevShots.h"

#if defined(BEDROCK_DEV_TOOLS) || !defined(_WIN32)

#include "DevSwitch.h"
#include "AppPaths.h"
#include "MinerGUI.h"
#include "SignatureAnalyzer.h"
#include "MiningHardware.h"
#include "RefineryData.h"
#include "CraftingData.h"
#include "UexPrices.h"
#include "UexMarket.h"
#include <ctime>
#include <memory>

#include <Windows.h>
#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <dwmapi.h>

// UEX's prices for the Ore Finder's pictures: terminals and prices from a
// snapshot of UEX, each report's age counted back from now - so the
// pictures say the same whenever they are taken
static std::shared_ptr<const SCMiner::UexPrices> shotPrices(long long now) {
    struct Terminal { int id; const char* system; const char* planet; const char* moon; const char* orbit; const char* station; };
    struct Price { int terminal; const char* name; const char* commodity; int sell; int buy; int sellStock; int buyStock; double days; };
    static const Terminal terminals[] = {
        {1, "Stanton", "ArcCorp", nullptr, "ArcCorp Lagrange Point 1", "ARC-L1 Wide Forest Station"},
        {10, "Stanton", "Crusader", "Daymar", "Crusader", nullptr},
        {12, "Stanton", "ArcCorp", nullptr, "ArcCorp", nullptr},
        {18, "Stanton", "Hurston", nullptr, "Hurston", nullptr},
        {23, "Stanton", "Crusader", "Yela", "Crusader", nullptr},
        {25, "Stanton", "Hurston", nullptr, "Hurston", "Everus Harbor"},
        {28, "Stanton", "Hurston", "Aberdeen", "Hurston", nullptr},
        {34, "Stanton", "Hurston", "Aberdeen", "Hurston", nullptr},
        {35, "Stanton", "Hurston", nullptr, "Hurston", nullptr},
        {39, "Stanton", "Hurston", nullptr, "Hurston", nullptr},
        {41, "Stanton", "Hurston", "Ita", "Hurston", nullptr},
        {66, "Stanton", "MicroTech", nullptr, "MicroTech", "Port Tressler"},
        {72, "Stanton", "MicroTech", "Calliope", "MicroTech", nullptr},
        {82, "Stanton", "MicroTech", nullptr, "MicroTech", nullptr},
        {89, "Stanton", "MicroTech", nullptr, "MicroTech", nullptr},
        {90, "Stanton", "Crusader", nullptr, "Crusader", nullptr},
        {486, "Pyro", "Pyro V", nullptr, "Pyro V Lagrange Point 4", "Rod's Fuel 'N Supplies"},
        {507, "Pyro", "Pyro V", "Ignis", "Pyro V", nullptr},
        {523, "Pyro", "Pyro V", nullptr, "Pyro V Lagrange Point 5", "Rat's Nest"},
        {549, "Pyro", "Terminus", nullptr, "Pyro VI Lagrange Point 4", "Dudley & Daughters"},
        {591, "Pyro", "Terminus", nullptr, "Terminus", nullptr},
        {605, "Pyro", "Pyro IV", nullptr, "Pyro V", nullptr},
        {665, "Pyro", "Bloom", nullptr, "Bloom", nullptr},
        {778, "Nyx", nullptr, nullptr, "Delamar", nullptr},
        {802, "Nyx", nullptr, nullptr, "Stanton Gateway (Nyx system)", "Stanton Gateway (Nyx)"},
    };
    // terminal, its name, commodity, pays, asks, stock (sell), stock (buy), days since reported
    static const Price prices[] = {
        {90, "TDD Orison", "Quantainium", 170000, 0, 1, 0, 3.7},
        {10, "ArcCorp 141", "Quantainium", 170000, 0, 1, 0, 4.3},
        {591, "Canard View", "Quantainium", 170000, 0, 1, 0, 8.2},
        {605, "Fallow Field", "Quantainium", 170000, 0, 1, 0, 11.5},
        {507, "Ashland", "Quantainium", 160000, 0, 1, 0, 4.5},
        {665, "The Golden Riviera", "Quantainium", 160000, 0, 1, 0, 8.2},
        {486, "Rod's Fuel", "Laranite", 9700, 0, 1, 0, 0.4},
        {802, "Stanton Gateway (Nyx)", "Laranite", 9200, 0, 1, 0, 4.8},
        {12, "TDD Area 18", "Laranite", 9100, 0, 1, 0, 2.6},
        {549, "Dudley & Daughters", "Laranite", 9100, 0, 4, 0, 5.8},
        {89, "TDD New Babbage", "Laranite", 9000, 0, 1, 0, 3.6},
        {18, "CBD Lorville", "Laranite", 8800, 0, 1, 0, 4.4},
        {66, "Port Tressler", "Aluminum", 4500, 0, 1, 0, 2.1},
        {25, "Everus Harbor", "Aluminum", 4000, 0, 1, 0, 3.7},
        {12, "TDD Area 18", "Aluminum", 3800, 0, 1, 0, 2.6},
        {89, "TDD New Babbage", "Aluminum", 3800, 0, 1, 0, 3.6},
        {90, "TDD Orison", "Aluminum", 3800, 0, 1, 0, 3.7},
        {18, "CBD Lorville", "Aluminum", 3800, 0, 1, 0, 4.4},
        {18, "CBD Lorville", "Gold", 31000, 0, 1, 0, 6.5},
        {1, "ARC-L1", "Gold", 30000, 0, 5, 0, 2.4},
        {89, "TDD New Babbage", "Gold", 30000, 0, 5, 0, 3.6},
        {90, "TDD Orison", "Gold", 30000, 0, 4, 0, 3.7},
        {507, "Ashland", "Gold", 29000, 0, 1, 0, 4.5},
        {523, "Rat's Nest", "Gold", 29000, 0, 6, 0, 8.3},
        {23, "Deakins Research", "Stileron", 150000, 0, 1, 0, 2.0},
        {72, "Rayari Kaltag", "Stileron", 140000, 0, 1, 0, 0.5},
        {665, "The Golden Riviera", "Savrilium", 150000, 0, 1, 0, 8.2},
        {90, "TDD Orison", "Savrilium", 130000, 0, 1, 0, 3.7},
        {802, "Stanton Gateway (Nyx)", "Quantainium", 150000, 0, 1, 0, 4.8},
        {778, "Levski", "Quantainium", 140000, 0, 1, 0, 4.5},
        {41, "HDMS-Woodruff", "Aphorite", 110000, 0, 1, 0, 0.7},
        {41, "HDMS-Woodruff", "Beradom", 170000, 0, 1, 0, 0.7},
        {82, "Shubin SM0-18", "Dolivine", 160000, 0, 1, 0, 2.7},
        {41, "HDMS-Woodruff", "Feynmaline", 390000, 0, 1, 0, 0.7},
        {35, "HDMS-Oparei", "Glacosite", 120000, 0, 1, 0, 1.0},
        {41, "HDMS-Woodruff", "Hadanite", 600000, 0, 1, 0, 0.8},
        {39, "HDMS-Stanhope", "Janalite", 1800000, 0, 1, 0, 0.8},
        {12, "TDD Area 18", "Sadaryx", 500000, 0, 1, 0, 2.6},
        {28, "HDMS-Anderson", "Gold", 0, 24360, 0, 3, 2.2},
        {34, "HDMS-Norgaard", "Gold", 0, 24360, 0, 4, 2.8},
    };
    auto text = [](const char* s) { return s ? "\"" + std::string(s) + "\"" : std::string("null"); };
    std::string places = "{\"status\":\"ok\",\"data\":[";
    for (const Terminal& t : terminals) {
        if (places.back() != '[') places += ",";
        places += "{\"id\":" + std::to_string(t.id) + ",\"star_system_name\":" + text(t.system) +
                  ",\"planet_name\":" + text(t.planet) + ",\"moon_name\":" + text(t.moon) +
                  ",\"orbit_name\":" + text(t.orbit) + ",\"space_station_name\":" + text(t.station) + "}";
    }
    places += "]}";
    std::string list = "{\"status\":\"ok\",\"data\":[";
    for (const Price& p : prices) {
        if (list.back() != '[') list += ",";
        list += "{\"id_terminal\":" + std::to_string(p.terminal) + ",\"terminal_name\":" + text(p.name) +
                ",\"commodity_name\":" + text(p.commodity) + ",\"price_sell\":" + std::to_string(p.sell) +
                ",\"price_buy\":" + std::to_string(p.buy) + ",\"status_sell\":" + std::to_string(p.sellStock) +
                ",\"status_buy\":" + std::to_string(p.buyStock) +
                ",\"date_modified\":" + std::to_string(now - (long long)(p.days * 86400)) + "}";
    }
    list += "]}";
    auto out = std::make_shared<SCMiner::UexPrices>();
    std::string why;
    if (!out->read(list, places, now, why)) std::cerr << "shot prices: " << why << std::endl;
    return out;
}

// The UEX marketplace for the pictures: listings from a snapshot of the
// marketplace for Ouratite - prices, qualities, places and ages - under
// made-up names, and when each was put up and last renewed. Counted back
// from now.
static std::shared_ptr<const std::vector<SCMiner::UexListing>> shotListings(const std::string& ore, long long now) {
    struct Row {
        bool buying; double price; const char* unit; int quality; const char* title; const char* place; const char* user;
        double days; double renewed;   // days since it went up; since its player last renewed it (0: never)
    };
    static const Row ouratite[] = {
        {true, 81000000, "unit", -1, "WTB 1000Q Ouratite", "", "Kestrel_Forge", 0.5, 0},
        {true, 80000000, "scu", 1000, "WTB Ouratite q1000", "", "Arclight_Mats", 1, 0},
        {true, 30000000, "scu", 960, "WTB Ouratite Q960", "", "Halcyon_Trade", 82, 8},
        {true, 25000000, "unit", 960, "Ouratite", "", "Moss_Crafting", 0.2, 0},
        {true, 10000000, "scu", 960, "Ouratite Q960", "New Babbage", "Old_Ledger", 40, 0},
        {false, 35000000, "scu", 960, "960q Ouratite - 35M/SCU - 1.7 SCU in stock", "Ruin Station", "Ruin_Runner", 3, 0},
        {false, 34000000, "scu", 960, "Ouratite 960Q", "CRU-L1", "Vela_Mining", 6, 0},
        {false, 30000000, "unit", 960, "Ouratite", "Levski", "Delamar_Rocks", 9, 0},
        {false, 12000000, "scu", 896, "Ouratite 896", "Area 18", "Dust_Collector", 63, 21},
        {false, 2500000, "scu", 700, "Ouratite Q700, bulk", "Orison", "Cloudview_Ore", 2, 0},
    };
    static const Row hephaestanite[] = {
        {true, 1500000, "scu", 950, "WTB Hephaestanite 950+", "", "Forge_Seeker", 2, 0},
        {true, 900000, "scu", 900, "Buying Hephaestanite Q900", "", "Old_Ledger", 20, 0},
        {false, 1800000, "scu", 960, "Hephaestanite 960", "Levski", "Delamar_Rocks", 4, 0},
    };
    static const Row hadanite[] = {
        {true, 900000, "cscu", 800, "WTB Hadanite 800+", "", "Gem_Cutter", 1, 0},
        {false, 650000, "unit", -1, "Hadanite, cleaned", "GrimHEX", "Yela_Digger", 5, 0},
    };
    const Row* rows = nullptr;
    size_t count = 0;
    if (ore == "Ouratite") { rows = ouratite; count = sizeof(ouratite) / sizeof(ouratite[0]); }
    if (ore == "Hephaestanite") { rows = hephaestanite; count = sizeof(hephaestanite) / sizeof(hephaestanite[0]); }
    if (ore == "Hadanite") { rows = hadanite; count = sizeof(hadanite) / sizeof(hadanite[0]); }
    auto out = std::make_shared<std::vector<SCMiner::UexListing>>();
    for (size_t i = 0; i < count; i++) {
        SCMiner::UexListing listing;
        listing.id = (long long)i + 1;
        listing.slug = "shot-listing-" + std::to_string(i + 1);
        listing.title = rows[i].title;
        listing.buying = rows[i].buying;
        listing.price = rows[i].price;
        listing.unit = rows[i].unit;
        listing.currency = "UEC";
        listing.quality = rows[i].quality;
        listing.location = rows[i].place;
        listing.user = rows[i].user;
        listing.added = now - (long long)(rows[i].days * 86400);
        listing.renewed = rows[i].renewed > 0 ? now - (long long)(rows[i].renewed * 86400) : listing.added;
        out->push_back(listing);
    }
    std::sort(out->begin(), out->end(), [](const SCMiner::UexListing& a, const SCMiner::UexListing& b) { return a.price > b.price; });
    return out;
}

// Photographs the windows into <dir>: main.png, overlay.png, planner.png.
// The windows are created outside every monitor and never activated, so
// nothing shows on the desktop; the picture comes from the window's own
// composition surface (PrintWindow with PW_RENDERFULLCONTENT).
int runScreenshots(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // The run has a data folder of its own - never the player's: the hooks
    // that set a window up for a picture tick, trash and save exactly like
    // a click does. BEDROCK_SHOT_DATA names the folder; otherwise it is
    // data\ under the pictures. What is not in it yet is written: a few notes with
    // links in them, a list of ores to look for, and a bundle with a bad link,
    // for the picture of an import refused.
    std::filesystem::path data = dir + "/data";
    if (const char* d = DEV_ENV("BEDROCK_SHOT_DATA")) {
        if (*d) data = d;
    }
    SCMiner::AppPaths::useDataFolder(data);
    auto put = [&](const char* relative, const std::string& text) {
        const std::filesystem::path p = data / relative;
        if (std::filesystem::exists(p, ec)) return;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary);
        std::string crlf;
        for (char ch : text) {
            if (ch == '\n') crlf += "\r\n";
            else crlf += ch;
        }
        out.write(crlf.data(), (std::streamsize)crlf.size());
    };
    put("looking_for.txt",
        "# Bedrock: the ores you are looking for.\n"
        "Hephaestanite\n"
        "Quantainium\n");
    put("notes/Routes/Monox run.txt",
        "Monox, day side, in the MOLE.\n"
        "\n"
        "Worth stopping for:\n"
        "- 31,185 is [[Stileron]] beside a ROC field, north of the outpost\n"
        "- 22,180 is [[Hephaestanite]] with harvestables around it\n"
        "- 37,350 reads as [[Iron]] x5 in a salvage field\n"
        "\n"
        "Hold full at 96 SCU. Refine at Checkmate, then back to\n"
        "[[Pyro II (Monox)]] tomorrow for the rest.\n");
    // Every ore named here is one the tables put at the Aaron Halo.
    put("notes/Routes/Aaron Halo lap.txt",
        "Aaron Halo, working outwards from the mining base.\n"
        "\n"
        "[[Quantainium]] shows as 2 % all through it, so take what the scanner\n"
        "gives and do not go hunting. [[Titanium]] and [[Aslarite]] are what\n"
        "pay for the trip.\n"
        "\n"
        "Pyrometric Chromalysis on the way back - nothing is lost and it is\n"
        "not the twelve-hour wait.\n");
    // The three slots the recipe really has, and where those three are
    // really mined.
    put("notes/Crafting/What the Palatino Arms needs.txt",
        "[[Palatino Arms]] - the recipe is on the shopping list.\n"
        "\n"
        "Three slots: [[Laranite]], [[Aslarite]], [[Savrilium]].\n"
        "\n"
        "Laranite off [[Pyro IV]], Aslarite out of the [[Aaron Halo]], and the\n"
        "Savrilium only in Nyx - [[Glaciem Ring]] or the [[Keeger Belt]]. Three\n"
        "systems for one pair of arms.\n"
        "\n"
        "The fabricator is bought at any refinery, so the building happens\n"
        "wherever the hold is.\n");
    put("import_bad.txt",
        "BEDROCK NOTES 1\n"
        "NOTE\tRoutes\tShared by a friend\t3\n"
        "Try the far side of Monox first.\n"
        "The [[Quantanium]] there is 2 % like everywhere else, skip it.\n"
        "Bring water.\n");
    std::cout << "data folder: " << data.string() << std::endl;

    SCMiner::MinerGUI gui;
    gui.setHeadless(true);
    if (!gui.initialize(GetModuleHandle(nullptr))) return 1;
    gui.show();
    // The shot run does not read region_config.txt, so the zoom can be asked
    // for here: BEDROCK_SHOT_SCALE=250 is how the window sizes at the far end
    // of the range get looked at without starting the app.
    if (const char* z = DEV_ENV("BEDROCK_SHOT_SCALE")) {
        if (*z) gui.setUiScale(std::atoi(z));
    }
    // ...and the update button, whose whole point is that it is usually not
    // there: BEDROCK_SHOT_UPDATE=3 shows it waiting, 5 ready to install
    if (const char* u = DEV_ENV("BEDROCK_SHOT_UPDATE")) {
        if (*u) gui.setUpdateStatus(std::atoi(u), "4.9.2", "something worth having", 42);
    }

    // Where the pictures are taken: on Monox in Pyro, in a MOLE.
    SCMiner::SignatureAnalyzer analyzer;
    analyzer.setSystemFilter("PYRO");
    analyzer.setPlaceFilter("Pyro II (Monox)");
    analyzer.setGround(SCMiner::SignatureAnalyzer::Ground::Surface);
    gui.setCurrentSystem("Pyro");
    gui.setPlace("Pyro - Monox", analyzer.placeFilter());
    gui.setShip("MOLE", true);
    gui.setLiveModeActive(true);
    gui.setRegionValues(2384, 420, 352, 132);
    gui.setRegionAuto(true);
    gui.setTrackingStatus(true, true, 2462, 446);
    auto scanned = [&](int value, double read) {
        // ...in two steps: outOfPlace() answers for the LAST analysis, and
        // the order arguments are evaluated in is the compiler's to choose
        const auto results = analyzer.analyzeSignature(value);
        gui.updateResults(value, results, read, analyzer.outOfPlace());
    };
    // 31,185: the Stileron beside a ROC field that the place filter was
    // built for - five of its six readings need an ore Monox has not got
    scanned(31185, 94);

    auto pump = [&]() {
        for (int i = 0; i < 12; i++) {
            gui.processMessages();
            Sleep(20);
        }
    };
    // One window as a picture. A window read through PrintWindow is painted
    // now first: PrintWindow hands back what the window last painted, and at
    // BEDROCK_SHOT_SCALE=150 the glass takes longer than the pumping, so the
    // picture would show the step before.
    auto grab = [&](HWND target, bool direct, bool& ok) -> cv::Mat {
        ok = true;
        RECT rc{};
        GetClientRect(target, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return cv::Mat();
        if (!direct) {
            // Painted now, and then DWM given the frame: PRINTWINDOW with
            // PW_RENDERFULLCONTENT reads what DWM last took, and a paint
            // that is done is not yet taken
            RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
#ifdef _WIN32
            DwmFlush();
            DwmFlush();
#endif
        }
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        // The planner is painted straight into the bitmap: DWM never composes
        // a window parked at -32000 and PrintWindow then hands back white
        if (direct && target == gui.notesHandle()) gui.paintNotesTo(mem);
        else if (direct && target == gui.craftingHandle()) gui.paintCraftingTo(mem);
        else if (direct && target == gui.refineryHandle()) gui.paintRefineryTo(mem);
        else if (direct && target == gui.grindHandle()) gui.paintGrindTo(mem);
        else if (direct && target == gui.shotHandle()) gui.paintShotTo(mem);
        else if (direct && target == gui.newsHandle()) gui.paintNewsTo(mem);
        else if (direct) gui.paintPlannerTo(mem);
        else {
#ifdef _WIN32
            // The overlay is kept out of every capture (toggleOverlayMode),
            // PrintWindow's too, which leaves overlay_*.png black. Let in for
            // the picture, and kept out again.
            DWORD affinity = WDA_NONE;
            GetWindowDisplayAffinity(target, &affinity);
            if (affinity != WDA_NONE) {
                SetWindowDisplayAffinity(target, WDA_NONE);
                DwmFlush();
            }
#endif
            // Read twice: at BEDROCK_SHOT_SCALE=150 the first read of a
            // window that had just changed came back one step behind
            PrintWindow(target, mem, PW_CLIENTONLY | 0x2 /*PW_RENDERFULLCONTENT*/);
#ifdef _WIN32
            DwmFlush();
#endif
            ok = PrintWindow(target, mem, PW_CLIENTONLY | 0x2 /*PW_RENDERFULLCONTENT*/) != FALSE;
#ifdef _WIN32
            if (affinity != WDA_NONE) SetWindowDisplayAffinity(target, affinity);
#endif
        }
        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(bi);
        bi.biWidth = rc.right;
        bi.biHeight = -rc.bottom;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        cv::Mat img(rc.bottom, rc.right, CV_8UC4);
        GetDIBits(mem, bmp, 0, rc.bottom, img.data, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        cv::Mat bgr;
        cv::cvtColor(img, bgr, cv::COLOR_BGRA2BGR);
        return bgr;
    };
    auto save = [&](const cv::Mat& bgr, const std::string& name, bool ok) {
        cv::imwrite(dir + "/" + name, bgr);
        std::cout << name << ": " << bgr.cols << "x" << bgr.rows << (ok ? "" : " (PrintWindow failed)")
                  << ", mean " << cv::mean(bgr)[0] << std::endl;
    };
    auto shoot = [&](HWND target, const std::string& name, bool direct = false) {
        bool ok = true;
        const cv::Mat bgr = grab(target, direct, ok);
        if (bgr.empty()) {
            std::cout << name << ": window has no size" << std::endl;
            return;
        }
        save(bgr, name, ok);
    };

    pump();
    {
        wchar_t title[128] = {};
        GetWindowTextW(gui.windowHandle(), title, 127);
        std::wstring wide(title);
        std::cout << "window title: " << std::string(wide.begin(), wide.end()) << " (" << wide.size()
                  << " chars)" << std::endl;
    }
    // The first minute, before anything has been read: the borderless notice
    // is up and everything below it has to clear it.
    gui.firstRunForShot();
    pump();
    shoot(gui.windowHandle(), "first_run.png");
    scanned(31185, 94);
    pump();

    shoot(gui.windowHandle(), "main.png");
    // 22,180 holds Hephaestanite, which the demo list is looking for: the
    // row blinks, and the blink is held on for the picture - in the compact
    // overlay too
    scanned(22180, 91);
    gui.setPulseForShot(true);
    pump();
    shoot(gui.windowHandle(), "main_blink.png");
    // ...and a gathering order taken in the game: its ores blink as well,
    // and the field names what it wants
    for (int c = 0; c < SCMiner::REP_CONTRACT_COUNT; c++) {
        const SCMiner::RepContract& rc = SCMiner::REP_CONTRACTS[c];
        if (rc.oreCount != 2 || std::string(SCMiner::ORDER_ORES[rc.oreFirst].name) != "Hephaestanite") continue;
        SCMiner::MinerGUI::ContractNow order;
        order.contract = &rc;
        order.running = true;
        gui.setContractsNow({order});
        pump();
        shoot(gui.windowHandle(), "main_order.png");
        gui.setContractsNow({});
        pump();
        break;
    }
    gui.toggleOverlayMode();
    pump();
    shoot(gui.windowHandle(), "overlay.png");
    // ...and the same window with an update waiting. The chip in the corner
    // is the only thing in this mode a click can reach, and the only place
    // a player who never leaves the overlay is told there is one at all.
    gui.setUpdateStatus(3, "1.2.9", "a newer one", 0);
    pump();
    shoot(gui.windowHandle(), "overlay_update.png");
    // A chip nobody can press is a picture. Two things stand in the way of
    // one in this mode - the client area answers every hit test as a drag
    // handle, and the click handler turns around at the overlay guard - so
    // the harness presses it and says which of the two let it through.
    {
        const Gdiplus::RectF chip = gui.updateChipForShot();
        bool arrived = false;
        gui.setUpdateActionCallback([&arrived] { arrived = true; });
        POINT centre = {(LONG)(chip.X + chip.Width / 2), (LONG)(chip.Y + chip.Height / 2)};
        POINT onScreen = centre;
        ClientToScreen(gui.windowHandle(), &onScreen);
        const LRESULT where = SendMessageW(gui.windowHandle(), WM_NCHITTEST, 0,
                                           MAKELPARAM(onScreen.x, onScreen.y));
        SendMessageW(gui.windowHandle(), WM_LBUTTONDOWN, 0, MAKELPARAM(centre.x, centre.y));
        gui.setUpdateActionCallback(nullptr);
        std::cout << "overlay update chip: " << (chip.Width > 0 ? "drawn" : "MISSING")
                  << ", hit test " << (where == HTCLIENT ? "HTCLIENT" : "not client")
                  << ", click " << (arrived ? "arrived" : "LOST") << std::endl;
    }
    gui.setUpdateStatus(-1, "", "", 0);
    pump();
    gui.toggleOverlayMode();
    pump();
    gui.setPulseForShot(false);
    // 119,000: read right, and not to be had from the four ores the tables
    // list for Monox. The window says so rather than greying six guesses
    // from elsewhere out of the way.
    scanned(119000, 95);
    pump();
    shoot(gui.windowHandle(), "main_unlisted.png");
    // 28,000 out in open space at Nyx - the log naming the system itself
    // as the start of a route. Salvage x14 can be out there, a ROC field
    // of seven cannot; that row says "not in open space" rather than "not
    // in NYX", and the ship's own line says where it is
    analyzer.setSystemFilter("NYX");
    analyzer.setPlaceFilter("");
    analyzer.setGround(SCMiner::SignatureAnalyzer::Ground::Space);
    gui.setCurrentSystem("NYX");
    gui.setPlace("Nyx - open space", "", true);
    scanned(28000, 96);
    pump();
    shoot(gui.windowHandle(), "main_space.png");
    // 12,855 is Aluminium x3 and nothing else. In Stanton its list has
    // Lagrange C, five points long, and from Pyro the belt with a sentence
    // for a note: rows that take a second line. Then back to open space at
    // Nyx for what follows.
    analyzer.setSystemFilter("STANTON");
    gui.setCurrentSystem("Stanton");
    gui.setPlace("Stanton - open space", "", true);
    scanned(12855, 97);
    pump();
    shoot(gui.windowHandle(), "main_long_places.png");
    analyzer.setSystemFilter("NYX");
    gui.setCurrentSystem("NYX");
    gui.setPlace("Nyx - open space", "", true);
    scanned(28000, 96);
    pump();
    // The rock view: what the window shows while the game's scan panel is up.
    // The figures are the ones off the screenshot in tests/scanpanel - a Tin
    // rock of 22,593 at 28.4 m, read by a Helix I with Focus III and
    // Rieger-C3. At that distance the laser is at 55 %, so the advice is to
    // close in rather than to fit anything, which is the case worth having a
    // picture of.
    {
                // A ship picked, so the header shows the ship row rather than the
        // notice - and the loadout card has something to be about.
        gui.pickShipForShot("mole");
        // A MOLE fitted the way one actually gets fitted: a forgiving wide
        // head, a hard one, and an S1 Helix on an S2 mount - which the game
        // allows and which out-fractures half the S2 range.
        SCMiner::MiningLoadout fit;
        fit.laser = SCMiner::laserById("thcn_helix_s1");
        fit.modules = {SCMiner::moduleById("passive_focus_mk3"),
                       SCMiner::moduleById("passive_rieger_mk3")};
        gui.setHeadForShot(0, fit);
        SCMiner::MiningLoadout wide;
        wide.laser = SCMiner::laserById("shin_hofstede_s2");
        gui.setHeadForShot(1, wide);
        SCMiner::MiningLoadout hard;
        hard.laser = SCMiner::laserById("thcn_helix_s2");
        hard.modules = {SCMiner::moduleById("active_surge")};
        gui.setHeadForShot(2, hard);
        // ...and the same ship with a crafted head, the way the game prints
        // it on the item: "Laser Power: +0.00 % -> +28.84 %"
        SCMiner::MiningLoadout crafted = fit;
        crafted.craftedPower = 28.84f;

        SCMiner::RockStats rock;
        rock.ore = "TIN (ORE)";
        rock.mass = 22593;
        rock.resistance = 0;
        rock.instability = 3.52f;
        rock.scu = 30.21f;
        rock.distance = 28.4f;
        gui.updateRock(rock);
        pump();
        shoot(gui.windowHandle(), "rock_far.png");

        // ...and the same rock once the ship has closed in
        rock.distance = 12.0f;
        gui.updateRock(rock);
        pump();
        shoot(gui.windowHandle(), "rock_near.png");

        // ...and what it holds, once the panel's composition list was read:
        // the Tin rock of tests/scanpanel part by part - the ore's own two
        // parts first, each with its quality and its share - in the window
        // and in the overlay
        {
            SCMiner::RockContents holds;
            holds.parts = {{"Tin", false, 10.99f, 537}, {"Tin", false, 63.84f, 340}, {"Copper", false, 8.36f, 652},
                           {"", true, 16.79f, 0}};
            holds.whole = true;
            gui.setRockContents(holds);
            pump();
            shoot(gui.windowHandle(), "rock_contents.png");
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents.png");
            gui.toggleOverlayMode();
            pump();
            // An iron rock while an ore pod is on the shopping list, whose
            // recipe takes Iron of 800 and better: the part that has it is
            // green, the part that has not says what is asked.
            SCMiner::RockStats iron = rock;
            iron.ore = "IRON (ORE)";
            iron.mass = 31949;
            iron.scu = 42.1f;
            gui.updateRock(iron);
            SCMiner::RockContents ironHolds;
            ironHolds.parts = {{"Iron", false, 9.99f, 874}, {"Iron", false, 53.23f, 325}, {"", true, 36.77f, 0}};
            ironHolds.whole = true;
            gui.cartOnlyForShot("MISC Enhanced Ore Pod");
            gui.setRockContents(ironHolds);
            pump();
            shoot(gui.windowHandle(), "rock_contents_list.png");
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_list.png");
            gui.toggleOverlayMode();
            pump();
            // ...and a rock of four ores, the most a panel lists
            SCMiner::RockStats bexalite = rock;
            bexalite.ore = "BEXALITE (RAW)";
            bexalite.mass = 4627;
            bexalite.resistance = 56;
            bexalite.instability = 64.16f;
            bexalite.scu = 5.54f;
            gui.updateRock(bexalite);
            gui.cartOnlyForShot(nullptr);
            SCMiner::RockContents four;
            four.parts = {{"Bexalite", false, 7.42f, 597}, {"Bexalite", false, 57.0f, 302}, {"Gold", false, 4.85f, 553},
                          {"Borase", false, 4.36f, 676}, {"", true, 26.34f, 0}};
            four.whole = true;
            gui.setRockContents(four);
            pump();
            shoot(gui.windowHandle(), "rock_contents_four.png");
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_four.png");
            gui.toggleOverlayMode();
            pump();
            // ...and the same rock while Gold is ticked in "I'm looking for":
            // its name in amber and its figures on the red beat, in the
            // window and in the overlay - the beat held on, then off, where
            // the frame alone is left
            gui.lookForShot("Gold", true);
            gui.setPulseForShot(true);
            pump();
            shoot(gui.windowHandle(), "rock_contents_wanted.png");
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_wanted.png");
            gui.setPulseForShot(false);
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_wanted_off.png");
            gui.toggleOverlayMode();
            gui.lookForShot("Gold", false);
            pump();
            // ...and a rock with a quality of every tier the overlay colours:
            // a 1000, an 800s, a 600s and a low one
            SCMiner::RockContents tiers;
            tiers.parts = {{"Bexalite", false, 6.12f, 1000}, {"Bexalite", false, 55.30f, 318},
                           {"Gold", false, 5.07f, 861},      {"Borase", false, 4.36f, 676},
                           {"", true, 29.15f, 0}};
            tiers.whole = true;
            gui.setRockContents(tiers);
            pump();
            shoot(gui.windowHandle(), "rock_contents_tiers.png");
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_tiers.png");
            gui.toggleOverlayMode();
            pump();
            // ...and a Hephaestanite rock as a Golem recording read it: two
            // parts of its own, Silicon and Quartz beside them
            SCMiner::RockStats heph = rock;
            heph.ore = "HEPHAESTANITE (RAW)";
            heph.mass = 11284;
            heph.resistance = 21;
            heph.instability = 37.62f;
            heph.scu = 13.76f;
            gui.updateRock(heph);
            SCMiner::RockContents hephHolds;
            hephHolds.parts = {{"Silicon", false, 4.34f, 510}, {"Hephaestanite", false, 6.58f, 572},
                               {"Hephaestanite", false, 58.89f, 330}, {"Quartz", false, 4.47f, 522},
                               {"", true, 25.70f, 0}};
            hephHolds.whole = true;
            gui.setRockContents(hephHolds);
            pump();
            shoot(gui.windowHandle(), "rock_contents_heph.png");
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_heph.png");
            gui.toggleOverlayMode();
            pump();
            // ...and a Hephaestanite rock of every colour: its own part of the
            // full roll in Epic, its bulk (rolled times 0.49) Common, Quartz and
            // Silicon (times 0.789) Rare and Uncommon - qualities its bands have
            SCMiner::RockContents hephTiers;
            hephTiers.parts = {{"Silicon", false, 3.12f, 672}, {"Hephaestanite", false, 5.47f, 896},
                               {"Hephaestanite", false, 61.02f, 330}, {"Quartz", false, 4.08f, 710},
                               {"", true, 26.31f, 0}};
            hephTiers.whole = true;
            gui.setRockContents(hephTiers);
            pump();
            shoot(gui.windowHandle(), "rock_contents_heph_tiers.png");
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_heph_tiers.png");
            gui.toggleOverlayMode();
            pump();
            // ...and a legendary one: 975 on its own part, the other two low
            SCMiner::RockContents hephTop;
            hephTop.parts = {{"Silicon", false, 4.66f, 510}, {"Hephaestanite", false, 3.91f, 975},
                             {"Hephaestanite", false, 47.35f, 330}, {"Quartz", false, 2.70f, 641},
                             {"", true, 41.38f, 0}};
            hephTop.whole = true;
            gui.setRockContents(hephTop);
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_heph_top.png");
            gui.toggleOverlayMode();
            // ...and in the rock view what a player on UEX pays for that 975
            {
                const long long now = (long long)std::time(nullptr);
                gui.setUexPrices(true, nullptr, 0, "");
                gui.setUexMarket("Hephaestanite", shotListings("Hephaestanite", now), now - 60, "");
                pump();
                shoot(gui.windowHandle(), "rock_contents_market.png");
                gui.setUexMarket("Hephaestanite", std::make_shared<std::vector<SCMiner::UexListing>>(), now, "");
            }
            gui.setRockContents(hephTiers);
            pump();
            // ...and both rocks with the ores on one line
            gui.setOverlayOreRows(false);
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_heph_line.png");
            gui.toggleOverlayMode();
            gui.updateRock(bexalite);
            gui.setRockContents(tiers);
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_contents_tiers_line.png");
            gui.toggleOverlayMode();
            gui.setOverlayOreRows(true);
            pump();
            gui.setRockContents(SCMiner::RockContents());
            gui.updateRock(rock);
            pump();
        }

        // a rock that will not crack: the advice turns to resistance
        rock.ore = "QUANTAINIUM (ORE)";
        rock.mass = 41000;
        rock.resistance = 68;
        rock.instability = 21.4f;
        gui.updateRock(rock);
        pump();
        shoot(gui.windowHandle(), "rock_hard.png");

        // the same hard rock, but with a crafted laser: the figure moves and
        // the window says where it came from
        gui.setLoadout(crafted);
        gui.updateRock(rock);
        pump();
        shoot(gui.windowHandle(), "rock_crafted.png");
        gui.setLoadout(fit);

        // ...and with a gadget on it: the figures it changes, changed, and no
        // second gadget offered - the game takes one per rock.
        rock.ore = "SAVRILIUM (ORE)";
        rock.mass = 48929;
        rock.resistance = 0;
        rock.instability = 489.48f;
        rock.distance = 0;
        gui.updateRock(rock);
        gui.setFittedGadget(SCMiner::gadgetById("thcn_boremax"));
        pump();
        shoot(gui.windowHandle(), "rock_gadget.png");
        gui.setFittedGadget(nullptr);
        // ...and one the game's log saw go on: the panel prints the rock's
        // figures with it from then on, and the view says what they were and
        // what they are now, without a click
        {
            SCMiner::RockStats bare = rock;
            bare.mass = 21470;
            bare.resistance = 56;
            bare.instability = 12.4f;
            gui.updateRock(bare);
            const SCMiner::MiningGadget* sabir = SCMiner::gadgetById("shin_sabir");
            gui.setGadgetPlaced(sabir, "MineableRock_AsteroidLegendary_Savrilium", 0);
            gui.updateRock(SCMiner::withGadget(bare, sabir));
            pump();
            shoot(gui.windowHandle(), "rock_gadget_placed.png");
            gui.setGadgetPlaced(nullptr, "", 0);
        }

        // The tallest the rock view gets. The borderless notice of a first
        // launch is still up - a rock can be scanned before any signature
        // has been read - and nothing on board holds the rock, so "what
        // would break it" stands between the heads and the gadgets. The
        // right arm has no head yet, so the answer carries its note as
        // well, and a Sabir on the rock that is not enough gives the gadget
        // card all three of its lines. The last row of gadgets still has
        // to clear the footer.
        gui.firstRunForShot();
        gui.setHeadForShot(2, SCMiner::MiningLoadout{});
        rock.ore = "QUANTAINIUM (ORE)";
        rock.mass = 62000;
        rock.resistance = 68;
        rock.instability = 21.4f;
        rock.scu = 40.0f;
        rock.distance = 12.0f;
        gui.updateRock(rock);
        gui.setFittedGadget(SCMiner::gadgetById("shin_sabir"));
        pump();
        shoot(gui.windowHandle(), "rock_wont_hold.png");
        gui.setFittedGadget(nullptr);
        gui.setHeadForShot(2, hard);
        // the same reading as before retires the notice again, and puts the
        // window's own buttons back where the notice had pushed them
        scanned(28000, 96);
        pump();

        // The rock in the compact overlay, which is the window that is up
        // from the seat: the MOLE on the tin rock, close in; then a
        // Prospector on a quantainium rock - 13,147 kg at 47 % in green,
        // held at 95 % on a Helix I with Focus III and Rieger-C3, a Sabir
        // a help rather than a need - and on a rock nothing on the ship
        // holds, where the overlay names the head that would.
        rock.ore = "TIN (ORE)";
        rock.mass = 22593;
        rock.resistance = 0;
        rock.instability = 3.52f;
        rock.scu = 30.21f;
        rock.distance = 12.0f;
        gui.updateRock(rock);
        gui.toggleOverlayMode();
        pump();
        shoot(gui.windowHandle(), "overlay_rock.png");
        gui.toggleOverlayMode();
        pump();
        gui.pickShipForShot("prospector");
        gui.setHeadForShot(0, fit);
        rock.ore = "QUANTAINIUM (RAW)";
        rock.mass = 13147;
        rock.resistance = 47;
        rock.resistanceAdjusted = true;
        rock.instability = 125.11f;
        rock.scu = 9.44f;
        rock.distance = 0;
        gui.updateRock(rock);
        gui.toggleOverlayMode();
        pump();
        shoot(gui.windowHandle(), "overlay_rock_hard.png");
        rock.mass = 41000;
        rock.resistance = 68;
        rock.resistanceAdjusted = false;
        rock.instability = 21.4f;
        rock.scu = 40.0f;
        gui.updateRock(rock);
        pump();
        shoot(gui.windowHandle(), "overlay_rock_short.png");
        gui.toggleOverlayMode();
        pump();
        // A MOLE on three Arbor MH2s at a gold rock of high resistance: all
        // three heads together do not hold it, and the overlay names the head
        // that would; then a lighter one, where the Arbors' 5 % minimum sits
        // at the very start of the scale
        {
            gui.pickShipForShot("mole");
            SCMiner::MiningLoadout arbor;
            arbor.laser = SCMiner::laserById("grin_arbor_s2");
            for (int h = 0; h < 3; h++) gui.setHeadForShot(h, arbor);
            SCMiner::RockStats gold = rock;
            gold.ore = "GOLD (ORE)";
            gold.mass = 22000;
            gold.resistance = 50;
            gold.resistanceAdjusted = false;
            gold.instability = 40.0f;
            gold.scu = 20.0f;
            gold.distance = 0;
            gui.updateRock(gold);
            gui.toggleOverlayMode();
            pump();
            shoot(gui.windowHandle(), "overlay_rock_mole_suggest.png");
            gold.mass = 12000;
            gold.resistance = 60;
            gui.updateRock(gold);
            pump();
            shoot(gui.windowHandle(), "overlay_rock_mole_floor.png");
            gui.toggleOverlayMode();
            pump();
        }
        // ...and the MOLE back the way the loadout pictures below expect it
        gui.pickShipForShot("mole");
        gui.setHeadForShot(0, fit);
        gui.setHeadForShot(1, wide);
        gui.setHeadForShot(2, hard);
        pump();

        // The rock view for the website: rocks the game really showed, as
        // the panel bench read them off the recordings, and as the app has
        // them live - no distance (the panel does not print it), the ore by
        // the name the app gives it - on the MOLE above, at a place the
        // tables have the ore
        {
            gui.setCurrentSystem("Stanton");
            gui.setPlace("Stanton - Yela", "Yela");
            gui.setFittedGadget(nullptr);
            struct Web {
                const char* name;
                const char* ore;
                float mass, resistance, instability, scu;
            };
            // A taranite rock read the same in 28 frames of a MOLE recording,
            // picked because its answer shows the most: one head does it, which
            // one, where to hold it, a gadget that is optional.
            const Web webRocks[] = {
                {"rock_web.png", "Taranite", 25005, 36, 302.71f, 32.38f},
            };
            for (const Web& w : webRocks) {
                SCMiner::RockStats web;
                web.ore = w.ore;
                web.mass = w.mass;
                web.resistance = w.resistance;
                web.instability = w.instability;
                web.scu = w.scu;
                web.distance = 0;
                gui.updateRock(web);
                pump();
                shoot(gui.windowHandle(), w.name);
                gui.toggleOverlayMode();
                pump();
                shoot(gui.windowHandle(), std::string("overlay_") + w.name);
                gui.toggleOverlayMode();
                pump();
            }
            gui.setCurrentSystem("NYX");
            gui.setPlace("Nyx - open space", "", true);
        }

        gui.clearRock();
        pump();
    
        // the loadout card itself
        gui.showLoadoutForShot(true);
        pump();
        shoot(gui.windowHandle(), "loadout.png");

        // ...and a crafted figure typed into it, keystroke by keystroke
        // through the real message loop: a painted field can take the click,
        // draw its cursor and swallow every key, because IsDialogMessage
        // answers "handled" for a bare key on a plain window and
        // TranslateMessage then never runs.
        gui.typeCraftedForShot(L"28.84");
        pump();
        shoot(gui.windowHandle(), "loadout_typed.png");
        std::cout << "  crafted bonus after typing 28.84: " << gui.craftedForShot()
                  << (std::abs(gui.craftedForShot() - 28.84f) < 0.01f ? "  OK" : "  WRONG")
                  << std::endl;

        // ...and the same head with two of the same module, which a
        // two-slot head takes: the multipliers stack, so 1.25 x 1.25 is a
        // real fit.
        {
            SCMiner::MiningLoadout stacked;
            stacked.laser = SCMiner::laserById("thcn_helix_s1");
            stacked.modules = {SCMiner::moduleById("passive_rieger_mk3"),
                               SCMiner::moduleById("passive_rieger_mk3")};
            gui.setHeadForShot(0, stacked);
            pump();
            shoot(gui.windowHandle(), "loadout_stacked.png");
            // ...and Brandt beside Stampede, which cut each other off: the
            // card says so under the list, and which of the two counts
            SCMiner::MiningLoadout cutOff;
            cutOff.laser = SCMiner::laserById("thcn_helix_s1");
            cutOff.modules = {SCMiner::moduleById("active_brandt"), SCMiner::moduleById("active_stampede")};
            gui.setHeadForShot(0, cutOff);
            pump();
            shoot(gui.windowHandle(), "loadout_cut_off.png");
            const SCMiner::LaserOutput both = SCMiner::outputOf(stacked, 0);
            std::cout << "  two Rieger-C3 on a Helix I: " << both.fracture
                      << " fracture power" << std::endl;
            gui.setHeadForShot(0, fit);
            pump();
        }

        gui.showLoadoutForShot(false);
        pump();

        // the two lists that drop out of the window: the ship, and the place
        // when the log has it wrong. Both have to clear the action row, which
        // is four real child windows and draws over anything painted under it.
        //
        // The list holds the player's own ships: a second Prospector - one
        // for heavy rocks, one for fiddly ones - under a name of its own,
        // what each flies under its name, and a new one of each kind at the
        // foot.
        gui.addFitForShot("prospector", "Prospector Hofstede");
        {
            // a crafted Hofstede with a Deluge: a precision fit
            SCMiner::MiningLoadout precise;
            precise.laser = SCMiner::laserById("shin_hofstede_s1");
            precise.modules = {SCMiner::moduleById("passive_deluge")};
            precise.craftedPower = 12.4f;
            gui.setHeadForShot(0, precise);
        }
        pump();
        gui.showShipPickerForShot(true);
        pump();
        shoot(gui.windowHandle(), "ships.png");
        gui.showShipPickerForShot(false);
        pump();
        // ...and the overlay's rock view names it the way the player did
        gui.updateRock(rock);
        gui.toggleOverlayMode();
        pump();
        shoot(gui.windowHandle(), "overlay_rock_named.png");
        // A module swapped on the ship: its HUD shows the Hofstede with a
        // Brandt where the fit has a Deluge. No ship on the list carries
        // that, so this one does from now on - the crafted bonus with it,
        // the laser being the same - and the overlay's last line says what
        // the figures are worked out with.
        {
            const SCMiner::MiningShip* prospector = SCMiner::shipById("prospector");
            const SCMiner::MiningLaser* hofstede = SCMiner::laserById("shin_hofstede_s1");
            bool changed = false;
            const std::string taken =
                gui.setSeenLoadout(*prospector, *hofstede, {SCMiner::moduleById("active_brandt")}, true, &changed);
            std::cout << "the HUD's loadout: ship '" << taken << "', changed " << (changed ? 1 : 0) << ", crafted "
                      << gui.craftedForShot() << std::endl;
            pump();
            shoot(gui.windowHandle(), "overlay_rock_hud.png");
            // ...and as it was, for the pictures after this one
            gui.setSeenLoadout(*prospector, *hofstede, {SCMiner::moduleById("passive_deluge")}, true, &changed);
            std::cout << "the HUD's loadout back: changed " << (changed ? 1 : 0) << ", crafted " << gui.craftedForShot()
                      << std::endl;
        }
        // ...and with the panel down the ship stands under the matches too -
        // also under three of them with "no exact match" over them
        gui.clearRock();
        pump();
        shoot(gui.windowHandle(), "overlay_named.png");
        scanned(10110, 77);
        pump();
        shoot(gui.windowHandle(), "overlay_named_three.png");
        gui.toggleOverlayMode();
        pump();
        // ...its card, the name for its title, and the title typed over as a
        // new ship opens it
        gui.showLoadoutForShot(true);
        pump();
        shoot(gui.windowHandle(), "loadout_named.png");
        gui.renameFitForShot(L"Prospector Hofst");
        pump();
        shoot(gui.windowHandle(), "loadout_rename.png");
        gui.stopRenameForShot();
        gui.showLoadoutForShot(false);
        gui.pickShipForShot("mole");
        pump();
        gui.showPlacePickerForShot(true);
        pump();
        shoot(gui.windowHandle(), "places.png");
        gui.showPlacePickerForShot(false);
        pump();
}

    // ...and back to Monox for the rest of the pictures
    analyzer.setSystemFilter("PYRO");
    analyzer.setPlaceFilter("Pyro II (Monox)");
    analyzer.setGround(SCMiner::SignatureAnalyzer::Ground::Surface);
    gui.setCurrentSystem("Pyro");
    gui.setPlace("Pyro - Monox", analyzer.placeFilter());
    scanned(31185, 94);
    gui.openAboutForShot();
    pump();
    shoot(gui.windowHandle(), "about.png");
    gui.openAboutForShot();   // ...and shut again
    pump();
    gui.openBackupForShot();
    pump();
    shoot(gui.windowHandle(), "backup.png");
    // ...and shut again through the window's own click handler, so the
    // action row comes back the way it does for a player
    SendMessageW(gui.windowHandle(), WM_LBUTTONDOWN, 0, MAKELPARAM(4, 4));
    pump();
    // The card that puts itself up once after an update. Here on purpose:
    // it is the one thing in the app nobody asks to see, so a picture of it
    // is the only way to find out it has grown a line too long.
    gui.showWhatsNew();
    pump();
    shoot(gui.windowHandle(), "whats_new.png");
    // ...and part way down it, which is the only way to see that the middle
    // scrolls while the name and the foot stay put
    gui.scrollWhatsNewForShot(420.0f);
    pump();
    shoot(gui.windowHandle(), "whats_new_scrolled.png");
    // ...and further down, to the end of the newest version's entry
    gui.scrollWhatsNewForShot(420.0f);
    pump();
    shoot(gui.windowHandle(), "whats_new_further.png");
    SendMessageW(gui.windowHandle(), WM_LBUTTONDOWN, 0, MAKELPARAM(4, 4));
    pump();
    // The first start's UEX offer over the main window: the account, then -
    // a key pasted - whether to report prices, then the pictures' 90 days
    {
        SCMiner::MinerGUI::UexAccount account;
        account.hasToken = true;
        gui.setUexAccount(account);
        gui.openUexWelcomeForShot(1);
        pump();
        shoot(gui.windowHandle(), "uex_welcome.png");
        account.keyEnd = "7f3a";
        account.note = "Key saved";
        account.noteAt = (long long)std::time(nullptr) - 5;
        gui.setUexAccount(account);
        gui.openUexWelcomeForShot(3);
        pump();
        shoot(gui.windowHandle(), "uex_welcome_ask.png");
        gui.openUexWelcomeForShot(2);
        pump();
        shoot(gui.windowHandle(), "uex_welcome_pictures.png");
        // ...and No said: the account kept, prices not reported
        account.note = "Account connected, reporting off";
        gui.setUexAccount(account);
        gui.openUexWelcomeForShot(1);
        pump();
        shoot(gui.windowHandle(), "uex_welcome_kept.png");
        // ...and reporting on, the last report taken and its screenshot kept;
        // then UEX took nothing: the account is no data runner yet - then the
        // same in the narrowest window
        account.on = true;
        account.since = (long long)std::time(nullptr) - 3600;
        account.lastShot = L"uex_last_screenshot.jpg";
        account.note = "Hephaestanite bought at Admin - Pyro Gateway (Stanton): accepted by UEX";
        account.noteAt = (long long)std::time(nullptr) - 40;
        gui.setUexAccount(account);
        gui.openUexWelcomeForShot(1);
        pump();
        shoot(gui.windowHandle(), "uex_reported.png");
        account.accountTrouble = "datarunner";
        account.note = "Agricium sold at Commodity Shop - Levski: UEX only takes reports from data runners";
        account.noteAt = (long long)std::time(nullptr) - 40;
        gui.setUexAccount(account);
        gui.openUexWelcomeForShot(1);
        pump();
        shoot(gui.windowHandle(), "uex_datarunner.png");
        RECT was = {};
        GetWindowRect(gui.windowHandle(), &was);
        SetWindowPos(gui.windowHandle(), nullptr, 0, 0, 1, was.bottom - was.top, SWP_NOMOVE | SWP_NOZORDER);
        pump();
        shoot(gui.windowHandle(), "uex_datarunner_narrow.png");
        gui.openUexWelcomeForShot(3);
        pump();
        shoot(gui.windowHandle(), "uex_welcome_ask_narrow.png");
        SetWindowPos(gui.windowHandle(), nullptr, 0, 0, was.right - was.left, was.bottom - was.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        account.on = false;
        account.accountTrouble.clear();
        account.lastShot.clear();
        gui.openUexWelcomeForShot(0);
        gui.setUexAccount(SCMiner::MinerGUI::UexAccount());
        pump();
    }
    // The news window, painted straight into the bitmap like the other
    // windows of its own: its 2.5 page, then the 2.0 page behind the tab
    gui.showNewsForShot();
    pump();
    if (gui.newsHandle()) shoot(gui.newsHandle(), "news.png", true);
    gui.newsPageForShot(1);
    pump();
    if (gui.newsHandle()) shoot(gui.newsHandle(), "news_2_0.png", true);
    gui.hideNewsForShot();
    pump();
    gui.openLookingForShot();
    pump();
    shoot(gui.windowHandle(), "looking_for.png");
    // ...and three letters typed into it. POSTED, not sent: they have to go
    // the way a real key goes - through the pump, past the shortcuts and
    // IsDialogMessage, through TranslateMessage into WM_CHAR. The picture
    // shows that typing gets through.
    for (wchar_t key : {L'H', L'E', L'P'}) {
        PostMessageW(gui.windowHandle(), WM_KEYDOWN, (WPARAM)key, 0);
        PostMessageW(gui.windowHandle(), WM_KEYUP, (WPARAM)key, 0xC0000000);
    }
    pump();
    shoot(gui.windowHandle(), "looking_for_typed.png");
    gui.setLookingOpenForShot(false);
    pump();
    // The Ore Finder with UEX's prices in it: where the ore sells best, and
    // the ore that fetches most, red - caught with its blink on
    const long long shotNow = (long long)std::time(nullptr);
    gui.setUexPrices(true, shotPrices(shotNow), shotNow - 12 * 60, "");
    gui.togglePlanner();
    gui.setPlanPulseForShot(true);
    gui.openOreForShot();
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner.png", true);
    // ...the ore that fetches most picked itself
    gui.openOreForShot("Quantainium");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_prices.png", true);
    // ...from Nyx, where none of the five is: the best buyer there under them
    gui.setCurrentSystem("NYX");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_prices_home.png", true);
    gui.setCurrentSystem("Pyro");
    // ...and the ground's two families: a gem a line, by its best buyer (a
    // gem's name opens its family)
    gui.openOreForShot("Hadanite");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_gems.png", true);
    gui.openOreForShot("Beradom");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_gems_roc.png", true);
    // ...and the UEX tab: what players buy and sell Ouratite for; then a
    // ground family's gem
    gui.setUexMarket("Ouratite", shotListings("Ouratite", shotNow), shotNow - 60, "");
    {
        // ...one of them a listing the player is talking about
        auto threads = std::make_shared<SCMiner::UexThreads>();
        threads->hashByListing[3] = "shotthread";
        gui.setUexThreads(threads);
    }
    gui.openUexTabForShot("Ouratite");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_market.png", true);
    gui.setUexMarket("Hadanite", shotListings("Hadanite", shotNow), shotNow - 60, "");
    gui.openUexTabForShot("Hand minables", "Hadanite");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_market_gems.png", true);
    gui.openUexTabForShot(nullptr);
    gui.setUexThreads(nullptr);
    // ...and Report to UEX: what goes there and the key, then the pictures'
    // 90 days, then on - a made-up name for UEX's answer, never the player's
    {
        SCMiner::MinerGUI::UexAccount account;
        account.hasToken = true;
        gui.setUexAccount(account);
        gui.openUexCardForShot(1);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_uex_report.png", true);
        account.keyEnd = "7f3a";
        account.note = "Key saved";
        account.noteAt = shotNow - 30;
        gui.setUexAccount(account);
        gui.openUexCardForShot(3);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_uex_ask.png", true);
        gui.openUexCardForShot(2);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_uex_pictures.png", true);
        account.on = true;
        account.since = shotNow - 3 * 86400;
        account.user = "DataRunner";
        account.note = "Gold sold at Levski: accepted by UEX";
        account.noteAt = shotNow - 120;
        gui.setUexAccount(account);
        gui.openUexCardForShot(1);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_uex_on.png", true);
        gui.openUexCardForShot(0);
        gui.setUexAccount(SCMiner::MinerGUI::UexAccount());
    }
    // ...and before UEX has answered, and after it could not be asked
    gui.setUexPrices(true, nullptr, 0, "");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_prices_asking.png", true);
    gui.setUexPrices(true, nullptr, 0, "could not reach it");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_prices_failed.png", true);
    gui.setUexPrices(true, shotPrices(shotNow), shotNow - 12 * 60, "");
    gui.setPlanPulseForShot(true);
    gui.openOreForShot();
    pump();
    // ...and the same window asked the other way round, from a note link
    gui.openPlaceForShot();
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_place.png", true);
    // ...and an ore with the longest rows there are: Lagrange C names five
    // points, the Pyro belt a sentence. Their small words take a second
    // line rather than running under the figure.
    gui.openOreForShot("Aluminium");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_long.png", true);
    // ...and an ore the player has traded: their own deals under what a rock
    // of it holds - bought refined for a recipe, sold as it came out of the
    // rock; a request the shop refused is not among them
    {
        std::vector<SCMiner::Trade> trades;
        auto deal = [&](const char* stamp, bool sold, const char* resource, double scu, double total, const char* shop,
                        const char* place, bool refused = false) {
            SCMiner::Trade t;
            t.stamp = stamp;
            t.sold = sold;
            t.resource = resource;
            t.scu = scu;
            t.total = total;
            t.shop = shop;
            t.place = place;
            t.refused = refused;
            trades.push_back(t);
        };
        const char* gold = "21825507-7923-4683-9bf3-9cfe316940e3";
        deal("2026-06-28T17:21:48.000", false, gold, 5, 144638, "SCShop_Admin_lt_base_g", "HUR-L1 Green Glade Station");
        deal("2026-06-28T17:49:24.000", false, gold, 50, 1377500, "SCShop_ht_delta_shubin_m_store", "Shubin Mining Facility SMO-18");
        deal("2026-09-30T19:02:11.000", true, gold, 12.4, 301320, "SCShop_Admin_lt_base_g", "", true);
        deal("2026-09-30T19:02:40.000", true, gold, 12.4, 301320, "SCShop_Admin_lt_base_g", "");
        gui.setTrades(trades);
        gui.openOreForShot("Gold");
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_deals.png", true);
        gui.setTrades({});
    }
    gui.togglePlanner();
    pump();
    // ...the refinery table, empty and then with a hold in it
    gui.toggleRefinery();
    pump();
    if (gui.refineryHandle()) shoot(gui.refineryHandle(), "refinery.png", true);
    gui.pickForShot();
    pump();
    if (gui.refineryHandle()) shoot(gui.refineryHandle(), "refinery_hold.png", true);
    // ...and the method dropped down, all nine with their factors
    gui.openMethodForShot();
    pump();
    if (gui.refineryHandle()) shoot(gui.refineryHandle(), "refinery_method.png", true);
    SendMessageW(gui.refineryHandle(), WM_KEYDOWN, VK_ESCAPE, 0);
    pump();
    // ...and an amount being typed. A hold is hardly ever a round number
    // of SCU, so the figure takes digits; these two shots go through the
    // window's own click and key handlers, so a picture showing "1.47_"
    // and then "1.47 SCU" is the feature working, not a mock of it.
    {
        int quantainium = -1;
        for (int i = 0; i < SCMiner::REFINERY_MATERIAL_COUNT; i++) {
            if (std::strcmp(SCMiner::REFINERY_MATERIALS[i], "Quantainium (Raw)") == 0) {
                quantainium = i;
                break;
            }
        }
        if (quantainium >= 0 && gui.typeAmountForShot(quantainium, L"1.47")) {
            pump();
            shoot(gui.refineryHandle(), "refinery_typing.png", true);
            SendMessageW(gui.refineryHandle(), WM_KEYDOWN, VK_RETURN, 0);
            pump();
            shoot(gui.refineryHandle(), "refinery_typed.png", true);
        } else {
            std::cout << "refinery_typing: no amount field to type into" << std::endl;
        }
    }
    // ...and the work orders read off a refinery's terminal: none yet, then
    // three - one done, one running, one whose quote was not seen
    if (gui.refineryHandle()) {
        gui.showRefineryOrdersForShot(true);
        pump();
        shoot(gui.refineryHandle(), "refinery_orders_none.png", true);
        const long long now = (long long)std::time(nullptr);
        std::vector<SCMiner::RefineryOrder> orders(3);
        orders[0].station = "Levski";
        orders[0].number = 2;
        orders[0].parts = {{"Iron (Ore)", 325, 114}};
        orders[0].method = "Pyrometric Chromalysis";
        orders[0].cost = 94;
        orders[0].seconds = 211;
        orders[0].placed = now - 300;
        orders[0].due = now - 89;
        orders[0].seen = now - 290;
        orders[1].station = "Levski";
        orders[1].number = 1;
        orders[1].parts = {{"Iron (Ore)", 874, 235}};
        orders[1].method = "Pyrometric Chromalysis";
        orders[1].cost = 194;
        orders[1].seconds = 442;
        orders[1].placed = now - 80;
        orders[1].due = now + 362;
        orders[1].seen = now - 70;
        orders[2].station = "Checkmate";
        orders[2].number = 1;
        orders[2].parts = {{"Quantainium (Raw)", 612, 312}, {"Gold (Ore)", 553, 41}};
        orders[2].due = now + 2 * 3600 + 14 * 60;
        orders[2].seen = now - 1800;
        // ...and one first read COMPLETED: done before anyone looked
        SCMiner::RefineryOrder found;
        found.station = "Pyro Gateway";
        found.number = 1;
        found.parts = {{"Torite (Ore)", 262, 134}};
        found.due = now - 600;
        found.seen = now - 600;
        found.completed = now - 600;
        orders.push_back(found);
        gui.setRefineryOrders(orders);
        pump();
        shoot(gui.refineryHandle(), "refinery_orders.png", true);
        // ...and the main window's Refinery button with the finished ones
        // counted on it: three of the four done
        orders[1].due = now - 30;
        gui.setRefineryOrders(orders);
        pump();
        shoot(gui.windowHandle(), "main_refinery_done.png");
        gui.setRefineryOrders({});
        gui.showRefineryOrdersForShot(false);
        pump();
    }
    gui.toggleRefinery();
    pump();
    // A library for the picture: every third blueprint owned
    {
        std::vector<std::string> stamps(SCMiner::CRAFT_BLUEPRINT_COUNT);
        for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b += 3) stamps[b] = "2026-03-11 15:54";
        gui.setOwnedBlueprints(stamps, "from the game's log");
    }
    // ...and ranks for the planner: one proven by a contract taken, one
    // counted since the wipe, one set by hand; every other outfit at the
    // bottom of its ladder. Made up, like the library.
    {
        std::vector<SCMiner::Standing> ranks;
        const int standing = SCMiner::Reputation::ladderByGuid("d31bd373-35ab-4ea4-b0a7-0ccc867ef082");
        auto add = [&](const char* outfit, int rank, long long reputation, int anchor, const char* when,
                       long long gained, int done, bool set) {
            for (int f = 0; f < SCMiner::REP_FACTION_COUNT; f++) {
                if (std::string(SCMiner::REP_FACTIONS[f].name) != outfit || standing < 0) continue;
                SCMiner::Standing s;
                s.faction = f;
                s.ladder = standing;
                s.rank = rank;
                s.reputation = reputation;
                s.anchorRank = anchor;
                s.anchorStamp = anchor >= 0 ? when : "";
                s.gained = gained;
                s.completions = done;
                s.set = set;
                s.setStamp = set ? when : "";
                s.proven = anchor;
                ranks.push_back(s);
            }
        };
        add("Citizens For Prosperity", 2, 4075, 2, "2026-03-27T16:52:00.000", 1875, 12, false);
        add("Foxwell Enforcement", 3, 11750, 2, "2026-06-02T20:11:00.000", 9550, 75, false);
        add("Headhunters", 3, 9875, 3, "2026-07-01T18:40:00.000", 4075, 64, false);
        add("Shubin Interstellar", 3, 6175, -1, "2026-10-04T11:05:00.000", 375, 3, true);
        add("Bit Zeros", 1, 1100, -1, "", 1100, 11, false);
        gui.setRanks(ranks, true, SCMiner::Reputation::LIVE_WIPE);
    }
    gui.toggleCrafting();
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprints.png", true);
    // ...with the box for the game's texts ticked, and what a write said
    gui.setGameTexts(true, "in the game from its next start (written 20:31)");
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprints_game_texts.png", true);
    gui.setGameTexts(false, "");
    // ...and one recipe opened up, which is where the sliders live
    gui.cartForShot();
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprints_list.png", true);
    // ...with the station picker over the list dropped down. Two pumps: the
    // list hangs from where the box was painted, which the first paint sets.
    gui.openCartPickForShot(30);
    pump();
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprints_pick.png", true);
    gui.openCartPickForShot(0);
    pump();
    gui.widestFilterForShot();
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprints_filters.png", true);
    // ...and the same bar put to work: one system, one outfit, and what is
    // left of the 1,606
    gui.narrowByContractForShot();
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprints_contract_filter.png", true);
    gui.openBlueprintForShot();
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_detail.png", true);
    // ...and the list once more, switched to the component filter
    gui.showComponentsForShot();
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprints_components.png", true);
    gui.openRecoilBlueprintForShot();
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_recoil.png", true);
    // ...the contracts that drop a recipe: the list of them, then one opened
    gui.openContractForShot(false);
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_contracts.png", true);
    gui.openContractForShot(true);
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_contract.png", true);
    // ...a contract that pays out of two pools: Foxwell's Orange escort run
    // in Nyx, opened from a repeater it hands out - the coolers of its
    // other pool on the card beside them
    if (gui.openCardForShot("CF-117 Bulldog Repeater", "Orange Lvl. Contract: Protect", "Nyx")) {
        pump();
        if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_contract_pools.png", true);
        gui.openContractForShot(true);
        pump();
    } else {
        std::cout << "[FAIL] no Orange escort contract under CF-117 Bulldog Repeater for the picture" << std::endl;
    }
    // ...and the Blueprints search finds an outfit however it is typed
    {
        const int spaced = gui.craftSearchCountForShot(L"Bit Zeros");
        const int joined = gui.craftSearchCountForShot(L"BitZeros");
        const int fox = gui.craftSearchCountForShot(L"fox");
        std::cout << (spaced > 0 && spaced == joined && fox > 0 ? "[PASS]" : "[FAIL]")
                  << " the Blueprints search finds outfits: 'Bit Zeros' " << spaced << ", 'BitZeros' " << joined
                  << ", 'fox' " << fox << std::endl;
    }
    // ...and a click on the card where another contract's row lies under it:
    // it closes the card, it does not open that contract
    {
        const int left = gui.clickUnderCardForShot();
        std::cout << (left == -1 ? "[PASS]" : "[FAIL]")
                  << " a click on the contract card closes it, the row under it stays shut ("
                  << left << ")" << std::endl;
        gui.openContractForShot(true);
        pump();
    }
    // ...and the planner the Blueprints window opens: what is worth grinding
    // where the player is, and how many runs it takes
    gui.openGrindForShot();
    // The notice stands in front of the planner on a fresh data folder, which
    // a shot run always is - put away first, so the list itself is in the
    // picture, and brought back for one of its own below
    gui.grindWarningForShot(false);
    pump();
    if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_planner.png", true);
    // ...the finder at work: a word of a blueprint's name, the list
    // narrowed to the sets that hand it out, the first one unfolded
    gui.grindSearchForShot(L"woodland");
    pump();
    if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_search.png", true);
    // ...and the planner opened from a contract card: the one set that
    // contract pays out of, its ladder unfolded
    for (int i = 0; i < SCMiner::CRAFT_SOURCE_COUNT; i++) {
        if (std::string(SCMiner::CRAFT_SOURCES[i].title) == "Reduce Outlaw's Operational Readiness") {
            gui.openGrindForSet(i);
            pump();
            if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_set.png", true);
            // ...with one of its missions taken in the game, and another
            // finished two minutes ago
            {
                std::vector<SCMiner::MinerGUI::ContractNow> now;
                bool taken = false, done = false;
                for (int c = 0; c < SCMiner::REP_CONTRACT_COUNT; c++) {
                    const SCMiner::RepContract& rc = SCMiner::REP_CONTRACTS[c];
                    if (std::string(rc.key).rfind("CitizensForProsperity_", 0) != 0) continue;
                    SCMiner::MinerGUI::ContractNow one;
                    one.contract = &rc;
                    if (!taken && std::string(rc.title) == "Take Outlaws at [LOCATION] Offline") {
                        one.running = true;
                        taken = true;
                    } else if (!done && std::string(rc.title) == "Eliminate Fuel Stores") {
                        one.finishedAt = (long long)std::time(nullptr) - 130;
                        done = true;
                    } else {
                        continue;
                    }
                    now.push_back(one);
                }
                gui.setContractsNow(now);
                pump();
                if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_live.png", true);
                gui.setContractsNow({});
                pump();
            }
            // ...and a set whose missions also pay out of another: the
            // Orange escort run's repeaters, with its coolers
            gui.grindSearchForShot(L"Orange Lvl. Contract: Protect");
            pump();
            if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_also.png", true);
            break;
        }
    }
    gui.grindSearchForShot(L"");
    pump();
    // ...the list held to what the player is offered at their ranks
    gui.grindOpenOnlyForShot(true);
    pump();
    if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_open_only.png", true);
    gui.grindOpenOnlyForShot(false);
    pump();
    // ...a set of an outfit whose rank was set by hand
    for (int i = 0; i < SCMiner::CRAFT_SOURCE_COUNT; i++) {
        const SCMiner::CraftSource& src = SCMiner::CRAFT_SOURCES[i];
        if (std::string(src.faction) == "Shubin Interstellar" && src.repLadder >= 0 && src.repRank >= 3) {
            gui.openGrindForSet(i);
            pump();
            if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_rank_set.png", true);
            break;
        }
    }
    gui.grindSearchForShot(L"");
    pump();
    // ...the ranks while the logs are still being read: nothing marked
    gui.setRanks({}, false, SCMiner::Reputation::LIVE_WIPE);
    pump();
    if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_reading.png", true);
    // ...and the notice that stands in front of it the first time, because
    // every number in that window rests on the library being honest
    gui.grindWarningForShot(true);
    pump();
    if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_warning.png", true);
    gui.grindWarningForShot(false);
    pump();
    gui.toggleGrind();
    pump();
    gui.toggleCrafting();
    pump();
    // ...and the notebook, with whatever notes\ holds
    gui.toggleNotes();
    gui.openNoteForShot();
    pump();
    if (gui.notesHandle()) shoot(gui.notesHandle(), "notes.png", true);
    // ...and the media rail with something on it. A picture this run has
    // already written is used, so the shot needs nothing from outside.
    {
        const std::string picture = dir + "/blueprints.png";
        if (std::filesystem::exists(picture)) {
            gui.addMediaForShot(std::filesystem::path(picture).wstring());
            pump();
            if (gui.notesHandle()) shoot(gui.notesHandle(), "notes_media.png", true);
            gui.foldMediaForShot(false);
            pump();
            if (gui.notesHandle()) shoot(gui.notesHandle(), "notes_media_shut.png", true);
            gui.foldMediaForShot(true);
            pump();
            // ...and the picture opened in Bedrock's own window
            gui.openShotForShot();
            pump();
            if (gui.shotHandle()) shoot(gui.shotHandle(), "note_picture.png", true);
        }
    }
    gui.suggestForShot();
    pump();
    if (gui.suggestHandle()) shoot(gui.suggestHandle(), "notes_suggest.png");
    // ...and the same in the window it belongs to, as it happens: a line
    // typed into the open note word by word, then a recipe's name letter by
    // letter - the names on offer from the third letter, under the end of the
    // text - and the first one taken, the way Enter takes it. Every step is a
    // frame in frames_autocomplete\ for a GIF; the fullest list is also
    // notes_autocomplete.png. The list is a window of its own, laid into the
    // notes picture where it stands. Windows only: the typing goes through
    // the rich edit control's own messages, which the Linux build's control
    // shim does not all answer, and the run crashes there.
#ifdef _WIN32
    gui.openNoteForShot();
    pump();
    if (gui.notesHandle()) {
        const std::string frames = dir + "/frames_autocomplete";
        std::filesystem::create_directories(frames, ec);
        int frame = 0;
        auto still = [&]() -> cv::Mat {
            bool okNotes = true;
            cv::Mat notes = grab(gui.notesHandle(), true, okNotes);   // draws, and learns where the text ends
            if (notes.empty() || !gui.suggestHandle() || !IsWindowVisible(gui.suggestHandle())) return notes;
            gui.placeSuggestForShot();
            bool okList = true;
            const cv::Mat list = grab(gui.suggestHandle(), false, okList);
            RECT at{};
            GetWindowRect(gui.suggestHandle(), &at);
            POINT origin{0, 0};
            ClientToScreen(gui.notesHandle(), &origin);
            const cv::Rect where(at.left - origin.x, at.top - origin.y, list.cols, list.rows);
            const cv::Rect inside = where & cv::Rect(0, 0, notes.cols, notes.rows);
            if (!list.empty() && inside.area() > 0) {
                list(cv::Rect(inside.x - where.x, inside.y - where.y, inside.width, inside.height))
                    .copyTo(notes(inside));
            }
            return notes;
        };
        auto keep = [&](const cv::Mat& picture) {
            if (picture.empty()) return;
            char name[32];
            std::snprintf(name, sizeof(name), "/%02d.png", frame++);
            cv::imwrite(frames + name, picture);
        };
        keep(still());
        for (const wchar_t* typed : {L"\r\nNext ", L"on ", L"the ", L"list: ", L"a", L"n", L"t", L"i", L"u"}) {
            gui.suggestInNoteForShot(typed);
            pump();
            const cv::Mat picture = still();
            keep(picture);
            if (std::wstring(typed) == L"u") save(picture, "notes_autocomplete.png", true);
        }
        gui.acceptSuggestForShot();
        pump();
        keep(still());
        std::cout << "frames_autocomplete: " << frame << " frames" << std::endl;
    }
#endif
    gui.dragNoteForShot(true);
    pump();
    if (gui.notesHandle()) shoot(gui.notesHandle(), "notes_drag.png", true);
    gui.dragNoteForShot(false);
    pump();
    // ...an import refused, for real: the bundle in the data folder carries
    // a [[Quantanium]] nobody spells that way. Before the trash, so the
    // tree in the picture is still whole.
    gui.importForShot((data / "import_bad.txt").wstring());
    pump();
    if (gui.notesHandle()) shoot(gui.notesHandle(), "notes_import.png", true);
    gui.openTrashForShot();
    pump();
    if (gui.notesHandle()) shoot(gui.notesHandle(), "notes_trash.png", true);
    gui.startNamingForShot();
    pump();
    if (gui.notesHandle()) shoot(gui.notesHandle(), "notes_naming.png", true);
    return 0;
}

#endif  // BEDROCK_DEV_TOOLS || !_WIN32
