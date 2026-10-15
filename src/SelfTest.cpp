// The self-test: what the program checks about itself, on both platforms.
//
// The Linux and Windows builds run the same checks: bedrock --selftest,
// Bedrock.exe --selftest. The pictures it reads come out of the program on
// both sides (EmbeddedData); beside a build, the ones under tests/ are taken
// instead.
#include "SelfTest.h"

#include "DevSwitch.h"
#include "Application.h"
#include "AppPaths.h"
#include "BlueprintLibrary.h"
#include "CraftingData.h"
#include "DataBackup.h"
#include "DevTools.h"
#include "EmbeddedData.h"
#include "GamePlaces.h"
#include "GameTextMarks.h"
#include "GameTexts.h"
#include "LoadoutReader.h"
#include "MiningHardware.h"
#include "P4kReader.h"
#include "GameState.h"
#include "MiningLocations.h"
#include "NoteBook.h"
#include "OCREngine.h"
#include "PanelShapes.h"
#include "RefineryData.h"
#include "RefiningData.h"
#include "ScanPanel.h"
#include "RockContents.h"
#include "RefineryTerminal.h"
#include "RefineryOrders.h"
#include "TradeBook.h"
#include "TextLineReader.h"
#include "MiniJson.h"
#include "RefineryShare.h"
#include "UexPrices.h"
#include "UexReports.h"
#include "UexMarket.h"
#include "Updater.h"
#include "resource.h"

#include <zstd.h>
#include <opencv2/opencv.hpp>

// rpcndr.h, pulled in by <windows.h>, defines a "small" macro, and one of
// the checks below names a rock that.
#ifdef small
#undef small
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Renders a synthetic scanner readout: a "pin" blob, a gap, then bright
// digits on a dark background - the same layout the OCR pipeline expects.
cv::Mat makeSyntheticSignatureImage(const std::string& digits) {
    cv::Mat image(48, 260, CV_8UC3, cv::Scalar(15, 15, 15));
    cv::rectangle(image, cv::Rect(6, 14, 18, 22), cv::Scalar(230, 230, 230), cv::FILLED);
    cv::putText(image, digits, cv::Point(44, 36), cv::FONT_HERSHEY_SIMPLEX,
                1.0, cv::Scalar(235, 235, 235), 2, cv::LINE_AA);
    return image;
}

// Renders the hard in-game case: orange digits on a bright cyan scanner sky.
// In grayscale both tones are nearly identical - only the warm (R-B) channel
// separates them.
cv::Mat makeOrangeOnSkyImage(const std::string& digits) {
    cv::Mat image(48, 260, CV_8UC3, cv::Scalar(235, 190, 25));   // BGR sky blue
    cv::rectangle(image, cv::Rect(6, 14, 18, 22), cv::Scalar(30, 140, 250), cv::FILLED);
    cv::putText(image, digits, cv::Point(44, 36), cv::FONT_HERSHEY_SIMPLEX,
                1.0, cv::Scalar(30, 140, 250), 2, cv::LINE_AA);  // BGR orange
    return image;
}

bool check(bool condition, const std::string& label) {
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << label << std::endl;
    return condition;
}

// A rock's composition list as one line, and whether it is the list expected
struct PartWanted { const char* ore; float percent; int quality; };   // ore "" for the inert rest
std::string partsAs(const SCMiner::RockContents& contents) {
    std::string out;
    char text[64];
    for (const SCMiner::RockPartRead& part : contents.parts) {
        std::snprintf(text, sizeof(text), "%s%s %.2f%%@%d", out.empty() ? "" : ", ",
                      part.inert ? "inert" : part.ore.empty() ? "?" : part.ore.c_str(), part.percent, part.quality);
        out += text;
    }
    return out.empty() ? "nothing" : out;
}
bool partsAre(const SCMiner::RockContents& contents, const std::vector<PartWanted>& wanted) {
    if (!contents.whole || contents.parts.size() != wanted.size()) return false;
    for (size_t i = 0; i < wanted.size(); i++) {
        const SCMiner::RockPartRead& part = contents.parts[i];
        const bool rest = wanted[i].ore[0] == 0;
        if (part.inert != rest || (!rest && part.ore != wanted[i].ore) || part.quality != wanted[i].quality ||
            std::abs(part.percent - wanted[i].percent) > 0.005f) {
            return false;
        }
    }
    return true;
}

// A test picture: beside the build where there is one, out of the exe where
// not. A release folder has no tests/ beside it, and the checks that read
// these pictures must run there too: that folder is what a player gets.
static cv::Mat testPicture(const std::string& underTests, const wchar_t* resource) {
    for (const char* up : {"", "../", "../../", "../../../"}) {
        // asked first: imread says so loudly about every file that is not there
        const std::string path = std::string(up) + "tests/" + underTests;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        cv::Mat beside = cv::imread(path, cv::IMREAD_COLOR);
        if (!beside.empty()) return beside;
    }
    // ...and out of the program itself otherwise, on both platforms alike
    const unsigned char* bytes = nullptr;
    size_t size = 0;
    std::string name;
    for (const wchar_t* c = resource; *c; c++) name += (char)*c;   // the names are plain ASCII
    if (!SCMiner::embeddedResource(name, &bytes, &size) || !bytes || size == 0) return cv::Mat();
    return cv::imdecode(cv::Mat(1, (int)size, CV_8UC1, const_cast<unsigned char*>(bytes)), cv::IMREAD_COLOR);
}

// The 1080p frame of tests/scanpanel: whole where it lies beside the build -
// 3.7 MB, too much to carry - and otherwise the region around its panel out of
// the exe, put back where it was on a black screen. The panel is found at the
// same box and read to the same figures either way.
static cv::Mat testFrame1080() {
    for (const char* up : {"", "../", "../../", "../../../"}) {
        const std::string path = std::string(up) + "tests/scanpanel/scan_fullscreen_1080.png";
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) continue;
        cv::Mat whole = cv::imread(path, cv::IMREAD_COLOR);
        if (!whole.empty()) return whole;
    }
    const cv::Mat region = testPicture("scanpanel/scan_panel_1080_region.png", L"SELFTEST_PANEL_1080");
    if (region.empty()) return cv::Mat();
    cv::Mat screen(1080, 1920, CV_8UC3, cv::Scalar(0, 0, 0));
    region.copyTo(screen(cv::Rect(1229, 335, region.cols, region.rows)));
    return screen;
}

}  // namespace

int runSelfTest() {
    std::cout << "\n=== Bedrock self-test ===\n" << std::endl;
    bool allPassed = true;

    // Figures are read, written and parsed with a decimal point whatever the
    // system's language: on Linux, Qt takes the C library's locale from the
    // environment, and under LANG=de_DE.UTF-8 64.16 parses as 64 and a
    // crafted laser's 12.4 per cent as 12 (linux_app.cpp).
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", 64.16);
        allPassed &= check(std::strtod("64.16", nullptr) == 64.16 && std::atof("12.4") == 12.4 &&
                               std::string(text) == "64.16",
                           "figures go with a decimal point, whatever the system's language (got " + std::string(text) +
                               ")");
    }

    // --- Blueprint library: the log line, and the names as the log writes them ---
    {
        std::string name, stamp;
        const bool event = SCMiner::BlueprintLibrary::parseReceived(
            "<2026-08-30T21:17:49.987Z> [Notice] <SHUDEvent_OnNotification> Added notification "
            "\"Received Blueprint: Arclight \"Stormfall\" Pistol: \" [85] to queue. New queue size: 3, "
            "MissionId: [00000000-0000-0000-0000-000000000000], ObjectiveId: [] [Team_CoreGameplayFeatures]",
            name, stamp);
        allPassed &= check(event && name == "Arclight \"Stormfall\" Pistol" && stamp == "2026-08-30 21:17",
                           "a Received Blueprint line yields name and time (got: " + name + " / " + stamp + ")");
        allPassed &= check(!SCMiner::BlueprintLibrary::parseReceived(
                               "<2026-08-30T21:17:49.987Z>    \"Received Blueprint: FS-9 LMG: \" [83]", name, stamp),
                           "a queue dump line is not an event");
        allPassed &= check(SCMiner::BlueprintLibrary::normalise("Ind/1/B Guardian") == "guardian",
                           "the class/size/grade prefix of a ship part is dropped");
        allPassed &= check(SCMiner::BlueprintLibrary::normalise("Monde Arms <EM4>[BP]</EM4>") == "monde arms",
                           "the HUD markup of older logs is dropped");
        allPassed &= check(SCMiner::BlueprintLibrary::normalise("Oracle\xC2\xA0Helmet") == "oracle helmet",
                           "a no-break space reads as a space");
        allPassed &= check(SCMiner::BlueprintLibrary::normalise("Antium Core Moss Camo ") ==
                               SCMiner::BlueprintLibrary::normalise("Antium Core Moss Camo"),
                           "a trailing space does not make a different name");
        allPassed &= check(SCMiner::BlueprintLibrary::normalise("FS-9 Magazine (75 cap)") == "fs 9 magazine 75 cap",
                           "punctuation folds to spaces");
    }

    // --- Contracts that paid nothing: a contract sure to pay out a
    //     blueprint while one is missing pays none once it has none left, so
    //     the rest of its set is owned. Only on logs of 4.10 on, only when no
    //     blueprint follows within the window, and a contract the log proves
    //     wrong assumes nothing any more ---
    {
        using SCMiner::BlueprintLibrary;
        using SCMiner::ContractScan;
        allPassed &= check(ContractScan::bareTitle("Ship In Distress <EM4>[300 Rep] [BP]*</EM4>") == "Ship In Distress" &&
                               ContractScan::bareTitle("Deep space hit <EM4>[BP]</EM4>") == "Deep space hit" &&
                               ContractScan::bareTitle("Crew Hasn't Checked In [60/100 Rep]*") == "Crew Hasn't Checked In",
                           "a contract's title comes off the log without Bedrock's marks and the reputation");
        allPassed &= check(ContractScan::titleMatches("Take Outlaws at {*} Offline", "take outlaws at Rat's Nest offline") &&
                               !ContractScan::titleMatches("Take Outlaws at {*} Offline", "Take Outlaws at Rat's Nest") &&
                               ContractScan::payoutFor("Ship In Distress") != nullptr &&
                               ContractScan::payoutFor("Deep space hit") == nullptr &&
                               ContractScan::payoutFor("Additional Resources For Research") == nullptr,
                           "only a contract sure to pay has a payout: not one whose pool goes by region, not a 25 % roll");

        auto rowOf = [](const char* name) {
            for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
                if (std::string(SCMiner::CRAFT_BLUEPRINTS[b].name) == name) return b;
            }
            return -1;
        };
        const int marlin = rowOf("Marlin"), rn7 = rowOf("RN-7s"), lawson = rowOf("Lawson Mining Laser");
        auto at = [](const char* time) { return std::string("<2026-08-27T") + time + ".000Z> "; };
        auto received = [](const char* name) {
            return std::string("[Notice] <SHUDEvent_OnNotification> Added notification \"Received Blueprint: ") + name +
                   ": \" [3] to queue. New queue size: 1";
        };
        auto complete = [](const char* title) {
            return std::string("[Notice] <SHUDEvent_OnNotification> Added notification \"Contract Complete: ") + title +
                   ": \" [12] to queue. New queue size: 1, MissionId: [11111111-2222-3333-4444-555555555555]";
        };
        const std::string v410 = "[Trace] @env_session:  'pub-sc-alpha-4100-12519617'";
        const std::string later = "[Notice] <Update Inventory Location> Player is changing location.";

        BlueprintLibrary lib;
        ContractScan scan;
        lib.feedLine(at("22:00:00") + v410, scan);
        lib.feedLine(at("22:10:00") + received("Marlin"), scan);
        lib.feedLine(at("22:47:00") + complete("Ship In Distress <EM4>[300 Rep] [BP]*</EM4>"), scan);
        lib.feedLine(at("22:47:50") + later, scan);
        allPassed &= check(lawson >= 0 && lib.owned(lawson) && lib.sourceOf(lawson) == "assumed: Ship In Distress" &&
                               lib.sourceOf(marlin) == "log" && lib.assumedCount() == 3,
                           "Ship In Distress paid nothing: the three of its set not received are assumed owned (" +
                               std::to_string(lib.assumedCount()) + ")");
        lib.feedLine(at("23:30:00") + received("RN-7s"), scan);
        lib.feedLine(at("23:40:00") + complete("Ship In Distress [300 Rep]*"), scan);
        lib.feedLine(at("23:41:00") + later, scan);
        allPassed &= check(lib.sourceOf(rn7) == "log" && !lib.owned(lawson) && lib.assumedCount() == 0 &&
                               lib.doubted().count("ship in distress") == 1,
                           "...RN-7s came later all the same: the contract's guesses go, and it guesses no more");

        BlueprintLibrary older, paid, twice;
        ContractScan s1, s2, s3;
        older.feedLine(at("22:00:00") + "[Trace] @env_session:  'pub-sc-alpha-490-12232306'", s1);
        older.feedLine(at("22:47:00") + complete("Ship In Distress [300 Rep]*"), s1);
        older.feedLine(at("22:47:50") + later, s1);
        paid.feedLine(at("22:00:00") + v410, s2);
        paid.feedLine(at("22:47:00") + complete("Ship In Distress [300 Rep]*"), s2);
        paid.feedLine(at("22:47:01") + received("Marlin"), s2);
        paid.feedLine(at("22:47:50") + later, s2);
        twice.feedLine(at("22:00:00") + v410, s3);
        twice.feedLine(at("22:47:00") + complete("Ship In Distress [300 Rep]*"), s3);
        twice.feedLine(at("22:47:10") + complete("Ship In Distress [300 Rep]*"), s3);
        twice.feedLine(at("22:47:11") + received("Marlin"), s3);
        twice.feedLine(at("22:48:00") + later, s3);
        allPassed &= check(older.assumedCount() == 0 && paid.assumedCount() == 0 && twice.assumedCount() == 0 &&
                               twice.finishFile(s3) == 0,
                           "nothing is assumed from a log of 4.9, a contract that paid, or two at once with one blueprint");
    }

    // --- The player's rank with each outfit: their own contracts
    //     out of the log, by the very contract the objective marker names; one
    //     shared by somebody else is not theirs, a PTU log in LIVE's folder is
    //     not LIVE's; the rank is the best lower bound of what is proven ---
    {
        using SCMiner::BlueprintLibrary;
        using SCMiner::ContractScan;
        namespace Rep = SCMiner::Reputation;
        const SCMiner::RepContract* covalex = Rep::contract("Covalex_Hauling/HaulCargo_AToB_Interstellar_Bulk_Plas_Scr_Cob");
        const SCMiner::RepContract* redWind = Rep::contract("RedWind_Hauling/HaulCargo_AToB_Interstellar_Bulk_Plas_Scr_Cob");
        allPassed &= check(covalex && redWind && covalex->ladder >= 0 && covalex->rank != redWind->rank,
                           "a contract is known by its generator and its name: the same name is a Master haul of "
                           "Covalex's and a Rookie one of Red Wind's");
        const SCMiner::RepContract* plain = Rep::contract("Adagio_Generator/Adaigo_Stanton1_Lawful_Salvage_Hard");
        allPassed &= check(plain && Rep::contract("Adagio_Generator/Adaigo_Stanton1_Lawful_Salvage_Hard_2") == plain &&
                               !Rep::contract("Adagio_Generator/Adaigo_Stanton1_Lawful_Salvage_Hardest_2"),
                           "the number the game puts after a contract's name is no part of it");

        // A contract of Citizens For Prosperity at Contractor that adds to its Standing
        int cfp = -1, ladder = Rep::ladderByGuid("d31bd373-35ab-4ea4-b0a7-0ccc867ef082");
        for (int f = 0; f < SCMiner::REP_FACTION_COUNT; f++) {
            if (std::string(SCMiner::REP_FACTIONS[f].name) == "Citizens For Prosperity") cfp = f;
        }
        std::string key;
        int award = 0;
        for (int c = 0; c < SCMiner::REP_CONTRACT_COUNT && key.empty(); c++) {
            const SCMiner::RepContract& rc = SCMiner::REP_CONTRACTS[c];
            if (rc.faction != cfp || rc.ladder != ladder || rc.rank != 2) continue;
            for (int k = 0; k < rc.awardCount; k++) {
                const SCMiner::RepAward& a = SCMiner::REP_AWARDS[rc.awardFirst + k];
                if (a.faction == cfp && a.ladder == ladder && a.amount > 0) {
                    key = rc.key;
                    award = a.amount;
                }
            }
        }
        const size_t slash = key.find('/');
        const std::string gen = key.substr(0, slash), name = slash == std::string::npos ? "" : key.substr(slash + 1);
        auto at = [](const char* time) { return std::string("<2026-10-01T") + time + ".000Z> "; };
        auto marker = [&](const char* mission) {
            return "[Notice] <CLocalMissionPhaseMarker::CreateMarker> Creating objective marker: missionId [" +
                   std::string(mission) + "], generator name [" + gen + "], contract [" + name +
                   "], contractDefinitionId[00000000-0000-0000-0000-000000000000], objectiveId [x]";
        };
        auto note = [](const char* kind, const char* mission) {
            return std::string("[Notice] <SHUDEvent_OnNotification> Added notification \"Contract ") + kind +
                   ":  Some Contract: \" [3] to queue. New queue size: 1, MissionId: [" + mission +
                   "], ObjectiveId: [] [Team_CoreGameplayFeatures][Missions][Comms]";
        };
        const char* own = "11111111-1111-1111-1111-111111111111";
        const char* shared = "22222222-2222-2222-2222-222222222222";
        const char* unnamed = "33333333-3333-3333-3333-333333333333";
        ContractScan scan;
        BlueprintLibrary lib;
        lib.feedLine(at("10:00:00") + "[Trace] @env_session:  'pub-sc-alpha-4100-12519617'", scan);
        lib.feedLine(at("10:01:00") + marker(own), scan);
        lib.feedLine(at("10:01:00") + note("Accepted", own), scan);
        lib.feedLine(at("10:02:00") + "[Notice] <MissionShared> Received share push message: ownerId[1] - missionId[" +
                         shared + "] [Team_GameServices][Missions]",
                     scan);
        lib.feedLine(at("10:02:01") + marker(shared), scan);
        lib.feedLine(at("10:02:02") + note("Accepted", shared), scan);
        lib.feedLine(at("10:03:00") + note("Accepted", unnamed), scan);
        lib.feedLine(at("10:20:00") + note("Complete", own), scan);
        lib.feedLine(at("10:40:00") + note("Complete", shared), scan);
        lib.finishFile(scan);
        int taken = 0, done = 0;
        for (const auto& [k, e] : lib.contracts()) {
            (void)k;
            (e.completed ? done : taken)++;
        }
        allPassed &= check(!key.empty() && taken == 1 && done == 1 && lib.contracts().begin()->second.contract == key &&
                               lib.contracts().begin()->second.version == 41000,
                           "the player's own contract is read off the log, taken and finished; one shared with them "
                           "and one no marker names are not (" + std::to_string(taken) + " taken, " +
                               std::to_string(done) + " finished)");
        SCMiner::Standing cfpStanding;
        for (const auto& st : lib.standings()) {
            if (st.faction == cfp && st.ladder == ladder) cfpStanding = st;
        }
        allPassed &= check(cfpStanding.rank == 2 && cfpStanding.anchorRank == 2 &&
                               cfpStanding.reputation == Rep::rankMin(ladder, 2) + award && cfpStanding.completions == 1,
                           "a Contractor contract taken proves Contractor, and what was finished after it adds on (" +
                               std::to_string(cfpStanding.reputation) + ")");

        // The same, from before the wipe, counts for nothing; a rank set
        // later counts on from itself
        std::vector<SCMiner::RankEvent> events;
        for (const auto& [k, e] : lib.contracts()) {
            (void)k;
            events.push_back(e);
        }
        std::vector<SCMiner::RankEvent> old = events;
        for (auto& e : old) e.version = 40700;
        const std::vector<SCMiner::Standing> before = Rep::standings(old, {}, Rep::LIVE_WIPE);
        SCMiner::RankSetting set{SCMiner::REP_FACTIONS[cfp].guid, SCMiner::REP_LADDERS[ladder].guid,
                                 Rep::rankMin(ladder, 4), "2026-10-01T10:10:00.000"};
        long long afterSet = -1;
        int setRank = -1;
        for (const auto& st : Rep::standings(events, {set}, Rep::LIVE_WIPE)) {
            if (st.faction == cfp && st.ladder == ladder) {
                afterSet = st.reputation;
                setRank = st.rank;
            }
        }
        allPassed &= check(before.empty() && setRank == 4 && afterSet == Rep::rankMin(ladder, 4) + award,
                           "contracts of a game before the wipe count for nothing, and a rank set by hand counts on "
                           "from itself, the contract taken before it no more");

        // A PTU log in LIVE's folder: its blueprints are not LIVE's
        auto received = [](const char* name) {
            return std::string("[Notice] <SHUDEvent_OnNotification> Added notification \"Received Blueprint: ") + name +
                   ": \" [3] to queue. New queue size: 1";
        };
        auto rowOf = [](const char* name) {
            for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
                if (std::string(SCMiner::CRAFT_BLUEPRINTS[b].name) == name) return b;
            }
            return -1;
        };
        BlueprintLibrary live;
        ContractScan ptuScan, pubScan;
        live.feedLine(at("09:00:00") + "[Trace] @env_session:  'ptu-sc-alpha-470-11414557'", ptuScan);
        live.feedLine(at("09:10:00") + received("Testudo Arms Nightveil"), ptuScan);
        live.finishFile(ptuScan);
        live.feedLine(at("11:00:00") + "[Trace] @env_session:  'pub-sc-alpha-470-11638371'", pubScan);
        live.feedLine(at("11:10:00") + received("Marlin"), pubScan);
        live.finishFile(pubScan);
        const int nightveil = rowOf("Testudo Arms Nightveil"), marlin = rowOf("Marlin");
        allPassed &= check(nightveil >= 0 && !live.owned(nightveil) && live.owned(marlin),
                           "a blueprint received in a PTU log that lay in LIVE's folder is not LIVE's");
    }

    // --- More off the game's log: what a gathering order wants
    //     brought, the contracts running right now, and a gadget the log saw
    //     go on a rock - whose figures the panel then prints with it ---
    {
        using SCMiner::BlueprintLibrary;
        using SCMiner::ContractScan;
        namespace Rep = SCMiner::Reputation;
        const std::string generator = "Shubin_ResourceGathering_ShipMining";
        const std::string name = "Shubin_RG_Discovery_ShipMining_Stanton_Torite_Bexalite";
        const SCMiner::RepContract* order = Rep::contract(generator + "/" + name);
        bool torite = false, bexalite = false;
        for (int k = 0; order && k < order->oreCount; k++) {
            const SCMiner::OrderOre& ore = SCMiner::ORDER_ORES[order->oreFirst + k];
            if (std::string(ore.name) == "Torite" && ore.least == 16 && ore.scu) torite = true;
            if (std::string(ore.name) == "Bexalite" && ore.least == 11 && ore.scu) bexalite = true;
        }
        const SCMiner::RepContract* freight =
            Rep::contract("Covalex_Hauling/HaulCargo_AToB_Interstellar_Bulk_PreIce_Tung_Lara");
        allPassed &= check(order && order->oreCount == 2 && torite && bexalite &&
                               std::string(order->title) == "Lrg. Purchase Order: Ship Mined Ore" && freight &&
                               freight->oreCount == 0,
                           "Shubin's large purchase order wants 16 SCU of Torite and 11 of Bexalite; a hauling "
                           "contract's Tungsten is freight, not an order");
        allPassed &= check(Rep::epochOf("2026-10-02T16:39:32.688") == 1790959172LL &&
                               Rep::epochOf("2026-10-02T16:39:32") == 1790959172LL && Rep::epochOf("yesterday") == -1,
                           "a log stamp is a time, with its milliseconds or without");

        auto at = [](const char* time) { return std::string("<2026-10-01T") + time + ".000Z> "; };
        auto marker = [&](const char* mission, const std::string& contract) {
            return "[Notice] <CLocalMissionPhaseMarker::CreateMarker> Creating objective marker: missionId [" +
                   std::string(mission) + "], generator name [" + generator + "], contract [" + contract +
                   "], contractDefinitionId[00000000-0000-0000-0000-000000000000], objectiveId [x]";
        };
        auto note = [](const char* kind, const char* mission) {
            return std::string("[Notice] <SHUDEvent_OnNotification> Added notification \"Contract ") + kind +
                   ":  Lrg. Purchase Order: Ship Mined Ore: \" [3] to queue. New queue size: 1, MissionId: [" + mission +
                   "], ObjectiveId: [] [Team_CoreGameplayFeatures][Missions][Comms]";
        };
        const char* own = "11111111-1111-1111-1111-111111111111";
        const char* shared = "22222222-2222-2222-2222-222222222222";
        ContractScan scan;
        BlueprintLibrary lib;
        lib.feedLine(at("10:00:00") + "[Trace] @env_session:  'pub-sc-alpha-4100-12519617'", scan);
        lib.feedLine(at("10:01:00") + marker(own, name + "_3"), scan);
        lib.feedLine(at("10:01:00") + note("Accepted", own), scan);
        const bool taken = scan.running().size() == 1 && !scan.running()[0].shared &&
                           Rep::contract(scan.running()[0].contract) == order;
        lib.feedLine(at("10:02:00") + "[Notice] <MissionShared> Received share push message: ownerId[1] - missionId[" +
                         shared + "] [Team_GameServices][Missions]",
                     scan);
        lib.feedLine(at("10:02:01") + marker(shared, name), scan);
        lib.feedLine(at("10:02:02") + note("Accepted", shared), scan);
        const bool both = scan.running().size() == 2 && scan.running()[1].shared;
        lib.feedLine(at("10:30:00") + "[Notice] <EndMission> Ending mission for player. MissionId[" + std::string(own) +
                         "] Player[Somebody] PlayerId[1] CompletionType[Abandon] Reason[Player left] "
                         "[Team_MissionFeatures][Missions]",
                     scan);
        const bool givenUp = scan.running().size() == 1 && scan.running()[0].mission == shared;
        lib.feedLine(at("10:40:00") + note("Complete", shared), scan);
        allPassed &= check(taken && both && givenUp && scan.running().empty() && lib.contracts().size() == 1 &&
                               scan.runningChanges() == 4,
                           "a contract taken runs until the log says it is over - given up, or finished - and one "
                           "shared runs as well, without counting as the player's own");

        // The gadget: what the panel prints with it on, taken back
        const SCMiner::MiningGadget* sabir = SCMiner::gadgetById("shin_sabir");
        SCMiner::RockStats rock;
        rock.mass = 21470;
        rock.resistance = 56;
        rock.instability = 12.4f;
        rock.ore = "SAVRILIUM (ORE)";
        const SCMiner::RockStats printed = SCMiner::withGadget(rock, sabir);
        const SCMiner::RockStats back = SCMiner::withoutGadget(printed, sabir);
        SCMiner::RockStats iron = rock;
        iron.mass = 8000;
        iron.ore = "IRON (ORE)";
        SCMiner::GadgetOnRock on;
        on.gadget = sabir;
        on.oreWord = "Savrilium";
        const SCMiner::RockStats first = on.take(iron);          // another ore: not the rock it went on
        const bool leftAlone = !on.onView && on.mass == 0 && first.resistance == iron.resistance;
        const SCMiner::RockStats own1 = on.take(printed);        // the first of its ore: taken for it
        const bool takenBack = on.onView && std::abs(own1.resistance - 56.0f) < 0.01f &&
                               std::abs(own1.instability - 12.4f) < 0.01f;
        const SCMiner::RockStats next = on.take(iron);
        allPassed &= check(sabir && printed.resistance < 56.0f && std::abs(back.resistance - 56.0f) < 0.01f &&
                               leftAlone && takenBack && !on.onView && next.resistance == iron.resistance,
                           "a gadget on a rock: the figures the panel prints with it are taken back to the rock's "
                           "own, and any other rock is read as it stands");

        // ...and the log's lines for it
        SCMiner::GameState::Event placed, inHand, another;
        const bool p = SCMiner::GameState::parse(
            "<2026-09-12T09:08:24.458Z> [Notice] <[ActorState] Place> [ACTOR STATE][CSCActorControlAdditiveStatePlace::"
            "DoPlace] 'Somebody' [204700000001] placed 'Mining_Gadget_SHIN_Sabir_800000000101' [800000000101] on "
            "entity 'MineableRock_AsteroidLegendary_Savrilium_812345675858' [812345675858] [Team_ActorFeatures][Actor]",
            &placed);
        const bool h = SCMiner::GameState::parse(
            "<2026-09-12T09:08:52.480Z> [Notice] <AttachmentReceived> Player[Somebody] Attachment[Mining_Gadget_SHIN_"
            "Sabir_800000000101, Mining_Gadget_SHIN_Sabir, 800000000101] Status[persistent] Port[weapon_attach_hand_"
            "right] Elapsed[49.816944] [Team_CoreGameplayFeatures][Inventory]",
            &inHand);
        another = inHand;
        another.gadgetEntity = "804707040000";
        SCMiner::GameState state;
        const bool went = state.apply(placed) && state.gadgetOnRock() == "Mining_Gadget_SHIN_Sabir" &&
                          state.gadgetRock() == "MineableRock_AsteroidLegendary_Savrilium";
        const bool stays = !state.apply(another) && !state.apply(placed) && !state.gadgetOnRock().empty();
        const bool off = state.apply(inHand) && state.gadgetOnRock().empty() && state.gadgetChanges() == 2;
        allPassed &= check(p && h && inHand.gadgetInHand && went && stays && off,
                           "the log says when a gadget goes on a rock, and when that very one is back in the hand");
    }

    // --- What a rock is made of: the parts of a rock of each ore
    //     and the eight values an ore's quality comes out at, checked against
    //     a Tin rock read off the scan panel - 10.99 % Tin at 537,
    //     63.84 % Tin at 340, 8.38 % Copper at 652 ---
    {
        auto bandOf = [](const char* ore, int quality) {
            for (int b = 0; b < SCMiner::ORE_BANDS_COUNT; b++) {
                if (std::string(SCMiner::ORE_BANDS[b].ore) != ore) continue;
                for (int k = 0; k < 8; k++) {
                    if (SCMiner::ORE_BANDS[b].bands[k] == quality) return k;
                }
            }
            return -1;
        };
        std::vector<const SCMiner::RockPart*> tin;
        for (int i = 0; i < SCMiner::ROCK_PART_COUNT; i++) {
            if (std::string(SCMiner::ROCK_PARTS[i].rock) == "Tin") tin.push_back(&SCMiner::ROCK_PARTS[i]);
        }
        const bool parts = tin.size() == 3 && std::string(tin[0]->ore) == "Tin" && tin[0]->qualityScale == 1.0f &&
                           std::string(tin[1]->ore) == "Tin" && tin[1]->most > 60.0f &&
                           std::string(tin[2]->ore) == "Copper";
        const bool inReach = parts && tin[0]->least <= 10.99f && 10.99f <= tin[0]->most &&
                             tin[1]->least <= 63.84f && 63.84f <= tin[1]->most &&
                             tin[2]->least <= 8.38f && 8.38f <= tin[2]->most &&
                             bandOf("Tin", 537) >= 0 && bandOf("Tin", 537) <= SCMiner::topQualityBand(tin[0]->qualityScale) &&
                             bandOf("Tin", 340) >= 0 && bandOf("Tin", 340) <= SCMiner::topQualityBand(tin[1]->qualityScale) &&
                             bandOf("Copper", 652) >= 0 &&
                             bandOf("Copper", 652) <= SCMiner::topQualityBand(tin[2]->qualityScale);
        allPassed &= check(SCMiner::topQualityBand(1.0f) == 7 && SCMiner::topQualityBand(0.789f) == 3 &&
                               SCMiner::topQualityBand(0.49f) == 1 && inReach,
                           "a rock is built of parts: a little of its ore at any quality, the bulk out of the two "
                           "lowest bands, a side ore out of the four lowest - as a scanned Tin rock reads");
        // ...and its quality roll is 501 and up, mostly just over it
        // (crafting/qualitydistribution): a Gold rock's little Gold is never
        // under 553, the bulk nearly always 264
        const SCMiner::RockPart* small = nullptr;
        const SCMiner::RockPart* bulk = nullptr;
        const SCMiner::RockPart* borase = nullptr;
        for (int i = 0; i < SCMiner::ROCK_PART_COUNT; i++) {
            const SCMiner::RockPart& p = SCMiner::ROCK_PARTS[i];
            if (std::string(p.rock) != "Gold") continue;
            if (std::string(p.ore) == "Gold" && p.qualityScale == 1.0f) small = &p;
            if (std::string(p.ore) == "Gold" && p.qualityScale < 0.5f) bulk = &p;
            if (std::string(p.ore) == "Borase") borase = &p;
        }
        bool oddsWhole = small && bulk && borase;
        for (const SCMiner::RockPart* p : {small, bulk, borase}) {
            if (!p) continue;
            double sum = 0;
            for (double o : SCMiner::MinerGUI::qualityOdds(*p)) sum += o;
            oddsWhole &= std::abs(sum - 1.0) < 1e-6;
        }
        allPassed &= check(oddsWhole && SCMiner::lowestQualityBand(*small) == 1 && SCMiner::lowestQualityBand(*bulk) == 0 &&
                               SCMiner::MinerGUI::rockPartQuality(*small) ==
                                   L"553–1000 · mostly 553 or 644" &&
                               SCMiner::MinerGUI::rockPartQuality(*bulk) == L"quality 264, rarely 553" &&
                               SCMiner::MinerGUI::rockPartQuality(*borase) == L"mostly 584 · 359–743",
                           "a rock's little part of its ore is 501 and up, its bulk nearly always the lowest band - "
                           "the Ore Finder says so of a Gold rock (got " +
                               [](std::wstring w) {
                                   std::string s;
                                   for (wchar_t c : w) s += c < 128 ? (char)c : c == 0x2013 ? '-' : c == 0xB7 ? '.' : '?';
                                   return s;
                               }((small ? SCMiner::MinerGUI::rockPartQuality(*small) : L"-") + L" | " +
                                 (bulk ? SCMiner::MinerGUI::rockPartQuality(*bulk) : L"-") + L" | " +
                                 (borase ? SCMiner::MinerGUI::rockPartQuality(*borase) : L"-")) + ")");
    }

    // --- The refinery's terminal: a work order writes nothing
    //     to the game's log, so the REFINEMENT CENTER is read off the
    //     screen - its figures, its names, its panels, and the book the
    //     orders go into ---
    {
        const bool figures =
            SCMiner::parseRefineryCount("874") == 874 && SCMiner::parseRefineryCount(".234") == 234 &&
            SCMiner::parseRefineryCount(" 874 ") == 874 && SCMiner::parseRefineryCount("--") == -1 &&
            SCMiner::parseRefineryCount("O") == 0 && SCMiner::parseRefineryCount("IRON") == -1 &&
            SCMiner::parseRefineryTime("7m 22s") == 442 && SCMiner::parseRefineryTime("  7m 3s") == 423 &&
            SCMiner::parseRefineryTime("1h 2m 3s") == 3723 && SCMiner::parseRefineryTime("45s") == 45 &&
            SCMiner::parseRefineryTime("--") == -1 && SCMiner::parseRefineryTime("7") == -1 &&
            std::abs(SCMiner::parseRefineryAmount("194.00") - 194.0) < 1e-9 &&
            std::abs(SCMiner::parseRefineryAmount("1.63") - 1.63) < 1e-9 && SCMiner::parseRefineryAmount("--") < 0;
        allPassed &= check(figures, "the refinery terminal's figures read: a count with a stray mark at its edge, a time "
                                    "in hours, minutes and seconds, an amount with its stop - and \"--\" as none");
        bool inert = false;
        const bool names =
            SCMiner::refiningMethodIn(" PYROMETRIC CHROMALYSIS ") == "Pyrometric Chromalysis" &&
            SCMiner::refiningMethodIn("SELECT AN OPTION ") == "" &&
            SCMiner::refineryMaterialIn("IRON (ORE] >") == "Iron (Ore)" &&
            SCMiner::refineryMaterialIn("IRON\xEF\xBC\x9A\xEF\xBC\x8E") == "Iron (Ore)" &&
            SCMiner::refineryMaterialIn("INERT MATERIALE", &inert) == "" && inert &&
            SCMiner::refineryMaterialIn("Warning:Accepting this work") == "" &&
            SCMiner::refineryStationAt("Levski", "Nyx") == "Levski" &&
            SCMiner::refineryStationAt("Pyro Gateway", "Nyx") == "Pyro Gateway (Nyx)" &&
            SCMiner::refineryStationAt("Monox", "Pyro") == "" &&
            SCMiner::refineryStationNamed("LEVSKI", "Nyx") == "Levski" &&
            SCMiner::refineryStationNamed("LEVSK1", "") == "Levski" &&
            SCMiner::refineryStationNamed("HUR-L1 GREEN GLADE STATION", "Stanton") == "HUR-L1 Green Glade Station" &&
            SCMiner::refineryStationNamed("PYRO GATEWAY", "Nyx") == "Pyro Gateway (Nyx)" &&
            SCMiner::refineryStationNamed("REFINEMENT CENTER", "Nyx") == "";
        allPassed &= check(names, "...the method and the material a line names, the inert rest, a warning box's line as "
                                  "none of them; the refinery at the place the log names, and the one the terminal's "
                                  "header names - a gateway's by the system");

        // The station's workload, off its REFINERY CAPACITY - as read on the
        // terminals at Checkmate, Pyro Gateway and Levski
        using SCMiner::refineryWorkloadIn;
        const bool workload =
            SCMiner::parseRefineryPercent("103%") == 103 && SCMiner::parseRefineryPercent("42804% ") == 42804 &&
            SCMiner::parseRefineryPercent("CURRENT CAPACITY 952%") == 952 &&
            SCMiner::parseRefineryPercent("1O3 %") == 103 && SCMiner::parseRefineryPercent("103") == -1 &&
            SCMiner::parseRefineryPercent("%") == -1 &&
            refineryWorkloadIn("Refinery currently has a light workload. No surcharge at this time.") == 0 &&
            refineryWorkloadIn("REFINERY CURRENTLY HAS A MODERATE") == 1 &&
            refineryWorkloadIn("REINERYCURRENTLYHASAMODERATE") == 1 &&
            refineryWorkloadIn("Refinery currently has a heavy workload. A moderate surcharge will be added.") == 2 &&
            refineryWorkloadIn("Refinery currently has an extreme workload. A large surcharge will be added.") == 3 &&
            refineryWorkloadIn("Refinerycurenty haanextm") == 3 &&
            refineryWorkloadIn("Workoad.largeurcharewil") == 3 &&
            refineryWorkloadIn("WORKLOAD. A MODERATE SURCHARGE WILL BE") == 2 &&
            refineryWorkloadIn("Refinery currently has a modrte workoad. A small surcharge will be added.") == 1 &&
            refineryWorkloadIn("Refinery currently has a hcavv workload. A moderate surcharge will be added.") == 2 &&
            refineryWorkloadIn("A SURCHARGE WILL BE ADDED") == -1 &&
            refineryWorkloadIn("// REFINERY CAPACITY") == -1 &&
            std::string(SCMiner::refineryWorkloadName(3)) == "extreme" && std::string(SCMiner::refineryWorkloadName(4)).empty() &&
            SCMiner::refineryMaterialIn(" CONSTRUCTION MA") == "Construction Salvage" &&
            SCMiner::refineryMaterialIn("CONSTRUCTION MATERIALS") == "Construction Salvage" &&
            SCMiner::refineryMaterialIn("CONSTRUCTION SALVAGE") == "Construction Salvage";
        allPassed &= check(workload, "a refinery's workload reads: CURRENT CAPACITY's figure, and the game's sentence on "
                                     "it by its word or, misread, by the surcharge's; the salvage's refined name");

        // ...and shared (RefineryShare): the report as the service takes it,
        // its answer, and when a reading goes again
        namespace S = SCMiner::RefineryShare;
        S::Reading r;
        r.station = "HUR-L1 Green Glade Station";
        r.observed = 1791709800;
        r.capacity = 106;
        r.workload = 1;
        r.method = "DINYX_SOLVENTATION";
        r.cost = 4210.4;
        r.seconds = 23040;
        r.items = {{"Iron (Ore)", 1050, 874, 958}, {"Gold \"(Ore)\"", 20, -1, 17}};
        S::Reading bare;
        bare.station = "Checkmate";
        bare.observed = 5;
        bare.workload = 0;
        std::map<std::string, S::StationState> states;
        std::string why;
        const bool readsBack = S::readStations(
            "{\"generated\":1,\"stations\":[{\"station\":\"Checkmate\",\"capacity\":103,\"workload\":\"moderate\","
            "\"observed\":1791713037,\"fresh\":true,\"sources\":2,\"confirmed\":true,\"seen_24h\":5,\"quotes_24h\":0},"
            "{\"station\":\"Levski\",\"capacity\":null,\"workload\":\"extreme\",\"observed\":7,\"sources\":0,"
            "\"confirmed\":false}]}", states, why);
        const bool shared =
            S::body(r, "2.6.7", 41002, "LIVE") ==
                "{\"v\":1,\"bedrock\":\"2.6.7\",\"game\":\"4.10.2\",\"channel\":\"LIVE\",\"observed\":1791709800,"
                "\"station\":\"HUR-L1 Green Glade Station\",\"capacity\":106,\"workload\":\"moderate\",\"quote\":{"
                "\"method\":\"DINYX_SOLVENTATION\",\"cost\":4210,\"seconds\":23040,\"items\":["
                "{\"material\":\"Iron (Ore)\",\"quantity\":1050,\"quality\":874,\"yield\":958},"
                "{\"material\":\"Gold \\\"(Ore)\\\"\",\"quantity\":20,\"yield\":17}]}}" &&
            S::body(bare, "2.6.7", 0, "") ==
                "{\"v\":1,\"bedrock\":\"2.6.7\",\"observed\":5,\"station\":\"Checkmate\",\"workload\":\"light\"}" &&
            readsBack && states.size() == 2 && states["Checkmate"].capacity == 103 && states["Checkmate"].workload == 1 &&
            states["Checkmate"].sources == 2 && states["Checkmate"].confirmed && states["Levski"].capacity == -1 &&
            states["Levski"].workload == 3 && !states["Levski"].confirmed &&
            !S::readStations("{\"stations\":5}", states, why) &&
            S::worthSending(106, 1, 1000, -1, -1, 0) && !S::worthSending(106, 1, 1100, 106, 1, 1000) &&
            !S::worthSending(103, 1, 1100, 106, 1, 1000) && S::worthSending(101, 1, 1100, 106, 1, 1000) &&
            S::worthSending(106, 2, 1100, 106, 1, 1000) && !S::worthSending(-1, 1, 1100, 106, 1, 1000) &&
            !S::worthSending(-1, 1, 1000 + S::RESEND_SECONDS, 106, 1, 1000) &&
            S::worthSending(-1, 2, 1100, 106, 1, 1000) && S::worthSending(106, 1, 1100, -1, 1, 1000) &&
            !S::worthSending(106, -1, 1100, 106, 1, 1000) &&
            S::worthSending(106, 1, 1000 + S::RESEND_SECONDS, 106, 1, 1000) &&
            !S::worthSending(-1, -1, 9999, 1, 1, 1);
        allPassed &= check(shared, "refinery workloads shared: the report as the service takes it, with a quote and "
                                   "without, the stations it hands back, and a reading sent again at once when its "
                                   "level changed or it moved five points, else ten minutes on - and never one "
                                   "that says less than the last");

        // The bars over the panels, drawn the size and colour they have at
        // 5120x1440: a SETUP's red band under one, a PROCESSING's amber, the
        // station's own panel with its orange heading under it and no band,
        // a thin rule of the HUD
        cv::Mat frame(1440, 5120, CV_8UC3, cv::Scalar(30, 34, 30));
        auto bar = [&](int x, int y, int w, int h, const cv::Scalar& band) {
            cv::rectangle(frame, cv::Rect(x, y, w, h), cv::Scalar(190, 192, 188), cv::FILLED);
            cv::putText(frame, "WORK ORDER", cv::Point(x + 12, y + h - 4), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                        cv::Scalar(40, 40, 40), 1);
            if (band[0] >= 0) cv::rectangle(frame, cv::Rect(x, y + h + 4, w, 36), band, cv::FILLED);
        };
        bar(1962, 274, 446, 18, cv::Scalar(41, 46, 79));
        bar(2462, 274, 444, 18, cv::Scalar(40, 77, 89));
        bar(1584, 274, 340, 18, cv::Scalar(-1, 0, 0));
        cv::putText(frame, "// MATERIAL SPECIALIZATIONS", cv::Point(1590, 322), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    cv::Scalar(40, 160, 230), 1);
        bar(4676, 46, 444, 8, cv::Scalar(8, 32, 71));
        // (in other light a bar more may come up - the station's own panel,
        // its heading tinted into a band - which its title turns away)
        const auto barsIn = [](const cv::Mat& picture, double k, bool only) {
            const std::vector<SCMiner::RefineryBar> bars = SCMiner::findRefineryBars(picture);
            const auto at = [&](int x, int width, SCMiner::RefineryBar::Band band) {
                return std::any_of(bars.begin(), bars.end(), [&](const SCMiner::RefineryBar& b) {
                    return b.band == band && std::abs(b.bar.x - x * k) <= 2 + 2 * k &&
                           std::abs(b.bar.width - width * k) <= 4 + 4 * k;
                });
            };
            return (!only || bars.size() == 2) && at(1962, 446, SCMiner::RefineryBar::Band::Setup) &&
                   at(2462, 444, SCMiner::RefineryBar::Band::Processing);
        };
        allPassed &= check(barsIn(frame, 1.0, true), "the work orders' bars found by their shape and the band under them: a "
                                               "SETUP's, a PROCESSING's, and neither the station's own panel nor a "
                                               "thin rule of the HUD");

        // ...and as other screens and other light show them: smaller (a
        // 1080 screen, or further off) and larger (4K), a little blurred,
        // darker, under a tint
        const auto madeOver = [](const cv::Mat& picture, double scale, double sigma, double gain, const cv::Scalar& tint) {
            cv::Mat out;
            cv::resize(picture, out, cv::Size(), scale, scale, scale < 1 ? cv::INTER_AREA : cv::INTER_CUBIC);
            if (sigma > 0) cv::GaussianBlur(out, out, cv::Size(), sigma);
            std::vector<cv::Mat> ch;
            cv::split(out, ch);
            for (int c = 0; c < 3; c++) ch[c].convertTo(ch[c], -1, gain * tint[c]);
            cv::merge(ch, out);
            return out;
        };
        const cv::Scalar plain(1, 1, 1), warmTint(0.92, 1.0, 1.07);
        const bool barsEverywhere = barsIn(madeOver(frame, 0.6, 0, 1, plain), 0.6, false) &&
                                    barsIn(madeOver(frame, 1.7, 0, 1, plain), 1.7, false) &&
                                    barsIn(madeOver(frame, 1.0, 1.2, 1, plain), 1.0, false) &&
                                    barsIn(madeOver(frame, 1.0, 0, 0.65, plain), 1.0, false) &&
                                    barsIn(madeOver(frame, 1.0, 0, 1, warmTint), 1.0, false);
        allPassed &= check(barsEverywhere, "...at 0.6 and 1.7 times the size, blurred, two thirds as light, and tinted "
                                           "warm");

        // Pyro's dress: a hatched block at a panel's top left, red stripes for
        // SETUP and orange for PROCESSING on near black (Checkmate) - and an
        // orange patch of the same size without stripes, as sand is, is none
        cv::Mat pyro(1440, 5120, CV_8UC3, cv::Scalar(26, 24, 30));
        auto hatch = [&](int x, int y, const cv::Scalar& ink) {
            for (int d = -40; d < 181 + 40; d += 7) {
                cv::line(pyro, cv::Point(x + d, y + 31), cv::Point(x + d + 31, y), ink, 3);
            }
            cv::rectangle(pyro, cv::Rect(x - 40, y - 2, 40, 35), cv::Scalar(26, 24, 30), cv::FILLED);
            cv::rectangle(pyro, cv::Rect(x + 181, y - 2, 40, 35), cv::Scalar(26, 24, 30), cv::FILLED);
        };
        hatch(1972, 285, cv::Scalar(42, 32, 127));
        hatch(2447, 280, cv::Scalar(68, 119, 203));
        hatch(2922, 280, cv::Scalar(106, 196, 180));
        cv::rectangle(pyro, cv::Rect(3200, 600, 181, 31), cv::Scalar(70, 110, 170), cv::FILLED);
        const auto blocksIn = [](const cv::Mat& picture, double k) {
            const std::vector<SCMiner::RefineryBar> blocks = SCMiner::findRefineryBars(picture);
            return blocks.size() == 3 && blocks[0].theme == SCMiner::RefineryBar::Theme::Pyro &&
                   blocks[0].band == SCMiner::RefineryBar::Band::Setup &&
                   blocks[1].band == SCMiner::RefineryBar::Band::Processing &&
                   blocks[2].band == SCMiner::RefineryBar::Band::Completed &&
                   std::abs(blocks[0].bar.x - 1964 * k) <= 4 + 4 * k && std::abs(blocks[0].bar.width - 446 * k) <= 8 + 8 * k;
        };
        allPassed &= check(blocksIn(pyro, 1.0), "...and Pyro's hatched blocks, red for SETUP, orange for PROCESSING and "
                                                "yellow-green for COMPLETED, taken for the bars they stand for - not an "
                                                "orange patch without stripes");
        const bool blocksEverywhere = blocksIn(madeOver(pyro, 0.6, 0, 1, plain), 0.6) &&
                                      blocksIn(madeOver(pyro, 1.7, 0, 1, plain), 1.7) &&
                                      blocksIn(madeOver(pyro, 1.0, 1.2, 1, plain), 1.0) &&
                                      blocksIn(madeOver(pyro, 1.0, 0, 0.65, plain), 1.0) &&
                                      blocksIn(madeOver(pyro, 1.0, 0, 1, warmTint), 1.0);
        allPassed &= check(blocksEverywhere, "...the blocks at 0.6 and 1.7 times the size, blurred, two thirds as light, "
                                             "and tinted warm");

        // Pyro Gateway's (Stanton): the same blocks paler, on a
        // warm ground lit between the stripes - none of it near black
        const cv::Scalar warm(48, 56, 66);
        cv::Mat gateway(1440, 5120, CV_8UC3, warm);
        auto hatchOn = [&](int x, int y, const cv::Scalar& ink) {
            for (int d = -40; d < 181 + 40; d += 7) {
                cv::line(gateway, cv::Point(x + d, y + 31), cv::Point(x + d + 31, y), ink, 3);
            }
            cv::rectangle(gateway, cv::Rect(x - 40, y - 2, 40, 35), warm, cv::FILLED);
            cv::rectangle(gateway, cv::Rect(x + 181, y - 2, 40, 35), warm, cv::FILLED);
        };
        hatchOn(2008, 280, cv::Scalar(40, 30, 130));
        hatchOn(2492, 280, cv::Scalar(60, 100, 170));
        cv::rectangle(gateway, cv::Rect(3200, 600, 181, 31), cv::Scalar(60, 100, 170), cv::FILLED);
        const std::vector<SCMiner::RefineryBar> pale = SCMiner::findRefineryBars(gateway);
        const bool paleFound = pale.size() == 2 && pale[0].band == SCMiner::RefineryBar::Band::Setup &&
                               pale[1].band == SCMiner::RefineryBar::Band::Processing;
        allPassed &= check(paleFound, "...Pyro Gateway's paler blocks on a ground lit between the stripes as well - "
                                      "and still no patch without stripes");

        // The book: a quote read once, the order it became read twice - then
        // a minute later, a garbled reading, another station, a second order
        using SCMiner::RefineryBar;
        using SCMiner::RefineryPanel;
        using SCMiner::RefineryScreen;
        auto row = [](const char* material, int quality, int quantity, int yield) {
            SCMiner::RefineryRow r;
            r.material = material;
            r.quality = quality;
            r.quantity = quantity;
            r.yield = yield;
            return r;
        };
        RefineryPanel setup;
        setup.kind = RefineryBar::Band::Setup;
        setup.method = "Pyrometric Chromalysis";
        setup.cost = 194;
        setup.seconds = 442;
        setup.rows = {row("Iron (Ore)", 325, 236, -1), row("Iron (Ore)", 874, 484, 235)};
        RefineryPanel running;
        running.kind = RefineryBar::Band::Processing;
        running.order = 1;
        running.rows = {row("Iron (Ore)", 874, -1, 235)};
        auto screen = [](std::vector<RefineryPanel> panels) {
            RefineryScreen s;
            s.panels = std::move(panels);
            return s;
        };
        auto at = [](RefineryPanel p, int seconds) {
            p.seconds = seconds;
            return p;
        };
        const long long t0 = 1791304360;
        SCMiner::RefineryOrderBook book;
        book.take("Levski", screen({setup}), t0);
        const bool once = !book.take("Levski", screen({at(running, 440)}), t0 + 4) && book.orders().empty();
        const bool twice = book.take("Levski", screen({at(running, 439)}), t0 + 5) && book.orders().size() == 1;
        const SCMiner::RefineryOrder first = book.orders().empty() ? SCMiner::RefineryOrder() : book.orders()[0];
        const bool quoted = first.number == 1 && first.due == t0 + 444 && first.method == "Pyrometric Chromalysis" &&
                            first.cost == 194 && first.seconds == 442 && first.placed == t0 + 2 && first.yieldTotal() == 235 &&
                            first.parts.size() == 1 && first.parts[0].quantity == 484 && first.complete();
        book.take("Levski", screen({at(running, 380)}), t0 + 64);
        const bool later = !book.take("Levski", screen({at(running, 379)}), t0 + 65) && book.orders().size() == 1;
        RefineryPanel garbled = at(running, 378);
        garbled.rows[0].quality = -1;
        const bool notGarbled = !book.take("Levski", screen({garbled}), t0 + 66) &&
                                !book.take("Levski", screen({garbled}), t0 + 67) && book.orders().size() == 1;
        const bool notElsewhere = !book.take("Checkmate", screen({at(running, 300)}), t0 + 144) &&
                                  book.orders().size() == 1;
        RefineryPanel second = running;
        second.order = 2;
        second.rows = {row("Iron (Ore)", 325, -1, 114)};
        book.take("Levski", screen({at(running, 300), at(second, 209)}), t0 + 144);
        book.take("Levski", screen({at(running, 299), at(second, 208)}), t0 + 145);
        const bool another = book.orders().size() == 2 && book.sorted()[0].number == 2 && book.sorted()[0].method.empty();
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "bedrock_selftest_refinery_orders.txt";
        SCMiner::RefineryOrderBook again;
        const bool handed = book.markHanded(0, t0 + 500) && !book.markHanded(0, t0 + 501);
        const bool kept = handed && book.save(file) && again.load(file) && again.orders().size() == 2 &&
                          again.orders()[0].parts.size() == 1 && again.orders()[0].parts[0].material == "Iron (Ore)" &&
                          again.orders()[0].parts[0].quantity == 484 && again.orders()[0].handed == t0 + 500 &&
                          !again.orders()[1].complete() &&
                          again.orders()[0].cost == 194 && again.orders()[0].due == t0 + 444 && again.remove(0) &&
                          again.orders().size() == 1 && again.orders()[0].number == 2;
        std::error_code gone;
        std::filesystem::remove(file, gone);
        // A long list scrolls: an order of four parts looked at in two halves,
        // its quote's list showing the last rows only - one order, four parts,
        // the quote on it; and an Iron quality Iron does not come in dropped
        SCMiner::RefineryOrderBook scrolled;
        RefineryPanel bigQuote = setup;
        bigQuote.cost = 1210;
        bigQuote.seconds = 3600;
        bigQuote.rows = {row("Gold (Ore)", 553, 120, 54), row("Copper (Ore)", 652, 300, 135)};
        RefineryPanel top = running;
        top.order = 3;
        top.rows = {row("Quantainium (Raw)", 669, -1, 210), row("Bexalite (Raw)", 756, -1, 88)};
        RefineryPanel bottom = top;
        bottom.rows = {row("Gold (Ore)", 553, -1, 54), row("Copper (Ore)", 652, -1, 135)};
        scrolled.take("Checkmate", screen({bigQuote}), t0);
        scrolled.take("Checkmate", screen({at(top, 3597)}), t0 + 3);
        scrolled.take("Checkmate", screen({at(top, 3596)}), t0 + 4);
        scrolled.take("Checkmate", screen({at(bottom, 3594)}), t0 + 6);
        scrolled.take("Checkmate", screen({at(bottom, 3593)}), t0 + 7);
        // Done and collected (Checkmate): COMPLETED marks both
        // orders done; one left off a look stays without the notice; under
        // "WORK ORDER DELIVERED", with the other moved up to number 1, the
        // first comes off on the second look - and the last over an empty
        // list, while an order still running stays
        SCMiner::RefineryOrderBook pickup;
        RefineryPanel one = running;
        RefineryPanel two = running;
        two.order = 2;
        two.rows = {row("Iron (Ore)", 664, -1, 52), row("Torite (Ore)", 873, -1, 31)};
        RefineryPanel three = running;
        three.order = 3;
        three.rows = {row("Gold (Ore)", 553, -1, 54)};
        pickup.take("Checkmate", screen({at(one, 300), at(two, 200), at(three, 4000)}), t0);
        pickup.take("Checkmate", screen({at(one, 299), at(two, 199), at(three, 3999)}), t0 + 1);
        auto done = [](RefineryPanel p, int number) {
            p.kind = RefineryBar::Band::Completed;
            p.order = number;
            p.seconds = -1;
            return p;
        };
        pickup.take("Checkmate", screen({done(one, 1), done(two, 2)}), t0 + 400);
        pickup.take("Checkmate", screen({done(one, 1), done(two, 2)}), t0 + 401);
        const bool bothDone = pickup.orders().size() == 3 && pickup.orders()[0].completed == t0 + 401 &&
                              pickup.orders()[1].completed == t0 + 401 && pickup.orders()[2].completed == 0 &&
                              pickup.anyDone(t0 + 401);
        pickup.take("Checkmate", screen({done(two, 1)}), t0 + 402);
        pickup.take("Checkmate", screen({done(two, 1)}), t0 + 403);
        const bool notWithoutNotice = pickup.orders().size() == 3;
        // ...under the notice with neither panel read, no telling which of
        // the two went: both stay; the other read, the first comes off
        RefineryScreen unread;
        unread.delivered = true;
        pickup.take("Checkmate", unread, t0 + 404);
        const bool notOnOneLook = pickup.orders().size() == 3;
        pickup.take("Checkmate", unread, t0 + 405);
        const bool notWhenUnsure = pickup.orders().size() == 3;
        RefineryScreen delivered = screen({done(two, 1)});
        delivered.delivered = true;
        pickup.take("Checkmate", delivered, t0 + 406);
        const bool firstOff = pickup.orders().size() == 2 && pickup.orders()[0].parts.size() == 2 &&
                              pickup.orders()[0].number == 1;
        // ...and one notice takes one: the last stays while it stands
        pickup.take("Checkmate", unread, t0 + 407);
        pickup.take("Checkmate", unread, t0 + 408);
        const bool oneEach = pickup.orders().size() == 2;
        RefineryScreen emptied;
        emptied.delivered = true;
        pickup.take("Checkmate", emptied, t0 + 430);
        pickup.take("Checkmate", emptied, t0 + 431);
        const bool lastOff = pickup.orders().size() == 1 && pickup.orders()[0].completed == 0 &&
                             pickup.orders()[0].parts[0].material == "Gold (Ore)";
        allPassed &= check(bothDone && notWithoutNotice && notOnOneLook && notWhenUnsure && firstOff && oneEach && lastOff,
                           "work orders done and collected: COMPLETED marks them done; under WORK ORDER DELIVERED "
                           "the one the list no longer shows comes off on the second look - one for each notice, "
                           "none while there is no telling which - the last one over an empty list; not without "
                           "the notice, nor one still running");
        const bool oneOrder = scrolled.orders().size() == 1 && scrolled.orders()[0].parts.size() == 4 &&
                              scrolled.orders()[0].yieldTotal() == 487 && scrolled.orders()[0].cost == 1210 &&
                              scrolled.orders()[0].method == "Pyrometric Chromalysis";
        const bool bands = SCMiner::refineryQualityFits("Iron (Ore)", 874) &&
                           !SCMiner::refineryQualityFits("Iron (Ore)", 674) &&
                           SCMiner::refineryQualityFits("Aluminum (Ore)", 511) &&
                           SCMiner::refineryQualityFits("Raw Ice", 561) &&
                           SCMiner::refineryQualityFits("Construction Rubble", 123);
        allPassed &= check(oneOrder && bands, "a long list scrolled through: one order of all its parts, with the quote "
                                              "whose list showed only some of them; a quality its ore does not come in "
                                              "is no reading");
        // Two set up one after the other, the second's quote read before the
        // first order was: each order its own quote - not the newest for both
        // (one job went to UEX, the other did not, 15 October 2026)
        SCMiner::RefineryOrderBook pair;
        RefineryPanel quoteB = setup;
        quoteB.cost = 201;
        quoteB.seconds = 470;
        quoteB.rows = {row("Iron (Ore)", 874, 500, 243)};
        RefineryPanel orderB = running;
        orderB.order = 2;
        orderB.rows = {row("Iron (Ore)", 874, -1, 243)};
        pair.take("Levski", screen({setup}), t0);
        pair.take("Levski", screen({quoteB}), t0 + 22);
        pair.take("Levski", screen({at(running, 415), at(orderB, 464)}), t0 + 30);
        pair.take("Levski", screen({at(running, 414), at(orderB, 463)}), t0 + 31);
        const bool eachOwn = pair.orders().size() == 2 && pair.orders()[0].cost == 194 && pair.orders()[0].complete() &&
                             pair.orders()[0].placed == t0 + 3 && pair.orders()[1].cost == 201 &&
                             pair.orders()[1].complete() && pair.orders()[1].placed == t0 + 24;
        // ...the same job set up twice: two quotes, an order each
        SCMiner::RefineryOrderBook twin;
        RefineryPanel running2 = running;
        running2.order = 2;
        twin.take("Levski", screen({setup}), t0);
        twin.take("Levski", screen({at(running, 440)}), t0 + 4);
        twin.take("Levski", screen({at(running, 439)}), t0 + 5);
        twin.take("Levski", screen({at(running, 430), setup}), t0 + 14);
        twin.take("Levski", screen({at(running, 425), at(running2, 440)}), t0 + 19);
        twin.take("Levski", screen({at(running, 424), at(running2, 439)}), t0 + 20);
        const bool twins = twin.orders().size() == 2 && twin.orders()[0].complete() && twin.orders()[1].complete() &&
                           twin.orders()[1].placed == t0 + 17;
        // ...an order first read five minutes after it was placed, the player
        // gone at once: its quote by its time and a part in common - none
        // without one, and the record says why
        SCMiner::RefineryOrderBook late;
        late.take("Levski", screen({setup}), t0);
        late.take("Levski", screen({at(running, 145)}), t0 + 300);
        late.take("Levski", screen({at(running, 144)}), t0 + 301);
        const bool lateQuoted = late.orders().size() == 1 && late.orders()[0].complete() && late.orders()[0].placed == t0 + 3;
        SCMiner::RefineryOrderBook stranger;
        RefineryPanel gold = running;
        gold.rows = {row("Gold (Ore)", 553, -1, 54)};
        stranger.take("Levski", screen({setup}), t0);
        stranger.take("Levski", screen({at(gold, 145)}), t0 + 300);
        stranger.take("Levski", screen({at(gold, 144)}), t0 + 301);
        const std::vector<std::string> notes = stranger.takeNotes();
        const bool unquoted = stranger.orders().size() == 1 && stranger.orders()[0].method.empty() && notes.size() == 2 &&
                          notes[0].find("quote at Levski: Pyrometric Chromalysis, 194 aUEC, 442 s") == 0 &&
                          notes[1].find("without a quote (1 quote at the station, none fits its time)") !=
                              std::string::npos &&
                          notes[1].find("lacks its method") != std::string::npos && stranger.takeNotes().empty();
        allPassed &= check(eachOwn && twins && lateQuoted && unquoted,
                           "quotes kept for their orders: two set up one after the other each to its own, the same job "
                           "twice two, an order read five minutes on still its own - not one without a part in common, "
                           "and the record says why");
        allPassed &= check(once && twice && quoted && later && notGarbled && notElsewhere && another && kept,
                           "the work orders' book: an order taken on the second reading that agrees, with the quote "
                           "read before it and what went in; the same order later only kept; a garbled reading, "
                           "another station's nothing; a second order beside it, not whole without a quote; saved "
                           "with its handover to UEX, loaded and one ticked off");
    }

    // --- The panel's composition list: what the reader makes of
    //     a share and a quality, which line is which ore, and when a list
    //     hangs together ---
    {
        using SCMiner::RockContents;
        float share = 0;
        int quality = 0;
        const bool shares = SCMiner::parseShare("10.55%", share) && std::abs(share - 10.55f) < 0.001f &&
                            SCMiner::parseShare("1055%", share) && std::abs(share - 10.55f) < 0.001f &&
                            SCMiner::parseShare("B.06%", share) && std::abs(share - 8.06f) < 0.001f &&
                            SCMiner::parseShare("100.00", share) && std::abs(share - 100.0f) < 0.001f &&
                            SCMiner::parseShare("3.71% SI", share) && std::abs(share - 3.71f) < 0.001f &&
                            !SCMiner::parseShare("105.5%", share) && !SCMiner::parseShare("5%", share) &&
                            !SCMiner::parseShare("LOCK", share) && !SCMiner::parseShare("100.01%", share);
        allPassed &= check(shares, "a share reads to two places with its stop or without, and nothing else is one");
        const bool qualities = SCMiner::parseQuality("664", quality) && quality == 664 &&
                               SCMiner::parseQuality("S04", quality) && quality == 504 &&
                               SCMiner::parseQuality("1000", quality) && quality == 1000 &&
                               !SCMiner::parseQuality("1001", quality) && !SCMiner::parseQuality("", quality) &&
                               SCMiner::qualityOfOre("Iron", 664) && !SCMiner::qualityOfOre("Iron", 654) &&
                               !SCMiner::qualityOfOre("Iron", 0);
        allPassed &= check(qualities, "a quality is a whole number up to 1000, and one of its ore's eight values");

        // a Bexalite rock as a Golem's panel lists it: Gold before Borase,
        // where the files have Borase first
        RockContents bexalite;
        bexalite.parts = {{"", false, 7.42f, 597}, {"", false, 57.0f, 302}, {"", false, 4.85f, 553},
                          {"", false, 4.36f, 676}, {"", true, 26.34f, 0}};
        const bool named = SCMiner::nameContentsByRock("Bexalite", bexalite) && SCMiner::contentsHold(bexalite) &&
                           partsAre(bexalite, {{"Bexalite", 7.42f, 597}, {"Bexalite", 57.0f, 302}, {"Gold", 4.85f, 553},
                                               {"Borase", 4.36f, 676}, {"", 26.34f, 0}});
        // a piece broken off an Iron rock: one line, all of it the bulk
        RockContents piece;
        piece.parts = {{"", false, 100.0f, 325}};
        const bool pieceNamed = SCMiner::nameContentsByRock("Iron", piece) && SCMiner::contentsHold(piece) &&
                                partsAre(piece, {{"Iron", 100.0f, 325}});
        // Hephaestanite and Quartz both begin at 330: that line's name has to be read
        RockContents heph;
        heph.parts = {{"", false, 3.71f, 510}, {"", false, 6.43f, 572}, {"", false, 57.79f, 330},
                      {"", false, 2.92f, 522}, {"", true, 29.12f, 0}};
        const bool twoOres = !SCMiner::nameContentsByRock("Hephaestanite", heph);
        // ...and a quality no ore of the rock has names nothing
        RockContents wrong;
        wrong.parts = {{"", false, 10.55f, 654}, {"", false, 36.1f, 325}, {"", true, 53.33f, 0}};
        const bool noOre = !SCMiner::nameContentsByRock("Iron", wrong);
        // an Ice rock's bulk read as 922 for 322: a value Ice has, and a
        // second line of it above what the bulk comes to
        RockContents ice;
        ice.parts = {{"", false, 11.4f, 659}, {"", false, 76.61f, 922}, {"", true, 11.97f, 0}};
        const bool twoBest = SCMiner::nameContentsByRock("Ice", ice) && SCMiner::contentsHold(ice) &&
                             !SCMiner::contentsFitRock("Ice", ice) && SCMiner::contentsFitRock("Bexalite", bexalite) &&
                             SCMiner::contentsFitRock("Iron", piece) && SCMiner::contentsFitRock("Nothing", ice);
        allPassed &= check(twoBest, "two lines of a rock's ore above what its bulk comes to are not that rock's lines");
        allPassed &= check(named && pieceNamed && twoOres && noOre,
                           "a line is the ore whose value its quality is, in any order and at any share - and has "
                           "to be read by name where two ores of the rock share the value (got " + partsAs(bexalite) + ")");

        auto holds = [](std::vector<SCMiner::RockPartRead> parts) {
            RockContents c;
            c.parts = std::move(parts);
            return SCMiner::contentsHold(c);
        };
        const bool sums = holds({{"Iron", false, 10.55f, 664}, {"Iron", false, 36.1f, 325}, {"", true, 53.33f, 0}}) &&
                          !holds({{"Iron", false, 10.58f, 664}, {"Iron", false, 36.1f, 325}, {"", true, 53.33f, 0}}) &&
                          !holds({{"Iron", false, 10.05f, 664}, {"Iron", false, 36.1f, 325}, {"", true, 53.33f, 0}}) &&
                          !holds({{"Iron", false, 10.55f, 654}, {"Iron", false, 36.1f, 325}, {"", true, 53.33f, 0}}) &&
                          !holds({{"", false, 10.55f, 664}, {"Iron", false, 36.1f, 325}, {"", true, 53.33f, 0}}) &&
                          !holds({{"Iron", false, 10.55f, 664}, {"Iron", false, 36.1f, 325}}) &&
                          holds({{"Iron", false, 100.0f, 325}}) && holds({{"Iron", false, 99.99f, 325}});
        allPassed &= check(sums, "a list holds when its shares add up to the rock - a few hundredths under 100, cut "
                                 "off and not rounded - and every quality is one its ore has");
    }

    // --- The trade book: what a commodity terminal's request
    //     says, which requests the shop refused, and where each was made.
    //     The lines are the game's own, their ids made up. ---
    {
        using SCMiner::Trade;
        using SCMiner::TradeBook;
        const std::string sale =
            "<2026-09-28T11:37:34.512Z> [Notice] <CEntityComponentCommodityUIProvider::SendCommoditySellRequest> Sending "
            "SShopCommoditySellRequest - playerId[1234567890123] shopId[9876543210987] "
            "shopName[SCShop_Trdpst_Warehouse_OTLW_Int_A] kioskId[5555555555555] amount[1021440.000000] "
            "resourceGUID[ca524406-0e3e-4d5e-baad-8a6c7b42054b] autoLoading[0] quantity[64] "
            "transactionMode[ResourceContainer] Cargo Box Data:  [boxSize[2] | unitAmount[32]] "
            "[Team_CoreGameplayFeatures][Shops][UI]";
        const std::string purchase =
            "<2026-09-28T11:30:00.101Z> [Notice] <CEntityComponentCommodityUIProvider::SendCommodityBuyRequest> Sending "
            "SShopCommodityBuyRequest - playerId[1234567890123] shopId[9876543210987] "
            "shopName[SCShop_ht_delta_shubin_m_store] kioskId[5555555555555] price[1377500.000000] "
            "shopPricePerCentiSCU[275.500000] resourceGUID[21825507-7923-4683-9bf3-9cfe316940e3] autoLoading[0] "
            "quantity[5000.000000 cSCU] Cargo Box Data: boxSize[1.000000] | unitAmount[50] "
            "[Team_CoreGameplayFeatures][Shops][UI]";
        Trade sold, bought, none;
        const bool parsed =
            TradeBook::parseRequest(sale, sold) && sold.sold && sold.stamp == "2026-09-28T11:37:34.512" &&
            sold.resource == "ca524406-0e3e-4d5e-baad-8a6c7b42054b" && sold.scu == 64.0 && sold.total == 1021440.0 &&
            sold.shop == "SCShop_Trdpst_Warehouse_OTLW_Int_A" && std::abs(sold.perScu() - 15960.0) < 0.01 &&
            TradeBook::parseRequest(purchase, bought) && !bought.sold && bought.scu == 50.0 && bought.total == 1377500.0 &&
            std::abs(bought.perScu() - 27550.0) < 0.01 && std::string(TradeBook::nameOf(sold.resource)) == "Elespo" &&
            std::string(TradeBook::nameOf(bought.resource)) == "Gold" &&
            !TradeBook::parseRequest("<2026-09-28T11:37:34.512Z> [Notice] <CreateCommodityInfo> Shopping Kiosk Context", none);
        allPassed &= check(parsed, "a sale's request says what, how much in SCU and for how much; a purchase's counts "
                                   "in hundredths of an SCU, and its price is not its price per one");

        auto refusal = [](const char* stamp, const char* type) {
            return std::string("<") + stamp +
                   "Z> [Error] <CEntityComponentCommodityUIProvider::RmToken_CommodityTransactionResponse> Commodity "
                   "Transaction Response Error - playerId[1234567890123] result[InvalidQuantityError] type[" + type +
                   "] [Team_CoreGameplayFeatures][Shops]";
        };
        std::string again = sale;
        again.replace(again.find("11:37:34.512"), 12, "11:38:00.000");
        TradeBook book;
        book.line(purchase, "Shubin Mining Facility SMO-18");
        book.line(sale, "Levski");
        book.line(refusal("2026-09-28T11:37:34.900", "Selling"), "Levski");   // the sale above: refused
        book.line(refusal("2026-09-28T11:37:36.000", "Buying"), "Levski");    // no purchase a moment before it
        book.line(again, "Levski");
        book.line(again, "");                                                  // the same line read again
        const std::vector<Trade> all = book.trades();
        const bool booked = all.size() == 3 && !all[0].sold && !all[0].refused &&
                            all[0].place == "Shubin Mining Facility SMO-18" && all[1].sold && all[1].refused &&
                            all[2].sold && !all[2].refused && all[2].place == "Levski";
        allPassed &= check(booked, "a request counts until the shop's error follows it within moments; a line read "
                                   "twice is one trade, and keeps the place it was made at");

        // a log that is over, all at once: where the trade was made is what
        // the lines before it said of the ship's place
        const std::string log =
            "<2026-09-11T10:27:45.903Z> Loading screen for Nyx : SC_Frontend closed after 44.25 seconds\n"
            "<2026-09-11T10:31:49.569Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
            "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Levski "
            "for route to destination rs_ext_nyx-pyro_jp1 [Team_CGP4][QuantumTravel]\r\n" +
            sale + "\n";
        TradeBook history;
        const bool has = TradeBook::hasRequest(log) && !TradeBook::hasRequest("<2026-09-11T10:27:45.903Z> Loading screen");
        history.replay(log);
        history.replay(log);
        const std::vector<Trade> seen = history.trades();
        allPassed &= check(has && seen.size() == 1 && seen[0].place == "Levski" && seen[0].scu == 64.0,
                           "a finished log's trades are read with the place its own lines put the ship at (got " +
                               std::string(seen.empty() ? "nothing" : seen[0].place) + ")");

    }

    // --- UEX's prices: the JSON reader, and the five places that
    //     pay most for a refined ore - terminals that are full, prices nobody
    //     has seen for a month and terminals that pay nothing left out ---
    {
        using SCMiner::JsonValue;
        using SCMiner::UexOffer;
        using SCMiner::UexPrices;
        JsonValue v;
        std::string why;
        const std::string text =
            " {\"a\": [1, -2.5e2, true, null], \"b\": \"x\\u00e9\\\"\\n\\ud83d\\ude00\", \"c\": {}} ";
        const JsonValue* a = SCMiner::parseJson(text, v, why) ? v.find("a") : nullptr;
        const bool reads = a && a->items.size() == 4 && a->items[0].number == 1.0 && a->items[1].number == -250.0 &&
                           a->items[2].boolean && a->items[3].kind == JsonValue::Kind::Null &&
                           v.textOr("b") == "x\xC3\xA9\"\n\xF0\x9F\x98\x80" && v.numberOr("missing", 7) == 7 &&
                           v.find("c") && v.find("c")->kind == JsonValue::Kind::Object;
        JsonValue bad;
        std::string badWhy;
        const bool refuses = !SCMiner::parseJson("{\"a\": 1,}", bad, badWhy) &&
                             !SCMiner::parseJson("[1] 2", bad, badWhy) &&
                             !SCMiner::parseJson(std::string(100, '['), bad, badWhy) &&
                             !SCMiner::parseJson("\"never closed", bad, badWhy) && !SCMiner::parseJson("", bad, badWhy);
        allPassed &= check(reads && refuses, "the JSON reader takes what UEX sends - escapes, a \\u pair as one "
                                             "character, exponents - and refuses what is broken or nested past reason");

        const long long now = 1791300000;   // a fixed now, about 2026-10-06
        auto terminal = [](int id, const char* system, const char* planet, const char* moon, const char* orbit,
                           const char* station) {
            auto text = [](const char* s) { return s ? "\"" + std::string(s) + "\"" : std::string("null"); };
            return "{\"id\":" + std::to_string(id) + ",\"star_system_name\":" + text(system) +
                   ",\"planet_name\":" + text(planet) + ",\"moon_name\":" + text(moon) + ",\"orbit_name\":" +
                   text(orbit) + ",\"space_station_name\":" + text(station) + "}";
        };
        const std::string terminals =
            "{\"status\":\"ok\",\"data\":[" + terminal(90, "Stanton", "Crusader", nullptr, "Crusader", nullptr) + "," +
            terminal(507, "Pyro", "Pyro V", "Ignis", "Pyro V", nullptr) + "," +
            terminal(742, "Pyro", nullptr, nullptr, "Nyx Gateway (Pyro system)", "Nyx Gateway (Pyro)") + "," +
            terminal(778, "Nyx", nullptr, nullptr, "Delamar", nullptr) + "]}";
        std::string rows;
        auto price = [&](int id, const char* name, const char* commodity, double sell, int status, double days) {
            if (!rows.empty()) rows += ",";
            rows += "{\"id_terminal\":" + std::to_string(id) + ",\"terminal_name\":\"" + name +
                    "\",\"commodity_name\":\"" + commodity + "\",\"price_sell\":" + std::to_string(sell) +
                    ",\"status_sell\":" + std::to_string(status) +
                    ",\"date_modified\":" + std::to_string(now - (long long)(days * 86400)) + "}";
        };
        price(90, "TDD Orison", "Quantainium", 170000, 1, 3.7);
        price(507, "Ashland", "Quantainium", 160000, 1, 4.5);
        price(742, "Nyx Gateway (Pyro)", "Quantainium", 180000, 7, 1);   // full: buys nothing now
        price(778, "Levski", "Quantainium", 190000, 1, 40);              // a month and more old
        price(778, "Levski", "Quantainium", 0, 1, 1);                    // pays nothing
        price(90, "TDD Orison", "Quantainium (Raw)", 50000, 1, 1);       // the ore, not refined
        price(742, "Nyx Gateway (Pyro)", "Aluminum", 4500, 2, 1);
        price(778, "Levski", "Lindinium", 51000, 1, 2);
        for (int i = 0; i < 7; i++) price(90 + i, "Terminal", "Gold", 25000 + 1000 * (i / 2), 1, 7 - i);
        price(507, "Ashland", "Laranite", 9100, 5, 1);                   // stock high: nearly full
        // ...and what terminals ask for it: the cheapest one that has any
        auto asks = [&](int id, const char* name, const char* commodity, double buy, int status, double days) {
            rows += ",{\"id_terminal\":" + std::to_string(id) + ",\"terminal_name\":\"" + name +
                    "\",\"commodity_name\":\"" + commodity + "\",\"price_sell\":0,\"price_buy\":" + std::to_string(buy) +
                    ",\"status_buy\":" + std::to_string(status) +
                    ",\"date_modified\":" + std::to_string(now - (long long)(days * 86400)) + "}";
        };
        asks(90, "TDD Orison", "Gold", 27550, 3, 1);
        asks(507, "Ashland", "Gold", 24000, 1, 1);                       // out of stock: no offer
        asks(778, "Levski", "Gold", 26000, 4, 2);
        const std::string list = "{\"status\":\"ok\",\"data\":[" + rows + "]}";
        UexPrices prices;
        const bool read = prices.read(list, terminals, now, why);
        const auto& q = prices.offersFor("Quantainium");
        const auto& al = prices.offersFor("Aluminium");
        const auto& lin = prices.offersFor("Lindinium");
        const auto& gold = prices.offersFor("Gold");
        const std::string dot = "  \xC2\xB7  ";
        bool goldOrdered = gold.size() == 5;
        for (size_t i = 1; goldOrdered && i < gold.size(); i++) {
            goldOrdered = gold[i - 1].price > gold[i].price ||
                          (gold[i - 1].price == gold[i].price && gold[i - 1].reported > gold[i].reported);
        }
        const bool offers = read && q.size() == 2 && q[0].terminal == "TDD Orison" && q[0].price == 170000 &&
                            q[0].place == "Stanton" + dot + "Crusader" && q[1].place == "Pyro" + dot + "Ignis" &&
                            al.size() == 1 && al[0].place == "Pyro" && lin.size() == 1 &&
                            lin[0].place == "Nyx" + dot + "Delamar" && goldOrdered && gold[0].price == 28000 &&
                            prices.bestPrice("Ice") == 0 &&
                            prices.mostValuable({"Gold", "Aluminium", "Quantainium", "Ice"}) == "Quantainium" &&
                            prices.mostValuable({"Ice"}).empty();
        // the best in a system (any case), the cheapest seller, the stock
        const UexOffer* pyro = prices.bestIn("Quantainium", "PYRO");
        const UexOffer* cheapest = prices.cheapestBuy("Gold");
        const auto& laranite = prices.offersFor("Laranite");
        const bool around = pyro && pyro->terminal == "Ashland" && pyro->price == 160000 &&
                            !prices.bestIn("Quantainium", "Nyx") && cheapest && cheapest->terminal == "Levski" &&
                            cheapest->price == 26000 && !laranite.empty() && laranite[0].nearlyFull() &&
                            !q[0].nearlyFull();
        allPassed &= check(around, "UEX's best buyer in the player's own system, the cheapest terminal selling it "
                                   "(one out of stock left out), and a terminal nearly full said so");
        UexPrices refused;
        std::string refusedWhy;
        const bool says = !refused.read("{\"status\":\"error\",\"message\":\"x\"}", "", now, refusedWhy) &&
                          !refusedWhy.empty() && !refused.read("<html>", "", now, refusedWhy) && refused.empty();
        allPassed &= check(offers && says,
                           "UEX's best five for a refined ore, best first and the newer report first of two alike; "
                           "full terminals, month-old prices, nought and the raw ore left out; a gateway goes by its "
                           "system, Levski by Delamar; Aluminium is UEX's Aluminum (got " +
                               std::to_string(q.size()) + " for Quantainium" + (why.empty() ? "" : ", " + why) + ")");
    }

    // --- Reporting to UEX: the refinery a job was placed at - only where
    //     exactly one fits -, UEX's names for what the refinery calls
    //     otherwise, the request as UEX wants it, its answers ---
    {
        using SCMiner::UexDirectory;
        namespace R = SCMiner::UexReports;
        auto terminal = [](int id, const char* type, const char* nick, const char* place, const char* moon,
                           const char* planet, int available = 1) {
            auto text = [](const char* s) { return s ? "\"" + std::string(s) + "\"" : std::string("null"); };
            return "{\"id\":" + std::to_string(id) + ",\"type\":" + text(type) + ",\"nickname\":" + text(nick) +
                   ",\"name\":" + text(nick) + ",\"displayname\":" + text(place) + ",\"space_station_name\":" +
                   text(place) + ",\"moon_name\":" + text(moon) + ",\"planet_name\":" + text(planet) +
                   ",\"is_available\":" + std::to_string(available) + "}";
        };
        const std::string terminals =
            "{\"status\":\"ok\",\"data\":[" +
            terminal(778, "commodity", "Levski", "Levski", nullptr, nullptr) + "," +
            terminal(786, "commodity_raw", "Levski", "Levski", nullptr, nullptr) + "," +
            terminal(788, "refinery", "Levski", "Levski", nullptr, nullptr) + "," +
            terminal(100, "commodity", "HUR-L1", "HUR-L1 Green Glade Station", nullptr, "Hurston") + "," +
            terminal(101, "refinery", "HUR-L1", "HUR-L1 Green Glade Station", nullptr, "Hurston") + "," +
            terminal(507, "commodity", "Ashland", "Ashland", "Ignis", "Pyro V") + "," +
            terminal(510, "commodity", "Chawla's Beach", "Chawla's Beach", nullptr, "Pyro IV") + "," +
            terminal(688, "commodity", "Sacren's Plot", "Sacren's Plot", nullptr, "Pyro IV") + "," +
            terminal(900, "commodity", "Shubin SM0-18", "Shubin SM0-18", "Wala", "ArcCorp") + "," +
            terminal(999, "commodity", "Gone", "Gone", nullptr, nullptr, 0) + "," +
            terminal(252, "commodity", "Pyro Gateway (Stanton)", "Pyro Gateway (Stanton)", nullptr, nullptr) + "," +
            terminal(743, "commodity", "Pyro Gateway (Nyx)", "Pyro Gateway (Nyx)", nullptr, nullptr) + "," +
            "{\"id\":436,\"type\":\"commodity\",\"nickname\":\"Checkmate\",\"name\":\"Admin - Checkmate\","
            "\"displayname\":\"Checkmate\",\"space_station_name\":\"Checkmate Station\","
            "\"orbit_name\":\"Monox Lagrange Point 4\",\"is_available\":1}]}";
        const std::string commodities =
            "{\"status\":\"ok\",\"data\":[{\"id\":33,\"name\":\"Gold\"},{\"id\":5,\"name\":\"Aluminum\"},"
            "{\"id\":6,\"name\":\"Aluminum (Ore)\"},{\"id\":125,\"name\":\"Ice (Raw)\"},{\"id\":162,\"name\":\"Stileron "
            "(Raw)\"},{\"id\":181,\"name\":\"Construction Material Rubble\"},{\"id\":48,\"name\":\"Iron (Ore)\"}]}";
        const std::string methods =
            "{\"status\":\"ok\",\"data\":[{\"id\":1,\"name\":\"Cormack\"},{\"id\":2,\"name\":\"Pyrometric Chromalysis\"}]}";
        UexDirectory directory;
        std::string why;
        const bool read = directory.read(terminals, commodities, methods, why);
        const bool names = read && directory.refineryTerminal("HUR-L1 Green Glade Station", why) == 101 &&
                           directory.refineryTerminal("Levski", why) == 788 &&
                           directory.refineryTerminal("Nowhere", why) == 0 && directory.commodity("Gold") == 33 &&
                           directory.commodity("Aluminium") == 5 && directory.commodity("Aluminium (Ore)") == 6 &&
                           directory.commodity("Raw Ice") == 125 && directory.commodity("Stileron (Ore)") == 162 &&
                           directory.commodity("Construction Rubble") == 181 && directory.commodity("Iron (Ore)") == 48 &&
                           directory.commodity("Unobtainium") == 0 && directory.method("Cormack Method") == 1 &&
                           directory.method("PYROMETRIC CHROMALYSIS") == 2 && directory.method("XCR Reaction") == 0;
        allPassed &= check(names, "UEX's names for what the refinery says otherwise: Raw Ice, Stileron (Ore), "
                                  "Construction Rubble, Aluminium, the Cormack Method - and a refinery by its station");
        SCMiner::UexRefineryJob::Item iron;
        iron.material = "Iron (Ore)";
        iron.quantity = 484;
        iron.yield = 235;
        const std::string job = R::refineryJob(788, 2, 194, 8, {{48, iron}}, false);
        const bool bodies = job == "{\"is_production\":0,\"id_terminal\":788,\"id_refinery_method\":2,\"cost\":194,"
                                   "\"time_minutes\":8,\"items\":[{\"id_commodity\":48,\"quantity\":484,\"yield\":235}]}";
        allPassed &= check(bodies, "a refinery job as UEX's documentation writes it");
        std::string user;
        const bool answers =
            R::verdict(200, "{\"status\":\"ok\",\"data\":{\"username\":\"Runner\"}}", &user).empty() && user == "Runner" &&
            R::verdict(200, "{\"status\":\"user_not_verified\"}").find("not verified") != std::string::npos &&
            R::keyTrouble(200, "{\"status\":\"invalid_secret_key\"}") &&
            !R::keyTrouble(200, "{\"status\":\"terminal_not_found\"}") && R::keyTrouble(403, "") &&
            // ...and the account behind a good key: restricted, barred,
            // unverified - the key stays, the next job tries again
            !R::keyTrouble(200, "{\"status\":\"user_not_allowed\"}") &&
            R::accountTrouble(200, "{\"status\":\"user_not_allowed\"}") == "datarunner" &&
            R::verdict(200, "{\"status\":\"user_not_allowed\"}").find("this account") != std::string::npos &&
            R::accountTrouble(200, "{\"status\":\"user_disabled\"}") == "blocked" &&
            !R::keyTrouble(200, "{\"status\":\"user_not_verified\"}") &&
            R::accountTrouble(200, "{\"status\":\"user_not_verified\"}") == "unverified" &&
            R::accountTrouble(200, "{\"status\":\"ok\"}").empty() &&
            R::verdict(500, "").find("500") != std::string::npos &&
            R::keyLooksRight(std::string(40, 'a')) && !R::keyLooksRight(std::string(39, 'a')) &&
            // the app token beside the program: "Ab9" mixed as CMakeLists.txt mixes it
            R::unmixAppToken("26,226,156\n") == "Ab9" && R::unmixAppToken("26, 226, 156") == "Ab9" &&
            R::unmixAppToken("").empty() && R::unmixAppToken("26,x,7").empty() &&
            R::unmixAppToken("26,226,156,256").empty() && R::unmixAppToken("91").empty() &&
            !R::keyLooksRight(std::string(39, 'a') + " ") &&
            R::jsonText("a\"b\\c\n") == "\"a\\\"b\\\\c\\n\"";
        allPassed &= check(answers, "UEX's answers read: taken, and by whom; the key turned away stops the reports, a "
                                    "terminal it does not know does not, nor an account it restricts; a key has 40 "
                                    "letters and digits");
        // ...and the app token beside the program, where a release carries it:
        // there, it has to read (a package without it reports nothing)
        {
            std::error_code ec;
            if (std::filesystem::exists(SCMiner::AppPaths::program() / "uex_token.dat", ec)) {
                allPassed &= check(SCMiner::Application::uexTokenForProbe().size() >= 20,
                                   "the UEX app token beside the program reads");
            } else {
                std::cout << "  (no uex_token.dat beside the program: this build asks UEX anonymously)" << std::endl;
            }
        }
    }

    // --- The UEX marketplace: its figures come as numbers or as
    //     strings; sold out, lapsed and other currencies are out; a quality
    //     of 0 or 1 is none said; renewed is the later of put up and approved;
    //     a buyer asking for better than the part is no buyer for it ---
    {
        namespace Mk = SCMiner::UexMarket;
        const long long now = 1791300000;
        auto row = [&](int id, const char* op, const std::string& price, const char* unit, const char* quality,
                       int soldOut, long long expires, const char* currency, long long added, long long approved) {
            return "{\"id\":" + std::to_string(id) + ",\"slug\":\"listing-" + std::to_string(id) + "\",\"title\":\"T" +
                   std::to_string(id) + "\",\"operation\":\"" + op + "\",\"price\":" + price + ",\"unit\":\"" + unit +
                   "\",\"quality\":" + quality + ",\"is_sold_out\":" + std::to_string(soldOut) +
                   ",\"date_expiration\":" + std::to_string(expires) + ",\"currency\":\"" + currency +
                   "\",\"date_added\":" + std::to_string(added) + ",\"date_approved\":" + std::to_string(approved) +
                   ",\"user_username\":\"someone\",\"location\":\"Levski\"}";
        };
        const long long day = 86400;
        const std::string json =
            "{\"status\":\"ok\",\"data\":[" +
            row(1, "buy", "\"81000000\"", "unit", "null", 0, now + 60 * day, "UEC", now - day, now - day) + "," +
            row(2, "buy", "30000000", "scu", "960", 0, now + 30 * day, "UEC", now - 82 * day, now - 8 * day) + "," +
            row(3, "sell", "\"2500000\"", "cscu", "1", 0, now + 9 * day, "UEC", now - 2 * day, now - 2 * day) + "," +
            row(4, "sell", "5", "scu", "700", 1, now + 9 * day, "UEC", now - 2 * day, now - 2 * day) + "," +
            row(5, "sell", "6", "scu", "700", 0, now - day, "UEC", now - 70 * day, now - 61 * day) + "," +
            row(6, "buy", "7", "scu", "700", 0, now + 9 * day, "WIF", now - 2 * day, now - 2 * day) + "]}";
        std::vector<SCMiner::UexListing> listings;
        std::string why;
        const bool read = Mk::readListings(json, now, listings, why);
        bool reads = read && listings.size() == 3 && listings[0].price == 81000000 && listings[0].buying &&
                     listings[0].quality == -1 && listings[1].quality == 960 && listings[1].renewed == now - 8 * day &&
                     listings[2].quality == -1 && !listings[2].buying && listings[2].perScu() == 250000000.0 &&
                     listings[1].perScu() == 30000000.0 && listings[0].perScu() == 0 &&
                     listings[0].url() == "https://uexcorp.space/marketplace/item/info/listing-1/";
        const auto forGood = Mk::buyersFor(listings, 975);
        const auto forLess = Mk::buyersFor(listings, 900);
        std::map<std::string, int> items;
        const bool named = Mk::readItems("{\"status\":\"ok\",\"data\":[{\"id_item\":5943,\"item_name\":\"Ouratite\"},"
                                         "{\"id_item\":\"5852\",\"item_name\":\"Aluminum\"}]}", items, why) &&
                           Mk::itemFor(items, "Ouratite") == 5943 && Mk::itemFor(items, "Aluminium") == 5852 &&
                           Mk::itemFor(items, "Ice") == 0;
        // the player's conversations: the newest about a listing, its page
        // from a notification, and that page's shape for the others
        SCMiner::UexThreads threads;
        const bool talks =
            Mk::readNegotiations("{\"status\":\"ok\",\"data\":[{\"id_listing\":7,\"hash\":\"aaa111\",\"date_modified\":5},"
                                 "{\"id_listing\":7,\"hash\":\"bbb222\",\"date_modified\":9},"
                                 "{\"id_listing\":8,\"hash\":\"ccc333\",\"date_modified\":1}]}", threads, why);
        const bool before = threads.threadUrl(7).empty() && threads.hasThread(7) && !threads.hasThread(9);
        Mk::readNotifications("{\"status\":\"ok\",\"data\":[{\"redir\":\"/marketplace/talk/bbb222/\"}]}", threads);
        const bool learnt = threads.threadUrl(7) == "https://uexcorp.space/marketplace/talk/bbb222/" &&
                            threads.threadUrl(8) == "https://uexcorp.space/marketplace/talk/ccc333/" &&
                            threads.threadUrl(9).empty();
        allPassed &= check(talks && before && learnt, "the player's UEX conversations: the newest about a listing, its "
                                                      "page from a notification, the page's shape for the rest");
        allPassed &= check(reads && forGood.size() == 2 && forLess.size() == 1 && named,
                           "the UEX marketplace read: figures as numbers or strings, sold out, lapsed and other "
                           "currencies out, quality 0 or 1 none, renewed from the approval, cSCU counted in SCU; "
                           "a buyer for better than the part is none (got " + std::to_string(listings.size()) + ")");
    }

    // --- The game's texts: the marks filled in, the file put together, whose
    //     file it is, user.cfg, and one file out of an archive like Data.p4k ---
    {
        using SCMiner::GameTexts;
        const std::string dot = " \xC2\xB7 ";
        // blue for what is missing, plain for what is owned - never <font>,
        // which the mobiGlas prints as raw text
        const std::string blue = "<EM4>", blueEnd = "</EM4>";
        auto owns = [](std::initializer_list<int> rows) {
            std::vector<int> v(rows);
            return GameTexts::OwnedFn([v](int b) { return std::find(v.begin(), v.end(), b) != v.end(); });
        };
        allPassed &= check(GameTexts::render("{S}{T2:5,7|[BP]}", "Title", owns({5})) == " <EM4>[BP]</EM4>",
                           "a title with a blueprint still to get keeps its blue [BP]");
        allPassed &= check(GameTexts::render("{S}{T2:5,7|[BP*]}", "Title ", owns({5, 7})) == "[BP*]",
                           "a title whose blueprints are all owned has a plain tag, and no second space");
        allPassed &= check(GameTexts::render("{I5|\xE2\x80\xA2 A}\\n{I6|\xE2\x80\xA2 B}\\n{I-1|\xE2\x80\xA2 C}", "",
                                             owns({6})) ==
                               blue + "\xE2\x80\xA2 A" + blueEnd + "\\n\xE2\x80\xA2 B\\n" +
                                   blue + "\xE2\x80\xA2 C" + blueEnd,
                           "a missing line is blue, an owned one plain, one with no row missing");
        allPassed &= check(GameTexts::render("x{O3:5,7}", "", owns({5})) == "x" + dot + blue + "2 missing" + blueEnd &&
                               GameTexts::render("x{O2:5,7}", "", owns({5, 7})) == "x" + dot + "all owned" &&
                               GameTexts::render("x{O2:5,7}", "", owns({})) == "x" + dot + blue + "none owned" + blueEnd,
                           "a list says in blue what is missing of it, or that all is owned");
        allPassed &= check(GameTexts::render("h{A3:5,7}", "", owns({5, 7})) == "h" &&
                               GameTexts::render("h{A2:5,7}", "", owns({5, 7})) == "h" + dot + "all owned",
                           "a line without a row keeps 'all owned' away");
        allPassed &= check(GameTexts::render("{S}{T2:5,7|[BP]}{I5|a}{O1:5}{A1:5}", "", owns({5})).find("<font") ==
                               std::string::npos,
                           "no <font> tag, which the mobiGlas shows as text");

        // A key in the case the game writes it, a ',P' the game has dropped,
        // a BOM, CRLF: the file comes back the same with only the marks added
        std::string title, withP;
        for (int i = 0; i < SCMiner::GAME_TEXT_MARK_COUNT; i++) {
            const std::string k = SCMiner::GAME_TEXT_MARKS[i].key;
            if (title.empty() && std::strncmp(SCMiner::GAME_TEXT_MARKS[i].text, "{S}{T", 5) == 0) title = k;
            if (withP.empty() && k.size() > 2 && k.compare(k.size() - 2, 2, ",P") == 0 &&
                std::strncmp(SCMiner::GAME_TEXT_MARKS[i].text, "\\n\\n", 4) == 0) {
                withP = k;    // a description's, which gets a list
            }
        }
        std::string upper = title;
        for (auto& c : upper) c = (char)std::toupper((unsigned char)c);
        const std::string base = "\xEF\xBB\xBF" + upper + "=Wanted\r\nnot_a_contract=Stays\r\n" +
                                 withP.substr(0, withP.size() - 2) + "=Text\r\n";
        std::string out;
        const int marked = GameTexts::compose(base, owns({}), out);
        const std::string first = "\xEF\xBB\xBF" + upper + "=Wanted <EM4>[BP";
        allPassed &= check(!title.empty() && !withP.empty() && marked == 2 && out.compare(0, first.size(), first) == 0 &&
                               out.find("\r\nnot_a_contract=Stays\r\n") != std::string::npos &&
                               out.find("=Text\\n\\n<EM4>BLUEPRINTS</EM4>") != std::string::npos &&
                               out.find("\r\nbedrock_game_texts=Written by Bedrock") != std::string::npos,
                           "the game's file comes back line for line, marks added, and Bedrock's line last (" +
                               std::to_string(marked) + " marked)");
        allPassed &= check(GameTexts::isOurs(out) && GameTexts::isOurs("a=b\\n<EM4>BLUEPRINTS</EM4> \xC2\xB7 one of 3") &&
                               !GameTexts::isOurs("a=b <EM4>Potential Blueprints</EM4>"),
                           "Bedrock's file is known as Bedrock's, a pack's is not");

        std::string cfg, language;
        bool changed = false;
        allPassed &= check(GameTexts::wantEnglish(cfg, language, changed) && changed && cfg == "g_language = english\r\n",
                           "an empty user.cfg is told g_language = english");
        cfg = "r_displayinfo = 0\r\ng_languageAudio = english";
        allPassed &= check(GameTexts::wantEnglish(cfg, language, changed) && changed &&
                               cfg == "r_displayinfo = 0\r\ng_languageAudio = english\r\ng_language = english\r\n",
                           "g_languageAudio is not g_language");
        cfg = "g_language = english\r\n";
        allPassed &= check(GameTexts::wantEnglish(cfg, language, changed) && !changed,
                           "a user.cfg that asks for english is left as it is");
        cfg = "g_language=german_(germany)\n";
        const bool english = GameTexts::wantEnglish(cfg, language, changed);
        allPassed &= check(!english && !changed && language == "german_(germany)",
                           "a game set to another language is not changed (got: " + language + ")");

        // A ZIP64 archive the way Data.p4k is made: one entry stored, one a
        // zstd frame whose sizes and offset stand in the ZIP64 extra field
        const std::string plain = "hello from the archive";
        // one contract Bedrock knows, for the whole write further down
        std::string ini = "\xEF\xBB\xBFkey=value\r\n" + title + "=A contract\r\n";
        for (int i = 0; i < 500; i++) ini += "k" + std::to_string(i) + "=a line of text to squeeze\r\n";
        std::string packed(ZSTD_compressBound(ini.size()), '\0');
        packed.resize(ZSTD_compress(packed.data(), packed.size(), ini.data(), ini.size(), 3));
        std::string zip, dir;
        auto put16 = [](std::string& s, uint16_t v) { s += (char)(v & 0xFF); s += (char)(v >> 8); };
        auto put32 = [&](std::string& s, uint32_t v) { put16(s, (uint16_t)v); put16(s, (uint16_t)(v >> 16)); };
        auto put64 = [&](std::string& s, uint64_t v) { put32(s, (uint32_t)v); put32(s, (uint32_t)(v >> 32)); };
        auto entry = [&](const std::string& name, uint16_t method, const std::string& data, size_t size, bool zip64) {
            const uint64_t at = zip.size();
            put32(zip, 0x04034b50); put16(zip, 45); put16(zip, 0); put16(zip, method);
            put32(zip, 0); put32(zip, 0); put32(zip, (uint32_t)data.size()); put32(zip, (uint32_t)size);
            put16(zip, (uint16_t)name.size()); put16(zip, 0);
            zip += name + data;
            std::string extra;
            if (zip64) { put16(extra, 1); put16(extra, 24); put64(extra, size); put64(extra, data.size()); put64(extra, at); }
            put32(dir, 0x02014b50); put16(dir, 45); put16(dir, 45); put16(dir, 0); put16(dir, method);
            put32(dir, 0); put32(dir, 0);
            put32(dir, zip64 ? 0xFFFFFFFFu : (uint32_t)data.size()); put32(dir, zip64 ? 0xFFFFFFFFu : (uint32_t)size);
            put16(dir, (uint16_t)name.size()); put16(dir, (uint16_t)extra.size()); put16(dir, 0); put16(dir, 0);
            put16(dir, 0); put32(dir, 0); put32(dir, zip64 ? 0xFFFFFFFFu : (uint32_t)at);
            dir += name + extra;
        };
        entry("Data\\Test\\plain.txt", 0, plain, plain.size(), false);
        entry("Data\\Localization\\english\\global.ini", 100, packed, ini.size(), true);
        const uint64_t dirAt = zip.size();
        zip += dir;
        const uint64_t end64At = zip.size();
        put32(zip, 0x06064b50); put64(zip, 44); put16(zip, 45); put16(zip, 45); put32(zip, 0); put32(zip, 0);
        put64(zip, 2); put64(zip, 2); put64(zip, dir.size()); put64(zip, dirAt);
        put32(zip, 0x07064b50); put32(zip, 0); put64(zip, end64At); put32(zip, 1);
        put32(zip, 0x06054b50); put16(zip, 0xFFFF); put16(zip, 0xFFFF); put16(zip, 0xFFFF); put16(zip, 0xFFFF);
        put32(zip, 0xFFFFFFFFu); put32(zip, 0xFFFFFFFFu); put16(zip, 0);
        const std::filesystem::path archive = std::filesystem::temp_directory_path() / "bedrock_selftest.p4k";
        {
            std::ofstream f(archive, std::ios::binary);
            f.write(zip.data(), (std::streamsize)zip.size());
        }
        std::string got, error;
        const bool readIni = SCMiner::P4kReader::read(archive, "data/localization/ENGLISH/global.ini", got, error);
        allPassed &= check(readIni && got == ini,
                           "a zstd entry comes out of a ZIP64 archive whole, found by any spelling of its name" +
                               (error.empty() ? std::string() : " (" + error + ")"));
        std::string stored;
        allPassed &= check(SCMiner::P4kReader::read(archive, "Data\\Test\\plain.txt", stored, error) && stored == plain,
                           "a stored entry comes out as it is");
        allPassed &= check(!SCMiner::P4kReader::read(archive, "Data\\missing.ini", stored, error),
                           "a name the archive does not have is said to be missing");
        std::error_code ec;
        std::filesystem::remove(archive, ec);

        // The whole write, in a game folder spelt the way Windows lets it be
        // and Linux tells apart: Bedrock's
        // file goes into the Data\localization there is, not into a second
        // data\Localization; USER.cfg is told english, no user.cfg is put
        // beside it; a pack in another spelling keeps Bedrock out; remove()
        // finds Bedrock's own in any spelling. Under Windows each spelling is
        // one file anyway, and the same checks hold.
        const std::filesystem::path channel = std::filesystem::temp_directory_path() / "bedrock_selftest_LIVE";
        std::filesystem::remove_all(channel, ec);
        std::filesystem::create_directories(channel / "Data" / "localization", ec);
        {
            std::ofstream f(channel / "Data.p4k", std::ios::binary);
            f.write(zip.data(), (std::streamsize)zip.size());
        }
        {
            std::ofstream f(channel / "USER.cfg", std::ios::binary);
            f << "r_displayinfo = 0\r\n";
        }
        auto slurp = [](const std::filesystem::path& p) {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream s;
            s << in.rdbuf();
            return s.str();
        };
        auto cfgFiles = [&channel]() {
            int n = 0;
            std::error_code e;
            for (std::filesystem::directory_iterator it(channel, e), end; !e && it != end; it.increment(e)) {
                std::string name = it->path().filename().string();
                for (auto& c : name) c = (char)std::tolower((unsigned char)c);
                if (name == "user.cfg") n++;
            }
            return n;
        };
        const std::vector<bool> none(SCMiner::CRAFT_BLUEPRINT_COUNT);
        const std::vector<std::string> steps = {"data", "Localization", "english", "global.ini"};
        const std::filesystem::path packPlace = channel / "Data" / "localization" / "english" / "global.ini";
        const GameTexts::Outcome wrote = GameTexts::write(channel, none);
        std::vector<std::filesystem::path> found = GameTexts::spellings(channel, steps);
        const std::string written = found.size() == 1 ? slurp(found[0]) : std::string();
        allPassed &= check(wrote.written && found.size() == 1 && found[0] == packPlace && GameTexts::isOurs(written) &&
                               written.find("=A contract <EM4>[BP") != std::string::npos &&
                               slurp(channel / "USER.cfg").find("g_language = english") != std::string::npos &&
                               cfgFiles() == 1,
                           "the file goes into the Data\\localization there is, USER.cfg is told english in its "
                           "own spelling (" + wrote.note + ")");

        const std::string pack = "\xEF\xBB\xBFsome_pack_key=Ein Paket\r\n";
        {
            std::ofstream f(packPlace, std::ios::binary | std::ios::trunc);
            f << pack;
        }
        const GameTexts::Outcome leftAlone = GameTexts::write(channel, none);
        const GameTexts::Outcome keptPack = GameTexts::remove(channel);
        allPassed &= check(!leftAlone.written && leftAlone.note.find("someone else's") != std::string::npos &&
                               keptPack.note.find("someone else's") != std::string::npos && slurp(packPlace) == pack &&
                               GameTexts::spellings(channel, steps).size() == 1,
                           "a pack in another spelling is seen, and left as it is");
#ifndef _WIN32
        // Only Linux can hold both: Bedrock's beside the pack, in the other
        // spelling. The game would read one of them - Bedrock's goes.
        std::filesystem::create_directories(channel / "data" / "Localization" / "english", ec);
        {
            std::ofstream f(channel / "data" / "Localization" / "english" / "global.ini", std::ios::binary);
            f << written;
        }
        const bool both = GameTexts::spellings(channel, steps).size() == 2;
        GameTexts::write(channel, none);
        found = GameTexts::spellings(channel, steps);
        allPassed &= check(both && found.size() == 1 && found[0] == packPlace && slurp(packPlace) == pack,
                           "Bedrock's file beside a pack in another spelling is taken away, the pack kept");
        std::filesystem::remove_all(channel / "data", ec);
#endif

        {
            std::ofstream f(packPlace, std::ios::binary | std::ios::trunc);
            f << written;
        }
        const GameTexts::Outcome taken = GameTexts::remove(channel);
        allPassed &= check(GameTexts::spellings(channel, steps).empty() && taken.note.rfind("taken away", 0) == 0,
                           "Bedrock's file is taken away in the spelling it has (" + taken.note + ")");

        {
            std::ofstream f(channel / "USER.cfg", std::ios::binary | std::ios::trunc);
            f << "g_language = german_(germany)\r\n";
        }
        const GameTexts::Outcome german = GameTexts::write(channel, none);
        allPassed &= check(!german.written && german.note.find("german_(germany)") != std::string::npos &&
                               GameTexts::spellings(channel, steps).empty() && cfgFiles() == 1,
                           "a USER.cfg set to another language is read in its spelling, and nothing is written");
        std::filesystem::remove_all(channel, ec);
        // and the game's text the first write kept for this channel
        std::filesystem::remove(SCMiner::AppPaths::data() / "game_text_bedrock_selftest_LIVE.ini", ec);
        std::filesystem::remove(SCMiner::AppPaths::data() / "game_text_bedrock_selftest_LIVE.txt", ec);

        // A patch after Bedrock's file was written: the channel's Data.p4k
        // newer than the file, and still for a while since - the file stands
        // on the old build and says so. Not while the archive is still being
        // written, not when the file is the newer, not for anyone else's. The
        // channels beside one are the folders with a Data.p4k of their own.
        {
            const std::filesystem::path root = std::filesystem::temp_directory_path() / "bedrock_selftest_channels";
            std::filesystem::remove_all(root, ec);
            const std::filesystem::path live = root / "LIVE", ptu = root / "PTU";
            for (const auto& d : {live, ptu}) {
                std::filesystem::create_directories(d / "data" / "Localization" / "english", ec);
                std::ofstream(d / "Data.p4k", std::ios::binary) << "p4k";
            }
            std::filesystem::create_directories(root / "Screenshots", ec);
            const std::filesystem::path ptuFile = ptu / "data" / "Localization" / "english" / "global.ini";
            std::ofstream(ptuFile, std::ios::binary) << "a=b\r\n" << GameTexts::MARKER_KEY << "=Written by Bedrock\r\n";
            const auto now = std::filesystem::file_time_type::clock::now();
            const auto minutes2 = std::chrono::minutes(2);
            std::filesystem::last_write_time(ptuFile, now - std::chrono::hours(48), ec);
            std::filesystem::last_write_time(ptu / "Data.p4k", now - std::chrono::hours(3), ec);
            const bool patched = GameTexts::outdatedSince(ptu, minutes2) != 0;
            std::filesystem::last_write_time(ptu / "Data.p4k", now - std::chrono::seconds(20), ec);
            const bool patching = GameTexts::outdatedSince(ptu, minutes2) != 0;
            std::filesystem::last_write_time(ptu / "Data.p4k", now - std::chrono::hours(72), ec);
            const bool older = GameTexts::outdatedSince(ptu, minutes2) != 0;
            std::filesystem::last_write_time(ptu / "Data.p4k", now - std::chrono::hours(3), ec);
            std::ofstream(ptuFile, std::ios::binary | std::ios::trunc) << "a=b\r\n";
            std::filesystem::last_write_time(ptuFile, now - std::chrono::hours(48), ec);
            const bool foreign = GameTexts::outdatedSince(ptu, minutes2) != 0;
            const std::vector<std::filesystem::path> beside = GameTexts::channelsBeside(live);
            allPassed &= check(patched && !patching && !older && !foreign && beside.size() == 2 && beside[0] == live &&
                                   beside[1] == ptu,
                               "Bedrock's file under a game patched since is found out - not while the patch runs, "
                               "not for anyone else's file; the channels beside LIVE are those with a Data.p4k");
            std::filesystem::remove_all(root, ec);
        }
    }

    // --- Notebook: the names it will let onto the disk ---
    {
        using SCMiner::NoteBook;
        allPassed &= check(NoteBook::sanitise("a/b*c?d\"e<f>g|h") == "abcdefgh",
                           "a note name drops what Windows forbids in a file name");
        allPassed &= check(NoteBook::sanitise("   ") == "Untitled",
                           "a note named nothing still gets a file");
        allPassed &= check(NoteBook::sanitise("CON") == "CON note",
                           "the names Windows keeps for itself are stepped around");
        allPassed &= check(NoteBook::sanitise(" Aaron Halo . ") == "Aaron Halo",
                           "a name does not start or end on a space or a dot");
    }

    // --- The second reader: its model travels inside the exe, loads, and
    //     reads; and what it will take from a line of text ---
    {
        using SCMiner::TextLineReader;
        TextLineReader lines;
        const bool loaded = lines.loadEmbedded();
        allPassed &= check(loaded, "the second reader's model is in the exe and builds");
        if (loaded) {
            // White figures on a dark plate, the separator where the HUD sets it
            cv::Mat plate(40, 150, CV_8UC3, cv::Scalar(60, 40, 40));
            cv::putText(plate, "16,780", cv::Point(8, 30), cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(240, 240, 240),
                        2, cv::LINE_AA);
            const auto line = lines.read(plate);
            const auto number = TextLineReader::signatureIn(line.text);
            allPassed &= check(number && number->value == 16780, "it reads a drawn 16,780 (got: " + line.text + ")");
        }
        const auto pinned = TextLineReader::signatureIn("2. 14.000");
        allPassed &= check(pinned && pinned->value == 14000 && pinned->digits == 5,
                           "a pin read as \"2.\" in front of the number is not part of it");
        allPassed &= check(!TextLineReader::signatureIn("14000") && !TextLineReader::signatureIn("535.2m"),
                           "a number without its thousands separator does not count");
        const auto wide = TextLineReader::signatureIn("86\xEF\xBC\x8C" "000");
        allPassed &= check(wide && wide->value == 86000, "the full-width comma counts as a comma");

        // The rules the two readers settle a disagreement by, on a made-up
        // reading. They come from moving footage: rules drawn from still
        // pictures are wrong in the one place that matters.
        if (loaded) {
            auto disagreement = [&](int glyphValue, int glyphLead, double lineMean, int lineValue) {
                SCMiner::GlyphReader::Reading reading;
                reading.value = glyphValue;
                reading.minMargin = glyphLead;
                reading.glyphCount = (int)std::to_string(lineValue).size();
                return TextLineReader::settle(reading, glyphValue, lineValue, lineMean);
            };
            using Verdict = TextLineReader::Verdict;
            allPassed &= check(disagreement(18000, 6, 0.97, 16000) == Verdict::Differs,
                               "a line read at 97 % overrules a glyph reading that did not hesitate");
            allPassed &= check(disagreement(20975, 6, 0.87, 20875) == Verdict::Doubts,
                               "...one read at 87 % does not - the glyph reader keeps its value");
            allPassed &= check(disagreement(18780, 0, 0.87, 16780) == Verdict::Differs,
                               "...but it does where the glyph reader hesitated over a digit");
            allPassed &= check(disagreement(7200, 6, 0.87, 72000) == Verdict::Differs,
                               "...and where the glyph reader lost one");
        }
    }

    // --- A backup of everything the player has made: the format, and the
    //     paths it is allowed to write ---
    {
        using SCMiner::DataBackup::Entry;
        namespace DB = SCMiner::DataBackup;
        // The two cases the format exists for: a file that talks about the
        // format, and one that does not end on a line break. Both have to
        // come back byte for byte, which is why the count is in bytes.
        const std::string tricky = "FILE\tfake.txt\t9\nno end";
        std::vector<Entry> out;
        out.push_back({"notes/Routes/Monox run.txt", tricky});
        out.push_back({"craft_list.txt", ""});
        const std::string text = DB::write(out);

        std::vector<Entry> back;
        std::string error;
        allPassed &= check(DB::read(text, back, error) && back.size() == 2,
                           "a backup reads back as many files as went in (" + error + ")");
        allPassed &= check(back.size() == 2 && back[0].bytes == tricky &&
                               back[0].path == out[0].path,
                           "a file whose text itself says FILE comes back byte for byte");
        allPassed &= check(back.size() == 2 && back[1].bytes.empty(),
                           "an empty file survives");

        allPassed &= check(!DB::read("something else entirely", back, error) &&
                               error.find("BEDROCK BACKUP") != std::string::npos,
                           "any other file is refused, and says why");
        const std::string cut = std::string(DB::MAGIC) + "\nFILE\tcut.txt\t40\nshort\n";
        allPassed &= check(!DB::read(cut, back, error) &&
                               error.find("cut.txt") != std::string::npos,
                           "a backup that stops in the middle of a file is refused by name");

        // What a backup may write to. It is a file somebody can be handed,
        // so it is treated as something that might be trying it on.
        allPassed &= check(DB::safePath("craft_list.txt") &&
                               DB::safePath("notes/Routes/Monox run.txt"),
                           "an ordinary path is allowed");
        const char* bad[] = {"../x", "notes/../../x", "/x", "C:/x", "C:", "",
                             "notes//x", "notes/", "notes/x.", "notes/x ", "a\\b"};
        std::string allowed;
        for (const char* one : bad) {
            if (DB::safePath(one)) allowed += std::string(" \"") + one + "\"";
        }
        allPassed &= check(allowed.empty(),
                           "a path that climbs out, names a drive or ends badly is refused (allowed:" +
                               (allowed.empty() ? std::string(" none") : allowed) + ")");

        // What a backup carries at all: the player's things, not a copy of
        // the game's texts or a picture a developer tool left lying about
        allPassed &= check(DB::carried("craft_list.txt") && DB::carried("notes/media/a.png") &&
                               !DB::carried("game_text_PTU.ini") && !DB::carried("scanpanel_live.PNG") &&
                               !DB::carried("debug/scan_log.csv") &&
                               !DB::carried("previous/2026-09-30 2015/craft_list.txt"),
                           "a backup carries the lists and a note's pictures, not a copy of the "
                           "game's texts, a loose picture or debug\\");
    }

    // --- ...and the round trip over a real folder, which is where the
    //     paths, the skipping and the putting-aside actually happen ---
    {
        namespace DB = SCMiner::DataBackup;
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "bedrock-backup-selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root / "notes" / "Routes", ec);
        fs::create_directories(root / "debug", ec);
        auto put = [&](const fs::path& p, const std::string& what) {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(what.data(), (std::streamsize)what.size());
        };
        put(root / "craft_list.txt", "two recipes\n");
        put(root / "notes" / "Routes" / "Monox run.txt", "Iron at Monox");
        put(root / "debug" / "scan_log.csv", "not worth carrying");
        fs::create_directories(root / "notes" / "media", ec);
        put(root / "notes" / "media" / "20260914_114200.jpg", "\xFF\xD8 a note's picture");
        put(root / "game_text_LIVE.ini", "the game's own texts");
        put(root / "game_text_LIVE.txt", "10491521 1234");
        put(root / "scanpanel_live.png", "a developer tool's picture");

        auto entries = DB::collect(root);
        bool carriedDebug = false, carriedNested = false, carriedPicture = false,
             carriedCopy = false, carriedLoose = false;
        for (const auto& e : entries) {
            if (e.path.find("debug/") == 0) carriedDebug = true;
            if (e.path == "notes/Routes/Monox run.txt") carriedNested = true;
            if (e.path == "notes/media/20260914_114200.jpg") carriedPicture = true;
            if (e.path.find("game_text_") == 0) carriedCopy = true;
            if (e.path == "scanpanel_live.png") carriedLoose = true;
        }
        allPassed &= check(entries.size() == 3 && carriedNested && carriedPicture && !carriedDebug,
                           "a backup carries the notes, their pictures and the lists, and leaves "
                           "debug\\ behind (" + std::to_string(entries.size()) + " files)");
        allPassed &= check(!carriedCopy && !carriedLoose,
                           "...and neither the copy of the game's texts nor the loose picture");

        // The player changes their mind about everything, then puts it back
        const std::string backup = DB::write(entries);
        put(root / "craft_list.txt", "nothing at all\n");
        fs::remove(root / "notes" / "Routes" / "Monox run.txt", ec);

        std::vector<DB::Entry> back;
        std::string error, aside;
        allPassed &= check(DB::read(backup, back, error), "the backup reads back (" + error + ")");
        // ...as an older backup would, with the copy and the picture in it
        back.push_back({"game_text_LIVE.ini", "a copy from another day"});
        back.push_back({"scanpanel_live.png", "an old picture"});
        allPassed &= check(DB::restore(root, back, aside, error) && !aside.empty(),
                           "a restore goes through (" + error + ")");
        auto slurp = [&](const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        };
        allPassed &= check(slurp(root / "craft_list.txt") == "two recipes\n",
                           "a file that was written over comes back as it was");
        allPassed &= check(slurp(root / "notes" / "Routes" / "Monox run.txt") == "Iron at Monox",
                           "a file that was deleted comes back, folder and all");
        allPassed &= check(fs::exists(root / DB::ASIDE / aside / "craft_list.txt", ec),
                           "what was there before the restore is put aside, not erased");
        allPassed &= check(fs::exists(root / "debug" / "scan_log.csv", ec),
                           "debug\\ is left where it is by a restore too");
        allPassed &= check(slurp(root / "game_text_LIVE.ini") == "the game's own texts" &&
                               slurp(root / "scanpanel_live.png") == "a developer tool's picture" &&
                               !fs::exists(root / DB::ASIDE / aside / "game_text_LIVE.ini", ec),
                           "what no backup carries is neither written back nor put aside");
        {
            std::vector<DB::Entry> copyOnly = {{"game_text_LIVE.ini", "only this"}};
            std::string none;
            allPassed &= check(!DB::restore(root, copyOnly, none, error) &&
                                   slurp(root / "craft_list.txt") == "two recipes\n",
                               "a backup with nothing to put back is refused before anything moves");
        }
        // ...and the folder it puts aside into is not itself carried next time
        auto again = DB::collect(root);
        bool carriedAside = false;
        for (const auto& e : again) {
            if (e.path.find(std::string(DB::ASIDE) + "/") == 0) carriedAside = true;
        }
        allPassed &= check(!carriedAside, "the put-aside folder is not swept into the next backup");
        fs::remove_all(root, ec);
    }

    // --- The blueprint list can be narrowed to what a contract hands out ---
    {
        int dropped = 0;
        for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
            if (SCMiner::CRAFT_BLUEPRINTS[b].srcCount) dropped++;
        }
        allPassed &= check(dropped > 0 && dropped < SCMiner::CRAFT_BLUEPRINT_COUNT,
                           "both halves of the contract filter hold something (" +
                               std::to_string(dropped) + " dropped, " +
                               std::to_string(SCMiner::CRAFT_BLUEPRINT_COUNT - dropped) + " not)");
        // Every named source has to be a row that exists, or the filter
        // would offer a recipe whose card then shows nothing
        bool inRange = true;
        for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
            const auto& bp = SCMiner::CRAFT_BLUEPRINTS[b];
            if (bp.srcCount && bp.srcFirst + bp.srcCount > SCMiner::CRAFT_SOURCE_COUNT) inRange = false;
        }
        allPassed &= check(inRange, "every blueprint's contracts point into the source table");
    }

    // --- The contract card: the wait after a run comes from respawnTime ---
    {
        int found = -1;
        for (int i = 0; i < SCMiner::CRAFT_SOURCE_COUNT; i++) {
            const auto& c = SCMiner::CRAFT_SOURCES[i];
            if (std::string(c.title) == "Orange Level Contract: Destroy Outlaw Threat" &&
                std::string(c.faction) == "Foxwell Enforcement" && std::string(c.standing) == "Contractor") {
                found = i;
                break;
            }
        }
        allPassed &= check(found >= 0 && SCMiner::CRAFT_SOURCES[found].respawn == 5 &&
                               SCMiner::CRAFT_SOURCES[found].respawnVar == 2 &&
                               SCMiner::CRAFT_SOURCES[found].cooldown == 0,
                           "Destroy Outlaw Threat is offered again 5 min (+/-2) after it ends, with no personal block");
        int withRespawn = 0;
        for (int i = 0; i < SCMiner::CRAFT_SOURCE_COUNT; i++) {
            if (SCMiner::CRAFT_SOURCES[i].respawn) withRespawn++;
        }
        allPassed &= check(withRespawn > SCMiner::CRAFT_SOURCE_COUNT / 2,
                           "most contracts carry a respawn time (" + std::to_string(withRespawn) + " of " +
                               std::to_string(SCMiner::CRAFT_SOURCE_COUNT) + ")");
    }

    // --- Every contract is named. A title can be nothing but a slot into
    //     the outfit's own strings - "[CONTRACTOR|BLACKBOXRECOVERTITLEEASY]"
    //     - which the generator fills from the organization record ---
    {
        int bare = 0, bitZeros = 0;
        for (int i = 0; i < SCMiner::CRAFT_SOURCE_COUNT; i++) {
            const std::string t = SCMiner::CRAFT_SOURCES[i].title;
            if (t.size() > 2 && t.front() == '[' && t.back() == ']' && t.find('|') != std::string::npos &&
                t.find(']') == t.size() - 1) {
                bare++;
            }
            if (t == "Getting To the Bottom of It" && std::string(SCMiner::CRAFT_SOURCES[i].faction) == "Bit Zeros") {
                bitZeros++;
            }
        }
        allPassed &= check(bare == 0, "no contract is named only by an outfit slot (" + std::to_string(bare) + " are)");
        allPassed &= check(bitZeros > 0, "Bit Zeros' Easy black box run is named Getting To the Bottom of It");
        // The first rung of a ladder is marked: the game offers it to a
        // newcomer and its standing's other rung to everyone else, so the
        // planner puts the other rung in front
        bool introFlag = false, otherFlag = true;
        for (int i = 0; i < SCMiner::CRAFT_SOURCE_COUNT; i++) {
            const std::string t = SCMiner::CRAFT_SOURCES[i].title;
            if (t == "Data Transfer") introFlag = introFlag || SCMiner::CRAFT_SOURCES[i].intro;
            if (t == "Pick Up, Put Down") otherFlag = otherFlag && !SCMiner::CRAFT_SOURCES[i].intro;
        }
        allPassed &= check(introFlag && otherFlag,
                           "Data Transfer is Bit Zeros' intro rung and Pick Up, Put Down is not");
        // A contract with more than one pool says so: Foxwell's Orange
        // Protect-and-Escort run in Nyx pays out of coolers AND repeaters
        bool coolersWithRepeaters = false;
        for (int i = 0; i < SCMiner::CRAFT_SOURCE_COUNT && !coolersWithRepeaters; i++) {
            const SCMiner::CraftSource& c = SCMiner::CRAFT_SOURCES[i];
            if (std::string(c.faction) != "Foxwell Enforcement" ||
                std::string(c.title).rfind("Orange Lvl. Contract: Protect", 0) != 0 ||
                std::string(c.system).find("Nyx") == std::string::npos) {
                continue;
            }
            bool ownCooler = false;
            for (int k = 0; k < c.poolCount; k++) {
                ownCooler = ownCooler || std::string(SCMiner::CRAFT_POOL_ITEMS[c.poolFirst + k]) == "Frost-Star SL";
            }
            if (!ownCooler) continue;
            for (int k = 0; k < c.otherCount; k++) {
                const SCMiner::CraftPoolRun& run = SCMiner::CRAFT_OTHER_POOLS[c.otherFirst + k];
                for (int j = 0; j < run.count; j++) {
                    if (std::string(SCMiner::CRAFT_POOL_ITEMS[run.first + j]) == "CF-117 Bulldog Repeater") {
                        coolersWithRepeaters = true;
                    }
                }
            }
        }
        allPassed &= check(coolersWithRepeaters,
                           "Foxwell's Orange escort run in Nyx pays out of its coolers and its repeaters both");
    }

    // --- Notes out to a file and back in: the format has to survive a note
    //     that talks about the format ---
    {
        using SCMiner::NoteBook;
        std::vector<NoteBook::BundleNote> out;
        out.push_back({"Routes", "Monox run", "Iron at [[Monox]]\r\n\r\nNOTE\tfake\tline\t9\r\nlast"});
        out.push_back({"", "Loose", ""});
        const std::string text = NoteBook::writeBundle(out);

        std::vector<NoteBook::BundleNote> back;
        std::string error;
        allPassed &= check(NoteBook::readBundle(text, back, error) && back.size() == 2,
                           "a bundle reads back as many notes as went in (" + error + ")");
        allPassed &= check(back.size() == 2 && back[0].body == out[0].body,
                           "a note whose text itself says NOTE comes back whole");
        allPassed &= check(back.size() == 2 && back[0].folder == "Routes" &&
                               back[1].name == "Loose" && back[1].body.empty(),
                           "folder, name and an empty note all survive");

        allPassed &= check(!NoteBook::readBundle("just some text\r\nand more", back, error) &&
                               error.find("Bedrock") != std::string::npos,
                           "any other text file is refused, and says why");
        allPassed &= check(!NoteBook::readBundle(std::string(NoteBook::BUNDLE_MAGIC) +
                                                     "\r\nNOTE\tRoutes\tCut short\t4\r\none\r\ntwo\r\n",
                                                 back, error) &&
                               error.find("Cut short") != std::string::npos,
                           "a file that stops in the middle of a note is refused by name");
        allPassed &= check(!NoteBook::readBundle(std::string(NoteBook::BUNDLE_MAGIC) +
                                                     "\r\nNOTE\tRoutes\tNo count\r\none\r\n",
                                                 back, error),
                           "a heading without a line count is refused");
    }

    // --- What a row is made of, which is what the red blink asks. A find
    //     with two deposits in it has to answer for both of them ---
    {
        using SCMiner::MinerGUI;
        auto looksFor = [](const std::string& row, const std::string& picked) {
            return MinerGUI::anyPart(row, [&](const std::string& part) { return part == picked; });
        };
        allPassed &= check(looksFor("Hephaestanite + FPS Minable \xC3\x97" "6", "Hephaestanite"),
                           "the first half of a mix answers for the whole row");
        allPassed &= check(looksFor("Hephaestanite + FPS Minable \xC3\x97" "6", "FPS Minable"),
                           "...and so does the second, with its count taken off");
        allPassed &= check(!looksFor("Hephaestanite + FPS Minable \xC3\x97" "6", "Iron"),
                           "an ore that is not in the row does not answer for it");
        allPassed &= check(looksFor("Iron", "Iron") && !looksFor("Iron", "Ir"),
                           "a plain row still matches whole, and only whole");
        // on a planet the ground family is shown under both its names, but
        // the pick in the window holds the one the tables use
        allPassed &= check(looksFor("Harvestable/Salvage \xC3\x97" "8 + Iron \xC3\x97" "5", "Salvage"),
                           "a ground find picked as Salvage matches what a planet calls it");
    }

    // --- ...and the links inside, which is what an import stands or falls on ---
    {
        using SCMiner::MinerGUI;
        allPassed &= check(MinerGUI::brokenLink(L"Iron at [[Daymar]] for the [[Laranite]]").empty(),
                           "a note whose links all name something known passes");
        allPassed &= check(MinerGUI::brokenLink(L"plain text with no brackets at all").empty(),
                           "a note with no links passes");
        const std::wstring bad = MinerGUI::brokenLink(L"one\r\ntwo\r\nhaul [[Quantanium]] home");
        allPassed &= check(!bad.empty() && bad.find(L"line 3") != std::wstring::npos,
                           "a misspelt ore is caught, with the line it is on");
        allPassed &= check(!MinerGUI::brokenLink(L"see [[Daymar\r\nnext line").empty(),
                           "a [[ that never closes is caught");
    }

    // --- Notes: what the app offers while a name is being typed, and what
    //     it makes of the link once that offer is taken ---
    {
        using SCMiner::MinerGUI;
        // the caret sits at the end of what has been typed
        auto offered = [](const std::wstring& typed) {
            std::string out;
            for (const auto& link : MinerGUI::suggestionsAt(typed, (int)typed.size())) {
                out += (char)('0' + link.kind);
            }
            return out;
        };
        auto first = [](const std::wstring& typed) {
            const auto list = MinerGUI::suggestionsAt(typed, (int)typed.size());
            return list.empty() ? std::string("-") : list.front().name;
        };
        allPassed &= check(first(L"mining Laranite") == "Laranite",
                           "a finished ore name is offered (got: " + first(L"mining Laranite") + ")");
        allPassed &= check(first(L"heading to daym") == "Daymar",
                           "a half-typed place is offered, like a phone would (got: " +
                               first(L"heading to daym") + ")");
        allPassed &= check(first(L"build the Palatino Arm") == "Palatino Arms",
                           "a two-word recipe is offered across the space (got: " +
                               first(L"build the Palatino Arm") + ")");
        allPassed &= check(offered(L"the better stop").empty() == false,
                           "STOP is only ever offered, never applied on its own");

        // A window may never demand more room than the screen has. At 175 %
        // zoom the full window's smallest allowed height is 640 x 1.75 =
        // 1120 px; a 1080p screen cannot show that, so Windows does not shrink
        // it and the bottom border sits under the taskbar, out of reach.
        allPassed &= check(MinerGUI::fitToWork(1120, 1032) == 1032,
                           "a window taller than the screen is cut down to it");
        allPassed &= check(MinerGUI::fitToWork(800, 1032) == 800,
                           "...one that fits is left as it is");
        allPassed &= check(MinerGUI::fitToWork(1120, 0) == 1120,
                           "...and an unknown screen changes nothing");

        // --- the updater's two pieces of arithmetic ---
        // 4.10 is newer than 4.9. Compared as text it is not, and an app
        // that gets this wrong either never updates or updates in circles.
        // The script an update hands over to calls Windows' own tools by
        // their full path, never whatever the PATH finds first
        {
            const std::string script = SCMiner::Updater::applyScript(
                "C:\\Bedrock", "C:\\Data\\update\\Bedrock_2.5.1.zip", "C:\\Data\\update\\staging", 4242);
            bool full = script.find("set \"SYS=%SystemRoot%\\System32\"") != std::string::npos &&
                        script.find("\"%SYS%\\tasklist.exe\" /fi \"PID eq 4242\" | \"%SYS%\\find.exe\" \"4242\"") !=
                            std::string::npos &&
                        script.find("\"%SYS%\\ping.exe\"") != std::string::npos &&
                        script.find("\"%SYS%\\WindowsPowerShell\\v1.0\\powershell.exe\"") != std::string::npos &&
                        script.find("\"%SYS%\\xcopy.exe\"") != std::string::npos;
            std::string bare;
            for (const char* tool : {"tasklist", "find", "ping", "powershell", "xcopy"}) {
                for (const std::string& before : {std::string("\r\n"), std::string("| ")}) {
                    if (script.find(before + tool + " ") != std::string::npos) bare += std::string(" ") + tool;
                }
            }
            allPassed &= check(full && bare.empty(),
                               "the update script calls tasklist, find, ping, powershell and xcopy by their "
                               "full path (bare:" + (bare.empty() ? std::string(" none") : bare) + ")");
        }
        allPassed &= check(SCMiner::Updater::isNewer("4.10.0", "4.9.0"),
                           "4.10.0 counts as newer than 4.9.0");
        allPassed &= check(!SCMiner::Updater::isNewer("4.9.0", "4.10.0"),
                           "...and not the other way round");
        allPassed &= check(!SCMiner::Updater::isNewer("4.8.0", "4.8.0"),
                           "the same version is not an update");
        allPassed &= check(SCMiner::Updater::isNewer("4.8.1", "4.8.0"),
                           "...a patch release is");
        {
            SCMiner::Updater::Release r;
            const std::string hash(64, 'a');
            // One file for both platforms: the Windows build reads url= and
            // sha256=, the Linux build url_linux= and sha256_linux=, and each
            // passes over the other's lines
            const std::string good = "# Bedrock\nversion=4.9.0\n"
                                     "url=https://example.org/b.zip\nsha256=" + hash +
                                     "\nurl_linux=https://example.org/b.AppImage\nsha256_linux=" + hash +
                                     "\nnotes=something\n";
            allPassed &= check(SCMiner::Updater::parseManifest(good, r) &&
                                   r.version == "4.9.0" && r.notes == "something",
                               "a well-formed version file is read");
            const std::string plain = "version=4.9.0\nurl=http://example.org/b.zip\n"
                                      "url_linux=http://example.org/b.AppImage\n"
                                      "sha256=" + hash + "\nsha256_linux=" + hash;
            allPassed &= check(!SCMiner::Updater::parseManifest(plain, r),
                               "...one pointing at plain http is refused");
            allPassed &= check(
                !SCMiner::Updater::parseManifest("version=4.9.0\nurl=https://example.org/b.zip\n"
                                                 "url_linux=https://example.org/b.AppImage", r),
                "...and one with no checksum is refused");
        }

        // The group headings are filters of their own - "only armour" rather
        // than "only helmets", which is what the type list already offered
        int helmet = -1, shield = -1;
        for (int t = 0; t < SCMiner::CRAFT_TYPE_COUNT; t++) {
            if (std::string(SCMiner::CRAFT_TYPES[t]) == "Helmet (Armor)") helmet = t;
            if (std::string(SCMiner::CRAFT_TYPES[t]) == "Shield") shield = t;
        }
        allPassed &= check(helmet >= 0 && MinerGUI::typeInGroup(helmet, 1),
                           "a helmet falls under armour and clothing");
        allPassed &= check(shield >= 0 && MinerGUI::typeInGroup(shield, 0) &&
                               !MinerGUI::typeInGroup(shield, 1),
                           "...a shield under ship, and under nothing else");
        // A note as the edit control holds it: one line break is ONE
        // character, and the earlier link is still in the text
        {
            const std::wstring note = L"asdasd\r\r[[Iron]] Palatino Helmet Metropolis";
            const auto list = MinerGUI::suggestionsAt(note, (int)note.size());
            std::wstring after = note;
            if (!list.empty()) {
                after.replace(list[0].at, list[0].len,
                              L"[[" + std::wstring(list[0].name.begin(),
                                                   list[0].name.end()) + L"]]");
            }
            allPassed &= check(!list.empty() && list[0].name == "Palatino Helmet Metropolis",
                               "a name typed after an existing link is still recognised");
            allPassed &= check(after == L"asdasd\r\r[[Iron]] [[Palatino Helmet Metropolis]]",
                               "...and taking it up closes the brackets it opened");
        }
        // Nothing is CALLED Daystar; four recipes end in it.
        allPassed &= check(first(L"crafting Daystar").find("Daystar") != std::string::npos,
                           "a word from the middle or end of a name finds it (got: " +
                               first(L"crafting Daystar") + ")");
        // A system is a column beside the places, and is offered as well
        allPassed &= check(first(L"heading for Nyx") == "Nyx",
                           "a system is offered, not only a place (got: " +
                               first(L"heading for Nyx") + ")");
        allPassed &= check(!MinerGUI::suggestionsAt(L"heading for Nyx", 15).empty() &&
                               MinerGUI::suggestionsAt(L"heading for Nyx", 15).front().kind == 3,
                           "...and it is offered as a system");
        // Not cut at five: that would put 18 of the 23 out of reach
        allPassed &= check(offered(L"build a Palatino").size() >= 20,
                           "every Palatino recipe is reachable, not the first five (got: " +
                               std::to_string(offered(L"build a Palatino").size()) + ")");
        allPassed &= check(offered(L"a ").empty(), "one letter offers nothing");
        allPassed &= check(MinerGUI::suggestionsAt(L"[[Laranite]]", 12).empty(),
                           "nothing is offered inside a link that already exists");

        // and what a confirmed link reads as
        const auto links = MinerGUI::bracketLinksIn(L"[[Laranite]] at [[Daymar]], not stop");
        allPassed &= check(links.size() == 2, "two confirmed links are found");
        allPassed &= check(links.size() == 2 && links[0].kind == 0 && links[1].kind == 2,
                           "...as an ore and a place");
        allPassed &= check(MinerGUI::bracketLinksIn(L"Laranite at Daymar").empty(),
                           "a name nobody confirmed is not a link");
        allPassed &= check(MinerGUI::bracketLinksIn(L"[[Nothing Here]]").empty(),
                           "brackets around a name the game does not have are not a link");
    }

    // --- Notes: line breaks on the way from the edit control to the file ---
    {
        using SCMiner::MinerGUI;
        const std::wstring cr = L"\r";
        const std::wstring lf = L"\n";
        const std::wstring crlf = L"\r\n";
        allPassed &= check(MinerGUI::toFileText(L"one" + cr + L"two") == L"one" + crlf + L"two",
                           "a lone CR from the edit control becomes CR+LF in the file");
        allPassed &= check(MinerGUI::toFileText(L"one" + lf + L"two") == L"one" + crlf + L"two",
                           "a lone LF from a file written elsewhere becomes CR+LF too");
        allPassed &= check(MinerGUI::toFileText(L"one" + crlf + L"two") == L"one" + crlf + L"two",
                           "CR+LF stays CR+LF and is not doubled");
        allPassed &= check(MinerGUI::toFileText(L"a" + cr + cr + L"b") == L"a" + crlf + crlf + L"b",
                           "an empty line between two lines is kept");
        allPassed &= check(MinerGUI::toFileText(L"") == L"", "nothing in, nothing out");
    }

    // --- Analyzer ---
    SCMiner::SignatureAnalyzer analyzer;

    auto top = analyzer.getMostLikely(6370);
    allPassed &= check(top.name == "Stileron" && top.clusterSize == 2 && top.difference < 1.0,
                       "6370 resolves to Stileron x2 (got: " + top.name +
                       " x" + std::to_string(top.clusterSize) + ")");

    top = analyzer.getMostLikely(3170);
    allPassed &= check(top.name == "Quantainium" && top.clusterSize == 1,
                       "3170 resolves to Quantainium x1 (got: " + top.name + ")");

    top = analyzer.getMostLikely(8600);
    allPassed &= check(top.name == "Ice" && top.clusterSize == 2,
                       "8600 resolves to Ice x2 (got: " + top.name +
                       " x" + std::to_string(top.clusterSize) + ")");

    std::string exactName;
    int exactCluster = 0;
    allPassed &= check(analyzer.isExactMatch(7080, exactName, exactCluster) &&
                       exactName == "Beryl" && exactCluster == 2,
                       "7080 is an exact match for Beryl x2");

    // Exact-first: in-game values are always base x cluster, so an exact hit
    // must suppress all fuzzy neighbors
    auto exactResults = analyzer.analyzeSignature(6370);
    bool onlyExact = !exactResults.empty();
    for (const auto& r : exactResults) {
        if (r.difference >= 1.0) onlyExact = false;
    }
    allPassed &= check(onlyExact && exactResults.size() == 1,
                       "Exact scan 6370 keeps only the exact match (" +
                       std::to_string(exactResults.size()) + " result)");

    // Pictures from other monitors are brought to the reader's reference
    // height and the answers mapped back - outwards, never inwards
    {
        const cv::Rect up = SCMiner::GlyphReader::fromReferenceScale(cv::Rect(10, 20, 30, 40), 1.5);
        allPassed &= check(up == cv::Rect(15, 30, 45, 60),
                           "a reference rect maps back at 1.5 (got " + std::to_string(up.x) + "," +
                           std::to_string(up.y) + " " + std::to_string(up.width) + "x" +
                           std::to_string(up.height) + ")");
        const cv::Rect down = SCMiner::GlyphReader::fromReferenceScale(cv::Rect(10, 20, 30, 40), 0.75);
        allPassed &= check(down == cv::Rect(7, 15, 23, 30),
                           "a reference rect maps back at 0.75 (got " + std::to_string(down.x) + "," +
                           std::to_string(down.y) + " " + std::to_string(down.width) + "x" +
                           std::to_string(down.height) + ")");
        SCMiner::GlyphReader::setHudScale(1.5);
        allPassed &= check(SCMiner::GlyphReader::hudScale() == 1.5, "the HUD scale can be set to 1.5 (4K)");
        SCMiner::GlyphReader::setHudScale(0.0);
        allPassed &= check(SCMiner::GlyphReader::hudScale() == 1.0, "an impossible HUD scale falls back to 1");
    }

    // Where the player is and what they fly, off Game.log: the four line
    // shapes the 4.10 log has for it, and one that says nothing
    {
        std::string sys, ship;
        bool leaving = false;
        const bool a = SCMiner::GameState::parseLine(
            "<2026-09-10T16:15:24.983Z> [Notice] <Calculate Route> [ItemNavigation][CL][22520] | NOT AUTH | "
            "AEGS_Idris_P_815000000303[815000000303]|CSCItemNavigation::CalculateRoute|Projected Start Location "
            "is Nyx for route to destination levski_all-001 [Team_CGP4][QuantumTravel]", &sys, &ship, &leaving);
        allPassed &= check(a && sys == "Nyx" && ship.empty(),
                           "a route line names the system, not the ship (got " + sys + ", " + ship + ")");
        sys.clear(); ship.clear();
        const bool p = SCMiner::GameState::parseLine(
            "<2026-09-10T14:34:38.704Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][22520] "
            "| NOT AUTH | ANVL_Asgard_822000000505[822000000505]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|"
            "Player has selected point LOC_RR_S3_L3 as their destination, routing locally [Team_CGP4][QuantumTravel]",
            &sys, &ship, &leaving);
        allPassed &= check(p && ship == "Anvil Asgard" && !leaving && sys.empty(),
                           "a quantum target the player picked names the player's ship (got " + ship + ")");
        sys.clear(); ship.clear();
        const bool q = SCMiner::GameState::parseLine(
            "<2026-09-10T15:05:14.000Z> [Notice] <Local Route Guard - Server Rerouted> [ItemNavigation][CL][22520] "
            "| NOT AUTH | VNCL_Stinger_800000000001[800000000001]|CSCItemNavigation::OnServerRerouted|...",
            &sys, &ship, &leaving);
        allPassed &= check(!q && ship.empty(), "a route the server replicates for another ship names nothing");
        const bool b = SCMiner::GameState::parseLine(
            "<2026-09-10T14:30:23.381Z> Loading screen for Pyro : SC_Frontend closed after 44.25 seconds",
            &sys, &ship, &leaving);
        allPassed &= check(b && sys == "Pyro" && ship.empty(), "the loading screen names the system");
        const bool c = SCMiner::GameState::parseLine(
            "<2026-09-10T14:53:43.531Z> [Notice] <Vehicle Control Flow> CVehicleMovementBase::ClearDriver: Local "
            "client node [200000000001] releasing control token for 'MISC_Prospector_824000000606' [824000000606] "
            "[Team_CGP4][Vehicle]", &sys, &ship, &leaving);
        allPassed &= check(c && ship == "MISC Prospector" && leaving, "leaving the seat names the ship");
        sys.clear(); ship.clear(); leaving = false;
        const bool j = SCMiner::GameState::parseLine(
            "<2026-09-10T14:53:19.373Z> [Notice] <SHUDEvent_OnNotification> Added notification \"You have joined "
            "channel 'MISC Prospector : Someone'.\" [7] to queue [Team_HUD][HUD]", &sys, &ship, &leaving);
        allPassed &= check(j && ship == "MISC Prospector" && !leaving, "boarding a ship names it by its crew channel");
        const bool l = SCMiner::GameState::parseLine(
            "<2026-09-10T15:07:38.737Z> [Notice] <UpdateNotificationItem> Notification \"You have left the channel "
            "'Aegis Idris-P : Someone'.\"", &sys, &ship, &leaving);
        allPassed &= check(l && ship == "Aegis Idris-P" && leaving, "leaving a ship names it by its crew channel");
        allPassed &= check(!SCMiner::GameState::parseLine(
                               "<2026-09-10T15:07:38.737Z> [Notice] <SHUDEvent_OnNotification> Added notification "
                               "\"You have joined channel 'Global'.\"", &sys, &ship, &leaving),
                           "a channel without an owner is not a ship");
        allPassed &= check(SCMiner::GameState::maker("DRAK_Golem") == "Drake" &&
                           SCMiner::GameState::label("DRAK_Golem") == "Drake Golem" &&
                           SCMiner::GameState::label("AEGS_Idris_P") == "Aegis Idris P" &&
                           SCMiner::GameState::maker("Drake Golem") == "Drake" &&
                           SCMiner::GameState::label("MISC Prospector") == "MISC Prospector",
                           "ship classes and display names read as maker and model");
        allPassed &= check(!SCMiner::GameState::parseLine(
                               "<2026-09-10T16:15:24.983Z> [Notice] <Update Inventory Location> Player [x] is changing location.",
                               &sys, &ship, &leaving),
                           "a line that says nothing says nothing");
    }

    // Which hull: a target picked in it names it by its id, and so does its
    // seat left; boarding names only the kind, and the hull is unknown until
    // the next pick - it may be the other Prospector
    {
        SCMiner::GameState::Event left;
        SCMiner::GameState::parse(
            "<2026-09-10T14:53:43.531Z> [Notice] <Vehicle Control Flow> CVehicleMovementBase::ClearDriver: Local "
            "client node [200000000001] releasing control token for 'MISC_Prospector_824000000606' [824000000606] "
            "[Team_CGP4][Vehicle]", &left);
        allPassed &= check(left.hull == "824000000606" && left.leaving, "leaving the seat names the hull by its id");
        SCMiner::GameState seat;
        auto feed = [&](const std::string& line) {
            SCMiner::GameState::Event event;
            if (SCMiner::GameState::parse(line, &event)) seat.apply(event);
        };
        auto pickIn = [](const std::string& id) {
            return "<2026-09-27T09:15:47.965Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL]"
                   "[4888] | NOT AUTH | MISC_Prospector_" + id + "[" + id + "]|CSCItemNavigation::"
                   "OnPlayerSelectedQuantumTarget|Player has selected point pyro5c as their destination, routing "
                   "locally [Team_CGP4][QuantumTravel]";
        };
        feed(pickIn("840000000707"));
        const std::string picked = seat.hull();
        feed("<2026-09-27T09:40:00.000Z> [Notice] <Vehicle Control Flow> CVehicleMovementBase::ClearDriver: Local "
             "client node [200000000001] releasing control token for 'MISC_Prospector_840000000707' [840000000707] "
             "[Team_CGP4][Vehicle]");
        const std::string gotOut = seat.hull();
        feed("<2026-09-27T09:42:00.000Z> [Notice] <SHUDEvent_OnNotification> Added notification \"You have joined "
             "channel 'MISC Prospector : Someone'.\" [7] to queue [Team_HUD][HUD]");
        const std::string boarded = seat.hull();
        feed(pickIn("810000000202"));
        const std::string other = seat.hull();
        feed("<2026-09-27T09:50:00.000Z> [Notice] <SHUDEvent_OnNotification> Added notification \"You have joined "
             "channel 'Argo MOLE : Someone'.\" [7] to queue [Team_HUD][HUD]");
        allPassed &= check(picked == "840000000707" && gotOut == "840000000707" && boarded.empty() &&
                               other == "810000000202" && seat.hull().empty() && seat.ship() == "Argo MOLE",
                           "the hull is known from a target picked in it until the next boarding (got " + picked +
                               ", " + gotOut + ", '" + boarded + "', " + other + ")");
        // ...and each boarding counts, so that two of one kind boarded one after
        // the other are two seats to the loadout read off the HUD
        allPassed &= check(seat.boardings() == 3,
                           "sitting down in the first, again after leaving it, and in the MOLE are three boardings (got " +
                               std::to_string(seat.boardings()) + ")");

        // A ship destroyed with the player in it: the one line 4.10 writes for it
        SCMiner::GameState::Event died;
        SCMiner::GameState::parse(
            "<2026-05-31T13:30:44.823Z> [Notice] <[ActorState] Dead> [ACTOR STATE][CSCActorControlStateDead::"
            "PrePhysicsUpdate] Actor 'Someone' [200000000001] ejected from zone 'ARGO_MOLE_812345678901' "
            "[812345678901] to zone 'OOC_Stanton_3b_Wala' [200000000002] due to previous zone being in a destroyed "
            "vehicle with detached interior. [Team_ActorTech][Actor]", &died);
        seat.apply(died);
        SCMiner::GameState::Event walked;
        SCMiner::GameState::parse(
            "<2026-05-31T13:31:00.000Z> [Notice] <[ActorState] Dead> Actor 'Someone' [200000000001] ejected from "
            "zone 'OOC_Stanton_3b_Wala' [200000000002] to zone 'Somewhere' [3]", &walked);
        allPassed &= check(died.lost == "812345678901" && seat.lostHulls().size() == 1 &&
                               seat.lostHulls().back() == "812345678901" && walked.lost.empty(),
                           "a hull destroyed with the player in it is named by its id, and nothing else is");
    }

    // ...and the fit that flew it waits for the replacement: the next hull of
    // its kind no fit knows is the claimed one
    {
        using SCMiner::MinerGUI;
        const SCMiner::MiningShip* prospector = nullptr;
        const SCMiner::MiningShip* mole = nullptr;
        for (int i = 0; i < SCMiner::MINING_SHIP_COUNT; i++) {
            if (std::string(SCMiner::MINING_SHIPS[i].name).find("Prospector") != std::string::npos) prospector = &SCMiner::MINING_SHIPS[i];
            if (std::string(SCMiner::MINING_SHIPS[i].name).find("MOLE") != std::string::npos) mole = &SCMiner::MINING_SHIPS[i];
        }
        std::vector<MinerGUI::ShipFit> fits(3);
        fits[0].ship = prospector; fits[0].name = "Fast"; fits[0].hulls = {"111111111111"};
        fits[1].ship = prospector; fits[1].name = "Wide"; fits[1].hulls = {"222222222222"};
        fits[2].ship = mole; fits[2].name = "MOLE"; fits[2].hulls = {"333333333333"};
        const int gone = MinerGUI::fitLosingHull(fits, "111111111111");
        const int mismatch = MinerGUI::takeReplacement(fits, mole, "444444444444");        // another kind
        const int known = MinerGUI::takeReplacement(fits, prospector, "222222222222");     // the other one
        const int claimed = MinerGUI::takeReplacement(fits, prospector, "555555555555");
        const int again = MinerGUI::takeReplacement(fits, prospector, "666666666666");     // nothing waits now
        allPassed &= check(gone == 0 && mismatch < 0 && known < 0 && claimed == 0 && again < 0 &&
                               fits[0].hulls == std::vector<std::string>{"555555555555"} && !fits[0].lost &&
                               fits[1].hulls == std::vector<std::string>{"222222222222"},
                           "a lost Prospector's fit takes the next unknown Prospector hull, and only that one");
        MinerGUI::fitLosingHull(fits, "555555555555");
        MinerGUI::fitLosingHull(fits, "222222222222");
        allPassed &= check(MinerGUI::takeReplacement(fits, prospector, "777777777777") < 0 && fits[0].lost &&
                               fits[1].lost,
                           "two lost of one kind: no guess, the player picks as before");
        std::ostringstream written;
        MinerGUI::LoadoutFile file;
        file.fits = fits;
        MinerGUI::writeLoadoutFile(written, file);
        std::istringstream again2(written.str());
        const MinerGUI::LoadoutFile back = MinerGUI::readLoadoutFile(again2);
        allPassed &= check(back.fits.size() == 3 && back.fits[0].lost && back.fits[1].lost && !back.fits[2].lost,
                           "lost=1 is kept in loadout.txt, for a claim in another session");
    }

    // ...and the loadout a one-seat ship's HUD shows picks the fit that has it:
    // two Prospectors told apart by their heads, not by a hull the log may
    // not name before the next quantum jump
    {
        using SCMiner::MinerGUI;
        auto laserNamed = [](const char* name) -> const SCMiner::MiningLaser* {
            for (int i = 0; i < SCMiner::MINING_LASER_COUNT; i++) {
                if (std::string(SCMiner::MINING_LASERS[i].name) == name) return &SCMiner::MINING_LASERS[i];
            }
            return nullptr;
        };
        auto moduleNamed = [](const char* name) -> const SCMiner::MiningModule* {
            for (int i = 0; i < SCMiner::MINING_MODULE_COUNT; i++) {
                if (std::string(SCMiner::MINING_MODULES[i].name) == name) return &SCMiner::MINING_MODULES[i];
            }
            return nullptr;
        };
        const SCMiner::MiningShip* prospector = nullptr;
        const SCMiner::MiningShip* golem = nullptr;
        for (int i = 0; i < SCMiner::MINING_SHIP_COUNT; i++) {
            if (std::string(SCMiner::MINING_SHIPS[i].id) == "prospector") prospector = &SCMiner::MINING_SHIPS[i];
            if (std::string(SCMiner::MINING_SHIPS[i].id) == "golem") golem = &SCMiner::MINING_SHIPS[i];
        }
        const SCMiner::MiningLaser* helix = laserNamed("Helix I Mining Laser");
        const SCMiner::MiningLaser* hofstede = laserNamed("Hofstede-S1 Mining Laser");
        const SCMiner::MiningLaser* pitman = laserNamed("Pitman Mining Laser");
        const SCMiner::MiningModule* focus3 = moduleNamed("Focus III Module");
        const SCMiner::MiningModule* rieger3 = moduleNamed("Rieger-C3 Module");
        std::vector<MinerGUI::ShipFit> fits(3);
        fits[0].ship = prospector; fits[0].name = "Helix"; fits[0].heads = {SCMiner::MiningLoadout{helix, {focus3, rieger3}}};
        fits[1].ship = prospector; fits[1].name = "Hofstede"; fits[1].heads = {SCMiner::MiningLoadout{hofstede, {}}};
        fits[2].ship = golem; fits[2].name = "Golem"; fits[2].heads = {SCMiner::MiningLoadout{pitman, {}}};
        const int helixFit = MinerGUI::fitForSeen(fits, 1, prospector, helix, {rieger3, focus3});
        const int hofstedeFit = MinerGUI::fitForSeen(fits, 0, prospector, hofstede, {});
        const int short1 = MinerGUI::fitForSeen(fits, 0, prospector, helix, {focus3});
        const int notGolem = MinerGUI::fitForSeen(fits, 0, golem, helix, {focus3, rieger3});
        const int golemFit = MinerGUI::fitForSeen(fits, 0, golem, pitman, {});
        allPassed &= check(helix && hofstede && pitman && focus3 && rieger3 && helixFit == 0 && hofstedeFit == 1 &&
                               short1 < 0 && notGolem < 0 && golemFit == 2,
                           "the HUD's laser and modules pick the fit that has them, in any order, and no other");

        // ...and where no fit has what the HUD shows, the ship flown takes it
        // for its own: a module swapped on the ship is swapped in its fit.
        // Which fit that is:
        const SCMiner::MiningModule* surge = moduleNamed("Surge Module");
        const SCMiner::MiningLaser* lancet = laserNamed("Lancet MH1 Mining Laser");
        fits[0].hulls = {"111111111111"};
        fits[1].hulls = {"222222222222"};
        fits[1].last = true;
        // the hull's, where the log has named it - whichever is chosen, however little they share
        const int byHull = MinerGUI::fitToAdapt(fits, 1, prospector, "111111111111", hofstede, {surge});
        // two Prospectors and no hull named: not guessed, though one is the
        // other with a module swapped - a guess would overwrite the other ship
        const int bySwap = MinerGUI::fitToAdapt(fits, 1, prospector, "", helix, {focus3, surge});
        const int byChoice = MinerGUI::fitToAdapt(fits, 0, prospector, "", lancet, {});
        // a hull neither has flown: a ship of its own, put on the list
        const int newHull = MinerGUI::fitToAdapt(fits, 0, prospector, "333333333333", helix, {focus3, surge});
        // the one Prospector there is takes it, with a hull named or not
        const std::vector<MinerGUI::ShipFit> one = {fits[0], fits[2]};
        const int onlyOne = MinerGUI::fitToAdapt(one, 1, prospector, "", helix, {focus3, surge});
        const int onlyOneNew = MinerGUI::fitToAdapt(one, 1, prospector, "333333333333", lancet, {});
        // a Prospector's hull is no Golem's, and with no fit of the kind there is none to take it
        const int ofKind = MinerGUI::fitToAdapt(fits, 0, golem, "111111111111", pitman, {surge});
        const int noneOfKind = MinerGUI::fitToAdapt({fits[0], fits[1]}, 0, golem, "", pitman, {});
        allPassed &= check(surge && lancet && byHull == 0 && bySwap == MinerGUI::FIT_UNSURE &&
                               byChoice == MinerGUI::FIT_UNSURE && newHull == -1 && onlyOne == 0 && onlyOneNew == 0 &&
                               ofKind == 2 && noneOfKind == -1,
                           "a loadout no fit has is the flown ship's: by its hull, or the one of its kind - of "
                           "several, with no hull named, none (got " + std::to_string(byHull) + ", " +
                               std::to_string(bySwap) + ", " + std::to_string(byChoice) + ", " + std::to_string(newHull) +
                               ", " + std::to_string(onlyOne) + ", " + std::to_string(onlyOneNew) + ", " +
                               std::to_string(ofKind) + ", " + std::to_string(noneOfKind) + ")");
        // ...and of two that carry the same, neither is the one for sure
        {
            std::vector<MinerGUI::ShipFit> twins = {fits[0], fits[0], fits[2]};
            twins[1].name = "Helix 2";
            twins[1].hulls = {"222222222222"};
            allPassed &= check(MinerGUI::fitsWithSeen(twins, prospector, helix, {rieger3, focus3}) == 2 &&
                                   MinerGUI::fitsWithSeen(fits, prospector, helix, {rieger3, focus3}) == 1 &&
                                   MinerGUI::fitOfHull(twins, prospector, "222222222222") == 1 &&
                                   MinerGUI::fitOfHull(twins, golem, "222222222222") == -1 &&
                                   MinerGUI::fitOfHull(twins, prospector, "") == -1,
                               "two fits that carry the same are counted as two, and a hull names its own fit");
        }

        // What it takes: the modules as the HUD lists them, the crafted
        // laser's figure kept while the laser is - aside with it when another
        // is fitted, and back when it returns
        using Modules = std::vector<const SCMiner::MiningModule*>;
        MinerGUI::ShipFit flown = fits[0];   // Helix I with Focus III and Rieger-C3
        flown.heads[0].craftedPower = 21.72f;
        const bool swapped = MinerGUI::fitTakes(flown, *helix, {focus3, surge}) && flown.heads[0].laser == helix &&
                             flown.heads[0].modules == Modules{focus3, surge} &&
                             flown.heads[0].craftedPower == 21.72f && flown.crafted.empty();
        const bool aside = MinerGUI::fitTakes(flown, *hofstede, {}) && flown.heads[0].laser == hofstede &&
                           flown.heads[0].modules.empty() && flown.heads[0].craftedPower == 0.0f &&
                           flown.crafted.size() == 1 && flown.crafted[0].first == helix;
        // (as loadout.txt keeps it)
        std::ostringstream written;
        MinerGUI::LoadoutFile file;
        file.fits = {flown};
        MinerGUI::writeLoadoutFile(written, file);
        std::istringstream again(written.str());
        const MinerGUI::LoadoutFile read = MinerGUI::readLoadoutFile(again);
        const bool kept = read.fits.size() == 1 && read.fits[0].crafted.size() == 1 &&
                          read.fits[0].crafted[0].first == helix &&
                          std::abs(read.fits[0].crafted[0].second - 21.72f) < 0.01f;
        // An active module the HUD's list lost - not firing, the Head column
        // cannot say - stays on the ship while the head has a slot for it;
        // a list that fills the head is the whole of it
        {
            MinerGUI::ShipFit withSurge = fits[0];
            withSurge.heads[0].modules = {focus3, surge};
            const bool kept = MinerGUI::fitTakes(withSurge, *helix, {focus3}) &&
                              withSurge.heads[0].modules == Modules{focus3, surge};
            const bool full = MinerGUI::fitTakes(withSurge, *helix, {focus3, rieger3}) &&
                              withSurge.heads[0].modules == Modules{focus3, rieger3};
            MinerGUI::ShipFit otherLaser = fits[0];
            otherLaser.heads[0].modules = {surge};
            const bool newLaser = MinerGUI::fitTakes(otherLaser, *lancet, {}) && otherLaser.heads[0].modules.empty();
            allPassed &= check(kept && full && newLaser,
                               "an active module the HUD's list lost stays on the ship while there is a slot for "
                               "it - not when the list fills the head, nor on another laser");
        }
        // ...and picked by hand on the card, the same: the Lancet does not
        // take the Helix's figure, and the Helix gets it back
        {
            std::vector<std::pair<const SCMiner::MiningLaser*, float>> kept;
            SCMiner::MiningLoadout head{helix, {focus3}};
            head.craftedPower = 28.84f;
            MinerGUI::putLaser(kept, head, lancet);
            const bool lancetBare = head.laser == lancet && head.craftedPower == 0.0f && kept.size() == 1;
            head.craftedPower = 12.5f;   // typed in for the Lancet
            MinerGUI::putLaser(kept, head, helix);
            const bool helixBack = head.laser == helix && head.craftedPower == 28.84f && kept.size() == 1 &&
                                   kept[0].first == lancet && kept[0].second == 12.5f;
            MinerGUI::putLaser(kept, head, helix);   // the same laser again changes nothing
            allPassed &= check(lancetBare && helixBack && head.craftedPower == 28.84f,
                               "a laser picked on the card leaves the last one's crafted figure aside, and the "
                               "one that returns gets its own back");
        }
        const bool back = MinerGUI::fitTakes(flown, *helix, {rieger3}) && flown.heads[0].craftedPower == 21.72f &&
                          flown.crafted.empty() && flown.heads[0].modules == Modules{rieger3};
        allPassed &= check(swapped && aside && kept && back,
                           "the fit takes the HUD's modules; a crafted laser's bonus stays with its laser, aside "
                           "while another is fitted - in loadout.txt too - and back with it");

        // What a ship of the kind cannot carry changes nothing: the Golem's
        // own laser on a Prospector, another on a Golem, more modules than
        // the head has slots, and a ship of three heads at all
        MinerGUI::ShipFit ownLaser = fits[2];
        MinerGUI::ShipFit threeHeads;
        for (int i = 0; i < SCMiner::MINING_SHIP_COUNT; i++) {
            if (std::string(SCMiner::MINING_SHIPS[i].id) == "mole") threeHeads.ship = &SCMiner::MINING_SHIPS[i];
        }
        threeHeads.heads.assign(3, SCMiner::MiningLoadout{});
        const bool refused = !MinerGUI::fitTakes(flown, *pitman, {}) && !MinerGUI::fitTakes(ownLaser, *helix, {}) &&
                             !MinerGUI::fitTakes(flown, *hofstede, {focus3, rieger3}) &&
                             !MinerGUI::fitTakes(threeHeads, *helix, {}) && flown.heads[0].laser == helix &&
                             ownLaser.heads[0].laser == pitman && threeHeads.heads[0].laser == nullptr &&
                             MinerGUI::fitTakes(ownLaser, *pitman, {focus3}) &&
                             ownLaser.heads[0].modules == Modules{focus3};
        allPassed &= check(refused, "a reading no ship of the kind could carry changes no fit: another ship's laser, "
                                    "more modules than slots, a MOLE");

        // ...and whose HUD it is: by the scan panel's labels, and with no
        // panel up by the ship the log has the player sitting in
        using SCMiner::hudShipOf;
        auto is = [](const char* got, const char* want) {
            return want ? got && std::string(got) == want : got == nullptr;
        };
        const bool whose = is(hudShipOf('L', "", false), "prospector") && is(hudShipOf('M', "Drake Golem", true), "prospector") &&
                           is(hudShipOf('G', "", false), "golem") && is(hudShipOf('H', "", false), "golem") &&
                           is(hudShipOf('K', "MISC Prospector", true), nullptr) &&   // a MOLE's panel is a MOLE's
                           is(hudShipOf(0, "MISC Prospector", true), "prospector") &&
                           is(hudShipOf(0, "Drake Golem", true), "golem") &&
                           is(hudShipOf(0, "MISC Prospector", false), nullptr) &&   // out of the seat
                           is(hudShipOf(0, "Argo MOLE", true), nullptr) && is(hudShipOf(0, "", true), nullptr);
        allPassed &= check(whose, "the HUD is the Prospector's or the Golem's by the panel's labels, and with no panel "
                                  "up by the ship the log has the player sitting in - never a MOLE's");
    }

    // Where exactly: the names the log uses, the tables' places behind
    // them, and the state machine across a jump, on the log of a trip from
    // Levski to Monox by way of the Nyx-Pyro jump
    {
        using SCMiner::placeByLogName;
        using SCMiner::placeByDestination;
        const auto monox = placeByLogName("Monox", "");
        allPassed &= check(monox.system == "Pyro" && monox.oreLocation == "Pyro II (Monox)",
                           "Monox is Pyro II in the ore tables (got " + monox.system + ", " + monox.oreLocation + ")");
        const auto hurl1 = placeByLogName("HUR-L1 Green Glade Station", "");
        allPassed &= check(hurl1.name == "HUR-L1" && hurl1.system == "Stanton" && hurl1.oreLocation == "Lagrange A",
                           "HUR-L1 is Lagrange A of the tables (got " + hurl1.name + ", " + hurl1.oreLocation + ")");
        allPassed &= check(placeByLogName("HUR L2", "").oreLocation == "Lagrange F", "HUR L2 is Lagrange F");
        {
            // ...and all twenty of Stanton's can be picked by hand, each with
            // its group of the tables
            int lagrangePoints = 0;
            bool hur3 = false;
            for (const SCMiner::GamePlace& place : SCMiner::minablePlaces()) {
                if (place.system == "Stanton" && place.oreLocation.rfind("Lagrange ", 0) == 0) lagrangePoints++;
                if (place.name == "HUR-L3") hur3 = place.oreLocation == "Lagrange E";
            }
            allPassed &= check(lagrangePoints == 20 && hur3,
                               "Stanton's twenty Lagrange points in the list a place is picked from by hand, HUR-L3 "
                               "as Lagrange E (got " + std::to_string(lagrangePoints) + ")");
            // ...and Pyro's clusters as one entry, a place the log can then
            // name more closely
            int clusters = 0;
            for (const SCMiner::GamePlace& place : SCMiner::minablePlaces()) {
                if (place.system == "Pyro" && place.oreLocation == "Pyro Deep Space Asteroids") clusters++;
            }
            allPassed &= check(clusters == 1 && placeByLogName("Asteroid clusters", "").system == "Pyro" &&
                                   placeByLogName("Asteroid clusters", "").oreLocation == "Pyro Deep Space Asteroids" &&
                                   SCMiner::contradictsHandPlace("Asteroid clusters", "PYR2 L4", "Pyro"),
                               "Pyro's asteroid clusters as one entry to pick, the tables' Pyro Deep Space Asteroids - "
                               "and PYR2 L4 off the log takes over from it");
        }
        allPassed &= check(placeByLogName("Grim HEX", "").oreLocation == "Yela Asteroid Belt",
                           "Grim HEX sits in Yela's belt");
        allPassed &= check(placeByLogName("Magda ", "").name == "Magda", "a trailing space is trimmed");
        allPassed &= check(placeByLogName("Stanton Gateway", "Nyx").system.empty(),
                           "a gateway does not say which system it stands in");
        allPassed &= check(!placeByLogName("Nyx System", "").known() && !placeByLogName("Stanton", "").known(),
                           "the system itself is no place");
        // A terminal opened says where: the inventory request's codes
        using SCMiner::placeByLocationCode;
        const auto gate = placeByLocationCode("RR_JP_StantonPyro", "");
        const auto back = placeByLocationCode("RR_JP_PyroStanton", "");
        const auto levski = placeByLocationCode("Nyx_Levski", "");
        const auto checkmate = placeByLocationCode("RR_P2_L4", "");
        const bool codes = gate.name == "Pyro Gateway" && gate.system == "Stanton" && back.name == "Stanton Gateway" &&
                           back.system == "Pyro" && levski.name == "Levski" && levski.system == "Nyx" &&
                           placeByLocationCode("Stanton1_Lorville", "").name == "Lorville" &&
                           placeByLocationCode("RR_HUR_LEO", "").name == "Everus Harbor" &&
                           placeByLocationCode("RR_HUR_L1", "").name == "HUR-L1" && checkmate.name == "Checkmate" &&
                           checkmate.system == "Pyro" && placeByLocationCode("RR_P6_LEO", "").name == "Ruin Station" &&
                           placeByLocationCode("GrimHEX", "").name == "Grim HEX" &&
                           !placeByLocationCode("INVALID_LOCATION_ID", "").known() &&
                           !placeByLocationCode("Pyro4_Outpost_col_m_scrp_indy_001", "").known();
        allPassed &= check(codes, "where a terminal was opened: a gateway in the system it stands in, Levski, Lorville, "
                                  "a planet's station, Stanton's and Pyro's rest stops by name - an outpost not at all");
        allPassed &= check(placeByLogName("Asteroid Cluster", "Pyro").oreLocation == "Pyro Deep Space Asteroids" &&
                               placeByLogName("Asteroid Cluster", "Stanton").oreLocation.empty(),
                           "an asteroid cluster is Pyro's deep space only in Pyro");
        allPassed &= check(placeByLogName("RAB-CHARLIE", "").oreLocation == "Pyro Deep Space Asteroids",
                           "a remote asteroid base is Pyro's deep space");
        allPassed &= check(placeByDestination("pyro2", "").name == "Monox" &&
                               placeByDestination("pyro5c", "").oreLocation == "Pyro V-c (Adir)",
                           "pyro2 is Monox, pyro5c is Adir");
        allPassed &= check(placeByDestination("OOC_Stanton_2b_Daymar", "").oreLocation == "Daymar" &&
                               placeByDestination("OOC_Stanton_4_Microtech", "").oreLocation == "microTech",
                           "Stanton's object containers name the body at the end");
        allPassed &= check(placeByDestination("LOC_RR_S1_L2", "").oreLocation == "Lagrange F",
                           "the rest stop at HUR-L2 is Lagrange F");
        allPassed &= check(placeByDestination("levski_all-001", "").system == "Nyx", "levski_all-001 is Levski");
        // A planet's low-orbit station is no Lagrange point, and a Lagrange
        // point itself is one
        allPassed &= check(placeByDestination("rs_ext_pyro3_leo", "").name == "Orbituary" &&
                               placeByDestination("rs_ext_pyro6_leo", "").name == "Ruin Station" &&
                               placeByDestination("rs_ext_cru-leo1", "").name == "Seraphim Station" &&
                               placeByDestination("rs_ext_pyro2_l4", "").name == "PYR2 L4" &&
                               placeByDestination("P5_L3", "").name == "PYR5 L3" &&
                               placeByDestination("OOC_Stanton2_L2", "").name == "CRU-L2" &&
                               placeByDestination("OOC_Stanton2_L2", "").oreLocation == "Lagrange E",
                           "rs_ext_pyro3_leo is Orbituary, rs_ext_pyro6_leo Ruin Station, rs_ext_cru-leo1 Seraphim "
                           "Station; rs_ext_pyro2_l4 and P5_L3 are Pyro's Lagrange points, OOC_Stanton2_L2 is CRU-L2");
        allPassed &= check(SCMiner::jumpDestination("rs_ext_nyx-pyro_jp1") == "Pyro" &&
                               SCMiner::jumpDestination("LOC_rs_ext_stan-magnus_jp1") == "Magnus" &&
                               SCMiner::jumpDestination("pyro2").empty(),
                           "a jump point names the far side");
        allPassed &= check(!placeByDestination("MISSION_QT_Quantum_Beacon_784491487948", "").known() &&
                               !placeByDestination("ObjectContainer_RestStop", "").known(),
                           "a beacon or an unnamed rest stop says nothing");
        // The state across a jump from Nyx to Pyro, as the game log writes it
        SCMiner::GameState state;
        auto feed = [&](const char* line) {
            SCMiner::GameState::Event event;
            if (SCMiner::GameState::parse(line, &event)) state.apply(event);
        };
        feed("<2026-09-11T10:27:45.903Z> Loading screen for Nyx : SC_Frontend closed after 44.25 seconds");
        feed("<2026-09-11T10:31:49.569Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
             "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Levski "
             "for route to destination rs_ext_nyx-pyro_jp1 [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.system() == "Nyx" && state.place() == "Levski",
                           "a route from Levski puts the ship at Levski (got " + state.place() + ")");
        feed("<2026-09-11T10:31:49.556Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][41716] | "
             "NOT AUTH | ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|Player has "
             "selected point rs_ext_nyx-pyro_jp1 as their destination, routing locally [Team_CGP4][QuantumTravel]");
        feed("<2026-09-11T10:34:25.413Z> [Notice] <Quantum Drive Arrived - Arrived at Final Destination> "
             "[ItemNavigation][CL][41716] | NOT AUTH | ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::"
             "OnQuantumDriveArrived|Quantum Drive has arrived at final destination [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.system() == "Nyx" && state.place() == "Pyro Gateway",
                           "arriving at the jump point is still this side (got " + state.system() + ", " + state.place() + ")");
        feed("<2026-09-11T10:38:03.569Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
             "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Nyx "
             "Gateway for route to destination pyro2 [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.system() == "Pyro" && state.place() == "Nyx Gateway",
                           "the gateway after the jump is the far side (got " + state.system() + ")");
        feed("<2026-09-11T10:38:03.569Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][41716] | "
             "NOT AUTH | ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|Player has "
             "selected point pyro2 as their destination, routing locally [Team_CGP4][QuantumTravel]");
        feed("<2026-09-11T10:41:37.000Z> [Notice] <Quantum Drive Arrived - Arrived at Final Destination> "
             "[ItemNavigation][CL][41716] | NOT AUTH | ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::"
             "OnQuantumDriveArrived|Quantum Drive has arrived at final destination [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.system() == "Pyro" && state.place() == "Monox",
                           "arriving at pyro2 puts the ship at Monox (got " + state.place() + ")");
        feed("<2026-09-11T11:00:00.000Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
             "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Pyro "
             "for route to destination pyro1 [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.system() == "Pyro" && state.place().empty() && state.inSpace(),
                           "a route from the system itself is out in space");
        feed("<2026-09-11T11:00:00.000Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
             "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Stanton "
             "Gateway for route to destination levski_all-001 [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.system() == "Pyro" && state.place() == "Stanton Gateway",
                           "a gateway on its own changes no system (got " + state.system() + ")");
        // Arriving at a jump point and then NOT crossing. The gateway on
        // this side is named after the far system, so the far system's own
        // name is the one thing it cannot prove: "Nyx Gateway" stands in
        // Pyro.
        feed("<2026-09-11T11:20:00.000Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][41716] | "
             "NOT AUTH | ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|Player has "
             "selected point rs_ext_pyro-nyx_jp as their destination, routing locally [Team_CGP4][QuantumTravel]");
        feed("<2026-09-11T11:24:00.000Z> [Notice] <Quantum Drive Arrived - Arrived at Final Destination> "
             "[ItemNavigation][CL][41716] | NOT AUTH | ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::"
             "OnQuantumDriveArrived|Quantum Drive has arrived at final destination [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.system() == "Pyro" && state.place() == "Nyx Gateway",
                           "the jump point to Nyx is still in Pyro (got " + state.system() + ", " + state.place() + ")");
        feed("<2026-09-11T11:30:00.000Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
             "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Nyx "
             "Gateway for route to destination pyro2 [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.system() == "Pyro" && state.place() == "Nyx Gateway",
                           "...and Nyx Gateway is where you wait for it, not where you land (got " +
                               state.system() + ")");
        allPassed &= check(!state.inSpace(), "...and a place named again is not open space any more");
        // A hop to something the map cannot place - one's own parked ship,
        // a beacon, a party member - leaves the place where it was.
        feed("<2026-09-11T11:10:00.000Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
             "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Monox "
             "for route to destination UnattendedVehicleMarker_821192826829 [Team_CGP4][QuantumTravel]");
        feed("<2026-09-11T11:10:01.000Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][41716] | "
             "NOT AUTH | ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|Player has "
             "selected point UnattendedVehicleMarker_821192826829 as their destination, routing locally [Team_CGP4][QuantumTravel]");
        feed("<2026-09-11T11:11:31.000Z> [Notice] <Quantum Drive Arrived - Arrived at Final Destination> "
             "[ItemNavigation][CL][41716] | NOT AUTH | ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::"
             "OnQuantumDriveArrived|Quantum Drive has arrived at final destination [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.place() == "Monox",
                           "a hop to something unplaceable leaves the place alone (got " +
                               (state.place().empty() ? std::string("nothing") : state.place()) + ")");

        // ...but a place the ship cannot be at any more does not survive the
        // system changing: Bloom is in Pyro, so a jump to Nyx clears it.
        feed("<2026-09-11T11:12:00.000Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
             "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Bloom "
             "for route to destination UnattendedVehicleMarker_1 [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.place() == "Bloom" && state.system() == "Pyro",
                           "a route from Bloom puts the ship in Pyro");
        feed("<2026-09-11T11:13:00.000Z> Loading screen for Nyx : SC_Frontend closed after 21.00 seconds");
        allPassed &= check(state.system() == "Nyx" && state.place().empty(),
                           "...and changing system to Nyx drops Bloom, which is not in it (got " +
                               (state.place().empty() ? std::string("nothing") : state.place()) + ")");

        // ...until the moon itself streams in. The proximity sensor line is
        // the only one written on ARRIVAL, and it names the body's own
        // object container.
        feed("<2026-09-13T21:54:42.000Z> [Notice] <Calculate Route> [ItemNavigation][CL][41716] | NOT AUTH | "
             "ARGO_MOLE_820000000404[820000000404]|CSCItemNavigation::CalculateRoute|Projected Start Location is Nyx "
             "Gateway for route to destination NavPoint_Dynamic_817077013330 [Team_CGP4][QuantumTravel]");
        allPassed &= check(state.place() == "Nyx Gateway",
                           "a route to a dynamic nav point still says only where it set off from");
        feed("<2026-09-13T21:58:02.000Z> [Notice] <ProximitySensorMakingLocalHelper> Proximity sensor "
             "[DoorProximitySensorItem_Template_807238271341 - Class(DoorProximitySensorItem_Template) - "
             "Context(Streamable Immovable) - Socpak()], helper is being made in zone [pyro4 - "
             "Class(OrbitingObjectContainer) - Context(Unstreamable Immovable) - "
             "Socpak(Data/objectcontainers/pu/system/pyro/pyro4.socpak)] [Team_ActorTech][Entity]");
        allPassed &= check(state.system() == "Pyro" && state.place() == "Pyro IV",
                           "the body streaming in says where the ship landed (got " +
                               (state.place().empty() ? std::string("nothing") : state.place()) + ")");
        feed("<2026-09-13T22:31:18.000Z> [Notice] <ProximitySensorMakingLocalHelper> Proximity sensor "
             "[DoorProximitySensorItem_2x5x1_5_804698222453 - Class(DoorProximitySensorItem_2x5x1_5) - "
             "Context(Streamable Immovable) - Socpak()], helper is being made in zone [OOC_Stanton_2b_Daymar - "
             "Class(OrbitingObjectContainer) - Context(Unstreamable Immovable) - "
             "Socpak(Data/objectcontainers/pu/system/stanton/stanton2b.socpak)] [Team_ActorTech][Entity]");
        allPassed &= check(state.system() == "Stanton" && state.place() == "Daymar",
                           "...and it carries the system with it (got " + state.system() + ", " + state.place() + ")");
        {
            SCMiner::GameState::Event ev;
            SCMiner::GameState::parse(
                "<2026-09-13T22:31:18.000Z> [Notice] <ProximitySensorMakingLocalHelper> Proximity sensor [x], "
                "helper is being made in zone [Area18_central-001 - Class(ObjectContainer) - Context(Streamable "
                "Immovable) - Socpak(Data/objectcontainers/pu/loc/flagship/stanton/area18.socpak)]",
                &ev);
            allPassed &= check(ev.zone.empty(), "a zone that is not a body is no place");
        }

        // Whose lines: the navigation lines carry the ship's entity id, and the
        // log has every ship in range. Another ship's arrival between the
        // player's pick and their own must not set the place early (33 in 200
        // logs), nor may other ships' route lines move the system and the place
        // (163).
        {
            SCMiner::GameState::Event ev;
            SCMiner::GameState::parse(
                "<2026-09-27T09:16:22.155Z> [Notice] <Quantum Drive Arrived - Arrived at Final Destination> "
                "[ItemNavigation][CL][4888] | NOT AUTH | MISC_Prospector_840000000707[840000000707]|CSCItemNavigation::"
                "OnQuantumDriveArrived|Quantum Drive has arrived at final destination [Team_CGP4][QuantumTravel]", &ev);
            allPassed &= check(ev.arrived && ev.navShip == "840000000707" && !ev.playerAct &&
                                   ev.stamp == "2026-09-27T09:16:22",
                               "an arrival line names its ship by id and carries its time (got " + ev.navShip + ", " +
                                   ev.stamp + ")");
            SCMiner::GameState::Event pick;
            SCMiner::GameState::parse(
                "<2026-09-27T09:15:47.965Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][4888] "
                "| NOT AUTH | MISC_Prospector_840000000707[840000000707]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|"
                "Player has selected point pyro5c as their destination, routing locally [Team_CGP4][QuantumTravel]", &pick);
            allPassed &= check(pick.playerAct && pick.navShip == "840000000707" && pick.target == "pyro5c",
                               "a pick line is the player's own act in that ship");
        }
        {
            SCMiner::GameState own;
            auto feedTo = [&](const char* line) {
                SCMiner::GameState::Event event;
                if (SCMiner::GameState::parse(line, &event)) own.apply(event);
            };
            feedTo("<2026-09-27T09:15:47.965Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][4888] "
                   "| NOT AUTH | MISC_Prospector_840000000707[840000000707]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|"
                   "Player has selected point pyro5c as their destination, routing locally [Team_CGP4][QuantumTravel]");
            feedTo("<2026-09-27T09:15:47.966Z> [Notice] <Calculate Route> [ItemNavigation][CL][4888] | NOT AUTH | "
                   "MISC_Prospector_840000000707[840000000707]|CSCItemNavigation::CalculateRoute|Projected Start Location is "
                   "Ignis for route to destination pyro5c [Team_CGP4][QuantumTravel]");
            allPassed &= check(own.place() == "Ignis" && own.system() == "Pyro", "the player's own route sets off from Ignis");
            feedTo("<2026-09-27T09:16:01.000Z> [Notice] <Quantum Drive Arrived - Arrived at Final Destination> "
                   "[ItemNavigation][CL][4888] | NOT AUTH | ANVL_Hornet_F7A_Mk2_800000000001[800000000001]|CSCItemNavigation::"
                   "OnQuantumDriveArrived|Quantum Drive has arrived at final destination [Team_CGP4][QuantumTravel]");
            allPassed &= check(own.place() == "Ignis",
                               "another ship arriving is not the player arriving (got " + own.place() + ")");
            feedTo("<2026-09-27T09:16:10.000Z> [Notice] <Calculate Route> [ItemNavigation][CL][4888] | NOT AUTH | "
                   "AEGS_Idris_P_800000000002[800000000002]|CSCItemNavigation::CalculateRoute|Projected Start Location is Nyx "
                   "for route to destination levski_all-001 [Team_CGP4][QuantumTravel]");
            allPassed &= check(own.place() == "Ignis" && own.system() == "Pyro" && !own.inSpace(),
                               "another ship's route moves neither the place nor the system (got " + own.system() + ", " +
                                   own.place() + ")");
            feedTo("<2026-09-27T09:16:22.155Z> [Notice] <Quantum Drive Arrived - Arrived at Final Destination> "
                   "[ItemNavigation][CL][4888] | NOT AUTH | MISC_Prospector_840000000707[840000000707]|CSCItemNavigation::"
                   "OnQuantumDriveArrived|Quantum Drive has arrived at final destination [Team_CGP4][QuantumTravel]");
            allPassed &= check(own.place() == "Adir", "...and the player's own arrival does (got " + own.place() + ")");
            // A new ship's first route line comes a few hundred milliseconds
            // before the line that names the player picking in it; held, it
            // counts once that line comes
            feedTo("<2026-09-27T12:37:05.503Z> [Notice] <Calculate Route> [ItemNavigation][CL][4888] | NOT AUTH | "
                   "ARGO_MOLE_800000000003[800000000003]|CSCItemNavigation::CalculateRoute|Projected Start Location is Monox "
                   "for route to destination pyro1 [Team_CGP4][QuantumTravel]");
            allPassed &= check(own.place() == "Adir",
                               "a route of a ship the player has not acted in waits (got " + own.place() + ")");
            feedTo("<2026-09-27T12:37:05.671Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][4888] "
                   "| NOT AUTH | ARGO_MOLE_800000000003[800000000003]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|"
                   "Player has selected point pyro1 as their destination, routing locally [Team_CGP4][QuantumTravel]");
            allPassed &= check(own.place() == "Monox",
                               "...and counts once the player's own line names that ship (got " + own.place() + ")");
            // The ten-minute statistics: the one body with terrain loaded
            feedTo("<2026-09-27T12:47:53.737Z>   planet cells:    0 [    0] meshes:    0 [    0] name: OOC_Stanton_3a_Lyria");
            allPassed &= check(own.place() == "Monox", "a body with no terrain loaded says nothing");
            feedTo("<2026-09-27T12:47:53.737Z>   planet cells: 1591 [16384] meshes:  117 [ 2048] name: OOC_Stanton_3b_Wala");
            allPassed &= check(own.place() == "Wala" && own.system() == "Stanton",
                               "the body with terrain loaded is where the ship is (got " + own.system() + ", " +
                                   own.place() + ")");
            allPassed &= check(own.namedPlace() == "Wala" && own.namedSystem() == "Stanton" &&
                                   own.namedStamp() == "2026-09-27T12:47:53",
                               "...and it is what the log last named, with its time (got " + own.namedPlace() + " " +
                                   own.namedStamp() + ")");
            feedTo("<2026-09-27T12:50:00.000Z> Loading screen for Pyro : SC_Frontend closed after 21.00 seconds");
            allPassed &= check(own.namedPlace().empty() && own.namedSystem() == "Pyro" &&
                                   own.namedStamp() == "2026-09-27T12:50:00",
                               "a loading screen names the system alone (got " + own.namedSystem() + ")");
        }
        // A place set by hand against what the log named since
        allPassed &= check(SCMiner::contradictsHandPlace("Wala", "Lyria", "Stanton") &&
                               SCMiner::contradictsHandPlace("Wala", "", "Pyro") &&
                               SCMiner::contradictsHandPlace("Monox", "Nyx Gateway", "Pyro"),
                           "a hand-set place is contradicted by another place, or by another system");
        allPassed &= check(!SCMiner::contradictsHandPlace("Wala", "wala", "Stanton") &&
                               !SCMiner::contradictsHandPlace("Wala", "", "Stanton") &&
                               !SCMiner::contradictsHandPlace("Wala", "", "") &&
                               !SCMiner::contradictsHandPlace("Nowhere Special", "Lyria", "Stanton"),
                           "...and not by itself, by its own system, by nothing, or when the tables do not know it");

        // The SCAN RESULTS panel, found by structure rather than by place.
        // It is anchored to the rock in 3D and slides with the view, so there
        // is no crop to take - the search looks for the one arrangement no
        // landscape and no other part of the HUD produces: two long
        // horizontal rules of the same width, at the same left edge, a
        // panel's height apart.
        //
        // The frame is tests/scanpanel/scan_fullscreen_1080.png and the box
        // is the one its README measured by template matching, 1349,435
        // 218x278. Where the frame is not beside the build - a packaged exe
        // - its panel's region comes out of the exe: see testFrame1080.
        {
            const cv::Mat frame = testFrame1080();
            allPassed &= check(!frame.empty(), "the 1080p scan panel frame is beside the build or inside the exe");
            if (!frame.empty()) {
                const SCMiner::ScanPanel found = SCMiner::findScanPanel(frame);
                allPassed &= check(found.found, "the scan panel is found in a whole 1080p frame");
                if (found.found) {
                    const int dx = std::abs(found.box.x - 1349);
                    const int dy = std::abs(found.box.y - 435);
                    const int dw = std::abs(found.box.width - 218);
                    const int dh = std::abs(found.box.height - 278);
                    allPassed &= check(dx <= 8 && dw <= 12,
                                       "...across the right columns (x off by " + std::to_string(dx) +
                                           ", width by " + std::to_string(dw) + ")");
                    allPassed &= check(dy <= 16 && dh <= 20,
                                       "...and the right rows (y off by " + std::to_string(dy) +
                                           ", height by " + std::to_string(dh) + ")");
                    allPassed &= check(found.confidence >= 0.9,
                                       "...on a pair of rules that agree (" +
                                           std::to_string((int)(found.confidence * 100)) + " %)");
                }
                // ...and on a taller frame, which is the case that matters: a 1440-tall
                // capture, such as a 5120x1440 ultrawide, comes in above the reference
                // height and is brought DOWN to it. This only proves the scaling back and
                // forth is right - a real ultrawide capture would also put the panel
                // somewhere else entirely, because it hangs off the rock.
                //
                // The other direction has a floor. Below the reference height
                // the rules are thinner than a pixel and do not survive the
                // downscale: at 960x540 this frame yields two rules and no
                // pair. Blowing such a frame back up does not put the structure
                // back, so the search says "not found" rather than guessing.
                cv::Mat tall;
                cv::resize(frame, tall, cv::Size(), 4.0 / 3, 4.0 / 3, cv::INTER_LINEAR);
                const SCMiner::ScanPanel big = SCMiner::findScanPanel(tall);
                allPassed &= check(big.found &&
                                       std::abs((int)std::lround(big.box.x * 0.75) - found.box.x) <= 24,
                                   "...and again on a 1440-tall frame, in the same place");
            }
        }

        // loadout.txt: one [ship] section per fit, the player's own ships.
        // Older files read as fits named after their ship - the one-ship file
        // with no section, and one section per kind of ship - and two
        // Prospectors of one's own, with names and hulls, come back from the
        // file as they went in.
        {
            using SCMiner::MinerGUI;
            std::istringstream before("ship=prospector\nhead=thcn_helix_s1\ncrafted=0\n"
                                      "module=passive_rieger_mk3\nmodule=passive_focus_mk3\n");
            MinerGUI::LoadoutFile file = MinerGUI::readLoadoutFile(before);
            const bool old = file.ship && std::string(file.ship->id) == "prospector" && file.fits.size() == 1 &&
                             file.fits[0].ship == file.ship && file.fits[0].name == "MISC Prospector" &&
                             file.fits[0].heads.size() == 1 && file.fits[0].heads[0].laser &&
                             std::string(file.fits[0].heads[0].laser->id) == "thcn_helix_s1" &&
                             file.fits[0].heads[0].modules.size() == 2;
            allPassed &= check(old, "the one-ship loadout.txt of before is read as a Prospector named after it");
            std::istringstream kinds("ship=mole\n[prospector]\nhead=thcn_helix_s1\ncrafted=0\n[mole]\n"
                                     "head=thcn_helix_s2\nhead=thcn_helix_s2\nhead=thcn_helix_s2\ncrafted=2.5\n");
            const MinerGUI::LoadoutFile perKind = MinerGUI::readLoadoutFile(kinds);
            allPassed &= check(perKind.fits.size() == 2 && perKind.fits[0].name == "MISC Prospector" &&
                                   perKind.fits[1].name == "Argo MOLE" && perKind.fits[1].heads.size() == 3 &&
                                   std::abs(perKind.fits[1].heads[2].craftedPower - 2.5f) < 0.001f &&
                                   perKind.ship == SCMiner::shipById("mole") && perKind.fit.empty(),
                               "...and the file of one section per kind as one ship each, named after it");

            MinerGUI::ShipFit precise;
            precise.ship = SCMiner::shipById("prospector");
            precise.name = "Prospector Hofstede";
            SCMiner::MiningLoadout hofstede;
            hofstede.laser = SCMiner::laserById("shin_hofstede_s1");
            hofstede.craftedPower = 12.5f;
            precise.heads = {hofstede};
            precise.hulls = {"810000000202", "840000000707"};
            precise.last = true;
            file.fits.push_back(precise);
            file.fit = precise.name;
            std::ostringstream written;
            MinerGUI::writeLoadoutFile(written, file);
            std::istringstream again(written.str());
            const MinerGUI::LoadoutFile back = MinerGUI::readLoadoutFile(again);
            const bool both = back.fits.size() == 2 && back.fit == "Prospector Hofstede" && back.ship == file.ship &&
                              back.fits[1].name == "Prospector Hofstede" && back.fits[1].hulls.size() == 2 &&
                              back.fits[1].hulls[1] == "840000000707" && back.fits[1].last && !back.fits[0].last &&
                              back.fits[1].heads.size() == 1 && back.fits[1].heads[0].laser == hofstede.laser &&
                              std::abs(back.fits[1].heads[0].craftedPower - 12.5f) < 0.001f &&
                              back.fits[0].heads[0].modules.size() == 2;
            allPassed &= check(both, "two Prospectors of one's own come back with their names, hulls and heads");
            std::istringstream twice("[prospector]\nname=Rock Cracker\n[prospector]\nname=rock cracker\n");
            const MinerGUI::LoadoutFile same = MinerGUI::readLoadoutFile(twice);
            allPassed &= check(same.fits.size() == 2 && same.fits[0].name == "Rock Cracker" &&
                                   same.fits[1].name == "rock cracker 2",
                               "two ships of one name are told apart by a number, the first keeping it");
            // a head the ship cannot take is trimmed, not the ship dropped
            std::istringstream odd("ship=mole\n[prospector]\nhead=thcn_helix_s2\n[mole]\nhead=thcn_helix_s2\n");
            const MinerGUI::LoadoutFile trimmed = MinerGUI::readLoadoutFile(odd);
            allPassed &= check(trimmed.fits.size() == 2 && trimmed.fits[0].heads.size() == 1 &&
                                   !trimmed.fits[0].heads[0].laser && trimmed.fits[1].heads.size() == 3,
                               "an S2 head written under the Prospector is dropped, and the MOLE gets its three");
        }

        // The Golem with the game's own labels written out - MASS:,
        // RESISTANCE:, INSTABILITY: - which run on under the first numerals
        // of their figures. The short column alone clears its bar on only 22
        // of 154 looks at such a panel. A captured panel, put back where it
        // was on the screen.
        {
            const cv::Mat golemCrop = testPicture("scanpanel/panel_golem_long_1440.png", L"SELFTEST_GOLEM");
            allPassed &= check(!golemCrop.empty(), "the long Golem panel's picture is beside the build or inside the exe");
            if (!golemCrop.empty()) {
                cv::Mat golemScreen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                golemCrop.copyTo(golemScreen(cv::Rect(3150, 480, golemCrop.cols, golemCrop.rows)));
                // a few looks, as the app takes them: the orange pass, where
                // the Golem's amber column is looked for, comes every other look
                SCMiner::ScanPanel golem;
                bool golemSure = false;
                for (int look = 0; look < 4 && !golem.found; look++) {
                    golem = SCMiner::ScanPanel();
                    golemSure = SCMiner::readPanelByShape(golemScreen, golem);
                }
                const SCMiner::RockStats& g = golem.stats;
                allPassed &= check(golem.found && golemSure && golem.labelGlyph == 'H' && g.mass == 4627.0f &&
                                       g.resistance == 56.0f && !g.resistanceAdjusted &&
                                       std::abs(g.instability - 64.16f) < 0.005f,
                                   "...and the Golem's, its labels written out over the figures (got " +
                                       std::string(1, golem.labelGlyph ? golem.labelGlyph : '-') + ", " +
                                       std::to_string((int)g.mass) + ", " + std::to_string((int)g.resistance) +
                                       " %, " + std::to_string(g.instability) + ")");
            }
        }

        // What a one-seat ship's mining HUD shows fitted, read as the app reads
        // it (LoadoutReader): the HUD's spellings of names as the second reader
        // gives them back, and a captured Prospector HUD, put back where it was
        // on the screen
        {
            auto laserName = [](const char* line) {
                const SCMiner::MiningLaser* l = SCMiner::laserByHudLine(line);
                return std::string(l ? l->name : "-");
            };
            auto moduleName = [](const char* line) {
                const SCMiner::MiningModule* m = SCMiner::moduleByHudLine(line);
                return std::string(m ? m->name : "-");
            };
            allPassed &= check(laserName("HELIX I MINING LASER") == "Helix I Mining Laser" &&
                                   laserName("HELIXIMINING LASER") == "Helix I Mining Laser" &&
                                   laserName("PITNAN MINING LASER") == "Pitman Mining Laser" &&
                                   laserName("KLEIN-S1 MINING LASER") == "Klein-S1 Mining Laser" &&
                                   laserName("BITUAN MTHNSLASER") == "-",
                               "laser names as the HUD writes them, one letter off at most, nothing for less");
            allPassed &= check(moduleName("RIEGER-C3 MODULE") == "Rieger-C3 Module" &&
                                   moduleName("FOCUS III MODULE") == "Focus III Module" &&
                                   moduleName("MINING MODULES") == "-",
                               "module names as the HUD writes them, and the list's heading names none");
            // Every laser a Prospector or a Golem takes, by its name over the
            // table - and never one of the MOLE's, a letter away from each
            // (HELIX II is a Helix I with its numeral read twice)
            {
                const char* hud[][2] = {
                    {"PITMAN MINING LASER", "Pitman Mining Laser"},
                    {"HELIX I MINING LASER", "Helix I Mining Laser"},
                    {"LANCET MH1 MINING LASER", "Lancet MH1 Mining Laser"},
                    {"LANCET MHI MINING LASER", "Lancet MH1 Mining Laser"},
                    {"KLEIN-S1 MINING LASER", "Klein-S1 Mining Laser"},
                    {"HOFSTEDE-S1 MINING LASER", "Hofstede-S1 Mining Laser"},
                    {"HOFSTEDE-SI MINING LASER", "Hofstede-S1 Mining Laser"},
                    {"IMPACT I MINING LASER", "Impact I Mining Laser"},
                    {"ARBOR MH1 MINING LASER", "Arbor MH1 Mining Laser"},
                    {"HELIX II MINING LASER", "Helix I Mining Laser"},
                    {"IMPACT II MINING LASER", "Impact I Mining Laser"},
                    {"KLEIN-S2 MINING LASER", "Klein-S1 Mining Laser"},
                };
                std::string wrong;
                for (const auto& h : hud) {
                    if (laserName(h[0]) != h[1]) wrong += std::string(" ") + h[0] + " -> " + laserName(h[0]) + ";";
                }
                allPassed &= check(wrong.empty(), "every laser a Prospector or a Golem takes, by its name on the HUD, and "
                                                  "none of the MOLE's" + (wrong.empty() ? "" : " (got" + wrong + ")"));
            }
            // Every module of 4.10 by its block in the HUD's list: its name, and
            // the lines under it the game prints for what it does - its own
            // texts (hud_mining_modifier_*), for each figure its record shows in
            // the UI. A crafted one is a bought one here: the blueprints of the
            // Deluge, Overrun, Clearcut, Stampede and Torrent III change nothing
            // it does (4.10 game files). Each is that module; cut off where the
            // Prospector's list cuts its lines, 25 letters in ("Optimal Charge
            // Window (+4"), still that module; and with its numeral run together
            // by the second reader, as it writes FOCUS III as FOCUS I, that
            // module or none - never another. "Extraction Rate" is not the
            // optimal charge rate, and the FLTR, XTR, Vaux and Forel are
            // recognised like the rest.
            {
                struct Block {
                    const char* name;
                    std::vector<std::string> lines;
                };
                const std::vector<Block> blocks = {
                    {"Brandt Module", {"Laser Power (+35%)", "Resistance (+15.5%)", "Shatter Damage (-30%)"}},
                    {"Clearcut Module", {"Optimal Charge Window (+30%)"}},
                    {"Forel Module", {"Extraction Rate (+50%)", "Resistance (+15.5%)", "Overcharge Rate (-60%)"}},
                    {"Lifeline Module", {"Instability (-20%)", "Resistance (-15.5%)", "Overcharge Rate (+60%)"}},
                    {"Optimum Module", {"Laser Power (-15%)", "Instability (-10%)", "Overcharge Rate (-80%)"}},
                    {"Rime Module", {"Laser Power (-15%)", "Resistance (-24.8%)", "Shatter Damage (-10%)"}},
                    {"Stampede Module",
                     {"Laser Power (+35%)", "Extraction Rate (-15%)", "Instability (-10%)", "Shatter Damage (-10%)"}},
                    {"Surge Module", {"Laser Power (+50%)", "Instability (+10%)", "Resistance (-15.5%)"}},
                    {"Torpid Module", {"Optimal Charge Rate (+60%)", "Shatter Damage (+40%)", "Overcharge Rate (-60%)"}},
                    {"Deluge Module", {"Resistance (-15.5%)"}},
                    {"FLTR Module", {"Extraction Rate (-15%)", "Inert Material Filtering (+20%)"}},
                    {"FLTR-L Module", {"Extraction Rate (-10%)", "Inert Material Filtering (+23%)"}},
                    {"FLTR-XL Module", {"Extraction Rate (-5%)", "Inert Material Filtering (+24%)"}},
                    {"Focus Module", {"Laser Power (-15%)", "Optimal Charge Window (+30%)"}},
                    {"Focus II Module", {"Laser Power (-10%)", "Optimal Charge Window (+37%)"}},
                    {"Focus III Module", {"Laser Power (-5%)", "Optimal Charge Window (+40%)"}},
                    {"Overrun Module", {"Resistance (-24.8%)"}},
                    {"Rieger Module", {"Laser Power (+15%)", "Optimal Charge Window (-10%)"}},
                    {"Rieger-C2 Module", {"Laser Power (+20%)", "Optimal Charge Window (-3%)"}},
                    {"Rieger-C3 Module", {"Laser Power (+25%)", "Optimal Charge Window (-1%)"}},
                    {"Torrent Module", {"Optimal Charge Window (-10%)", "Optimal Charge Rate (+30%)"}},
                    {"Torrent II Module", {"Optimal Charge Window (-3%)", "Optimal Charge Rate (+35%)"}},
                    {"Torrent III Module", {"Optimal Charge Window (-1%)", "Optimal Charge Rate (+45%)"}},
                    {"Vaux Module", {"Extraction Rate (+15%)", "Optimal Charge Rate (-20%)"}},
                    {"Vaux-C2 Module", {"Extraction Rate (+20%)", "Optimal Charge Rate (-15%)"}},
                    {"Vaux-C3 Module", {"Extraction Rate (+25%)", "Optimal Charge Rate (-5%)"}},
                    {"XTR Module", {"Extraction Rate (-15%)", "Optimal Charge Window (+15%)", "Inert Material Filtering (+5%)"}},
                    {"XTR-L Module",
                     {"Extraction Rate (-10%)", "Optimal Charge Window (+22%)", "Inert Material Filtering (+5.75%)"}},
                    {"XTR-XL Module",
                     {"Extraction Rate (-5%)", "Optimal Charge Window (+25%)", "Inert Material Filtering (+6%)"}},
                };
                auto upper = [](std::string s) {
                    for (char& ch : s) ch = (char)std::toupper((unsigned char)ch);
                    return s;
                };
                auto named = [](const SCMiner::MiningModule* m) { return std::string(m ? m->name : "-"); };
                std::string whole, cut, runTogether;
                int known = 0;
                for (const Block& b : blocks) {
                    bool inData = false;
                    for (int i = 0; i < SCMiner::MINING_MODULE_COUNT; i++) {
                        if (std::string(SCMiner::MINING_MODULES[i].name) == b.name) inData = true;
                    }
                    if (inData) known++;
                    const std::string name = upper(b.name);
                    const std::string got = named(SCMiner::moduleByHudBlock(name, b.lines));
                    if (got != b.name) whole += " " + name + " -> " + got + ";";
                    std::vector<std::string> clipped;
                    for (const std::string& line : b.lines) clipped.push_back(line.substr(0, 25));
                    const std::string gotCut = named(SCMiner::moduleByHudBlock(name, clipped));
                    if (gotCut != b.name) cut += " " + name + " -> " + gotCut + ";";
                    // III and II run together into I
                    std::string together = name;
                    for (const char* numeral : {" III ", " II "}) {
                        const size_t at = together.find(numeral);
                        if (at != std::string::npos) together.replace(at, std::strlen(numeral), " I ");
                    }
                    for (const auto& lines : {b.lines, clipped}) {
                        const std::string gotTogether = named(SCMiner::moduleByHudBlock(together, lines));
                        if (gotTogether != b.name && gotTogether != "-") {
                            runTogether += " " + together + " -> " + gotTogether + ";";
                        }
                    }
                }
                allPassed &= check(known == SCMiner::MINING_MODULE_COUNT && (int)blocks.size() == known,
                                   "the HUD's blocks here are every module there is (" + std::to_string(known) + " of " +
                                       std::to_string(SCMiner::MINING_MODULE_COUNT) + ")");
                allPassed &= check(whole.empty(), "every module of 4.10 by its block on the HUD, crafted ones too" +
                                                      (whole.empty() ? "" : " (got" + whole + ")"));
                allPassed &= check(cut.empty(), "...cut off where the Prospector's list cuts its lines" +
                                                    (cut.empty() ? "" : " (got" + cut + ")"));
                allPassed &= check(runTogether.empty(), "...and with a numeral run together, that module or none" +
                                                            (runTogether.empty() ? "" : " (got" + runTogether + ")"));
                // An FLTR whose one line lost a digit - the 1 of -15 % - says
                // it is an FLTR-XL; the Head column cannot tell them apart,
                // so it is neither. A Rieger-C3 read as C2 is put right by
                // its figure as before - the Head column checks that one.
                auto nameOf = [](const SCMiner::MiningModule* m) { return std::string(m ? m->name : "-"); };
                const std::string fltrMisread = nameOf(SCMiner::moduleByHudBlock("FLTR MODULE", {"Extraction Rate (-5%)"}));
                const std::string fltrWhole = nameOf(SCMiner::moduleByHudBlock("FLTR MODULE", {"Extraction Rate (-15%)"}));
                const std::string riegerMisread =
                    nameOf(SCMiner::moduleByHudBlock("RIEGER-C2 MODULE", {"Laser Power (+25%)"}));
                allPassed &= check(fltrMisread == "-" && fltrWhole == "FLTR Module" && riegerMisread == "Rieger-C3 Module",
                                   "a name and a figure that say two modules the Head column cannot tell apart are "
                                   "neither (got " + fltrMisread + ", " + fltrWhole + ", " + riegerMisread + ")");
            }
            // The table's Head column checks the list: FRACTURE PWR 119 and
            // WINDOW SIZE 83 are a Helix I with a Focus III and a Rieger-C3,
            // and with the Focus III lost to the rock behind it the list
            // says 125 and 59. The HUD's grey 100 the reader may take for
            // another figure, unsure of it - a Golem's FRACTURE PWR can come
            // back 200 at 0.56.
            {
                auto laserCalled = [](const char* name) -> const SCMiner::MiningLaser& {
                    for (int i = 0; i < SCMiner::MINING_LASER_COUNT; i++) {
                        if (std::string(SCMiner::MINING_LASERS[i].name) == name) return SCMiner::MINING_LASERS[i];
                    }
                    return SCMiner::MINING_LASERS[0];
                };
                auto moduleCalled = [](const char* name) -> const SCMiner::MiningModule* {
                    for (int i = 0; i < SCMiner::MINING_MODULE_COUNT; i++) {
                        if (std::string(SCMiner::MINING_MODULES[i].name) == name) return &SCMiner::MINING_MODULES[i];
                    }
                    return nullptr;
                };
                const SCMiner::MiningLaser& helix = laserCalled("Helix I Mining Laser");
                const SCMiner::MiningLaser& pitman = laserCalled("Pitman Mining Laser");
                const SCMiner::MiningModule* focus3 = moduleCalled("Focus III Module");
                const SCMiner::MiningModule* focus2 = moduleCalled("Focus II Module");
                const SCMiner::MiningModule* rieger3 = moduleCalled("Rieger-C3 Module");
                const SCMiner::MiningModule* surge = moduleCalled("Surge Module");
                const int helixHud[6] = {119, 70, 100, 100, -1, 83};
                const double sure[6] = {0.99, 0.99, 0.99, 0.99, 0, 0.99};
                const bool helixChecks = SCMiner::headColumnAgrees(helix, {focus3, rieger3}, helixHud, sure) &&
                                         SCMiner::headColumnAgrees(helix, {rieger3, focus3}, helixHud, sure) &&
                                         !SCMiner::headColumnAgrees(helix, {rieger3}, helixHud, sure) &&
                                         !SCMiner::headColumnAgrees(helix, {}, helixHud, sure) &&
                                         !SCMiner::headColumnAgrees(helix, {focus2, rieger3}, helixHud, sure);
                const int pitmanHud[6] = {200, 125, 135, 60, -1, 140};
                const double greyUnsure[6] = {0.56, 0.99, 0.99, 0.99, 0, 0.99};
                const bool pitmanChecks = SCMiner::headColumnAgrees(pitman, {}, pitmanHud, greyUnsure) &&
                                          !SCMiner::headColumnAgrees(pitman, {}, pitmanHud, sure);
                // an active module counts while it fires, which the HUD may show
                const int surgeIdle[6] = {100, 70, 100, 100, -1, 60};
                const int surgeFiring[6] = {150, 59, 110, 100, -1, 60};
                const bool surgeChecks = SCMiner::headColumnAgrees(helix, {surge}, surgeIdle, sure) &&
                                         SCMiner::headColumnAgrees(helix, {surge}, surgeFiring, sure) &&
                                         !SCMiner::headColumnAgrees(helix, {}, surgeFiring, sure);
                allPassed &= check(helixChecks && pitmanChecks && surgeChecks,
                                   "the table's Head column agrees with the laser and modules read, and not with a list "
                                   "that lost one; a grey 100 read unsure does not count against it");
            }
            const cv::Mat hud = testPicture("scanpanel/loadout_prospector_1440.jpg", L"SELFTEST_LOADOUT");
            allPassed &= check(!hud.empty(), "the Prospector HUD's picture is beside the build or inside the exe");
            SCMiner::TextLineReader* const reader = panelLines();
            if (!hud.empty() && reader) {
                cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                hud.copyTo(screen(cv::Rect(1772, 590, hud.cols, hud.rows)));
                SCMiner::SeenLoadout seen;
                SCMiner::readSeenLoadout(screen, *reader, seen);
                std::string got = seen.laser ? seen.laser->name : "-";
                for (const SCMiner::MiningModule* m : seen.modules) got += std::string(", ") + m->name;
                std::vector<std::string> names;
                for (const SCMiner::MiningModule* m : seen.modules) names.push_back(m->name);
                std::sort(names.begin(), names.end());
                allPassed &= check(seen.complete() && seen.laser && std::string(seen.laser->name) == "Helix I Mining Laser" &&
                                       names == std::vector<std::string>{"Focus III Module", "Rieger-C3 Module"},
                                   "...and a Prospector's HUD reads as Helix I with Focus III and Rieger-C3 (got " + got + ")");
            }
        }

        // The shape reader, on a captured panel put back where it was on a
        // 5120 x 1440 screen: the labels found as a picture, the figures read
        // glyph by glyph where the labels say they are, the volume
        // right-aligned against SCU.
        {
            const cv::Mat crop = testPicture("scanpanel/panel_prospector_1440.png", L"SELFTEST_PROSPECTOR");
            allPassed &= check(!crop.empty(), "the Prospector's panel picture is beside the build or inside the exe");
            if (!crop.empty()) {
                cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                crop.copyTo(screen(cv::Rect(2977, 578, crop.cols, crop.rows)));
                SCMiner::ScanPanel shape;
                const bool sure = SCMiner::readPanelByShape(screen, shape);
                allPassed &= check(shape.found, "the shape reader finds the panel's labels and places its figures");
                const SCMiner::RockStats& r = shape.stats;
                allPassed &= check(sure && r.mass == 2895.0f && r.resistance == 0.0f && !r.resistanceAdjusted &&
                                       std::abs(r.instability - 3.58f) < 0.005f && std::abs(r.scu - 2.57f) < 0.005f,
                                   "...and reads 2,895 kg, 0 % in white, 3.58 and 2.57 SCU (got " +
                                       std::to_string((int)r.mass) + ", " + std::to_string((int)r.resistance) +
                                       " %, " + std::to_string(r.instability) + ", " + std::to_string(r.scu) + ")");

                // The MOLE's pilot seat writes the labels in a narrow orange
                // face beside white figures: a captured panel, put back where
                // it was on the screen
                const cv::Mat cockpit = testPicture("scanpanel/panel_mole_cockpit_1440.png", L"SELFTEST_MOLE");
                allPassed &= check(!cockpit.empty(),
                                   "the MOLE cockpit's panel picture is beside the build or inside the exe");
                if (!cockpit.empty()) {
                    cv::Mat moleScreen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                    cockpit.copyTo(moleScreen(cv::Rect(2960, 560, cockpit.cols, cockpit.rows)));
                    // a few looks, as the app takes them: the orange ink is
                    // looked in every other look while nothing is found
                    SCMiner::ScanPanel mole;
                    bool moleSure = false;
                    for (int look = 0; look < 4 && !mole.found; look++) {
                        mole = SCMiner::ScanPanel();
                        moleSure = SCMiner::readPanelByShape(moleScreen, mole);
                    }
                    const SCMiner::RockStats& o = mole.stats;
                    allPassed &= check(mole.found && moleSure && o.mass == 25005.0f && o.resistance == 36.0f &&
                                           !o.resistanceAdjusted && std::abs(o.instability - 302.71f) < 0.005f &&
                                           std::abs(o.scu - 32.38f) < 0.005f,
                                       "...and the MOLE cockpit's, orange labels and white figures (got " +
                                           std::to_string((int)o.mass) + ", " + std::to_string((int)o.resistance) +
                                           " %, " + std::to_string(o.instability) + ", " + std::to_string(o.scu) + ")");
                    // The composition lists under those two panels: the
                    // MOLE's Taranite rock part by part, named by the rock's
                    // kind and again by reading the names off a cut-out the
                    // way the app hands it over; and the Prospector's piece
                    // of an Iron rock, one line whose 100.00% runs into its
                    // name.
                    if (SCMiner::TextLineReader* const second = panelLines()) {
                        const std::vector<PartWanted> taranite = {{"Taranite", 9.37f, 957}, {"Taranite", 38.51f, 310},
                                                                  {"", 52.11f, 0}};
                        SCMiner::RockContents holds;
                        SCMiner::readRockContents(moleScreen, mole, "Taranite", *second, holds);
                        allPassed &= check(partsAre(holds, taranite),
                                           "the MOLE's composition list reads as 9.37 % Taranite at 957, 38.51 % at 310 "
                                           "and 52.11 % inert (got " + partsAs(holds) + "; " + holds.note + ")");
                        const cv::Rect part = SCMiner::rockContentsRegion(mole, moleScreen.size());
                        SCMiner::ScanPanel moved = mole;
                        moved.labels -= part.tl();
                        SCMiner::RockContents again;
                        if (part.area() > 0) SCMiner::readRockContents(moleScreen(part).clone(), moved, "", *second, again);
                        allPassed &= check(partsAre(again, taranite),
                                           "...and the same off the cut-out the app reads, the names read too (got " +
                                               partsAs(again) + "; " + again.note + ")");
                        SCMiner::RockContents pieceRead;
                        SCMiner::readRockContents(screen, shape, "Iron", *second, pieceRead);
                        allPassed &= check(partsAre(pieceRead, {{"Iron", 100.0f, 325}}),
                                           "...and a piece of an Iron rock as one line, all of it Iron at 325 (got " +
                                               partsAs(pieceRead) + "; " + pieceRead.note + ")");
                    }
                    // The ore's name as each reader writes it. Tesseract
                    // gives "IRON (ORE)"; the second reader loses the
                    // opening bracket, reads it as a C, or writes the
                    // full-width one its alphabet carries - and some HUDs
                    // shorten the name itself (the Golem: HEPH (RAW)).
                    allPassed &= check(SCMiner::snapOreName("IRON (ORE)") == "Iron" &&
                                           SCMiner::snapOreName("IRON ORE）") == "Iron" &&
                                           SCMiner::snapOreName("IRONCORE)") == "Iron" &&
                                           SCMiner::snapOreName("HEPH (RAW)") == "Hephaestanite",
                                       "the ore's name is snapped however the reader wrote its brackets");
                    allPassed &= check(SCMiner::snapOreName("").empty() && SCMiner::snapOreName("XQZ").empty() &&
                                           SCMiner::snapOreName("TARGET").empty(),
                                       "...and a word that is no mineral is still no name");
                    // The MOLE's TURRET, over a sunlit red rock: its figures
                    // are the largest on the screen and no shapes are cut for
                    // that face, so nothing reads them glyph by glyph. The
                    // second reader does, and what proves three lines a panel
                    // is the labels it reads back with them.
                    const cv::Mat turret =
                        testPicture("scanpanel/panel_turret_stileron.png", L"SELFTEST_TURRET");
                    allPassed &= check(!turret.empty(),
                                       "the MOLE turret's panel picture is beside the build or inside the exe");
                    SCMiner::TextLineReader* const reader = panelLines();
                    allPassed &= check(reader != nullptr, "the second reader loads out of the exe");
                    if (!turret.empty() && reader) {
                        cv::Mat turretScreen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                        turret.copyTo(turretScreen(cv::Rect(3430, 505, turret.cols, turret.rows)));
                        const cv::Rect around =
                            cv::Rect(3400, 480, turret.cols + 60, turret.rows + 60) & cv::Rect(0, 0, 5120, 1440);
                        const std::vector<SCMiner::FigureLines> shapes =
                            SCMiner::figureLineCandidates(turretScreen, around, 3);
                        allPassed &= check(!shapes.empty(),
                                           "three even lines with a label and a figure are found in the turret");
                        SCMiner::ScanPanel tower;
                        const SCMiner::FigureLines* worked = nullptr;
                        for (const SCMiner::FigureLines& f : shapes) {
                            tower = SCMiner::ScanPanel();
                            if (SCMiner::readPanelRows(turretScreen, *reader, f.rows, f.values, tower)) {
                                worked = &f;
                                break;
                            }
                        }
                        const SCMiner::RockStats& t = tower.stats;
                        allPassed &= check(tower.found && t.mass == 173.0f && t.resistance == 56.0f &&
                                               !t.resistanceAdjusted && std::abs(t.instability - 5.81f) < 0.005f &&
                                               t.ore == "Stileron",
                                           "...and the second reader reads 173 kg, 56 % in white, 5.81, Stileron"
                                           " (got " + std::to_string((int)t.mass) + ", " +
                                               std::to_string((int)t.resistance) + " %, " +
                                               std::to_string(t.instability) + ", '" + t.ore + "')");
                        if (worked) {
                            // What the app is handed decides which way a reading
                            // goes: a mass that still carries the shapes' likeness
                            // is sent to the PanelSteadier, which sums the very
                            // reading that failed. A line reading must leave none.
                            auto clearly = [](int digit) {
                                std::array<float, 10> l{};
                                l[digit] = 0.9f;
                                return l;
                            };
                            SCMiner::ScanPanel mixed;
                            mixed.figures[0].text = "17";
                            mixed.figures[0].likeness = {clearly(1), clearly(7)};
                            mixed.figures[1].text = "5";
                            mixed.figures[1].likeness = {clearly(5)};
                            const bool mixedRead =
                                SCMiner::readPanelRows(turretScreen, *reader, worked->rows, worked->values, mixed);
                            allPassed &= check(mixedRead && mixed.figures[0].likeness.empty() &&
                                                   mixed.figures[1].likeness.empty(),
                                               "a line reading leaves no shape likeness behind for the app to steady");
                            // ...a numeral the shapes are sure of is not read over:
                            // this HUD's slashed zero is an 8 to a general reader
                            SCMiner::ScanPanel sure;
                            sure.figures[0].text = "179";
                            sure.figures[0].likeness = {clearly(1), clearly(7), clearly(9)};
                            allPassed &= check(
                                !SCMiner::readPanelRows(turretScreen, *reader, worked->rows, worked->values, sure),
                                "...a numeral the shapes are sure of is not read over by the second reader");
                            // ...one they only led by 0.05 is
                            SCMiner::ScanPanel unsure;
                            unsure.figures[0].text = "179";
                            unsure.figures[0].likeness = {clearly(1), clearly(7), clearly(9)};
                            unsure.figures[0].likeness[2][3] = 0.85f;
                            // (read first: the order C++ works out a call's
                            // arguments in is not fixed, and the message must
                            // not be built before the reading it reports)
                            const bool unsureRead =
                                SCMiner::readPanelRows(turretScreen, *reader, worked->rows, worked->values, unsure);
                            allPassed &= check(unsureRead && unsure.stats.mass == 173.0f,
                                               "...but one they only led by 0.05 is (got " +
                                                   std::to_string((int)unsure.stats.mass) + ")");
                            // ...and a panel whose mass runs off the screen is not
                            // read by line: a cut mass is still a whole number
                            std::array<cv::Rect, 4> atEdge = worked->rows;
                            atEdge[1].width = turretScreen.cols - atEdge[1].x;
                            SCMiner::ScanPanel edge;
                            allPassed &= check(
                                !SCMiner::readPanelRows(turretScreen, *reader, atEdge, worked->values, edge),
                                "...and a panel whose mass runs off the screen is not read by line");
                        }
                    }
                    // ...and the white one again after it
                    SCMiner::ScanPanel back;
                    bool backSure = false;
                    for (int look = 0; look < 4 && !back.found; look++) {
                        back = SCMiner::ScanPanel();
                        backSure = SCMiner::readPanelByShape(screen, back);
                    }
                    allPassed &= check(back.found && backSure && back.stats.mass == 2895.0f,
                                       "...and the white one again after it");
                }

                // ...and the whole look the app takes at a frame, with no
                // Tesseract at all: an engine that never started, which is
                // what every release package runs with. The app must still
                // look then; measuring beside a tessdata folder hides that case.
                {
                    SCMiner::GlyphReader templates;
                    templates.loadTemplates("./glyphs");
                    templates.loadMfdTemplates("./glyphs");
                    SCMiner::OCREngine never;
                    SCMiner::ScanPanel whole;
                    for (int look = 0; look < 4 && !whole.found; look++) {
                        whole = SCMiner::readScanPanel(screen, never, &templates, panelLines());
                    }
                    const SCMiner::RockStats& w = whole.stats;
                    allPassed &= check(whole.found && w.mass == 2895.0f && w.resistance == 0.0f &&
                                           std::abs(w.instability - 3.58f) < 0.005f,
                                       "the app's own look reads the rock with no Tesseract beside the exe (got " +
                                           std::to_string((int)w.mass) + " kg, " + std::to_string((int)w.resistance) +
                                           " %, " + std::to_string(w.instability) + ")");
                }
            }
        }

        // ...and on the panel that writes its labels out (RESISTANCE:,
        // INSTABILITY:, COMPOSITION), with its rows a little wider apart: the
        // older 1080p test picture, a Tin rock
        {
            const cv::Mat frame = testFrame1080();
            allPassed &= check(!frame.empty(), "the long-label panel's frame is beside the build or inside the exe");
            if (!frame.empty()) {
                // a few looks, as the app takes them: the reader tries its inks
                // by turns, and the tests before this one may have left it in
                // another
                SCMiner::ScanPanel shape;
                bool sure = false;
                for (int look = 0; look < 6 && !shape.found; look++) {
                    shape = SCMiner::ScanPanel();
                    sure = SCMiner::readPanelByShape(frame, shape);
                }
                const SCMiner::RockStats& r = shape.stats;
                allPassed &= check(shape.found && shape.note.find(" long") != std::string::npos,
                                   "the shape reader knows the panel with the labels written out");
                allPassed &= check(sure && r.mass == 22593.0f && r.resistance == 0.0f && !r.resistanceAdjusted &&
                                       std::abs(r.instability - 3.52f) < 0.005f && std::abs(r.scu - 30.21f) < 0.005f,
                                   "...and reads 22,593 kg, 0 %, 3.52 and 30.21 SCU off it (got " +
                                       std::to_string((int)r.mass) + ", " + std::to_string((int)r.resistance) +
                                       " %, " + std::to_string(r.instability) + ", " + std::to_string(r.scu) + ")");
                // ...and its composition list, at 1080 lines: two parts of
                // Tin, one of Copper, the rest inert
                if (SCMiner::TextLineReader* const second = panelLines()) {
                    SCMiner::RockContents holds;
                    SCMiner::readRockContents(frame, shape, "Tin", *second, holds);
                    allPassed &= check(partsAre(holds, {{"Tin", 10.99f, 537}, {"Tin", 63.84f, 340}, {"Copper", 8.36f, 652},
                                                        {"", 16.79f, 0}}),
                                       "...and its composition list: Tin at 537 and at 340, Copper at 652 (got " +
                                           partsAs(holds) + "; " + holds.note + ")");
                }
            }
        }

        // The ore's name as Tesseract read it, put right against the minerals
        // the game has - and left out when it is too far off to put right
        allPassed &= check(SCMiner::snapOreName("JCE (RAW)") == "Ice", "JCE (RAW) is Ice");
        allPassed &= check(SCMiner::snapOreName("SAVRILIUM (OFZ)") == "Savrilium", "SAVRILIUM (OFZ) is Savrilium");
        allPassed &= check(SCMiner::snapOreName("ALUMINUM (ORE)") == "Aluminium", "ALUMINUM (ORE) is Aluminium");
        allPassed &= check(SCMiner::snapOreName("RAW ICE") == "Ice" && SCMiner::snapOreName("RAWICE") == "Ice" &&
                               SCMiner::snapOreName("RAW").empty(),
                           "RAW ICE, as the game's own English has it, is Ice");
        allPassed &= check(SCMiner::snapOreName("BGE (RAW)").empty(), "BGE (RAW) is too far from any mineral to be one");
        allPassed &= check(SCMiner::snapOreName("G ORE)").empty(), "a line with no name in it names nothing");

        // Looks at one panel read as one: a numeral too close to call in one
        // look settles once the looks add up, and a look that is sure of other
        // numerals is another rock
        {
            auto look = [](const std::string& mass, int unsureAt, float unsureLead) {
                SCMiner::ScanPanel p;
                p.found = true;
                p.pinned = true;
                p.labelScale = 1.0;
                const std::string texts[3] = {mass, "0%", "0.00"};
                for (int f = 0; f < 3; f++) {
                    p.figures[f].text = texts[f];
                    p.figures[f].placed = true;
                    int g = 0;
                    for (char c : texts[f]) {
                        if (c < '0' || c > '9') continue;
                        std::array<float, 10> like{};
                        like.fill(0.70f);
                        const int d = c - '0';
                        like[d] = 0.95f;
                        // the runner-up: an 8 for a 0 and a 0 for anything else
                        const int rival = d == 0 ? 8 : 0;
                        like[rival] = 0.95f - (f == 0 && g == unsureAt ? unsureLead : 0.2f);
                        p.figures[f].likeness.push_back(like);
                        g++;
                    }
                }
                return p;
            };
            SCMiner::PanelSteadier steadier;
            SCMiner::RockStats rock;
            int looksToSettle = 0;
            for (int i = 1; i <= 6 && looksToSettle == 0; i++) {
                if (steadier.add(look("7148", 3, 0.004f), rock)) looksToSettle = i;
            }
            allPassed &= check(looksToSettle == 4 && rock.mass == 7148.0f,
                               "an 8 only 0.004 ahead of a 0 settles on the fourth look (" +
                                   std::to_string(looksToSettle) + ")");
            const bool other = steadier.add(look("5359", -1, 0.0f), rock);
            allPassed &= check(other && rock.mass == 5359.0f, "...and a sure look at another rock is taken at once");
            // ...also at another size: the HUD sways with the camera, and it
            // came back from a firing run at 0.91 of where it had settled
            SCMiner::ScanPanel swayed = look("1276", -1, 0.0f);
            swayed.labelScale = 0.91;
            const bool resized = steadier.add(swayed, rock);
            allPassed &= check(resized && rock.mass == 1276.0f,
                               "...and so is one at another size, as the HUD comes back from a firing run");
        }

        // Too strong up close: where to stand. A Helix I (minimum 20 %, full
        // power to 15 m, none from 45 m) on a rock that needs 5 % of it holds
        // at 45 % where it puts out a ninth of its power - 41.7 m on the
        // straight line; one that needs 30 % is not too strong at all.
        {
            const SCMiner::MiningLaser* helix = SCMiner::laserById("thcn_helix_s1");
            const SCMiner::DistanceAdvice back = SCMiner::distanceFor(*helix, 0.05f);
            allPassed &= check(back.backOff && std::abs(back.throttle - 45.0f) < 0.01f &&
                                   std::abs(back.metres - (45.0f - (0.05f / 0.45f) * 30.0f)) < 0.01f && !back.tooLittle,
                               "a rock needing 5 % of a Helix I: back off to about " +
                                   std::to_string((int)back.metres) + " m and hold near 45 %");
            allPassed &= check(!SCMiner::distanceFor(*helix, 0.30f).backOff,
                               "...and one needing 30 % needs no distance");
            allPassed &= check(SCMiner::distanceFor(*helix, 0.02f).tooLittle,
                               "...and one needing 2 % is past what a distance can help");
            allPassed &= check(std::abs(SCMiner::rangeFalloff(*helix, SCMiner::distanceForFalloff(*helix, 0.37f)) - 0.37f) < 0.001f,
                               "distanceForFalloff undoes rangeFalloff");
        }

        // The rocks this model has been measured against.
        //
        // The tolerances are what the file's 0.2 costs against readings taken
        // off a moving bar: two to three points, and six on the one read as
        // "about 45 %". They are not slack for its own sake - they are the
        // size of the term that is still missing, which resistanceCurveFactor
        // is the named candidate for. Tightening them would mean fitting the
        // constant to the readings again.
        //
        // Both read off the charge bar standing still, which is where the
        // beam is putting in exactly what the rock takes back. They disagree
        // by six per cent, which is inside how well a moving bar can be read,
        // so the constant sits between them - see BREAK_DECAY_PER_MASS.
        {
            // 6,939 kg: Helix I, Focus III and Rieger-C3, stock. Still at
            // 28-29 %, climbing at 30 %.
            SCMiner::MiningLoadout helixOne;
            helixOne.laser = SCMiner::laserById("thcn_helix_s1");
            helixOne.modules = {SCMiner::moduleById("passive_focus_mk3"),
                                SCMiner::moduleById("passive_rieger_mk3")};
            const float onePower = SCMiner::outputOf(helixOne, 0).atRange;
            allPassed &= check(std::abs(onePower - 4631.0f) < 2.0f,
                               "the Helix I fit reads 4,631 fracture power, as the HUD shows");
            const SCMiner::ThrottleAdvice small =
                SCMiner::throttleFor(6939.0f, helixOne.laser, onePower);
            allPassed &= check(std::abs(small.toHold - 28.5f) <= 3.0f,
                               "the 6,939 kg rock stands still at 28-29 % (model says " +
                                   std::to_string((int)std::lround(small.toHold)) + " %)");

            // ...and two more on the same head, which is what makes the three
            // of them comparable at all
            //
            // Through the resistance they were read at, and one-sided on
            // purpose. The formula asks more than held these two (see
            // resistanceLetsThrough); what must never happen is the other
            // way, a throttle below what the rock took. The white reading is
            // assumed, which is the stricter test: it gives the lower figure.
            const float helixOneRes = SCMiner::outputOf(helixOne, 0).resistance;
            const SCMiner::ThrottleAdvice mid = SCMiner::throttleFor(
                12560.0f, helixOne.laser,
                onePower * SCMiner::resistanceLetsThrough(17.0f, helixOneRes));
            allPassed &= check(mid.toHold >= 55.5f - 3.0f,
                               "the 12,560 kg rock at 17 % held at 55-56 %, and the model asks "
                               "no less (says " +
                                   std::to_string((int)std::lround(mid.toHold)) + " %)");
            const SCMiner::ThrottleAdvice mid2 = SCMiner::throttleFor(
                13628.0f, helixOne.laser,
                onePower * SCMiner::resistanceLetsThrough(13.0f, helixOneRes));
            allPassed &= check(mid2.toHold >= 57.0f - 3.0f,
                               "the 13,628 kg rock at 13 % held at 57 %, and the model asks "
                               "no less (says " +
                                   std::to_string((int)std::lround(mid2.toHold)) + " %)");

            // On the same head, rocks read in green while it fired: frame by
            // frame, the charge bar falls at the first throttle and climbs at
            // the second, so the rock holds in between. The straight 1 - R x M
            // says 107 % of the first - impossible - yet that rock broke.
            struct Held { float mass, green, falls, climbs; const char* what; };
            for (const Held& rock : {Held{13147.0f, 47.0f, 95.4f, 100.0f, "13,147 kg of quantainium at 47 %"},
                                     Held{19930.0f, 12.0f, 92.6f, 99.7f, "19,930 kg of titanium at 12 %"},
                                     Held{15707.0f, 14.0f, 75.0f, 82.9f, "15,707 kg of titanium at 14 %"}}) {
                const float own = SCMiner::rockOwnResistance(rock.green, true, helixOneRes / 100.0f);
                const SCMiner::ThrottleAdvice held = SCMiner::throttleFor(
                    rock.mass, helixOne.laser, onePower * SCMiner::resistanceLetsThrough(own, helixOneRes));
                allPassed &= check(held.possible && held.toHold >= rock.falls - 3.0f && held.toHold <= rock.climbs + 3.0f,
                                   std::string(rock.what) + " fell at " + std::to_string((int)std::lround(rock.falls)) +
                                       " % and climbed at " + std::to_string((int)std::lround(rock.climbs)) +
                                       " % (model says " + std::to_string((int)std::lround(held.toHold)) + " %)");
            }

            // What to reach for when the head on the ship will not do. The
            // suggestions take the rock's resistance through each head's own
            // factor, so the 48,929 kg savrilium is not handed to a stock
            // Helix II. Which modules count at once is read off the game
            // files (see countedModules): passive ones and Surge every copy,
            // the other actives one at a time.
            {
                const SCMiner::MiningModule* surge = SCMiner::moduleById("active_surge");
                const SCMiner::MiningModule* stampede = SCMiner::moduleById("active_stampede");
                const SCMiner::MiningModule* brandt = SCMiner::moduleById("active_brandt");
                const SCMiner::MiningModule* rieger = SCMiner::moduleById("passive_rieger_mk3");
                const SCMiner::MiningModule* deluge = SCMiner::moduleById("passive_deluge");
                auto onHelixTwo = [](std::vector<const SCMiner::MiningModule*> mods) {
                    SCMiner::MiningLoadout fit;
                    fit.laser = SCMiner::laserById("thcn_helix_s2");
                    fit.modules = std::move(mods);
                    return fit;
                };
                allPassed &= check(surge && !surge->interrupts && !surge->interruptible && stampede &&
                                       stampede->interrupts && stampede->interruptible,
                                   "the files say Surge neither cuts off nor is cut off, Stampede both");
                const float threeSurge = SCMiner::outputOf(onHelixTwo({surge, surge, surge}), 0).fracture;
                allPassed &= check(std::abs(threeSurge - 4930.0f * 1.5f * 1.5f * 1.5f) < 1.0f,
                                   "three Surge on one head count three times (" +
                                       std::to_string((int)std::lround(threeSurge)) + ")");
                const float oneStampede = SCMiner::outputOf(onHelixTwo({stampede}), 0).fracture;
                const float twoStampede = SCMiner::outputOf(onHelixTwo({stampede, stampede}), 0).fracture;
                allPassed &= check(std::abs(twoStampede - oneStampede) < 0.5f,
                                   "two Stampede count as one (" + std::to_string((int)std::lround(twoStampede)) +
                                       " against " + std::to_string((int)std::lround(oneStampede)) + ")");
                const SCMiner::LaserOutput pair = SCMiner::outputOf(onHelixTwo({brandt, stampede}), 0);
                allPassed &= check(std::abs(pair.fracture - 4930.0f * 1.35f) < 1.0f && std::abs(pair.resistance - 70.0f) < 0.5f,
                                   "Brandt and Stampede together count as the Stampede alone (" +
                                       std::to_string((int)std::lround(pair.fracture)) + ", resistance " +
                                       std::to_string((int)std::lround(pair.resistance)) + ")");
                const float both = SCMiner::outputOf(onHelixTwo({surge, stampede}), 0).fracture;
                allPassed &= check(std::abs(both - 4930.0f * 1.5f * 1.35f) < 1.0f,
                                   "Surge and Stampede together both count (" + std::to_string((int)std::lround(both)) + ")");
                const float riegers = SCMiner::outputOf(onHelixTwo({rieger, rieger, rieger}), 0).fracture;
                allPassed &= check(std::abs(riegers - 4930.0f * 1.25f * 1.25f * 1.25f) < 1.0f,
                                   "three Rieger-C3 on one head count three times (" +
                                       std::to_string((int)std::lround(riegers)) + ")");
                const float alone = SCMiner::outputOf(onHelixTwo({rieger, surge, stampede}), 0, false).fracture;
                allPassed &= check(std::abs(alone - 4930.0f * 1.25f) < 1.0f,
                                   "with no active module running only the Rieger counts (" +
                                       std::to_string((int)std::lround(alone)) + ")");

                // No suggestion runs two of the actives that cut each other off,
                // on any head, for the hard rocks on record and a spread of made-up ones
                auto twoCutting = [](const SCMiner::LaserSuggestion& p) {
                    int n = 0;
                    for (const SCMiner::MiningModule* m : p.modules) n += SCMiner::cutsOthersOff(*m) ? 1 : 0;
                    return n > 1;
                };
                int asked = 0, doubled = 0;
                for (float mass : {3000.0f, 13147.0f, 25000.0f, 48929.0f, 90000.0f}) {
                    for (float res : {0.0f, 30.0f, 67.0f, 75.0f, 90.0f}) {
                        for (int8_t size : {(int8_t)1, (int8_t)2}) {
                            SCMiner::RockStats r;
                            r.mass = mass;
                            r.resistance = res;
                            for (const SCMiner::LaserSuggestion& p : SCMiner::suggestLasers(r, size)) {
                                asked++;
                                if (twoCutting(p)) doubled++;
                            }
                        }
                    }
                }
                allPassed &= check(asked > 0 && doubled == 0,
                                   "no suggested head runs two actives that cut each other off (" +
                                       std::to_string(doubled) + " of " + std::to_string(asked) + ")");

                SCMiner::RockStats sav;
                sav.mass = 48929.0f;
                sav.resistance = 75.0f;      // its own, as the MOLE's cockpit printed it
                const std::vector<SCMiner::LaserSuggestion> picks = SCMiner::suggestLasers(sav, 2);
                // The 48,929 kg savrilium is a Helix II with three Surge, near
                // 78 %, and the quantainium a Helix I with two, near 41 %.
                auto has = [](const SCMiner::LaserSuggestion& p, const SCMiner::MiningModule* m) {
                    return std::count(p.modules.begin(), p.modules.end(), m);
                };
                const SCMiner::LaserSuggestion* top = picks.empty() ? nullptr : &picks[0];
                const bool threeSurgeHead = top && top->breaks && top->laser == SCMiner::laserById("thcn_helix_s2") &&
                                            has(*top, surge) == 3 && top->throttle > 65.0f && top->throttle < 90.0f;
                allPassed &= check(threeSurgeHead,
                                   "48,929 kg at 75 % is a Helix II with three Surge, near " +
                                       std::to_string((int)std::lround(top ? top->throttle : 0.0f)) + " %");
                SCMiner::RockStats quant;
                quant.mass = 13147.0f;
                quant.resistance = 67.0f;    // 47 % in green through a Helix I
                const std::vector<SCMiner::LaserSuggestion> one = SCMiner::suggestLasers(quant, 1);
                const bool twoSurgeHelix = !one.empty() && one[0].breaks && one[0].laser == helixOne.laser &&
                                           has(one[0], surge) == 2 && one[0].throttle > 30.0f && one[0].throttle < 55.0f;
                allPassed &= check(twoSurgeHelix,
                                   "the quantainium wants a Helix I with two Surge, near " +
                                       std::to_string((int)std::lround(one.empty() ? 0.0f : one[0].throttle)) + " %");
                (void)deluge;
            }

            // ...and whether a gadget is NEEDED or would help, off the balance
            // rather than a threshold: a stock Helix I cannot hold the
            // quantainium and a Sabir makes it hold; with Focus III and
            // Rieger-C3 it holds at 95 % and the Sabir would help; the ice
            // rock needs none to break.
            {
                SCMiner::RockStats quant;
                quant.mass = 13147.0f;
                quant.resistance = 47.0f;
                quant.resistanceAdjusted = true;
                quant.instability = 125.11f;
                SCMiner::MiningLoadout stock;
                stock.laser = helixOne.laser;
                SCMiner::BreakBalance bal;
                bal.needs = quant.mass * SCMiner::BREAK_DECAY_PER_MASS;
                bal.ownResistance = 67.0f;
                bal.powerThrough = [&stock](float own) {
                    const SCMiner::LaserOutput o = SCMiner::outputOf(stock, 0);
                    return o.atRange * SCMiner::resistanceLetsThrough(own, o.resistance);
                };
                const SCMiner::MiningGadget* sabir = SCMiner::gadgetById("shin_sabir");
                const SCMiner::GadgetAdvice needed = SCMiner::advise(quant, stock, &bal);
                allPassed &= check(sabir && needed.gadget == sabir && needed.reason == SCMiner::MiningGoal::Breakable &&
                                       needed.because.find("needed") == 0,
                                   "a stock Helix I cannot hold the quantainium, and a Sabir is " +
                                       (needed.because.empty() ? std::string("?") : needed.because));
                bal.powerThrough = [&helixOne](float own) {
                    const SCMiner::LaserOutput o = SCMiner::outputOf(helixOne, 0);
                    return o.atRange * SCMiner::resistanceLetsThrough(own, o.resistance);
                };
                const SCMiner::GadgetAdvice helps = SCMiner::advise(quant, helixOne, &bal);
                allPassed &= check(helps.gadget == sabir && helps.because.find("would help") != std::string::npos,
                                   "...with Focus III and Rieger-C3 the Sabir is " +
                                       (helps.because.empty() ? std::string("?") : helps.because));
                SCMiner::RockStats ice;
                ice.mass = 7148.0f;
                bal.needs = ice.mass * SCMiner::BREAK_DECAY_PER_MASS;
                bal.ownResistance = 0.0f;
                const SCMiner::GadgetAdvice easy = SCMiner::advise(ice, helixOne, &bal);
                allPassed &= check(easy.reason != SCMiner::MiningGoal::Breakable,
                                   "...and 7,148 kg of ice at 0 % needs no gadget to break");
            }

            // The 22,457 kg rock is deliberately NOT a test. It was read four
            // times and gave 97, 96, 95 and 89 - eight points of spread on one
            // rock, at INST 24.44. A test written against any one of those
            // would be a test against a wave caught at one moment.

            // 31,298 kg: Helix II crafted +28.84 %, three Rieger-C3. Still at
            // about 45 %.
            SCMiner::MiningLoadout helixTwo;
            helixTwo.laser = SCMiner::laserById("thcn_helix_s2");
            helixTwo.craftedPower = 28.84f;
            helixTwo.modules = {SCMiner::moduleById("passive_rieger_mk3"),
                                SCMiner::moduleById("passive_rieger_mk3"),
                                SCMiner::moduleById("passive_rieger_mk3")};
            const float twoPower = SCMiner::outputOf(helixTwo, 0).atRange;
            const SCMiner::ThrottleAdvice big =
                SCMiner::throttleFor(31298.0f, helixTwo.laser, twoPower);
            // The one read with the other head, and the loosest: "about 45 %"
            // off a moving bar, and the only figure that also depends on a
            // crafting bonus being entered right.
            allPassed &= check(std::abs(big.toHold - 45.0f) <= 6.0f,
                               "...and the 31,298 kg rock at about 45 % (model says " +
                                   std::to_string((int)std::lround(big.toHold)) + " %)");

            // The same rock took a Hofstede-S2 crafted +21.72 % with a
            // Rieger-C3 a long time - so the model must call that possible,
            // and must not call it comfortable.
            SCMiner::MiningLoadout hof;
            hof.laser = SCMiner::laserById("shin_hofstede_s2");
            hof.craftedPower = 21.72f;
            hof.modules = {SCMiner::moduleById("passive_rieger_mk3")};
            const SCMiner::ThrottleAdvice slow =
                SCMiner::throttleFor(31298.0f, hof.laser, SCMiner::outputOf(hof, 0).atRange);
            // What the window says, not the flag behind it. At the file's 0.2
            // this head comes out at 101 % - a hair over - and the rock was
            // broken with it. That hair is the whole reason the answer is
            // three-valued rather than two: past BREAK_COMFORTABLE and inside
            // BREAK_EDGE the app says "at the edge, slow going", which is
            // what happened. Demanding possible() here would be demanding a
            // hard line the measurements say is not there.
            allPassed &= check(slow.toHold / 100.0f <= SCMiner::BREAK_EDGE &&
                                   slow.toHold / 100.0f > SCMiner::BREAK_COMFORTABLE,
                               "...and a Hofstede-S2 breaks it at the edge, as it did (model says " +
                                   std::to_string((int)std::lround(slow.toHold)) + " %)");
        }

        // 48,929 kg of savrilium at 75 %: the rock that needs resistance in
        // the model. One Helix II with three Rieger-C3 and +28.84 % crafted -
        // the game says IMPOSSIBLE and the bar does not move at 100 %, where
        // a model without resistance says it holds at 79 %.
        {
            SCMiner::MiningLoadout helix;
            helix.laser = SCMiner::laserById("thcn_helix_s2");
            helix.craftedPower = 28.84f;
            helix.modules = {SCMiner::moduleById("passive_rieger_mk3"),
                             SCMiner::moduleById("passive_rieger_mk3"),
                             SCMiner::moduleById("passive_rieger_mk3")};
            const SCMiner::LaserOutput h = SCMiner::outputOf(helix, 0);
            allPassed &= check(std::abs(h.resistance - 70.0f) < 0.5f,
                               "a Helix II takes a rock's resistance to 70 %, as its HUD line shows");
            const float helixThrough = h.atRange * SCMiner::resistanceLetsThrough(75.0f, h.resistance);
            const SCMiner::ThrottleAdvice alone =
                SCMiner::throttleFor(48929.0f, helix.laser, helixThrough);
            allPassed &= check(!alone.possible,
                               "one Helix II cannot hold 48,929 kg at 75 %, as the game said (model says " +
                                   std::to_string((int)std::lround(alone.toHold)) + " %)");

            // The turret prints the same rock as 52 %, in green
            const float own = SCMiner::rockOwnResistance(52.0f, true, h.resistance / 100.0f);
            allPassed &= check(std::abs(own - 75.0f) < 1.5f,
                               "...and 52 % in green from the turret is that same 75 % rock (reads " +
                                   std::to_string((int)std::lround(own)) + " %)");

            // A laser can push it up as well, and then the panel prints red:
            // the MOLE's own stock Arbor MH2 adds a quarter. Red goes back to
            // the rock's own figure the same way green does.
            SCMiner::MiningLoadout arbor;
            arbor.laser = SCMiner::laserById("grin_arbor_s2");
            const float arborFactor = SCMiner::outputOf(arbor, 0).resistance;
            const float fromRed = SCMiner::rockOwnResistance(93.75f, true, arborFactor / 100.0f);
            allPassed &= check(std::abs(arborFactor - 125.0f) < 0.5f && std::abs(fromRed - 75.0f) < 0.5f,
                               "...and a stock Arbor MH2 raises it to 125 %, so 94 % in red is that "
                               "rock too (reads " + std::to_string((int)std::lround(fromRed)) + " %)");

            // Three people at three heads can
            SCMiner::MiningLoadout hof;
            hof.laser = SCMiner::laserById("shin_hofstede_s2");
            hof.craftedPower = 21.72f;
            hof.modules = {SCMiner::moduleById("passive_focus_mk3"),
                           SCMiner::moduleById("passive_fltr_mk3")};
            const SCMiner::LaserOutput o = SCMiner::outputOf(hof, 0);
            const float crewThrough =
                2 * helixThrough + o.atRange * SCMiner::resistanceLetsThrough(75.0f, o.resistance);
            const SCMiner::ThrottleAdvice crew =
                SCMiner::throttleFor(48929.0f, helix.laser, crewThrough);
            allPassed &= check(crew.possible,
                               "...while all three heads of that MOLE together can (model says " +
                                   std::to_string((int)std::lround(crew.toHold)) + " %)");
        }

        // The quality slider's marks stay reachable: the magnet takes the
        // nearest of the three, and an arrow key stops on one rather than
        // stepping over it
        {
            using G = SCMiner::MinerGUI;
            allPassed &= check(G::magnetQuality(496, 1, 1000, 10) == 500 &&
                                   G::magnetQuality(504, 1, 1000, 10) == 500,
                               "the pointer beside the 500 mark reads as 500");
            allPassed &= check(G::magnetQuality(480, 1, 1000, 10) == 480,
                               "...and twenty away from it is still 480");
            allPassed &= check(G::magnetQuality(6, 1, 1000, 10) == 1 &&
                                   G::magnetQuality(995, 1, 1000, 10) == 1000,
                               "...and the two ends pull as well");
            allPassed &= check(G::magnetQuality(497, 490, 1000, 10) == 500,
                               "a slot that starts at 490 still has its 500");
            allPassed &= check(G::stepQuality(498, 1, 1000, true, false) == 500,
                               "an arrow key stops on 500 instead of stepping to 503");
            allPassed &= check(G::stepQuality(503, 1, 1000, false, false) == 500,
                               "...from the other side too");
            allPassed &= check(G::stepQuality(500, 1, 1000, true, false) == 505 &&
                                   G::stepQuality(500, 1, 1000, false, false) == 495,
                               "...and the press after it carries on past");
            allPassed &= check(G::stepQuality(490, 1, 1000, true, true) == 500,
                               "a big step stops on the mark it would jump over");
            allPassed &= check(G::stepQuality(998, 1, 1000, true, false) == 1000 &&
                                   G::stepQuality(3, 1, 1000, false, false) == 1,
                               "...and neither end can be stepped past");
        }

        // Out in space there are no ROC deposits and no hand minables: a
        // 28,000 can still be salvage x14 there, never a ROC field of 7
        {
            using Ground = SCMiner::SignatureAnalyzer::Ground;
            SCMiner::SignatureAnalyzer space;
            space.setSystemFilter("NYX");
            space.setGround(Ground::Space);
            bool rocInScope = false, salvageInScope = false;
            for (const auto& r : space.analyzeSignature(28000)) {
                if (r.name.find("ROC Deposit") != std::string::npos && r.inSelectedSystem) rocInScope = true;
                if (r.name.find("Salvage") != std::string::npos && r.inSelectedSystem) salvageInScope = true;
            }
            allPassed &= check(!rocInScope && salvageInScope,
                               "in open space a 28,000 is salvage, not a ROC field");
            allPassed &= check(space.scopeName() == "open space", "...and what it is out of is open space");
            SCMiner::SignatureAnalyzer body;
            body.setSystemFilter("NYX");
            body.setGround(Ground::Surface);
            bool rocOnBody = false;
            for (const auto& r : body.analyzeSignature(28000)) {
                if (r.name.find("ROC Deposit") != std::string::npos && r.inSelectedSystem) rocOnBody = true;
            }
            allPassed &= check(rocOnBody, "on a body the same 28,000 can be a ROC field");
        }

        // The analyzer narrowed to a place: an ore the tables list there
        // stays in scope, one they do not is out
        SCMiner::SignatureAnalyzer narrowed;
        narrowed.setSystemFilter("PYRO");
        narrowed.setPlaceFilter("Pyro II (Monox)");
        std::string listed, absent;
        for (const auto& mineral : SCMiner::MINERAL_SIGNATURES) {
            bool at = false;
            for (const auto& row : SCMiner::ORE_LOCATIONS) {
                if (row.family == SCMiner::MiningFamily::Ship && mineral.name == row.mineral &&
                    std::string(row.location) == "Pyro II (Monox)") at = true;
            }
            if (at && listed.empty()) listed = mineral.name;
            if (!at && (mineral.systems & SCMiner::SYS_PYRO) && absent.empty()) absent = mineral.name;
        }
        allPassed &= check(!listed.empty() && !absent.empty() && narrowed.placeFilter() == "Pyro II (Monox)",
                           "the tables list some ores at Monox and not others (" + listed + " / " + absent + ")");
        bool listedIn = false, absentIn = false;
        for (const auto& mineral : SCMiner::MINERAL_SIGNATURES) {
            if (mineral.name != listed && mineral.name != absent) continue;
            for (const auto& r : narrowed.analyzeSignature(mineral.baseSignature * 2)) {
                if (r.name == listed && r.inSelectedSystem) listedIn = true;
                if (r.name == absent && r.inSelectedSystem) absentIn = true;
            }
        }
        allPassed &= check(listedIn && !absentIn, "at Monox only Monox's ores stay in scope");
        narrowed.setPlaceFilter("Nowhere");
        allPassed &= check(narrowed.placeFilter().empty(), "a place the tables do not know is no filter");
        // The 2,000 family by where the ship is
        SCMiner::SignatureAnalyzer grounded;
        auto nameOf = [&](SCMiner::SignatureAnalyzer& a) {
            for (const auto& r : a.analyzeSignature(12000)) {
                if (r.category == SCMiner::SignalCategory::Salvage) return r.name;
            }
            return std::string("?");
        };
        allPassed &= check(nameOf(grounded) == "Salvage", "in space 12,000 is salvage (got " + nameOf(grounded) + ")");
        grounded.setGround(SCMiner::SignatureAnalyzer::Ground::Surface);
        allPassed &= check(nameOf(grounded) == "Harvestable/Salvage",
                           "on a body 12,000 is harvestables as much as salvage (got " + nameOf(grounded) + ")");
        allPassed &= check(SCMiner::GameState::model("Drake Golem Teach") == "Golem" &&
                               SCMiner::GameState::model("ARGO_MOLE") == "MOLE" &&
                               SCMiner::GameState::model("MISC Prospector") == "Prospector" &&
                               SCMiner::GameState::model("Consolidated Outland Nomad") == "Nomad",
                           "the ship shows as its model alone");
        allPassed &= check(placeByLogName("Monox", "").surface == 1 && placeByLogName("Checkmate", "").surface == 0 &&
                               placeByLogName("HUR-L1", "").surface == 0,
                           "a moon is ground under the ship, a station is not");
    }

    // Bedrock's own windows are blacked out of a picture of the screen, and
    // nothing else is: otherwise the overlay over the game is read back as
    // a signature of its own making
    {
        cv::Mat picture(50, 100, CV_8UC3, cv::Scalar::all(255));
        // the picture is of the screen at 1000,500; an overlay over its top
        // right corner, and a window nowhere near it
        SCMiner::ScreenCapture::maskCovered(picture, cv::Rect(1000, 500, 100, 50),
                                            {cv::Rect(1090, 490, 50, 30), cv::Rect(0, 0, 300, 300)});
        const bool covered = picture.at<cv::Vec3b>(0, 90) == cv::Vec3b(0, 0, 0) &&
                             picture.at<cv::Vec3b>(19, 99) == cv::Vec3b(0, 0, 0) &&
                             picture.at<cv::Vec3b>(20, 99) == cv::Vec3b(255, 255, 255) &&
                             picture.at<cv::Vec3b>(0, 89) == cv::Vec3b(255, 255, 255) &&
                             cv::countNonZero(picture.reshape(1)) == (100 * 50 - 10 * 20) * 3;
        allPassed &= check(covered, "what of the picture one of Bedrock's own windows lies over is blacked out, "
                                    "and only that");
        // ...and a window of Bedrock's under the game's is the game where
        // the game's window lies over it: otherwise the full window behind
        // the game on a one-screen machine blacks out the middle of the
        // screen
        using SCMiner::ScreenCapture;
        const cv::Rect game(0, 0, 2560, 1440);
        const auto behind = ScreenCapture::uncovered(cv::Rect(600, 300, 1200, 800), game);
        const auto across = ScreenCapture::uncovered(cv::Rect(2400, 100, 600, 400), game);
        const auto apart = ScreenCapture::uncovered(cv::Rect(2600, 0, 800, 600), game);
        const auto round = ScreenCapture::uncovered(cv::Rect(0, 0, 100, 100), cv::Rect(25, 25, 50, 50));
        int roundArea = 0;
        for (const cv::Rect& part : round) roundArea += part.area();
        cv::Mat ring(100, 100, CV_8UC1, cv::Scalar(255));
        for (const cv::Rect& part : round) ring(part).setTo(0);
        allPassed &= check(behind.empty() && across.size() == 1 && across[0] == cv::Rect(2560, 100, 440, 400) &&
                               apart.size() == 1 && apart[0] == cv::Rect(2600, 0, 800, 600) && roundArea == 7500 &&
                               cv::countNonZero(ring) == 2500,
                           "a window of Bedrock's under the game's is left in the picture where the game's lies "
                           "over it, and only there");
    }

    // The automatic aim box reproduces the measured setup and scales with
    // the viewport height (352x180 centred at 2560,475, the span 53 read
    // markers were measured over) - and the search reaches 250 px around
    // it, short of the cockpit's own figures
    {
        int margin = 0;
        const auto wide = SCMiner::Application::defaultAimBox(cv::Rect(0, 0, 5120, 1440), &margin);
        allPassed &= check(std::abs(wide.x - 2384) <= 6 && std::abs(wide.y - 385) <= 2 &&
                           std::abs(wide.width - 352) <= 2 && std::abs(wide.height - 180) <= 2 && margin == 250,
                           "5120x1440 gives the measured aim box (got " + std::to_string(wide.x) + "," +
                           std::to_string(wide.y) + " " + std::to_string(wide.width) + "x" +
                           std::to_string(wide.height) + ", margin " + std::to_string(margin) + ")");
        const auto hd = SCMiner::Application::defaultAimBox(cv::Rect(2560, 0, 1920, 1080), &margin);
        allPassed &= check(hd.x + hd.width / 2 == 2560 + 960 && std::abs(hd.height - 135) <= 1 &&
                           std::abs(hd.width - 264) <= 1 && margin == 188 && hd.y > 0 && hd.y < 400,
                           "1920x1080 on a second monitor scales the box with the height and keeps it there");
    }

    // Two ores in one rock: the marker shows their sum
    {
        const auto pair = analyzer.decomposePairs(20405);
        allPassed &= check(pair.size() == 1 && pair.front().firstName == "Riccite" &&
                           pair.front().secondName == "Silicon" && pair.front().secondCount == 4,
                           "20405 splits into Riccite x1 + Silicon x4 only");
        allPassed &= check(analyzer.isKnownSignature(20405) && analyzer.isKnownSignature(24660),
                           "20405 and 24660 (Silicon x4/x5 + Riccite) count as signatures");
        const auto rows = analyzer.analyzeSignature(24660);
        allPassed &= check(!rows.empty() && rows.front().combination &&
                           rows.front().name.find("Silicon") != std::string::npos &&
                           rows.front().name.find("Riccite") != std::string::npos,
                           "24660 is listed first as the Silicon + Riccite pair (got " +
                           (rows.empty() ? std::string("nothing") : rows.front().name) + ")");
        // The same ore twice is a cluster, not a pair - and one the sizes rule out
        bool sameOre = false;
        for (const auto& c : analyzer.decomposePairs(12680, 8)) {
            if (c.firstName == c.secondName) sameOre = true;
        }
        allPassed &= check(!sameOre && !analyzer.isKnownSignature(12680),
                           "Quantainium x4 (12680) is not explained as x2 + x2");
        // A plausible value is never left unexplained: 70,000 (read live,
        // in no table) gets constellations, the likeliest first
        {
            const auto stories = analyzer.decomposeAll(70000, 6);
            const auto rows = analyzer.analyzeSignature(70000);
            allPassed &= check(!stories.empty() && stories.front().terms.size() <= 3 &&
                               !rows.empty() && rows.front().combination,
                               "70000 is explained by " + std::to_string(stories.size()) +
                               " constellation(s), shown as rows");
            const auto one = analyzer.decomposeAll(20405, 6);
            allPassed &= check(!one.empty() && one.front().terms.size() == 2,
                               "20405's first constellation is the Riccite + Silicon pair");
        }
        // Repairs aim at single signatures only; sums count as read
        allPassed &= check(!analyzer.isSingleSignature(18960) && analyzer.isSingleSignature(16960) &&
                           !analyzer.isSingleSignature(20405) && analyzer.isKnownSignature(20405),
                           "18960 and 20405 are no single signatures, 16960 is, 20405 is a sum");
    }

    // Collision between the special families: 12000 = ROC x3 = FPS x4 = Salvage x6
    auto collisions = analyzer.analyzeSignature(12000);
    int exactCount = 0;
    bool evenSplit = true;
    for (const auto& r : collisions) {
        if (r.difference < 1.0) {
            exactCount++;
            if (r.probability < 30.0 || r.probability > 37.0) evenSplit = false;
        }
    }
    allPassed &= check(exactCount == 3 && (int)collisions.size() == 3 && evenSplit,
                       "12000 yields exactly ROC x3 / FPS x4 / Salvage x6 at ~33% each (got " +
                       std::to_string(exactCount) + " exact)");

    // One-digit repair: 18960 is a 6->8 misread of Copper x4 (16960)
    int repaired = 0;
    bool didRepair = analyzer.snapToExact(18960, repaired);
    allPassed &= check(didRepair && repaired == 16960,
                       "18960 repairs to 16960 (Copper x4), got " + std::to_string(repaired));

    repaired = 0;
    didRepair = analyzer.snapToExact(18780, repaired);
    allPassed &= check(didRepair && repaired == 16780,
                       "18780 repairs to 16780 (Tin x4), got " + std::to_string(repaired));

    // An already-exact value must never be "repaired" into a neighbor
    repaired = 0;
    allPassed &= check(!analyzer.snapToExact(8480, repaired),
                       "Exact value 8480 is left untouched by the repair pass");

    // An unrepairable value must stay unrepaired rather than snap at random
    repaired = 0;
    allPassed &= check(!analyzer.snapToExact(55555, repaired),
                       "Nonsense value 55555 is not force-fitted to the table");

    // Beyond every family nothing resolves - the analyzer must not invent a
    // cluster for a reading no signature can produce
    auto beyondAll = analyzer.analyzeSignature(270000);
    int beyondExact = 0;
    for (const auto& r : beyondAll) {
        if (r.difference < 1.0) beyondExact++;
    }
    allPassed &= check(beyondExact == 0,
                       "270000 yields no exact match (got " +
                       std::to_string(beyondExact) + ")");

    // No exact match -> fuzzy OCR-misread fallback kicks in
    auto fuzzy = analyzer.analyzeSignature(6377);
    allPassed &= check(!fuzzy.empty() && fuzzy[0].name == "Stileron" &&
                       fuzzy[0].clusterSize == 2 && fuzzy[0].difference >= 1.0,
                       "6377 (no exact value) falls back to closest match Stileron x2");

    // System filter: Stileron does not spawn in Stanton; the exact hit stays
    // visible (dimmed) while in-system near-misses are offered as alternatives
    analyzer.setSystemFilter("STANTON");
    auto filtered = analyzer.analyzeSignature(6370);
    bool stileronDimmed = false;
    bool hasInSystemAlternative = false;
    for (const auto& r : filtered) {
        if (r.name == "Stileron") stileronDimmed = !r.inSelectedSystem;
        if (r.inSelectedSystem) hasInSystemAlternative = true;
    }
    allPassed &= check(stileronDimmed && hasInSystemAlternative,
                       "System filter dims Stileron in Stanton but offers alternatives");
    analyzer.setSystemFilter("");

    // --- OCR ---
    SCMiner::OCRConfig ocrConfig;
    ocrConfig.tessdataPath = "./tessdata";
    ocrConfig.pageSegMode = 8;
    ocrConfig.minConfidence = 20.0;

    SCMiner::OCREngine ocr;
    bool ocrReady = ocr.initialize(ocrConfig);
    std::cout << "  [info] Tesseract "
              << (ocrReady ? "available"
                           : "not available (not in a release build, or no tessdata) - the signatures never use "
                             "it; the rock view reads its figures without it")
              << std::endl;

    if (ocrReady) {
        SCMiner::ScreenCapture capture;
        cv::Mat synthetic = makeSyntheticSignatureImage("6,370");
        auto value = ocr.recognizeBest(capture.preprocessCandidates(synthetic));
        allPassed &= check(value.has_value() && *value == 6370,
                           std::string("OCR pipeline reads bright-on-dark as 6370 (got: ") +
                           (value ? std::to_string(*value) : "nothing") + ")");

        // Orange digits on a bright scanner sky (warm-channel path)
        cv::Mat sky = makeOrangeOnSkyImage("6,370");
        auto valueSky = ocr.recognizeBest(capture.preprocessCandidates(sky));
        allPassed &= check(valueSky.has_value() && *valueSky == 6370,
                           std::string("OCR pipeline reads orange-on-sky as 6370 (got: ") +
                           (valueSky ? std::to_string(*valueSky) : "nothing") + ")");

        // Cold/blue digits on a warm backdrop (e.g. Drake-style UI)
        cv::Mat warmBg(48, 260, CV_8UC3, cv::Scalar(20, 60, 150));      // BGR dark red-brown
        cv::rectangle(warmBg, cv::Rect(6, 14, 18, 22), cv::Scalar(250, 200, 80), cv::FILLED);
        cv::putText(warmBg, "6,370", cv::Point(44, 36), cv::FONT_HERSHEY_SIMPLEX,
                    1.0, cv::Scalar(250, 200, 80), 2, cv::LINE_AA);     // BGR light blue
        auto valueCold = ocr.recognizeBest(capture.preprocessCandidates(warmBg));
        allPassed &= check(valueCold.has_value() && *valueCold == 6370,
                           std::string("OCR pipeline reads blue-on-warm as 6370 (got: ") +
                           (valueCold ? std::to_string(*valueCold) : "nothing") + ")");

        // A flat sky capture (no digits) must fail cleanly, not hallucinate
        cv::Mat flat(27, 83, CV_8UC3, cv::Scalar(235, 190, 25));
        auto valueFlat = ocr.recognizeBest(capture.preprocessCandidates(flat));
        allPassed &= check(!valueFlat.has_value(),
                           "Flat sky capture yields no reading (no phantom digits)");
    }

    // --- a second line of HUD text must not blind the reader ---
    //
    // The marker is rarely alone in its crop. Under the ship's light
    // amplification, flying low over a planet, the compass ribbon sits a
    // few pixels under it and lands in the same box. collectRows() is
    // handed everything a channel found and the row test refuses more than
    // six shapes, so the find is split by baseline; otherwise a crop holding
    // ten shapes is thrown away with four good digits inside it.
    //
    // The fixtures are frames of real LAMP captures, cropped to the
    // rectangle the tracker was holding. Synthetic pictures are no use here:
    // a marker drawn with putText has no plate for the thresholds to be
    // learnt over.
    //
    // LAMP has no colour of its own: every manufacturer tints it
    // differently, and the mining ships cover three - ARGO yellow, Drake
    // amber, MISC blue. Only the yellow one is asserted here.
    //
    // tests/lamp_marker_21350.png is the blue one (a Prospector over a lit
    // ice planet) and does NOT read, at framings from 115x36 to 220x80 or
    // upscaled up to 3x. preprocessCandidates only undoes a colour cast
    // above CAST_THRESHOLD 1.6; the yellow marker sits at 3.23, the blue one
    // at 1.30-1.51. Light amplification over a bright planet makes a weak
    // cast (pale, not saturated), so the correction never engages. The file
    // is kept but not asserted: the gap is understood.
    //
    // mfd_marker_15420 is a Drake radar-screen marker: dark plate, wide
    // face, read against the panel set. canopy_marker_60000 is a real
    // Prospector reading; it reads 80,000 when the panel templates are mixed
    // into the canopy pool. Both guard the plumbing (which set, which bar),
    // not how well the panel set generalises.
    static const struct LampFixture { const char* file; int value; const wchar_t* resource; } LAMP_FIXTURES[] = {
        {"lamp_marker_3170.png", 3170, L"SELFTEST_MARKER_LAMP"},
        {"mfd_marker_15420.png", 15420, L"SELFTEST_MARKER_MFD"},
        {"canopy_marker_60000.png", 60000, L"SELFTEST_MARKER_CANOPY"},
    };
    for (const LampFixture& fx : LAMP_FIXTURES) {
        // Beside the build, or out of the exe: in a hand-out's folder these
        // are what reads the templates the exe carries, not a ./glyphs folder
        const std::string fixture = std::string("tests/") + fx.file;
        const cv::Mat crop = testPicture(fx.file, fx.resource);
        allPassed &= check(!crop.empty(), fixture + " is beside the build or inside the exe");
        if (!crop.empty()) {
            SCMiner::SignatureAnalyzer marker;
            const std::vector<int> table = marker.knownSignatures();
            auto isSignature = [&marker](int v) { return marker.isSingleSignature(v); };
            auto isSum = [&marker](int v) { return marker.isKnownSignature(v); };
            // The crop IS the marker, so it goes to the reader whole - the
            // same call --ocr makes on a saved capture. The locator is not
            // in the way here: it works on a search area, and finds nothing
            // in a picture that is already cut down to one marker.
            SCMiner::GlyphReader lampReader;
            lampReader.loadTemplates("./glyphs");   // falls back to the built-in set
            SCMiner::GlyphReader::Reading reading;
            std::optional<int> value = lampReader.read(crop, &reading, isSignature,
                                                       &table, isSum);
            if (value && !(SCMiner::GlyphReader::goodEnough(reading) &&
                           marker.isKnownSignature(*value))) {
                value.reset();
            }
            allPassed &= check(value.has_value() && *value == fx.value,
                               std::string("LAMP fixture ") + fx.file + " reads " +
                               std::to_string(fx.value) + " (got: " +
                               (value ? std::to_string(*value) : "nothing") + ")");
        }
    }

    // --- Signature table vs. SCMDB ---
    {
        // Transcribed from the SCMDB scan-signature chart, build 4.10.0-live
        // (https://scmdb.net -> Resources -> Scan Signature Identifier).
        // Keeping this list separate from MiningData.h is the point: it fails
        // loudly if the shipped table ever drifts from the published data.
        struct Reference { const char* name; int signature; int maxCluster; };
        static const Reference SCMDB_4_10[] = {
            {"Quantainium", 3170, 2}, {"Stileron", 3185, 2}, {"Savrilium", 3200, 2},
            {"Ouratite", 3370, 3},    {"Riccite", 3385, 3},  {"Lindinium", 3400, 3},
            {"Beryl", 3540, 4},       {"Taranite", 3555, 4}, {"Borase", 3570, 4},
            {"Gold", 3585, 4},        {"Bexalite", 3600, 4},
            {"Laranite", 3825, 5},    {"Aslarite", 3840, 5}, {"Titanium", 3855, 5},
            {"Tungsten", 3870, 5},    {"Agricium", 3885, 5}, {"Torite", 3900, 5},
            {"Hephaestanite", 4180, 6}, {"Tin", 4195, 6},    {"Quartz", 4210, 6},
            {"Corundum", 4225, 6},    {"Copper", 4240, 6},   {"Silicon", 4255, 6},
            {"Iron", 4270, 6},        {"Aluminium", 4285, 6}, {"Ice", 4300, 6},
        };

        int mismatches = 0;
        for (const auto& ref : SCMDB_4_10) {
            const SCMiner::MineralSignature* entry = nullptr;
            for (const auto& m : SCMiner::MINERAL_SIGNATURES) {
                if (m.name == ref.name) { entry = &m; break; }
            }
            if (!entry) {
                std::cout << "    missing mineral: " << ref.name << std::endl;
                mismatches++;
                continue;
            }
            if (entry->baseSignature != ref.signature) {
                std::cout << "    " << ref.name << ": signature " << entry->baseSignature
                          << " but SCMDB says " << ref.signature << std::endl;
                mismatches++;
            }
            if (SCMiner::maxClusterSize(entry->rarity) != ref.maxCluster) {
                std::cout << "    " << ref.name << ": max cluster "
                          << SCMiner::maxClusterSize(entry->rarity)
                          << " but SCMDB says " << ref.maxCluster << std::endl;
                mismatches++;
            }
        }

        const size_t refCount = sizeof(SCMDB_4_10) / sizeof(SCMDB_4_10[0]);
        allPassed &= check(mismatches == 0 &&
                           SCMiner::MINERAL_SIGNATURES.size() == refCount,
                           "All " + std::to_string(refCount) +
                           " mineral signatures match SCMDB 4.10 (" +
                           std::to_string(SCMiner::MINERAL_SIGNATURES.size()) + " shipped, " +
                           std::to_string(mismatches) + " mismatches)");

        // Ground deposits (Strata): a lone deposit, or a field of 6..14 -
        // sizes 2..5 do not occur, which is what makes 32000 a real reading
        auto sizesOf = [](const char* name) {
            for (const auto& s : SCMiner::SPECIAL_SIGNATURES) {
                if (s.name == name) return SCMiner::clusterSizesFor(s);
            }
            return std::vector<int>{};
        };
        const auto roc = sizesOf("ROC Deposit");
        const auto fps = sizesOf("FPS Minable");
        allPassed &= check(roc.front() == 1 && roc.back() >= 21 && fps.back() >= 18,
                           "Ground/FPS node counts reach the field sizes seen in game");

        // 32000 only exists in Strata's ground range - it must resolve, and it
        // must NOT be offered as Savrilium x10: Legendary ores cap at 2 nodes
        auto field8 = analyzer.analyzeSignature(32000);
        bool hasRocEight = false;
        bool hasOversizedShip = false;
        for (const auto& r : field8) {
            if (r.difference < 1.0 && r.name == "ROC Deposit" && r.clusterSize == 8) {
                hasRocEight = true;
            }
            if (r.category == SCMiner::SignalCategory::ShipMining &&
                r.clusterSize > SCMiner::maxClusterSize(r.rarity)) {
                hasOversizedShip = true;
            }
        }
        allPassed &= check(hasRocEight && !hasOversizedShip,
                           "32000 is a vehicle-mining field of 8, never Savrilium x10");

        // No ship cluster anywhere may exceed its rarity cap
        bool capRespected = true;
        for (int value = 3000; value <= 120000; value += 5) {
            for (const auto& r : analyzer.analyzeSignature(value)) {
                if (r.category == SCMiner::SignalCategory::ShipMining &&
                    r.clusterSize > SCMiner::maxClusterSize(r.rarity)) {
                    capRespected = false;
                    break;
                }
            }
            if (!capRespected) break;
        }
        allPassed &= check(capRespected,
                           "Rarity caps hold across the whole signature range");

        std::string n; int c = 0;
        const bool b25800 = analyzer.isExactMatch(25800, n, c);
        const bool b56000 = analyzer.isExactMatch(56000, n, c);
        allPassed &= check(b25800 && b56000,
                           "Published boundaries 25800 (Ice x6) and 56000 (ROC x14) resolve");
        // Values captured in game: large deposit fields must resolve
        const bool b54000 = analyzer.isExactMatch(54000, n, c);
        const bool b84000 = analyzer.isExactMatch(84000, n, c);
        allPassed &= check(b54000 && b84000,
                           "Field readings 54000 and 84000 seen in game resolve");
        const bool b270000 = analyzer.isExactMatch(270000, n, c);
        allPassed &= check(!b270000,
                           "270000 is beyond every family and stays unmatched");

        // Group sizes only one source lists are kept, but flagged
        auto unusual = analyzer.analyzeSignature(8000);
        bool flagged = false;
        for (const auto& r : unusual) {
            if (r.name == "ROC Deposit" && r.clusterSize == 2) flagged = r.unusualCluster;
        }
        allPassed &= check(flagged, "A ground pair (8000) is accepted but flagged unusual");
    }

    // --- Label locator (head tracking) ---
    {
        // The locator verifies its candidates with the reader's pins, which
        // are loaded once per process by whoever loads templates first. The
        // app does that at start-up; this test loads them itself rather than
        // relying on the fixture block above, which loads none when the
        // fixtures are missing.
        SCMiner::GlyphReader pinLoader;
        pinLoader.loadTemplates("./glyphs");
        SCMiner::ScreenCapture capture;

        // A REAL marker, not a drawing: the canopy fixture - a Prospector
        // 60,000 as the game draws it - placed in a large dark scene, to be
        // found again. A drawn stand-in is verified only by whichever pin
        // template happens to resemble it. The marker comes from beside the
        // build or out of the exe, like the fixture readings above.
        cv::Mat scene(900, 2400, CV_8UC3, cv::Scalar(18, 14, 10));
        const cv::Mat marker = testPicture("canopy_marker_60000.png", L"SELFTEST_MARKER_CANOPY");
        cv::Rect placed(1550, 300, 190, 46);
        std::vector<cv::Rect> labels;
        bool found = false;
        if (marker.empty()) {
            // the picture is in the exe, so its absence is a fault
            allPassed &= check(false, "tests/canopy_marker_60000.png is beside the build or inside the exe");
        } else {
            placed = cv::Rect(1550, 300, marker.cols, marker.rows);
            marker.copyTo(scene(placed));
            labels = capture.locateSignatureLabels(scene);
        }
        for (const auto& label : labels) {
            // The hit must sit ON the marker. Its size is not prescribed: the
            // locator hands back what the reader recognised inside the
            // candidate, which is the plate on a normal marker and the bare
            // digit row when the plate has been washed out.
            // How much of the MARKER the label covers - not how much of the label
            // is marker. The box is padded on purpose (a tight box teaches the
            // reader the digits instead of the plate, see SCMINER_PAD_DOWN), so a
            // box that holds the marker with sky around it is the intended result.
            // The box the locator hands back lies on the marker and spans
            // most of its width. Not "covers most of the fixture": the
            // fixture picture carries sky round the plate, and the box is
            // cut to the row with 0.6 h of plate above and below it.
            const double inside = (label & placed).area() / static_cast<double>((std::max)(1, label.area()));
            const cv::Point centre(label.x + label.width / 2, label.y + label.height / 2);
            if (inside > 0.9 && label.width > placed.width * 0.6 && placed.contains(centre)) {
                found = true;
                break;
            }
        }
        std::string where;
        for (const auto& label : labels) {
            where += " " + std::to_string(label.x) + "," + std::to_string(label.y) + " " +
                     std::to_string(label.width) + "x" + std::to_string(label.height);
        }
        allPassed &= check(found,
                           "Locator finds the marker in a 2400x900 scene (" +
                           std::to_string(labels.size()) + " candidates:" + where +
                           ", marker at " + std::to_string(placed.x) + "," +
                           std::to_string(placed.y) + " " + std::to_string(placed.width) +
                           "x" + std::to_string(placed.height) + ")");

        // An empty scene must not produce phantom markers
        cv::Mat empty(900, 2400, CV_8UC3, cv::Scalar(18, 14, 10));
        allPassed &= check(capture.locateSignatureLabels(empty).empty(),
                           "Locator reports nothing on an empty scene");
    }

    // --- Report generation ---
    std::string report = analyzer.generateReport(6370);
    allPassed &= check(report.find("Stileron") != std::string::npos,
                       "Report generation mentions the best match");

    // --- Where-to-find table ---
    {
        const auto bex = SCMiner::locationsFor("Bexalite", SCMiner::MiningFamily::Ship);
        allPassed &= check(!bex.empty() && bex.front().chance >= bex.back().chance,
                           "Location table lists Bexalite, best chance first");
        allPassed &= check(SCMiner::locationsFor("Quantainium", SCMiner::MiningFamily::Ship).size() >= 3,
                           "Quantainium has several known locations");
        allPassed &= check(!SCMiner::bestOfFamily(SCMiner::MiningFamily::ROC).empty(),
                           "ROC deposits have locations");
        allPassed &= check(SCMiner::locationsFor("Nonsense", SCMiner::MiningFamily::Ship).empty(),
                           "Unknown minerals have no locations");
        // Every ship ore of the signature table has at least one place to be found
        int orphans = 0;
        for (const auto& m : SCMiner::MINERAL_SIGNATURES) {
            if (SCMiner::locationsFor(m.name, SCMiner::MiningFamily::Ship).empty()) orphans++;
        }
        allPassed &= check(orphans == 0, "Every ship ore has a known location (" + std::to_string(orphans) + " without)");
    }

    // --- refinery table ---
    // The station bonuses are the one table in the app that is not in the
    // game files, so what can be checked is checked: that every station
    // points at a profile that exists, that the lookup cannot walk off
    // either end, and that a value picked by hand is still where it was.
    {
        int bad = 0;
        for (int s = 0; s < SCMiner::REFINERY_COUNT; s++) {
            const int p = SCMiner::REFINERIES[s].profile;
            if (p < 0 || p >= SCMiner::REFINERY_PROFILE_COUNT) bad++;
        }
        allPassed &= check(bad == 0, "Every refinery points at a profile that exists");
        allPassed &= check(
            SCMiner::refineryYield(-1, 0) == SCMiner::REFINERY_NONE &&
            SCMiner::refineryYield(SCMiner::REFINERY_COUNT, 0) == SCMiner::REFINERY_NONE &&
            SCMiner::refineryYield(0, -1) == SCMiner::REFINERY_NONE &&
            SCMiner::refineryYield(0, SCMiner::REFINERY_MATERIAL_COUNT) == SCMiner::REFINERY_NONE,
            "Refinery lookup answers NONE outside the table");
        int titanium = -1, icarus = -1;
        for (int m = 0; m < SCMiner::REFINERY_MATERIAL_COUNT; m++) {
            if (std::string(SCMiner::REFINERY_MATERIALS[m]) == "Titanium (Ore)") titanium = m;
        }
        for (int s = 0; s < SCMiner::REFINERY_COUNT; s++) {
            if (std::string(SCMiner::REFINERIES[s].name) == "MIC-L5 Modern Icarus Station") icarus = s;
        }
        allPassed &= check(titanium >= 0 && icarus >= 0 &&
                           SCMiner::refineryYield(icarus, titanium) == 13,
                           "MIC-L5 still refines Titanium at +13");
        // Every Pyro station runs one profile - the shape that says these
        // are the game's own numbers rather than somebody's measurements
        int pyroProfile = -1, pyroOdd = 0;
        for (int s = 0; s < SCMiner::REFINERY_COUNT; s++) {
            if (std::string(SCMiner::REFINERIES[s].system) != "Pyro") continue;
            if (pyroProfile < 0) pyroProfile = SCMiner::REFINERIES[s].profile;
            else if (SCMiner::REFINERIES[s].profile != pyroProfile) pyroOdd++;
        }
        allPassed &= check(pyroProfile >= 0 && pyroOdd == 0, "Every Pyro refinery shares one profile");
    }

    // --- refining: the numbers against the game's own words ---
    //
    // The factors come from Regolith Co.'s tables, which nothing here can
    // verify directly. What CAN be verified is that they say the same thing
    // as global.ini, which rates every method in words - and they do, on all
    // three axes for all nine. That agreement is the whole reason these
    // numbers are in the app, so it is asserted rather than remembered.
    {
        // The three tiers the rebalance left intact: yield 1 / 0.85 / 0.70,
        // cost 1 / 2 / 6, and nine time steps from 0.125 to 12. Measured off
        // nine in-game work orders and unchanged from the source tables, which
        // is why those tables are still worth having.
        {
            std::set<int> yields, costs;
            for (int i = 0; i < SCMiner::REFINING_METHOD_COUNT; i++) {
                yields.insert((int)std::lround(SCMiner::REFINING_METHODS[i].yield * 100));
                costs.insert((int)std::lround(SCMiner::REFINING_METHODS[i].cost));
            }
            allPassed &= check(yields == std::set<int>{70, 85, 100} &&
                               costs == std::set<int>{1, 2, 6},
                               "The method tiers are yield 1/0.85/0.70 and cost 1/2/6");
        }

        // The slowest method really is the slowest, and the fastest fastest
        float slowest = 0.0f, fastest = 1e9f;
        const char* slowName = "";
        for (int i = 0; i < SCMiner::REFINING_METHOD_COUNT; i++) {
            const auto& m = SCMiner::REFINING_METHODS[i];
            if (m.time > slowest) { slowest = m.time; slowName = m.name; }
            fastest = (std::min)(fastest, m.time);
        }
        allPassed &= check(std::string(slowName) == "Dinyx Solventation" && slowest == 12.0f &&
                           fastest == 0.125f,
                           "Dinyx is the slowest at x12, XCR the fastest at x0.125");

        // ...and the inverse really is the inverse: what oreNeeded asks for,
        // put through refine(), comes back out as what was wanted
        int titanium = -1, icarus = -1;
        for (int m = 0; m < SCMiner::REFINERY_MATERIAL_COUNT; m++) {
            if (std::string(SCMiner::REFINERY_MATERIALS[m]) == "Titanium (Ore)") titanium = m;
        }
        for (int s = 0; s < SCMiner::REFINERY_COUNT; s++) {
            if (std::string(SCMiner::REFINERIES[s].name) == "MIC-L5 Modern Icarus Station") icarus = s;
        }
        bool roundTrips = titanium >= 0 && icarus >= 0;
        if (roundTrips) {
            for (int i = 0; i < SCMiner::REFINING_METHOD_COUNT && roundTrips; i++) {
                const float want = 250.0f;   // cSCU of refined titanium
                const float raw = SCMiner::oreNeeded(want, icarus, titanium, i);
                const float back = SCMiner::refine(raw, icarus, titanium, i).yield;
                if (std::abs(back - want) > 0.01f) roundTrips = false;
            }
        }
        allPassed &= check(roundTrips,
                           "oreNeeded is the exact inverse of the yield, for every method");

        // ---- a real work order, reproduced to the unit ----
        //
        // Nine screenshots of the refinery terminal at Levski: the same load of
        // aluminium in three qualities, run through every method in turn. This
        // is the only check of the refining arithmetic against the game rather
        // than against another table.
        int levski = -1, aluminium = -1;
        for (int s = 0; s < SCMiner::REFINERY_COUNT; s++) {
            if (std::string(SCMiner::REFINERIES[s].name) == "Levski") levski = s;
        }
        for (int m = 0; m < SCMiner::REFINERY_MATERIAL_COUNT; m++) {
            if (std::string(SCMiner::REFINERY_MATERIALS[m]) == "Aluminum (Ore)") aluminium = m;
        }
        const int loaded[3] = {2263, 996, 426};        // quality 614, 783, 896
        struct Order { const char* method; int got[3]; };
        const Order seen[] = {
            {"DINYX_SOLVENTATION",     {1100, 484, 207}},
            {"FERRON_EXCHANGE",        {1100, 484, 207}},
            {"PYROMETRIC_CHROMALYSIS", {1100, 484, 207}},
            {"THERMONATIC_DEPOSITION", { 935, 411, 176}},
            {"ELECTROSTAROLYSIS",      { 935, 411, 176}},
            {"GASKIN_PROCESS",         { 935, 411, 176}},
            {"KAZEN_WINNOWING",        { 770, 339, 144}},
            {"CORMACK",                { 770, 339, 144}},
            {"XCR_REACTION",           { 770, 339, 144}},
        };
        int off = 0, checked = 0;
        if (levski >= 0 && aluminium >= 0) {
            for (const auto& order : seen) {
                int method = -1;
                for (int i = 0; i < SCMiner::REFINING_METHOD_COUNT; i++) {
                    if (std::string(SCMiner::REFINING_METHODS[i].key) == order.method) method = i;
                }
                if (method < 0) { off++; continue; }
                for (int q = 0; q < 3; q++) {
                    const float got =
                        SCMiner::refine((float)loaded[q], levski, aluminium, method).yield;
                    // the terminal shows whole units; a hair either way is fine
                    if (std::abs(got - (float)order.got[q]) > 1.0f) {
                        off++;
                        std::cout << "        " << order.method << " q" << loaded[q]
                                  << ": " << (int)std::lround(got) << " against " << order.got[q]
                                  << " observed" << std::endl;
                    }
                    checked++;
                }
            }
        }
        allPassed &= check(levski >= 0 && aluminium >= 0 && checked == 27 && off == 0,
                           "A real Levski work order comes back exactly, all 27 figures");

        // ...and its bill and its clock, from the same screenshots
        if (levski >= 0 && aluminium >= 0) {
            const float whole = 2263.0f + 996.0f + 426.0f;
            int dinyx = -1, electro = -1;
            for (int i = 0; i < SCMiner::REFINING_METHOD_COUNT; i++) {
                const std::string k = SCMiner::REFINING_METHODS[i].key;
                if (k == "DINYX_SOLVENTATION") dinyx = i;
                if (k == "ELECTROSTAROLYSIS") electro = i;
            }
            const float bill = SCMiner::refine(whole, levski, aluminium, dinyx).cost;
            const float wait = SCMiner::refine(whole, levski, aluminium, electro).seconds;
            allPassed &= check(std::abs(bill - 195.0f) < 1.0f && std::abs(wait - 2751.0f) < 5.0f,
                               "...and its bill (" + std::to_string((int)std::lround(bill)) +
                               " aUEC, seen 195) and its clock (" +
                               std::to_string((int)std::lround(wait / 60.0f)) +
                               " min, seen 46)");
        }
    }

#ifdef _WIN32
    // --- HDR tone curve ---
    // Checked here because on a machine that never runs HDR the code that uses
    // it never executes, and a mistake would only show up on somebody else's
    // display. The curve turns a half-float scRGB value into a display byte.
    {
        const auto curve = SCMiner::DxgiCapture::makeToneCurve();
        auto atValue = [&curve](float v) {
            cv::Mat f(1, 1, CV_32F, &v);
            cv::Mat h;
            f.convertTo(h, CV_16F);
            return static_cast<int>(curve[h.ptr<unsigned short>(0)[0]]);
        };
        allPassed &= check(curve.size() == 65536, "Tone curve covers every half-float pattern");
        allPassed &= check(atValue(0.0f) == 0, "Tone curve maps black to 0");
        allPassed &= check(atValue(-2.0f) == 0, "Tone curve clamps wide-gamut negatives");

        // Measured with --capture on this game in HDR: the world sits around
        // scRGB 0.25, the marker plate between them, the digits at 4.6 and up.
        const int world = atValue(0.25f);
        const int plate = atValue(1.5f);
        const int digits = atValue(4.6f);

        allPassed &= check(digits >= 235, "HUD light lands near white (" + std::to_string(digits) + ")");
        allPassed &= check(world > 20 && world < plate,
                           "The world stays visible below the HUD (" + std::to_string(world) + ")");
        // The check that matters: the reader needs a real gap between the
        // digits and the plate they sit on; below forty levels nothing is
        // ever read.
        allPassed &= check(digits - plate >= 60,
                           "Digits stand clear of the plate (" + std::to_string(plate) + " -> " +
                               std::to_string(digits) + ")");
        // A sun brighter than the HUD must not collapse onto the same value
        const int sun = atValue(12.0f);
        allPassed &= check(sun > digits && sun <= 255,
                           "Above the HUD level highlights still separate (" + std::to_string(digits) +
                               " < " + std::to_string(sun) + ")");
        bool monotonic = true;
        for (float v = 0.0f; v < 8.0f; v += 0.05f) {
            if (atValue(v) > atValue(v + 0.05f)) { monotonic = false; break; }
        }
        allPassed &= check(monotonic, "Tone curve never darkens as light increases");
    }
#endif  // _WIN32: the HDR path is Desktop Duplication, which has no Linux side

    std::cout << "\n=== Self-test " << (allPassed ? "PASSED" : "FAILED") << " ===\n" << std::endl;
    return allPassed ? 0 : 1;
}
