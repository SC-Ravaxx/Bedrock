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
#include "ColourScheme.h"
#include "CraftingData.h"
#include "CraftQuality.h"
#include "DataBackup.h"
#include "DevTools.h"
#include "EmbeddedData.h"
#include "GamePlaces.h"
#include "GameFiles.h"
#include "GameTextMarks.h"
#include "GameTexts.h"
#include "LaserRange.h"
#include "ToneMap.h"
#include "LoadoutReader.h"
#include "MiningHardware.h"
#include "P4kReader.h"
#include "GameState.h"
#include "MiningLocations.h"
#include "NoteBook.h"
#include "OCREngine.h"
#include "PanelShapes.h"
#include "PhoneServer.h"
#include "Plugins.h"
#include "PluginHost.h"
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
#include "CommodityTerminal.h"
#include "Datarunner.h"
#include "UexPrices.h"
#include "WhereToBuy.h"
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
                               ContractScan::bareTitle("Crew Hasn't Checked In [60/100 Rep]*") == "Crew Hasn't Checked In" &&
                               ContractScan::bareTitle("Master Rank - Direct Grade B Cargo Haul \xC2\xB7 Lorville > Area18 "
                                                       "<EM4>[BP]</EM4>") == "Master Rank - Direct Grade B Cargo Haul" &&
                               ContractScan::bareTitle("Red Wind Seeking New Haulers \xC2\xB7 to Area18") ==
                                   "Red Wind Seeking New Haulers",
                           "a contract's title comes off the log without Bedrock's marks, the route of a haul and the reputation");
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

        // The contract the objective marker names: Disable Outlaw Stronghold
        // at ... pays out of one pool in Pyro I, Monox and Bloom and out of
        // another in Pyro IV, Pyro V and Terminus. By the title nothing is
        // sure, by the contract its whole pool is.
        auto marker = [](const char* mission, const char* contract) {
            return std::string("[Notice] <CLocalMissionPhaseMarker::CreateMarker> Creating objective marker: missionId [") +
                   mission + "], generator name [CitizensForProsperity_DestroyItems], contract [" + contract +
                   "], contractDefinitionId[8b18d982-1e25-41a5-b159-e7e4ea5c9620], objectiveId "
                   "[2a2bd487-7482-5841-9a54-d616df63de71], markerEntityId [85661]";
        };
        auto completeOf = [](const char* title, const char* mission) {
            return std::string("[Notice] <SHUDEvent_OnNotification> Added notification \"Contract Complete: ") + title +
                   ": \" [235] to queue. New queue size: 1, MissionId: [" + mission + "], ObjectiveId: []";
        };
        auto pays = [](const SCMiner::ContractPayout* p, int row) {
            if (!p || row < 0) return false;
            const std::string rows = std::string(",") + p->rows + ",";
            return rows.find("," + std::to_string(row) + ",") != std::string::npos;
        };
        const char* regionA = "CFP_Pyro_RegionA_Rank0_VE_1AsteroidBase_Criminals_Sabotage";
        const auto* monox = ContractScan::payoutForContract(std::string("CitizensForProsperity_DestroyItems/") + regionA);
        const auto* terminus = ContractScan::payoutForContract(
            "CitizensForProsperity_DestroyItems/CFP_Pyro_RegionC_Rank0_VE_1AsteroidBase_Criminals_Sabotage");
        const int legs = rowOf("ADP-mk4 Legs Woodland"), shotgun = rowOf("BR-2 Shotgun"), pab = rowOf("PAB-1 Arms");
        allPassed &= check(ContractScan::payoutFor("Disable Outlaw Stronghold at Asteroid Base") == nullptr &&
                               pays(monox, legs) && !pays(monox, pab) && pays(terminus, pab) && !pays(terminus, legs) &&
                               ContractScan::payoutForContract("CitizensForProsperity_DestroyItems/nothing") == nullptr,
                           "Disable Outlaw Stronghold at ... is sure by the contract, not by the title: one pool in "
                           "Pyro I, Monox and Bloom, another in Pyro IV, Pyro V and Terminus");

        BlueprintLibrary named, unnamed;
        ContractScan s4, s5;
        const char* mission = "8243b854-f258-4aef-8d1f-3cc19c43a756";
        const char* title = "Disable Outlaw Stronghold at Asteroid Base <EM4>[BP]</EM4>";
        named.feedLine(at("14:00:00") + v410, s4);
        named.feedLine(at("14:47:50") + received("BR-2 Shotgun"), s4);
        named.feedLine(at("14:52:47") + marker(mission, regionA), s4);
        named.feedLine(at("15:00:43") + completeOf(title, mission), s4);
        named.feedLine(at("15:01:30") + later, s4);
        unnamed.feedLine(at("14:00:00") + v410, s5);
        unnamed.feedLine(at("14:47:50") + received("BR-2 Shotgun"), s5);
        unnamed.feedLine(at("15:00:43") + completeOf(title, mission), s5);
        unnamed.feedLine(at("15:01:30") + later, s5);
        allPassed &= check(legs >= 0 && named.owned(legs) &&
                               named.sourceOf(legs) == "assumed: Disable Outlaw Stronghold at Asteroid Base" &&
                               named.sourceOf(shotgun) == "log" && pab >= 0 && !named.owned(pab) &&
                               named.assumedCount() == 13 && unnamed.assumedCount() == 0,
                           "...completed in Monox, the marker in the log, no blueprint: the 13 of its pool not "
                           "received are assumed owned, none of the other region's; without the marker none (" +
                               std::to_string(named.assumedCount()) + ", " + std::to_string(unnamed.assumedCount()) + ")");
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

        // A shared contract outlasts the game: after a restart the next log
        // says "Contract Accepted" for it again, without the share. Read in
        // order, or the later log first, it counts for nothing.
        {
            const std::string envLine = "[Trace] @env_session:  'pub-sc-alpha-4100-12519617'";
            auto withShare = [&](BlueprintLibrary& into, ContractScan& s) {
                into.feedLine(at("10:00:00") + envLine, s);
                into.feedLine(at("10:02:00") + "[Notice] <MissionShared> Received share push message: ownerId[1] - "
                                               "missionId[" + shared + "] [Team_GameServices][Missions]",
                              s);
                into.feedLine(at("10:02:01") + marker(shared), s);
                into.feedLine(at("10:02:02") + note("Accepted", shared), s);
                into.finishFile(s);
            };
            auto namedAgain = [&](BlueprintLibrary& into, ContractScan& s, bool finish) {
                into.feedLine(at("11:00:00") + envLine, s);
                into.feedLine(at("11:01:00") + marker(shared), s);
                into.feedLine(at("11:01:00") + note("Accepted", shared), s);
                if (!finish) return;
                into.feedLine(at("11:30:00") + note("Complete", shared), s);
                into.finishFile(s);
            };
            auto counted = [&](const BlueprintLibrary& of) {
                int n = 0;
                for (const auto& [k, e] : of.contracts()) {
                    (void)k;
                    n += e.mission == shared;
                }
                return n;
            };
            ContractScan one, two;
            BlueprintLibrary inOrder;
            withShare(inOrder, one);
            namedAgain(inOrder, two, false);
            const bool runsShared = two.running().size() == 1 && two.running()[0].shared;
            two.reset();
            namedAgain(inOrder, two, true);
            allPassed &= check(counted(inOrder) == 0 && runsShared,
                               "a shared contract the next log takes again is still not the player's own, and runs "
                               "as shared (" + std::to_string(counted(inOrder)) + " counted)");

            ContractScan three, four;
            BlueprintLibrary laterFirst;
            namedAgain(laterFirst, three, true);
            const int before = counted(laterFirst);
            withShare(laterFirst, four);
            allPassed &= check(before == 2 && counted(laterFirst) == 0,
                               "the later log read first: its share, found after, takes the contract back (" +
                                   std::to_string(before) + " -> " + std::to_string(counted(laterFirst)) + ")");

            BlueprintLibrary kept;
            std::istringstream archive(std::string("accepted\t2026-10-01T11:01:00.000\t") + shared + "\t" + key +
                                       "\t41000\ncompleted\t2026-10-01T11:30:00.000\t" + shared + "\t" + key +
                                       "\t41000\naccepted\t2026-10-01T10:01:00.000\t" + own + "\t" + key +
                                       "\t41000\nshared\t2026-10-01T10:02:00.000\t" + shared + "\n");
            kept.readRankArchive(archive);
            allPassed &= check(kept.contracts().size() == 1 && kept.contracts().begin()->second.mission == own,
                               "contract_log_backup.txt: a share line below a contract's lines takes them back (" +
                                   std::to_string(kept.contracts().size()) + " kept)");
        }

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
                                   L"553–1000 · Mostly 553 or 644" &&
                               SCMiner::MinerGUI::rockPartQuality(*bulk) == L"Quality 264, rarely 553" &&
                               SCMiner::MinerGUI::rockPartQuality(*borase) == L"Mostly 584 · 359–743",
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
        // ...and the other refined names that are not the material's: what
        // PROCESSING lists for raw ice and raw silicon
        const bool refined = SCMiner::refineryMaterialIn("PRESSURIZED ICE") == "Raw Ice" &&
                             SCMiner::refineryMaterialIn("PRESSURIZED lCE") == "Raw Ice" &&
                             SCMiner::refineryMaterialIn("PRESSURIZEDICE") == "Raw Ice" &&
                             SCMiner::refineryMaterialIn("RAW ICE") == "Raw Ice" &&
                             SCMiner::refineryMaterialIn("SILICON") == "Raw Silicon" &&
                             SCMiner::refineryMaterialIn("RAW SILICON") == "Raw Silicon" &&
                             SCMiner::refineryMaterialIn("PRESSURE") == "";
        allPassed &= check(refined, "the refined names that are not the raw material's: PRESSURIZED ICE for raw ice, "
                                    "SILICON for raw silicon");

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

        // "Cheapest": a capacity alone stands for its step (light under 100 %,
        // moderate under 200, heavy under 300), a level read beats it; the
        // lower surcharge goes first even at more aUEC per SCU, then the fewer
        // aUEC per SCU, without cargo the newer reading; never read goes last
        {
            auto rank = [](int level, double perScu, long long observed) {
                S::PriceRank p;
                p.level = level;
                p.perScu = perScu;
                p.observed = observed;
                return p;
            };
            const bool cheapest =
                S::surchargeLevel(781, 3) == 3 && S::surchargeLevel(31, -1) == 0 && S::surchargeLevel(99, -1) == 0 &&
                S::surchargeLevel(100, -1) == 1 && S::surchargeLevel(245, -1) == 2 &&
                S::surchargeLevel(304, -1) == 3 && S::surchargeLevel(278, 1) == 1 && S::surchargeLevel(-1, 0) == 0 &&
                S::surchargeLevel(-1, -1) == -1 &&
                S::cheaperFirst(rank(0, 520.0, 0), rank(3, 480.0, 0), true) &&
                !S::cheaperFirst(rank(3, 480.0, 0), rank(0, 520.0, 0), true) &&
                S::cheaperFirst(rank(0, 518.0, 0), rank(0, 523.0, 0), true) &&
                !S::cheaperFirst(rank(0, 523.0, 0), rank(0, 518.0, 0), true) &&
                !S::cheaperFirst(rank(1, 518.001, 0), rank(1, 518.0, 0), true) &&
                !S::cheaperFirst(rank(1, 518.0, 0), rank(1, 518.001, 0), true) &&
                S::cheaperFirst(rank(3, 900.0, 0), rank(-1, 100.0, 0), true) &&
                !S::cheaperFirst(rank(-1, 100.0, 0), rank(3, 900.0, 0), true) &&
                S::cheaperFirst(rank(1, 0.0, 2000), rank(1, 0.0, 1000), false) &&
                !S::cheaperFirst(rank(1, 0.0, 1000), rank(1, 0.0, 2000), false) &&
                S::cheaperFirst(rank(0, 0.0, 1000), rank(1, 0.0, 2000), false) &&
                !S::cheaperFirst(rank(-1, 0.0, 0), rank(-1, 0.0, 0), false) &&
                !S::cheaperFirst(rank(2, 1e12, 0), rank(2, 1e12, 0), true);
            allPassed &= check(cheapest, "refinery \"Cheapest\": the lighter workload first, even at more aUEC per "
                                         "SCU refined, then the fewer aUEC per SCU (or the newer reading with nothing "
                                         "ticked), a capacity alone by its step of 100 %, and never read last");
        }

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
        // From an hour up the terminal gives the time left to the nearest
        // minute. Checkmate, 8 November 2026: an order read as number 2,
        // 24 s later as number 1 (the one before it collected) with its time
        // 23 s on - one order, not a second beside it; the other order read
        // then its own
        SCMiner::RefineryOrderBook rounded;
        RefineryPanel quoteBig = setup;
        quoteBig.cost = 133210;
        quoteBig.seconds = 15240;
        quoteBig.rows = {row("Stileron (Ore)", 330, 5916, 2662), row("Stileron (Ore)", 517, 622, 279),
                         row("Stileron (Ore)", 681, 612, 275)};
        RefineryPanel big = running;
        big.order = 2;
        big.rows = {row("Stileron (Ore)", 330, -1, 2662), row("Stileron (Ore)", 517, -1, 279),
                    row("Stileron (Ore)", 681, -1, 275)};
        RefineryPanel quoteSmall = setup;
        quoteSmall.cost = 10474;
        quoteSmall.seconds = 1854;
        quoteSmall.rows = {row("Stileron (Ore)", 747, 558, 253)};
        RefineryPanel small2 = running;
        small2.order = 2;
        small2.rows = {row("Stileron (Ore)", 747, -1, 253)};
        const long long t1 = 1794139300;
        rounded.take("Checkmate", screen({quoteBig}), t1);
        rounded.take("Checkmate", screen({at(big, 15180)}), t1 + 48);
        rounded.take("Checkmate", screen({at(big, 15180)}), t1 + 49);
        rounded.take("Checkmate", screen({quoteSmall}), t1 + 60);
        RefineryPanel bigUp = big;
        bigUp.order = 1;
        rounded.take("Checkmate", screen({at(bigUp, 15180), at(small2, 1844)}), t1 + 72);
        rounded.take("Checkmate", screen({at(bigUp, 15180), at(small2, 1843)}), t1 + 73);
        const auto& roundedOrders = rounded.orders();
        const bool oneEachRounded = roundedOrders.size() == 2 && roundedOrders[0].number == 1 &&
                                    roundedOrders[0].due == t1 + 49 + 15180 && roundedOrders[0].cost == 133210 &&
                                    roundedOrders[1].number == 2 && roundedOrders[1].cost == 10474;
        // ...its quote although the two roundings put it placed 39 s before
        // the quote was last seen (from 15 s it went without)
        SCMiner::RefineryOrderBook early;
        RefineryPanel quoteIron = setup;
        quoteIron.seconds = 7260;
        quoteIron.rows = {row("Iron (Ore)", 874, 484, 235)};
        early.take("Levski", screen({quoteIron}), t1);
        early.take("Levski", screen({at(running, 7200)}), t1 + 20);
        early.take("Levski", screen({at(running, 7200)}), t1 + 21);
        const bool roundedQuote = early.orders().size() == 1 && early.orders()[0].complete() &&
                                  early.orders()[0].placed == t1 - 39;
        // ...two alike set up half a minute apart still two, each with its
        // quote; and a long list scrolled to other parts half a minute on
        // still the one order
        SCMiner::RefineryOrderBook alike;
        RefineryPanel running2Long = running;
        running2Long.order = 2;
        alike.take("Levski", screen({quoteIron}), t1);
        alike.take("Levski", screen({at(running, 7260)}), t1 + 5);
        alike.take("Levski", screen({at(running, 7260)}), t1 + 6);
        alike.take("Levski", screen({quoteIron}), t1 + 30);
        alike.take("Levski", screen({at(running, 7200), at(running2Long, 7260)}), t1 + 35);
        alike.take("Levski", screen({at(running, 7200), at(running2Long, 7260)}), t1 + 36);
        const bool alikeTwo = alike.orders().size() == 2 && alike.orders()[0].number == 1 &&
                              alike.orders()[1].number == 2 && alike.orders()[0].complete() &&
                              alike.orders()[1].complete();
        SCMiner::RefineryOrderBook scrolledLong;
        RefineryPanel topLong = running;
        topLong.order = 3;
        topLong.rows = {row("Quantainium (Raw)", 669, -1, 210), row("Bexalite (Raw)", 756, -1, 88)};
        RefineryPanel bottomLong = topLong;
        bottomLong.rows = {row("Gold (Ore)", 553, -1, 54), row("Copper (Ore)", 652, -1, 135)};
        scrolledLong.take("Checkmate", screen({at(topLong, 7200)}), t1);
        scrolledLong.take("Checkmate", screen({at(topLong, 7200)}), t1 + 1);
        scrolledLong.take("Checkmate", screen({at(bottomLong, 7140)}), t1 + 40);
        scrolledLong.take("Checkmate", screen({at(bottomLong, 7140)}), t1 + 41);
        const bool scrolledOne = scrolledLong.orders().size() == 1 && scrolledLong.orders()[0].parts.size() == 4;
        allPassed &= check(oneEachRounded && roundedQuote && alikeTwo && scrolledOne,
                           "work orders of an hour and more, their time to the minute: one read under a smaller number "
                           "half a minute on still one, scrolled to other parts too; its quote despite the rounding; "
                           "two alike set up one after the other still two");
        allPassed &= check(once && twice && quoted && later && notGarbled && notElsewhere && another && kept,
                           "the work orders' book: an order taken on the second reading that agrees, with the quote "
                           "read before it and what went in; the same order later only kept; a garbled reading, "
                           "another station's nothing; a second order beside it, not whole without a quote; saved "
                           "with its handover to UEX, loaded and one ticked off");
    }

    // --- A work order as a calendar entry for the phone's camera
    //     (refineryOrderEvent, the Refinery window's QR code): the moment it
    //     is done in UTC, a reminder then, small enough to scan ---
    {
        SCMiner::RefineryOrder order;
        order.station = "Checkmate";
        order.number = 1;
        order.parts = {{"Quantainium (Raw)", 612, 312}, {"Gold (Ore)", 553, 41}};
        order.due = 1794055440;   // 7 November 2026, 12:44:00 UTC: 13:44 in Berlin, winter time
        const std::string entry = SCMiner::refineryOrderEvent(order);
        const bool exact = entry == "BEGIN:VEVENT\r\n"
                                    "SUMMARY:Checkmate: Quantainium\\, Gold ready\r\n"
                                    "DTSTART:20261107T124400Z\r\n"
                                    "DTEND:20261107T125900Z\r\n"
                                    "BEGIN:VALARM\r\n"
                                    "TRIGGER:PT0M\r\n"
                                    "ACTION:DISPLAY\r\n"
                                    "DESCRIPTION:Refinery work order\r\n"
                                    "END:VALARM\r\n"
                                    "END:VEVENT";
        // summer time, and a quarter of an hour into the next day, month, year
        SCMiner::RefineryOrder summer = order;
        summer.station = "Levski";
        summer.parts = {{"Iron (Ore)", 874, 235}};
        summer.due = 1785541800;   // 31 July 2026, 23:50 UTC: 1:50 on 1 August in Berlin, summer time
        const std::string july = SCMiner::refineryOrderEvent(summer);
        SCMiner::RefineryOrder newYear = order;
        newYear.due = 1798761570;  // 31 December 2026, 23:59:30 UTC
        const std::string december = SCMiner::refineryOrderEvent(newYear);
        const bool utc =
            july.find("\r\nSUMMARY:Levski: Iron ready\r\nDTSTART:20260731T235000Z\r\nDTEND:20260801T000500Z\r\n") !=
                std::string::npos &&
            december.find("\r\nDTSTART:20261231T235930Z\r\nDTEND:20270101T001430Z\r\n") != std::string::npos;
        // many materials: as many named as fit, the rest counted - for the
        // longest station's name eight long ones, one of them named
        SCMiner::RefineryOrder many = summer;
        many.parts.clear();
        for (const char* m : {"Agricium (Ore)", "Beryl (Raw)", "Gold (Ore)", "Laranite (Raw)", "Taranite (Raw)",
                              "Bexalite (Raw)", "Borase (Ore)"}) {
            many.parts.push_back({m, 500, 100});
        }
        const std::string seven = SCMiner::refineryOrderEvent(many);
        SCMiner::RefineryOrder longest = many;
        longest.station = "MIC-L1 Shallow Frontier Station";
        longest.parts.clear();
        for (const char* m : {"Construction Salvage", "Construction Rubble", "Hephaestanite", "Quantainium (Raw)",
                              "Lindinium (Ore)", "Savrilium (Ore)", "Stileron (Ore)", "Aslarite (Raw)"}) {
            longest.parts.push_back({m, 500, 100});
        }
        const std::string eight = SCMiner::refineryOrderEvent(longest);
        const bool counted =
            seven.find("\r\nSUMMARY:Levski: Agricium\\, Beryl\\, Gold\\, Laranite\\, Taranite +2 ready\r\n") !=
                std::string::npos &&
            eight.find("\r\nSUMMARY:MIC-L1 Shallow Frontier Station: Construction Salvage +7 ready\r\n") !=
                std::string::npos;
        // every line ended by CRLF but the last, none longer than 75 octets
        // (RFC 5545 folds longer ones), no stray CR or LF
        bool lines = true;
        size_t count = 0;
        for (const std::string& e : {entry, july, december, seven, eight}) {
            size_t start = 0;
            while (true) {
                const size_t end = e.find("\r\n", start);
                const std::string line = e.substr(start, end == std::string::npos ? std::string::npos : end - start);
                lines = lines && !line.empty() && line.size() <= 75 && line.find_first_of("\r\n") == std::string::npos;
                count++;
                if (end == std::string::npos) break;
                start = end + 2;
            }
        }
        // ...and a code a camera reads off the screen at ECC MEDIUM: 57
        // modules a side for the example (version 10), 61 at most (11)
        int n = 0, most = 0;
        SCMiner::PhoneServer::qrCode(entry, n);
        for (const std::string& e : {seven, eight}) {
            int side = 0;
            SCMiner::PhoneServer::qrCode(e, side);
            most = (side > 0 && most >= 0) ? (std::max)(most, side) : -1;
        }
        allPassed &= check(exact, "a work order as a calendar entry for the phone: a VEVENT done at its moment, a quarter "
                                  "of an hour long, a reminder at its start");
        allPassed &= check(utc, "...its times in UTC, whatever the clock here: in summer time, and a quarter of an hour "
                                "into the next day, month and year");
        allPassed &= check(counted && lines && count == 50 && eight.size() <= 251 && n == 57 && most > 0 && most <= 61,
                           "...as many materials named as fit, the rest counted; lines in CRLF, none over 75 octets; a "
                           "QR code of " + std::to_string(n) + " modules a side, " + std::to_string(most) +
                               " at most (" + std::to_string(eight.size()) + " bytes)");
    }

    // --- Which work order a Calendar button in the Refinery window is for
    //     (refineryOrderKey): two at one station read under the same number,
    //     as at Checkmate on 8 November 2026, are two; one looked at again -
    //     its number moved up, its time read a minute off, a part more - is
    //     the same ---
    {
        SCMiner::RefineryOrder big;
        big.station = "Checkmate";
        big.number = 2;
        big.placed = 1794139337;
        big.due = 1794154577;
        big.parts = {{"Stileron (Ore)", 330, 2662}, {"Stileron (Ore)", 517, 279}, {"Stileron (Ore)", 681, 275}};
        SCMiner::RefineryOrder quick;
        quick.station = "Checkmate";
        quick.number = 2;
        quick.placed = 1794139363;
        quick.due = 1794141217;
        quick.parts = {{"Stileron (Ore)", 747, 253}};
        SCMiner::RefineryOrder later = big;
        later.number = 1;
        later.due += 60;
        later.parts[0].yield = 2660;
        later.parts.push_back({"Stileron (Ore)", 874, 12});
        // ...and two read without their quote, so with no time placed: told
        // apart by when they are done
        SCMiner::RefineryOrder bare = big, bareToo = big;
        bare.placed = bareToo.placed = 0;
        bareToo.due += 3600;
        const std::string key = SCMiner::refineryOrderKey(big);
        const bool two = key != SCMiner::refineryOrderKey(quick) &&
                         SCMiner::refineryOrderKey(bare) != SCMiner::refineryOrderKey(bareToo);
        const bool same = key == SCMiner::refineryOrderKey(later);
        allPassed &= check(two && same, "which work order a Calendar button is for: two at one station under the same "
                                        "number are two, one looked at again - another number, a minute off, a part "
                                        "more - is the same");
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
        price(778, "Levski", "Pressurized Ice", 4600, 1, 1);             // what ice is sold as
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
                            prices.bestPrice("Ice") == 4600 && prices.bestPrice("Tin") == 0 &&
                            prices.mostValuable({"Gold", "Aluminium", "Quantainium", "Ice"}) == "Quantainium" &&
                            prices.mostValuable({"Tin"}).empty();
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
                           "system, Levski by Delamar; Aluminium is UEX's Aluminum, ice its Pressurized Ice (got " +
                               std::to_string(q.size()) + " for Quantainium" + (why.empty() ? "" : ", " + why) + ")");
    }

    // --- The gear's shops: where places stand (NavPlaces, out of the game's
    //     containers), UEX's shops for the gadgets, laser heads and modules,
    //     and the order they are listed in - the player's system first,
    //     nearest first, the other systems through their gateways ---
    {
        using SCMiner::NavPoint;
        using SCMiner::ShopInOrder;
        using SCMiner::UexShops;
        auto at = [](const std::string& name, const std::string& system, NavPoint& p) {
            return SCMiner::navPoint(name, system, p);
        };
        auto gm = [](const NavPoint& a, const NavPoint& b) { return SCMiner::travelMetres(a, b) / 1e9; };
        auto tenths = [](double v) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.2f", v);
            return std::string(text);
        };
        NavPoint hurston, arccorp, hurL1, arcL1, area18;
        const bool found = at("Hurston", "", hurston) && at("ArcCorp", "Stanton", arccorp) &&
                           at("HUR-L1 Green Glade Station", "", hurL1) && at("arc l1", "Stanton", arcL1) &&
                           at("Area 18", "", area18);
        const bool spaced = found && std::abs(gm(hurston, arccorp) - 22.88) < 0.01 &&
                         std::abs(gm(hurston, hurL1) - 1.29) < 0.01 && std::abs(gm(arccorp, arcL1) - 2.89) < 0.01 &&
                         gm(arccorp, area18) == 0.0;
        allPassed &= check(spaced, "places stand where the game's containers put them: Hurston to ArcCorp 22.88 Gm, "
                                "Hurston to HUR-L1 1.29, ArcCorp to ARC-L1 2.89, Area 18 on ArcCorp (got " +
                                    (found ? tenths(gm(hurston, arccorp)) + ", " + tenths(gm(hurston, hurL1)) + ", " +
                                                 tenths(gm(arccorp, arcL1))
                                           : std::string("a place not found")) +
                                    ")");

        // by the names the log, GamePlaces and UEX give them
        NavPoint p, q;
        const bool names =
            at("Pyro Gateway (Nyx)", "Stanton", p) && p.system == "Nyx" && !at("Stanton Gateway", "", p) &&
            at("Stanton Gateway", "Pyro", p) && p.system == "Pyro" && at("PYR2-L4", "", p) &&
            at("Checkmate Station", "Pyro", q) && p.x == q.x && p.y == q.y && at("Stanton-Pyro Jump Point", "", p) &&
            at("Pyro Gateway", "Stanton", q) && p.x == q.x && p.system == "Stanton" &&
            at("Nyx Gateway (Stanton system)", "", p) && at("Nyx Gateway", "Stanton", q) && p.x == q.x &&
            !at("Magnus Gateway", "Stanton", q) && !at("Castra Gateway", "Nyx", q) &&
            at("MicroTech", "Stanton", p) && at("Delamar", "Nyx", q) && !at("Aaron Halo", "Stanton", p) &&
            !at("Glaciem Ring", "Nyx", p) && !at("", "Stanton", p) && !at("Rustbucket", "Pyro", p) &&
            !at("HUR-L12", "Stanton", p) && !at("ARC-L1x", "Stanton", p);
        std::vector<SCMiner::GamePlace> logged;
        for (const char* name : {"Hurston", "Aberdeen", "Arial", "Ita", "Magda", "Crusader", "Cellin", "Daymar", "Yela",
                                 "ArcCorp", "Lyria", "Wala", "microTech", "Calliope", "Clio", "Euterpe", "Lorville",
                                 "Area18", "Orison", "New Babbage", "Everus Harbor", "Seraphim Station",
                                 "Port Tressler", "Baijini Point", "Grim HEX", "Yela Asteroid Belt", "Pyro I", "Monox",
                                 "Bloom", "Pyro IV", "Pyro V", "Ignis", "Vatra", "Adir", "Fairo", "Fuego", "Vuur",
                                 "Terminus", "Checkmate", "Ruin Station", "Orbituary", "Gaslight", "Patch City",
                                 "Rod's Fuel 'N Supplies", "Dudley & Daughters", "Starlight Service Station",
                                 "Megumi Refueling", "Endgame", "Shepherd's Rest", "Terminus Ring", "Levski",
                                 "Delamar", "People's Service Station Alpha", "PYR2 L4", "PYR6-L5",
                                 "MIC-L5 Modern Icarus Station"}) {
            logged.push_back(SCMiner::placeByLogName(name, ""));
        }
        for (const char* planet : {"HUR", "CRU", "ARC", "MIC"}) {
            for (int n = 1; n <= 5; n++) logged.push_back(SCMiner::placeByLogName(std::string(planet) + "-L" + std::to_string(n), ""));
        }
        for (const char* code : {"RR_JP_StantonPyro", "RR_JP_StantonMagnus", "RR_JP_StantonTerra", "RR_JP_PyroStanton",
                                 "RR_JP_PyroNyx", "RR_JP_NyxPyro", "RR_JP_NyxCastra", "RR_P2_L4", "RR_P6_LEO",
                                 "RR_HUR_LEO", "Nyx_Levski", "Stanton1_Lorville"}) {
            logged.push_back(SCMiner::placeByLocationCode(code, ""));
        }
        logged.push_back(SCMiner::placeByDestination("rs_ext_nyx-pyro_jp1", ""));
        logged.push_back(SCMiner::placeByDestination("levski_all-001", ""));
        std::string unplaced;
        for (const SCMiner::GamePlace& place : logged) {
            if (!place.known() || !at(place.name, place.system, p)) unplaced += (unplaced.empty() ? "" : ", ") + place.name;
        }
        allPassed &= check(names && unplaced.empty(),
                           "a place has its point by every name it goes by: a station by its Lagrange point, a gateway "
                           "by its system in brackets, the jump point it stands at or the log's code for it - by the "
                           "gateways' real names only -, Checkmate by PYR2 L4; a belt, a ring, a gateway of two "
                           "systems without one say none (" +
                               (unplaced.empty() ? std::string("every place the log names placed") : "unplaced: " + unplaced) + ")");

        // UEX's items_prices, as it sends them
        struct Row {
            const char* item;
            double price;
            const char* terminal;
            const char* system;
            const char* planet;
            const char* orbit;
            const char* station;
            const char* city;
            const char* outpost;
            long long modified;
        };
        auto answer = [](const std::vector<Row>& rows) {
            auto text = [](const char* s) {
                if (!s) return std::string("null");
                std::string out = "\"";
                for (const char* c = s; *c; c++) out += (*c == '"' ? std::string("\\\"") : std::string(1, *c));
                return out + "\"";
            };
            std::string json = "{\"status\":\"ok\",\"data\":[";
            for (size_t i = 0; i < rows.size(); i++) {
                const Row& r = rows[i];
                json += std::string(i ? "," : "") + "{\"id_category\":29,\"item_name\":" + text(r.item) +
                        ",\"price_buy\":" + std::to_string((long long)r.price) + ",\"terminal_name\":" + text(r.terminal) +
                        ",\"star_system_name\":" + text(r.system) + ",\"planet_name\":" + text(r.planet) +
                        ",\"orbit_name\":" + text(r.orbit) + ",\"moon_name\":null,\"space_station_name\":" +
                        text(r.station) + ",\"city_name\":" + text(r.city) + ",\"outpost_name\":" + text(r.outpost) +
                        ",\"date_modified\":" + std::to_string(r.modified) + ",\"game_version\":\"4.10.2\"}";
            }
            return json + "]}";
        };
        // every place UEX sold a laser head, a module or a gadget at (8 November 2026)
        const std::vector<Row> everywhere = {
            {"Head", 1, "Refinery Shop - Levski", "Nyx", nullptr, "Delamar", nullptr, "Levski", nullptr, 1},
            {"Head", 1, "Refinery Shop - Pyro Gateway (Nyx)", "Nyx", nullptr, "Pyro Gateway (Nyx system)", "Pyro Gateway (Nyx)", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Stanton Gateway (Nyx)", "Nyx", nullptr, "Stanton Gateway (Nyx system)", "Stanton Gateway (Nyx)", nullptr, nullptr, 1},
            {"Head", 1, "Tools & Supplies - Levski", "Nyx", nullptr, "Delamar", nullptr, "Levski", nullptr, 1},
            {"Head", 1, "Tools & Supplies - People's Service Station Alpha", "Nyx", nullptr, "People's Service Station Alpha", "People's Service Station Alpha", nullptr, nullptr, 1},
            {"Head", 1, "Tools & Supplies - People's Service Station Delta", "Nyx", nullptr, "People's Service Station Delta", "People's Service Station Delta", nullptr, nullptr, 1},
            {"Head", 1, "Tools & Supplies - People's Service Station Lambda", "Nyx", nullptr, "People's Service Station Lambda", "People's Service Station Lambda", nullptr, nullptr, 1},
            {"Head", 1, "Tools & Supplies - People's Service Station Theta", "Nyx", nullptr, "People's Service Station Theta", "People's Service Station Theta", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Checkmate", "Pyro", "Monox", "Monox Lagrange Point 4", "Checkmate Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Nyx Gateway (Pyro)", "Pyro", nullptr, "Nyx Gateway (Pyro system)", "Nyx Gateway (Pyro)", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Orbituary", "Pyro", "Bloom", "Bloom", "Orbituary", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Ruin Station", "Pyro", "Terminus", "Terminus", "Ruin Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Stanton Gateway (Pyro)", "Pyro", nullptr, "Stanton Gateway (Pyro system)", "Stanton Gateway (Pyro)", nullptr, nullptr, 1},
            {"Head", 1, "Shop Terminal - Bueno Ravine", "Pyro", "Bloom", "Bloom", nullptr, nullptr, "Bueno Ravine", 1},
            {"Head", 1, "Shop Terminal - Carver's Ridge", "Pyro", "Bloom", "Bloom", nullptr, nullptr, "Carver's Ridge", 1},
            {"Head", 1, "Shop Terminal - Ostler's Claim", "Pyro", "Monox", "Monox", nullptr, nullptr, "Ostler's Claim", 1},
            {"Head", 1, "Shop Terminal - Stag's Rut", "Pyro", "Pyro I", "Pyro I", nullptr, nullptr, "Stag's Rut", 1},
            {"Head", 1, "Shop Terminal - Yang's Place", "Pyro", "Monox", "Monox", nullptr, nullptr, "Yang's Place", 1},
            {"Head", 1, "Dumper's Depot - Area 18", "Stanton", "ArcCorp", "ArcCorp", nullptr, "Area 18", nullptr, 1},
            {"Head", 1, "Dumper's Depot - GrimHEX", "Stanton", "Crusader", "Crusader", "Green Imperial Housing Exchange", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - ARC-L1", "Stanton", "ArcCorp", "ArcCorp Lagrange Point 1", "ARC-L1 Wide Forest Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - ARC-L2", "Stanton", "ArcCorp", "ArcCorp Lagrange Point 2", "ARC-L2 Lively Pathway Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - ARC-L4", "Stanton", "ArcCorp", "ArcCorp Lagrange Point 4", "ARC-L4 Faint Glen Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - CRU-L1", "Stanton", "Crusader", "Crusader Lagrange Point 1", "CRU-L1 Ambitious Dream Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - HUR-L1", "Stanton", "Hurston", "Hurston Lagrange Point 1", "HUR-L1 Green Glade Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - HUR-L2", "Stanton", "Hurston", "Hurston Lagrange Point 2", "HUR-L2 Faithful Dream Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - MIC-L1", "Stanton", "MicroTech", "microTech Lagrange Point 1", "MIC-L1 Shallow Frontier Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - MIC-L2", "Stanton", "MicroTech", "microTech Lagrange Point 2", "MIC-L2 Long Forest Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - MIC-L5", "Stanton", "MicroTech", "microTech Lagrange Point 5", "MIC-L5 Modern Icarus Station", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Nyx Gateway (Stanton)", "Stanton", nullptr, "Nyx Gateway (Stanton system)", "Nyx Gateway (Stanton)", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Pyro Gateway (Stanton)", "Stanton", nullptr, "Pyro Gateway (Stanton system)", "Pyro Gateway (Stanton)", nullptr, nullptr, 1},
            {"Head", 1, "Refinery Shop - Terra Gateway (Stanton)", "Stanton", nullptr, "Terra Gateway (Stanton system)", "Terra Gateway (Stanton)", nullptr, nullptr, 1},
            {"Head", 1, "Shubin Interstellar - Commons - New Babbage", "Stanton", "MicroTech", "MicroTech", nullptr, "New Babbage", nullptr, 1},
            {"Head", 1, "Tammany and Sons - Metro Center - Lorville", "Stanton", "Hurston", "Hurston", nullptr, "Lorville", nullptr, 1},
        };
        UexShops all;
        std::string why;
        const bool readAll = all.read(answer(everywhere), why);
        std::string lost;
        for (const SCMiner::ShopOffer& o : all.offersFor("Head")) {
            if (!o.located) lost += (lost.empty() ? "" : ", ") + o.where;
        }
        const auto& heads = all.offersFor("Head");
        auto shopAt = [&](const std::string& where, const std::string& system) -> const SCMiner::ShopOffer* {
            for (const SCMiner::ShopOffer& o : heads) {
                if (o.where == where && o.system == system) return &o;
            }
            return nullptr;
        };
        const SCMiner::ShopOffer* lorville = shopAt("Lorville", "Stanton");
        const SCMiner::ShopOffer* gateway = shopAt("Pyro Gateway", "Nyx");
        const SCMiner::ShopOffer* outpost = shopAt("Stag's Rut", "Pyro");
        const bool split = lorville && lorville->shop == "Tammany and Sons - Metro Center" && gateway &&
                           gateway->shop == "Refinery Shop" && outpost && outpost->shop == "Shop Terminal" &&
                           shopAt("ARC-L1", "Stanton") && shopAt("GrimHEX", "Stanton");
        allPassed &= check(readAll && heads.size() == everywhere.size() && lost.empty() && split,
                           "every one of the 34 places UEX sells mining gear at has its point - an outpost by its "
                           "planet -, and a terminal's name gives the shop and the place (" +
                               std::to_string(heads.size()) + " read" + (lost.empty() ? "" : ", no point: " + lost) +
                               (why.empty() ? "" : ", " + why) + ")");

        // ...and in order: a Helix II at eight places, one of them three times
        const std::vector<Row> helix = {
            {"Helix II Mining Laser", 108000, "Refinery Shop - Levski", "Nyx", nullptr, "Delamar", nullptr, "Levski", nullptr, 1788000000},
            {"Helix II Mining Laser", 99000, "Dumper's Depot - Area 18", "Stanton", "ArcCorp", "ArcCorp", nullptr, "Area 18", nullptr, 1780000000},
            {"Helix II Mining Laser", 108000, "Dumper's Depot - Area 18", "Stanton", "ArcCorp", "ArcCorp", nullptr, "Area 18", nullptr, 1788000000},
            {"Helix II Mining Laser", 97000, "Dumper's Depot - Area 18", "Stanton", "ArcCorp", "ArcCorp", nullptr, "Area 18", nullptr, 1770000000},
            {"Helix II Mining Laser", 108000, "Refinery Shop - HUR-L1", "Stanton", "Hurston", "Hurston Lagrange Point 1", "HUR-L1 Green Glade Station", nullptr, nullptr, 1788000000},
            {"Helix II Mining Laser", 108000, "Admin - Everus Harbor", "Stanton", "Hurston", "Hurston", "Everus Harbor", nullptr, nullptr, 1788000000},
            {"Helix II Mining Laser", 102600, "Tammany and Sons - Metro Center - Lorville", "Stanton", "Hurston", "Hurston", nullptr, "Lorville", nullptr, 1788000000},
            {"Helix II Mining Laser", 108000, "Refinery Shop - Stanton Gateway (Pyro)", "Pyro", nullptr, "Stanton Gateway (Pyro system)", "Stanton Gateway (Pyro)", nullptr, nullptr, 1788000000},
            {"Helix II Mining Laser", 108000, "Refinery Shop - Nyx Gateway (Pyro)", "Pyro", nullptr, "Nyx Gateway (Pyro system)", "Nyx Gateway (Pyro)", nullptr, nullptr, 1788000000},
            {"Helix II Mining Laser", 108000, "Refinery Shop - Pyro Gateway (Nyx)", "Nyx", nullptr, "Pyro Gateway (Nyx system)", "Pyro Gateway (Nyx)", nullptr, nullptr, 1788000000},
            {"Helix II Mining Laser", 0, "Refinery Shop - MIC-L1", "Stanton", "MicroTech", "microTech Lagrange Point 1", "MIC-L1 Shallow Frontier Station", nullptr, nullptr, 1788000000},
            {"Rieger-C3 Module", 57000, "Refinery Shop - Levski", "Nyx", nullptr, "Delamar", nullptr, "Levski", nullptr, 1788000000},
        };
        UexShops shops;
        std::string shopsWhy;
        const bool readHelix = shops.read(answer(helix), shopsWhy);
        auto order = [&](const std::string& system, const std::string& place) {
            std::string out;
            for (const ShopInOrder& s : SCMiner::nearestFirst(shops.offersFor("Helix II Mining Laser"), system, place)) {
                out += (out.empty() ? "" : " | ") + s.offer.where + (s.home ? "" : " (" + s.offer.system + ")");
                if (s.metres >= 0) out += " " + tenths(s.metres / 1e9);
            }
            return out;
        };
        const auto atArc = SCMiner::nearestFirst(shops.offersFor("Helix II Mining Laser"), "Stanton",
                                                 "ARC-L1 Wide Forest Station");
        std::string sequence;
        for (const ShopInOrder& s : atArc) sequence += (sequence.empty() ? "" : ", ") + s.offer.where;
        // the way to Levski: to the Nyx Gateway in Stanton, on from the Stanton Gateway in Nyx
        NavPoint nyxOut, nyxIn, levski;
        const bool through = at("Nyx Gateway", "Stanton", nyxOut) && at("Stanton Gateway", "Nyx", nyxIn) &&
                             at("Levski", "Nyx", levski);
        double viaGateways = -1, toLevski = -2;
        if (through) viaGateways = gm(arcL1, nyxOut) + gm(nyxIn, levski);
        for (const ShopInOrder& s : atArc) {
            if (s.offer.where == "Levski") toLevski = s.metres / 1e9;
        }
        const bool nearest = readHelix && atArc.size() == 8 &&
                          sequence == "Area 18, Lorville, Everus Harbor, HUR-L1, Stanton Gateway, Levski, Nyx Gateway, "
                                      "Pyro Gateway" &&
                          atArc[0].offer.price == 108000 && std::abs(atArc[0].metres / 1e9 - 2.89) < 0.01 &&
                          atArc[1].offer.price == 102600 && atArc[3].home && !atArc[4].home && through &&
                          std::abs(toLevski - viaGateways) < 1e-6 && std::abs(toLevski - 104.89) < 0.01;
        allPassed &= check(nearest, "at ARC-L1 the shops come nearest first: Stanton's (Lorville before Everus Harbor at "
                                 "the same Hurston, being cheaper), then Pyro's and Nyx's by the way through their "
                                 "gateways (Levski 104.89 Gm); of one terminal reported twice the newer price; "
                                 "a price of nought left out (got " +
                                     order("Stanton", "ARC-L1 Wide Forest Station") + ")");
        // without a place: the player's system first, then by price - and
        // without even that, by price alone; no distances either way
        const auto inPyro = SCMiner::nearestFirst(shops.offersFor("Helix II Mining Laser"), "Pyro", "");
        const auto nowhere = SCMiner::nearestFirst(shops.offersFor("Helix II Mining Laser"), "", "");
        bool unmeasured = !inPyro.empty() && !nowhere.empty();
        for (const auto* list : {&inPyro, &nowhere}) {
            for (const ShopInOrder& s : *list) unmeasured = unmeasured && s.metres < 0;
        }
        const bool byPrice = unmeasured && inPyro.size() == 8 && inPyro[0].home && inPyro[1].home && !inPyro[2].home &&
                             inPyro[0].offer.where == "Nyx Gateway" && inPyro[2].offer.where == "Lorville" &&
                             nowhere[0].offer.where == "Lorville" && !nowhere[0].home &&
                             order("Nyx", "Levski").rfind("Levski 0.00 | Pyro Gateway 32.36 |", 0) == 0;
        allPassed &= check(byPrice && shops.offersFor("Rieger-C3 Module").size() == 1 &&
                               shops.offersFor("Pitman Mining Laser").empty(),
                           "where the player's place has no point, their system first and then the cheaper; from "
                           "Levski its own gateway at 32.36 Gm; an item UEX lists nowhere has no shop (got " +
                               order("Pyro", "") + " / " + order("Nyx", "Levski") + ")");
        UexShops refused;
        std::string refusedWhy;
        allPassed &= check(!refused.read("{\"status\":\"error\",\"message\":\"x\"}", refusedWhy) && !refusedWhy.empty() &&
                               !refused.read("<html>", refusedWhy) && refused.empty(),
                           "a shop list UEX refused, or no list at all, is no list");

        // UEX's names for the gear (8 November 2026) are Bedrock's: each of
        // Bedrock's but four is one UEX sells under that very name
        const std::vector<std::string> uexNames = {
            "BoreMax", "Okunis", "OptiMax", "Sabir", "Stalwart", "Waveshift",
            "Arbor MH1 Mining Laser", "Arbor MH2 Mining Laser", "Arbor MHV Mining Laser", "Helix I Mining Laser",
            "Helix II Mining Laser", "Hofstede-S1 Mining Laser", "Hofstede-S2 Mining Laser", "Impact I Mining Laser",
            "Impact II Mining Laser", "Klein-S1 Mining Laser", "Klein-S2 Mining Laser", "Lancet MH1 Mining Laser",
            "Lancet MH2 Mining Laser", "Lawson Mining Laser", "S0 Helix", "S00 Hofstede",
            "Brandt Module", "FLTR Module", "FLTR-L Module", "FLTR-XL Module", "Focus II Module", "Focus III Module",
            "Focus Module", "Forel Module", "Lifeline Module", "Optimum Module", "Rieger Module", "Rieger-C2 Module",
            "Rieger-C3 Module", "Rime Module", "Stampede Module", "Surge Module", "Torpid Module", "Torrent II Module",
            "Torrent III Module", "Torrent Module", "Vaux Module", "Vaux-C2 Module", "Vaux-C3 Module", "XTR Module",
            "XTR-L Module", "XTR-XL Module"};
        std::vector<std::string> notSold;
        int named = 0;
        auto sold = [&](const char* name) {
            named++;
            if (std::find(uexNames.begin(), uexNames.end(), name) == uexNames.end()) notSold.push_back(name);
        };
        for (const SCMiner::MiningLaser& l : SCMiner::MINING_LASERS) sold(l.name);
        for (const SCMiner::MiningModule& m : SCMiner::MINING_MODULES) sold(m.name);
        for (const SCMiner::MiningGadget& g : SCMiner::MINING_GADGETS) sold(g.name);
        std::sort(notSold.begin(), notSold.end());
        std::string notSoldText;
        for (const std::string& name : notSold) notSoldText += (notSoldText.empty() ? "" : ", ") + name;
        allPassed &= check(named == 48 &&
                               notSoldText == "Clearcut Module, Deluge Module, Overrun Module, Pitman Mining Laser",
                           "Bedrock names its 48 heads, modules and gadgets as UEX does - all but the four UEX "
                           "lists no shop for (" + std::to_string(named) + " named; not UEX's: " + notSoldText + ")");

        // The three lists as the app keeps them in uex\, read again while
        // young without asking UEX: a gadget's, a head's, a module's - and
        // with one of them a refusal, nothing and why
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path folder = fs::temp_directory_path(ec) / "bedrock-gear-shops-selftest";
            fs::remove_all(folder, ec);
            fs::create_directories(folder, ec);
            auto keep = [&](int category, const std::string& text) {
                const fs::path file = folder / ("items_prices_" + std::to_string(category) + ".json");
                std::ofstream(file, std::ios::binary) << text;
                return fs::file_size(file, ec) == text.size();
            };
            const bool kept =
                keep(28, answer({{"OptiMax", 5650, "Refinery Shop - HUR-L1", "Stanton", "Hurston",
                                  "Hurston Lagrange Point 1", "HUR-L1 Green Glade Station", nullptr, nullptr, 1}})) &&
                keep(29, answer({{"Helix II Mining Laser", 108000, "Dumper's Depot - Area 18", "Stanton", "ArcCorp",
                                  "ArcCorp", nullptr, "Area 18", nullptr, 1}})) &&
                keep(30, answer({{"Rieger-C3 Module", 57000, "Refinery Shop - Levski", "Nyx", nullptr, "Delamar",
                                  nullptr, "Levski", nullptr, 1}}));
            // (only once all three are there: a list missing would be asked for)
            SCMiner::Application::UexShopsOutcome fromKept, refusedOne;
            if (kept) fromKept = SCMiner::Application::loadUexShops(folder);
            if (kept && keep(29, "{\"status\":\"error\",\"message\":\"x\"}")) {
                refusedOne = SCMiner::Application::loadUexShops(folder);
            }
            fs::remove_all(folder, ec);
            const auto gadget = fromKept.shops ? SCMiner::nearestFirst(fromKept.shops->offersFor("OptiMax"), "Stanton", "HUR-L1")
                                           : std::vector<ShopInOrder>();
            allPassed &= check(kept && fromKept.shops && fromKept.note.empty() && fromKept.shops->itemCount() == 3 &&
                                   gadget.size() == 1 && gadget[0].home && gadget[0].metres == 0.0 &&
                                   fromKept.shops->offersFor("Rieger-C3 Module").size() == 1 && !refusedOne.shops &&
                                   !refusedOne.note.empty(),
                               "the gadgets', heads' and modules' shops kept in uex\\ are read as one list without "
                               "asking UEX while young; one of them refused leaves no list, and why (" +
                                   (fromKept.shops ? std::to_string(fromKept.shops->itemCount()) + " items" : "none: " + fromKept.note) +
                                   ", refused: " + (refusedOne.note.empty() ? std::string("no reason") : refusedOne.note) + ")");
        }

        // The Gear page's lines: one shop per place, a gateway named with its
        // system, the nearest places as many as the line holds and then how
        // many more - and without distances the places alone
        {
            auto shop = [](const char* where, const char* system, double gigametresAway, double price) {
                ShopInOrder s;
                s.offer.where = where;
                s.offer.system = system;
                s.offer.price = price;
                s.home = std::string(system) == "Pyro";
                s.metres = gigametresAway < 0 ? -1.0 : gigametresAway * 1e9;
                return s;
            };
            const std::vector<ShopInOrder> lancet = {
                shop("Orbituary", "Pyro", 28.04, 94000),     shop("Stanton Gateway", "Pyro", 47.81, 94000),
                shop("Stanton Gateway", "Pyro", 47.81, 96000), shop("Nyx Gateway", "Pyro", 57.7, 94000),
                shop("Area 18", "Stanton", 64.43, 94000),    shop("Levski", "Nyx", 90.1, 94000)};
            const auto places = SCMiner::onePerPlace(lancet);
            const std::string dot = "  \xC2\xB7  ";
            size_t allNamed = 0, twoNamed = 0, oneNamed = 9;
            const std::string all = SCMiner::shopLine(places, [](const std::string&) { return true; }, &allNamed);
            const std::string two =
                SCMiner::shopLine(places, [](const std::string& t) { return t.size() <= 70; }, &twoNamed);
            const std::string one = SCMiner::shopLine(places, [](const std::string&) { return false; }, &oneNamed);
            std::vector<ShopInOrder> unmeasured = places;
            for (ShopInOrder& s : unmeasured) s.metres = -1;
            const std::string bare = SCMiner::shopLine(unmeasured, [](const std::string& t) { return t.size() <= 50; });
            size_t noneNamed = 7;
            const bool lines =
                places.size() == 5 && places[1].offer.price == 94000 &&
                all == "Orbituary 28.0 Gm" + dot + "Stanton Gateway (Pyro) 47.8 Gm" + dot + "Nyx Gateway (Pyro) 57.7 Gm" +
                           dot + "Area 18 64.4 Gm" + dot + "Levski 90.1 Gm" &&
                allNamed == 5 && two == "Orbituary 28.0 Gm" + dot + "Stanton Gateway (Pyro) 47.8 Gm" + dot + "3 more" &&
                twoNamed == 2 && one == "Orbituary 28.0 Gm" + dot + "4 more" && oneNamed == 1 &&
                bare == "Orbituary" + dot + "Stanton Gateway (Pyro)" + dot + "3 more" &&
                SCMiner::shopLine({}, nullptr, &noneNamed).empty() && noneNamed == 0 &&
                SCMiner::shopPlace(places[3].offer) == "Area 18" && SCMiner::gigametres(0) == "0.0 Gm" &&
                SCMiner::gigametres(-1).empty();
            allPassed &= check(lines, "an item's line on the Gear page: one shop per place, a gateway with its system, "
                                      "as many places as the line holds and how many more (got \"" +
                                          two + "\" / \"" + one + "\" / \"" + bare + "\")");
        }
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
        // ...and the commodity terminal a Datarunner's picture was taken at:
        // the town its shop is named after before a wrong place, a station's
        // or an outpost's own name, SMO read as SM0, ore sales at the raw
        // terminal, a gateway with its system, a Pyro rest stop by its name
        // or its point - and every one at a place with several, none where
        // the terminal is gone
        using IdList = std::vector<int>;
        const bool terminalsFit =
            read && directory.tradeTerminals("SCShop_Levski_CargoOffice_Commodities", "Crusader") == IdList{778} &&
            directory.tradeTerminals("SCShop_Admin_lt_base_g", "HUR-L1") == IdList{100} &&
            directory.tradeTerminals("SCShop_Trdpst_Warehouse_OTLW_Int_A", "Ignis") == IdList{507} &&
            directory.tradeTerminals("SCShop_Admin_lt_base_g", "Shubin SMO-18") == IdList{900} &&
            directory.tradeTerminals("SCShop_Levski_Refinery_OreSales", "Levski") == IdList{786} &&
            directory.tradeTerminals("SCShop_Trdpst_Warehouse_CFP_Int_A", "Pyro IV") == IdList({510, 688}) &&
            directory.tradeTerminals("SCShop_Admin_lt_base_g", "Pyro Gateway (Stanton)") == IdList{252} &&
            directory.tradeTerminals("SCShop_Admin_lt_base_g", "Pyro Gateway (Nyx)") == IdList{743} &&
            directory.tradeTerminals("SCShop_Admin_lt_base_g", "Checkmate") == IdList{436} &&
            directory.tradeTerminals("SCShop_Admin_lt_base_g", "PYR2 L4") == IdList{436} &&
            directory.tradeTerminals("SCShop_Admin_lt_base_g", "Gone").empty() &&
            directory.tradeTerminals("SCShop_Admin_lt_base_g", "").empty() &&
            directory.terminalName(436) == "Admin - Checkmate" && directory.terminalName(12345).empty() &&
            directory.commodityTerminals().size() == 10 && directory.commodityNames().size() == 7;
        allPassed &= check(terminalsFit, "a Datarunner picture's terminal: by its shop's town, a station, an outpost, "
                                         "ore sales, a gateway's system, a Lagrange point - every one at a place with "
                                         "several, none where it is gone");
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
            R::verdict(200, "{\"status\":\"user_not_verified\"}").find("verified with RSI") != std::string::npos &&
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

    // --- UEX Datarunner: the commodity terminal's card, its lines as the
    //     reader takes them - the stock's level in the game's words, the
    //     stock, the price of a SCU or of one cargo box, the name with its
    //     figures exact ---
    {
        bool unitA = true, unitB = false, unitC = true, unitD = true;
        int exact = -1, slipped = -1, margin = 0, slippedAside = -1, marginAside = 0;
        const bool levels =
            SCMiner::commodityLevelIn("MAX INVENTORY", &exact) == 7 && exact == 0 &&
            // a slip in the word that tells the levels apart leaves another
            // level near; one in INVENTORY does not
            SCMiner::commodityLevelIn("HAX INVENTORY", &slipped, &margin) == 7 && slipped == 1 && margin == 2 &&
            SCMiner::commodityLevelIn("MAX INVENTOR", &slippedAside, &marginAside) == 7 && slippedAside == 1 &&
            marginAside >= 3 &&
            SCMiner::commodityLevelIn("VERY LOV INVENTORY") == 2 && SCMiner::commodityLevelIn("Low Inventory") == 3 &&
            SCMiner::commodityLevelIn("HEDIUN INVENTORY") == 4 && SCMiner::commodityLevelIn("Very High Inventory") == 6 &&
            SCMiner::commodityLevelIn("Out of Stock") == 1 && SCMiner::commodityLevelIn("No Inventory") == 1 &&
            SCMiner::commodityLevelIn("Inventory Updating") == 0 && SCMiner::commodityLevelIn("NOW INVENTORY") == -1 &&
            SCMiner::commodityLevelIn("INVENTORY") == -1 && SCMiner::commodityLevelIn("NAX INVENTORY") == 7 &&
            SCMiner::commodityLevelIn("SHOP QUANTITY") == -1 &&
            std::string(SCMiner::commodityLevelWords(7)) == "MAX INVENTORY" &&
            std::string(SCMiner::commodityLevelWords(1)) == "OUT OF STOCK" &&
            std::string(SCMiner::commodityLevelWords(0)).empty();
        allPassed &= check(levels, "a commodity card's level in the game's words, exactly or with a slip, on UEX's scale");
        const bool figures =
            SCMiner::parseShopQuantity("24,000 SCU") == 24000 && SCMiner::parseShopQuantity("4SCU") == 4 &&
            SCMiner::parseShopQuantity("OSCU") == 0 && SCMiner::parseShopQuantity("0 SCU") == 0 &&
            SCMiner::parseShopQuantity("lOSCU") == 10 && SCMiner::parseShopQuantity("MAXSCU") == -1 &&
            SCMiner::parseShopQuantity("1,O5O SCU") == 1050 && SCMiner::parseShopQuantity("24,00 SCU") == -1 &&
            SCMiner::parseShopQuantity("SHOP QUANTITY") == -1 && SCMiner::parseShopQuantity("1050 SCU") == -1 &&
            SCMiner::parseShopQuantity("000 SCU") == -1 && SCMiner::parseShopQuantity("0 SCU") == 0 &&
            SCMiner::parseCommodityPrice("4.167/SCU", unitA) == 4167 && !unitA &&
            SCMiner::parseCommodityPrice("02.850/SCU", unitA) == -1 &&
            SCMiner::parseCommodityPrice("\xC2\xA4" "1,879/Units", unitB) == 1879 && unitB &&
            SCMiner::parseCommodityPrice("316/SCU", unitC) == 316 && !unitC &&
            SCMiner::parseCommodityPrice("1,108", unitC) == -1 && SCMiner::parseCommodityPrice("1316/SCU", unitC) == -1 &&
            // the currency sign is no figure: the 6,705 after it is 6,705
            // (another tool took the sign for a 9, and sent 96,705)
            SCMiner::parseCommodityPrice("\xC2\xA4" "6,705/SCU", unitD) == 6705 && !unitD &&
            SCMiner::parseCommodityPrice("96705/SCU", unitD) == -1;
        allPassed &= check(figures, "a commodity card's stock and price: thousands set off by commas, no nought in front, "
                                    "the currency sign no figure, a price per SCU or per cargo box");
        const bool totals = SCMiner::parseCargoTotal("Total: 1 SCU") == 1 && SCMiner::parseCargoTotal("Total: 8 SCU") == 8 &&
                            SCMiner::parseCargoTotal("Total:16SCU") == 16 && SCMiner::parseCargoTotal("TOTAL 1,024 SCU") == 1024 &&
                            SCMiner::parseCargoTotal("Total: l SCU") == 1 && SCMiner::parseCargoTotal("Tatal: 1 SCU") == 1 &&
                            SCMiner::parseCargoTotal("Total:L SCU") == 1 && SCMiner::parseCargoTotal("Total: B SCU") == 8 &&
                            SCMiner::parseCargoTotal("Total: LOW SCU") == -1 &&
                            SCMiner::parseCargoTotal("Est. Loading Time 00:02:00") == -1 &&
                            SCMiner::parseCargoTotal("1 UNITS") == -1 && SCMiner::parseCargoTotal("Total:") == -1 &&
                            SCMiner::parseCargoTotal("Total: 1") == -1 && SCMiner::parseCargoTotal("") == -1;
        allPassed &= check(totals, "the foot of a card opened for buying: \"Total: 8 SCU\", the box picked times the units");
        const bool named = SCMiner::commodityNamed("SHIP AMMUNITION SIZE 1", "Ship Ammunition - Size 1") &&
                           !SCMiner::commodityNamed("SHIP AMMUNITION SIZE 2", "Ship Ammunition - Size 1") &&
                           SCMiner::commodityNamed("Ship Ammunition- Sizel", "Ship Ammunition - Size 1") &&
                           SCMiner::commodityNamed("HEPHAESTANITE", "Hephaestanite") &&
                           SCMiner::commodityNamed("Agricultural Supplies", "Agricultural Supplies") &&
                           !SCMiner::commodityNamed("IRON", "Tin");
        allPassed &= check(named, "a commodity card's name: the letters with a slip or two, the figures exact");
        // ...and how sure each value is: two readings that agree, the
        // game's words exactly; a box's price none for UEX
        SCMiner::CommodityCard card;
        card.level = 7;
        card.levelSlips = 0;
        card.stock = 1050;
        card.stockReads = 2;
        card.price = 4167;
        card.priceReads = 2;
        SCMiner::CommodityCard slip = card;
        slip.levelSlips = 1;
        slip.levelMargin = 3;
        SCMiner::CommodityCard once = card;
        once.levelSlips = 1;
        once.levelMargin = 2;
        once.stockReads = 1;
        once.priceReads = 1;
        SCMiner::CommodityCard box = card;
        box.perUnit = true;
        SCMiner::CommodityCard empty = card;
        empty.level = 1;
        using SCMiner::Certainty;
        const bool sure = card.levelSure() == Certainty::Sure && card.stockSure() == Certainty::Sure &&
                          card.priceSure() == Certainty::Sure && slip.levelSure() == Certainty::Sure &&
                          once.levelSure() == Certainty::Check &&
                          once.stockSure() == Certainty::Check && once.priceSure() == Certainty::Check &&
                          box.priceSure() == Certainty::Missing && empty.stockSure() == Certainty::Check &&
                          SCMiner::CommodityCard().levelSure() == Certainty::Missing &&
                          SCMiner::CommodityCard().stockSure() == Certainty::Missing &&
                          SCMiner::CommodityCard().priceSure() == Certainty::Missing;
        allPassed &= check(sure, "how sure a card's values are: sure where two readings agree or the words are the "
                                 "game's, to check where read once, missing where a cargo box is picked");
        // ...but one unit of the 1 SCU box is a SCU: its price stands, where
        // "Total: 1 SCU" was made out at both sizes - not once, not 8 SCU
        SCMiner::CommodityCard oneBox = box;
        oneBox.totalScu = 1;
        oneBox.totalReads = 2;
        SCMiner::CommodityCard oneBoxOnce = oneBox;
        oneBoxOnce.totalReads = 1;
        SCMiner::CommodityCard eightBox = oneBox;
        eightBox.totalScu = 8;
        const bool oneScu = oneBox.oneScuBox() && oneBox.priceSure() == Certainty::Sure && !oneBoxOnce.oneScuBox() &&
                            oneBoxOnce.priceSure() == Certainty::Missing && !eightBox.oneScuBox() &&
                            eightBox.priceSure() == Certainty::Missing && !box.oneScuBox() && !card.oneScuBox();
        allPassed &= check(oneScu, "a card opened on the 1 SCU box, one unit of it: its price per unit is the price per "
                                   "SCU, where \"Total: 1 SCU\" was read twice");
    }

    // --- UEX Datarunner: the report as UEX's documentation writes it - one
    //     side of the list, the cargo box sizes per row where the log has
    //     them, the game version, the picture's date, the picture -, UEX's
    //     answers, the terminal the log saw fill, and the five minutes kept
    //     over a restart ---
    {
        namespace R = SCMiner::UexReports;
        namespace D = SCMiner::Datarunner;
        SCMiner::UexPriceRow argon, iron;
        argon.commodity = 111;
        argon.price = 316;
        argon.stock = 24000;
        argon.level = 7;
        argon.boxes = "1,2,4,8,16,24,32";
        iron.commodity = 44;
        iron.price = 2850;
        iron.stock = 4;
        iron.level = 2;
        const std::string bought = R::priceReport(252, false, {argon, iron}, 1791700000, "4.10.2", "QUJD", true);
        SCMiner::UexPriceRow waste;
        waste.commodity = 192;
        waste.price = 229.5;
        waste.stock = 0;
        waste.level = 1;
        waste.boxes = "8\",\"x";
        const std::string sold = R::priceReport(436, true, {waste}, 0, "", "", false);
        const bool bodies =
            bought == "{\"id_terminal\":252,\"type\":\"commodity\",\"is_production\":1,\"prices\":["
                      "{\"id_commodity\":111,\"price_buy\":316,\"scu_buy\":24000,\"status_buy\":7,"
                      "\"container_sizes\":\"1,2,4,8,16,24,32\"},"
                      "{\"id_commodity\":44,\"price_buy\":2850,\"scu_buy\":4,\"status_buy\":2}],"
                      "\"game_version\":\"4.10.2\",\"date_added\":1791700000,\"screenshot\":\"QUJD\"}" &&
            sold == "{\"id_terminal\":436,\"type\":\"commodity\",\"is_production\":0,\"prices\":["
                    "{\"id_commodity\":192,\"price_sell\":229.5,\"scu_sell\":0,\"status_sell\":1}]}" &&
            bought.find("_sell") == std::string::npos && sold.find("_buy") == std::string::npos &&
            R::base64((const unsigned char*)"Man", 3) == "TWFu" && R::base64((const unsigned char*)"Ma", 2) == "TWE=" &&
            R::base64((const unsigned char*)"M", 1) == "TQ==";
        allPassed &= check(bodies, "a Datarunner report as UEX's documentation writes it: one side of the list, the "
                                   "box sizes per row where known, the game version, the date and the picture where "
                                   "there is one, and nothing else");
        std::string user;
        const std::vector<std::string> ids = R::reportIds(
            "{\"status\":\"ok\",\"http_code\":200,\"data\":{\"ids_reports\":[\"964307\",\"964311\"],\"date_added\":"
            "\"1787083440\",\"username\":\"Runner\"},\"message\":\"\"}");
        auto said = [](const char* status) { return R::verdict(400, std::string("{\"status\":\"") + status + "\"}"); };
        const bool answers =
            R::verdict(200, "{\"status\":\"ok\",\"data\":{\"ids_reports\":[\"964307\"],\"username\":\"Runner\"}}", &user)
                .empty() &&
            user == "Runner" && ids == std::vector<std::string>{"964307", "964311"} &&
            R::reportIds("{\"status\":\"ok\",\"data\":{\"ids_reports\":[0]}}") == std::vector<std::string>{"0"} &&
            R::reportIds("{\"status\":\"duplicated_report\"}").empty() &&
            said("duplicated_report") == "Already reported in the last 5 minutes" &&
            said("screenshot_required").find("screenshot") != std::string::npos &&
            said("screenshot_length_exceeds_limit").find("10 MB") != std::string::npos &&
            said("invalid_game_version").find("game version") != std::string::npos &&
            said("ptu_reports_not_allowed").find("PTU") != std::string::npos &&
            said("too_many_reports").find("30 minutes") != std::string::npos &&
            said("max_rows_exceeded").find("500") != std::string::npos &&
            said("user_not_allowed").find("this account") != std::string::npos &&
            said("user_disabled").find("blocked") != std::string::npos &&
            said("invalid_date").find("date") != std::string::npos &&
            said("terminal_not_found").find("Terminal") != std::string::npos &&
            said("has_both_price_buy_and_price_sell") == "UEX responded: has both price buy and price sell" &&
            R::accountTrouble(400, "{\"status\":\"user_not_allowed\"}") == "datarunner" &&
            R::duplicateNamed("{\"status\":\"duplicated_report\",\"report\":\"44\"}") == "44" &&
            R::duplicateNamed("{\"status\":\"duplicated_report\",\"data\":{\"report\":{\"id_commodity\":44}}}") == "44" &&
            R::duplicateNamed("{\"status\":\"duplicated_report\"}").empty() &&
            R::duplicateNamed("{\"status\":\"ok\",\"report\":\"44\"}").empty();
        allPassed &= check(answers, "UEX's answers to a Datarunner report: its ids, each refusal in the window's words, "
                                    "the row a duplicate stopped at");

        // the channel of a screenshot folder: HOTFIX is LIVE's servers
        const bool channels = D::channelOf("C:/Games/StarCitizen/LIVE/ScreenShots") == "LIVE" &&
                              D::channelOf("C:/Games/StarCitizen/HOTFIX/ScreenShots") == "LIVE" &&
                              D::channelOf("C:/Games/StarCitizen/PTU/ScreenShots") == "PTU" &&
                              D::channelOf("C:/Games/StarCitizen/EPTU/ScreenShots") == "PTU" &&
                              D::channelOf("C:/Games/StarCitizen/TECH-PREVIEW/ScreenShots") == "PTU" &&
                              D::channelOf("C:/Games/StarCitizen/LIVE") == "LIVE" &&
                              D::channelOf("D:/Pictures").empty() &&
                              D::screenshotsBeside("C:/Games/StarCitizen/LIVE/Game.log") ==
                                  std::filesystem::path("C:/Games/StarCitizen/LIVE") / "ScreenShots" &&
                              D::screenshotsBeside("").empty();
        allPassed &= check(channels, "a screenshot folder's channel: LIVE and HOTFIX are LIVE, PTU, EPTU and "
                                     "TECH-PREVIEW are PTU, any other folder neither");

        // the terminal the log saw fill - its shop, the place, the sizes of
        // what it sells; the player's own boxes in lines of the same name
        // are no list (Everus Harbor's admin office, 11 October 2026)
        SCMiner::TradeBook book;
        const std::string filling = "<2026-10-11T21:48:41.127Z> [Notice] <CEntityComponentCommodityUIProvider::"
                                    "LoadShopInventoryData::<lambda_1>::operator ()> AddingCommodityBox - "
                                    "playerId[1] shopId[863685056484] shopName[SCShop_Admin_lt_base_g] "
                                    "commodityName[ResourceType.";
        const std::string team = " [Team_CoreGameplayFeatures][Shops][UI]\r";
        book.live("<2026-10-11T21:40:00.000Z> [Notice] <RequestLocationInventory> Player[Someone] requested inventory "
                  "for Location[RR_JP_StantonPyro] [Team_CoreGameplayFeatures][Inventory]");
        book.live(filling + "Titanium] Available Box Sizes:  boxSize[8] boxSize[16] boxSize[24] boxSize[32]" + team);
        book.live(filling + "ShipAmmoSize4] Available Box Sizes:  boxSize[1] boxSize[2] boxSize[4] boxSize[8] "
                            "boxSize[16] boxSize[24] boxSize[32]" + team);
        book.live(filling + "RMC] Available Box Sizes:  boxSize[1] boxSize[3] boxSize[64]" + team);
        book.live("<2026-10-11T21:48:44.000Z> [Notice] <CEntityComponentCommodityUIProvider::AddPlayerCommodityItem> "
                  "AddingCommodityBox - playerId[1] shopId[863685056484] shopName[SCShop_Admin_lt_base_g] "
                  "commodityName[] Cargo Box Data:  [2 | unitAmount[62]]" + team);
        book.live("<2026-10-11T21:55:00.000Z> [Notice] <CEntityComponentCommodityUIProvider::LoadShopInventoryData::"
                  "<lambda_1>::operator ()> AddingCommodityBox - playerId[1] shopId[111] "
                  "shopName[SCShop_Trdpst_Warehouse_OTLW_Int_A] commodityName[ResourceType.Scrap] Available Box "
                  "Sizes:  boxSize[1] boxSize[2]" + team);
        const long long at = SCMiner::TradeBook::millisOf("2026-10-11T21:48:41.127");
        const SCMiner::TradeBook::TerminalFill* first = book.terminalAt(at + 60000, 30 * 60000);
        const SCMiner::TradeBook::TerminalFill* second = book.terminalAt(at + 10 * 60000, 30 * 60000);
        std::string resource, sizes;
        const bool fills =
            book.terminalFills().size() == 2 && first && first->ms == at && first->shop == "SCShop_Admin_lt_base_g" &&
            first->place == "Pyro Gateway (Stanton)" && first->boxes.size() == 3 &&
            first->boxes.at("Titanium") == "8,16,24,32" && first->boxes.at("ShipAmmoSize4") == "1,2,4,8,16,24,32" &&
            first->boxes.at("RMC") == "1,3,64" && second && second->boxes.size() == 1 &&
            second->boxes.at("Scrap") == "1,2" && !book.terminalAt(at - 1000, 30 * 60000) &&
            !book.terminalAt(at + 120 * 60000, 30 * 60000) &&
            !SCMiner::TradeBook::parseBoxes("<2026-10-11T21:48:44.000Z> AddingCommodityBox - commodityName[] Cargo Box "
                                            "Data:  [2 | unitAmount[62]]", resource, sizes) &&
            at == 1791755321127LL && SCMiner::TradeBook::millisOf("noon") == -1 &&
            D::boxesUexTakes("8,16,24,32") && !D::boxesUexTakes("1,3,64") && !D::boxesUexTakes("");
        // ...and a terminal in use: the last line of one, whatever it lists
        const bool used = book.terminalUsedMs() == SCMiner::TradeBook::millisOf("2026-10-11T21:55:00.000") &&
                          book.terminalUsedShop() == "SCShop_Trdpst_Warehouse_OTLW_Int_A" &&
                          SCMiner::TradeBook().terminalUsedMs() == 0;
        allPassed &= check(used, "a commodity terminal in use, from the game's log: the time and the shop of its last line");
        allPassed &= check(fills, "a terminal's list filling, from the game's log: its shop, the place, the box sizes of "
                                  "what it sells - the one filled last before a picture, within half an hour");
        const bool resources = SCMiner::TradeBook::resourceIs("ShipAmmoSize4", "Ship Ammunition - Size 4") &&
                               !SCMiner::TradeBook::resourceIs("ShipAmmoSize4", "Ship Ammunition - Size 1") &&
                               SCMiner::TradeBook::resourceIs("RMC", "Recycled Material Composite") &&
                               SCMiner::TradeBook::resourceIs("PlasmaFuel", "Hydrogen Fuel") &&
                               !SCMiner::TradeBook::resourceIs("Hydrogen", "Hydrogen Fuel") &&
                               SCMiner::TradeBook::resourceIs("ShipCountermeasuresDecoy", "Ship Decoy Countermeasures") &&
                               SCMiner::TradeBook::resourceIs("Etam", "E'tam") &&
                               SCMiner::TradeBook::resourceIs("LifeCureMedsticks", "LifeCure Medsticks") &&
                               SCMiner::TradeBook::resourceIs("CK13SeedBlend", "CK13-GID Seed Blend") &&
                               SCMiner::TradeBook::resourceIs("Titanium", "Titanium") &&
                               !SCMiner::TradeBook::resourceIs("Titanium", "Titanium (Ore)");
        allPassed &= check(resources, "a resource's name in the game's records read as the commodity UEX writes so");

        // a picture's rows matched to UEX's commodities and its terminal:
        // a name letter for letter is sure, one with a slip to check, the
        // log's list first; the box sizes for the BUY list only, and only
        // where UEX takes them all
        SCMiner::UexDirectory uex;
        std::string why;
        const bool uexRead = uex.read(
            "{\"status\":\"ok\",\"data\":[{\"id\":252,\"type\":\"commodity\",\"name\":\"Admin - Pyro Gateway "
            "(Stanton)\",\"nickname\":\"Pyro Gateway (Stanton)\",\"space_station_name\":\"Pyro Gateway (Stanton)\","
            "\"is_available\":1},{\"id\":18,\"type\":\"commodity\",\"name\":\"CBD - Central Business District - "
            "Lorville\",\"nickname\":\"CBD Lorville\",\"city_name\":\"Lorville\",\"is_available\":1},{\"id\":51,"
            "\"type\":\"commodity\",\"name\":\"Admin - L19 Residences - Metro Center - Lorville\",\"nickname\":"
            "\"Lorville L19\",\"city_name\":\"Lorville\",\"is_available\":1}]}",
            "{\"status\":\"ok\",\"data\":[{\"id\":234,\"name\":\"Titanium\"},{\"id\":204,\"name\":\"Ship Ammunition - "
            "Size 4\"},{\"id\":201,\"name\":\"Ship Ammunition - Size 1\"},{\"id\":151,\"name\":\"Recycled Material "
            "Composite\"},{\"id\":30,\"name\":\"Gold\"},{\"id\":31,\"name\":\"Gald\"}]}",
            "{\"status\":\"ok\",\"data\":[]}", why);
        D::Shot shot;
        shot.side = 1;
        auto rowOf = [](const char* name) {
            D::Row row;
            row.read = name;
            return row;
        };
        shot.rows = {rowOf("TITANIUM"), rowOf("SHIP AMMUNITION - SIZE 4"), rowOf("RECYCLED MATERIAL COMPOSITE"),
                     rowOf("Ship Ammunition- Sizel"), rowOf("GXLD"), rowOf("UNOBTAINIUM")};
        D::match(shot, uex, first);
        D::Shot selling = shot;
        selling.side = 2;
        D::match(selling, uex, first);
        D::Shot lorville;
        SCMiner::TradeBook::TerminalFill city;
        city.shop = "SCShop_Admin_hur_cbd";
        city.place = "Lorville";
        D::match(lorville, uex, &city);
        using SCMiner::Certainty;
        std::string named;
        Certainty how = Certainty::Sure;
        const bool matched =
            uexRead && shot.terminal == 252 && shot.terminals.size() == 1 && shot.rows[0].commodity == 234 &&
            shot.rows[0].nameSure == Certainty::Sure && shot.rows[0].boxes == "8,16,24,32" &&
            shot.rows[1].commodity == 204 && shot.rows[1].name == "Ship Ammunition - Size 4" &&
            shot.rows[1].boxes == "1,2,4,8,16,24,32" && shot.rows[2].commodity == 151 && shot.rows[2].boxes.empty() &&
            shot.rows[3].commodity == 201 && shot.rows[3].nameSure == Certainty::Check &&
            shot.rows[4].commodity == 0 && shot.rows[4].nameSure == Certainty::Missing &&
            shot.rows[5].commodity == 0 && selling.rows[0].commodity == 234 && selling.rows[0].boxes.empty() &&
            selling.rows[1].commodity == 204 && selling.rows[1].boxes.empty() &&
            D::nearestCommodities("GXLD", uex.commodityNames(), 2) ==
                std::vector<std::pair<std::string, int>>{{"Gold", 30}, {"Gald", 31}} &&
            D::nearestCommodities("SHIP AMMUNITION", uex.commodityNames(), 1).front().second == 204 &&
            lorville.terminal == 0 && lorville.terminals.size() == 2 &&
            D::matchCommodity("GXLD", uex.commodityNames(), {30}, named, how) == 30 && how == Certainty::Check &&
            D::matchCommodity("", uex.commodityNames(), {}, named, how) == 0 && how == Certainty::Missing;
        // the 1 SCU box read as picked where the log lists none for the
        // commodity (Titanium: 8 to 32): "Total: 1 SCU" misread, the price
        // one box's after all - where it lists one, the price stands
        D::Shot picked;
        picked.side = 1;
        picked.rows = {rowOf("TITANIUM"), rowOf("SHIP AMMUNITION - SIZE 4")};
        for (D::Row& row : picked.rows) {
            row.oneScuBox = true;
            row.price = 7681;
            row.priceSure = Certainty::Sure;
        }
        D::match(picked, uex, first);
        const bool oneScuListed = picked.rows[0].perUnit && !picked.rows[0].oneScuBox &&
                                  picked.rows[0].priceSure == Certainty::Missing && !picked.rows[1].perUnit &&
                                  picked.rows[1].oneScuBox && picked.rows[1].priceSure == Certainty::Sure;
        allPassed &= check(oneScuListed, "the 1 SCU box read as picked stands only where the game's log lists one for the "
                                         "commodity");
        allPassed &= check(matched, "a Datarunner picture's rows matched to UEX: a name letter for letter sure, one with "
                                    "a slip to check, the log's list first, the nearest names to pick from; box sizes "
                                    "where UEX takes them all, the log's of what the terminal sells only; both of "
                                    "Lorville's terminals to pick from");

        // what stands in the way of Send, and the five minutes - kept over a
        // restart in the record of what went
        const long long now = 1791754800;
        D::Shot ready;
        ready.terminal = 252;
        ready.side = 1;
        ready.channel = "LIVE";
        ready.taken = now - 120;
        D::Row good;
        good.commodity = 234;
        good.level = 7;
        good.stock = 6000;
        good.price = 7681;
        good.nameSure = good.levelSure = good.stockSure = good.priceSure = Certainty::Sure;
        D::Row unsure = good;
        unsure.commodity = 204;
        unsure.priceSure = Certainty::Check;
        ready.rows = {good, unsure};
        D::Shot checked = ready;
        checked.rows[1].priceSure = Certainty::Sure;
        D::Shot boxPrice = checked;
        boxPrice.rows[1].perUnit = true;
        boxPrice.rows[1].name = "Ship Ammunition - Size 4";
        D::Shot noTerminal = checked;
        noTerminal.terminal = 0;
        D::Shot old = checked;
        old.taken = now - 31ll * 24 * 3600;
        D::Shot allGone = checked;
        allGone.rows[0].removed = allGone.rows[1].removed = true;
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path folder = fs::temp_directory_path(ec) / "bedrock-datarunner-selftest";
        fs::remove_all(folder, ec);
        fs::create_directories(folder, ec);
        {
            // sent a minute ago, off a picture taken under five minutes
            // before this one
            D::Sent went;
            went.terminal = 252;
            went.side = 1;
            went.commodity = 234;
            went.sent = now - 60;
            went.taken = now - 400;
            std::ofstream out(folder / "reported.txt");
            out << "refinery|Levski|1|1791000000\n" << D::sentKey(went) << "\nprice|252|3|234|1|2\n";
        }
        std::vector<D::Sent> record;   // read back as after a restart
        {
            std::ifstream in(folder / "reported.txt");
            std::string line;
            while (std::getline(in, line)) {
                D::Sent s;
                if (D::readSentKey(line, s)) record.push_back(s);
            }
        }
        fs::remove_all(folder, ec);
        D::Shot later = checked;
        later.taken = now - 10;       // a new look, six and a half minutes after the one that went
        long long wait = 0;
        const D::Hold heldLater = D::holdOf(later, later.rows[0], record, now, &wait);
        const bool sendable =
            D::whyNotSend(checked, {}, now).empty() &&
            D::whyNotSend(ready, {}, now) == "1 value to check - Compare with the picture, then click Confirm" &&
            D::whyNotSend(boxPrice, {}, now) == "Ship Ammunition - Size 4 shows a price per unit - Remove its row, or take "
                                                "the picture again with nothing selected" &&
            D::whyNotSend(noTerminal, {}, now).rfind("Terminal not known", 0) == 0 &&
            D::whyNotSend(old, {}, now).rfind("Picture older than 30 days", 0) == 0 &&
            D::whyNotSend(allGone, {}, now).rfind("Nothing to send", 0) == 0 &&
            record.size() == 1 && D::sentKey(record[0]) == "price|252|1|234|" + std::to_string(now - 60) + "|" +
                                                               std::to_string(now - 400) &&
            D::holdOf(checked, checked.rows[0], record, now) == D::Hold::Sent &&
            D::rowsToSend(checked, record, now).size() == 1 && heldLater == D::Hold::Wait && wait == 240 &&
            D::whyNotSend(later, record, now) == "Reported less than 5 minutes ago - Send in 4 min" &&
            D::holdOf(later, later.rows[1], record, now) == D::Hold::None &&
            D::whyNotSend(later, record, now + 240).empty();
        allPassed &= check(sendable, "Send waits for every value sure, a terminal, a list and a date UEX takes; a row "
                                     "another picture of the same moment carried is left out, and the same commodity "
                                     "waits UEX's five minutes - over a restart too");
        // Bedrock's own looks at a terminal in use: the rows two looks in a
        // row show alike, on the same list of the same shop, however the
        // list moved around them, make a picture - those of them no picture
        // of that shop's list from the last half hour has; a row the player
        // took out counts as had, an opened card's and a half-read one are
        // no rows for it
        {
            auto whole = [](const char* read, int level, int stock, int price) {
                D::Row row;
                row.read = read;
                row.level = level;
                row.stock = stock;
                row.price = price;
                row.levelSure = row.stockSure = row.priceSure = Certainty::Sure;
                return row;
            };
            D::Shot look;
            look.side = 1;
            look.shop = "SCShop_Trdpst_Warehouse_OTLW_Int_A";
            look.taken = now;
            look.rows = {whole("Neon", 2, 23, 15449), whole("E'tam", 2, 5, 16295),
                         whole("LifeCure Medsticks", 7, 1600, 574)};
            const D::Shot again = look;
            D::Shot moved = look;            // a sale: E'tam's stock went down
            moved.rows[1].stock = 4;
            D::Shot scrolled = look;         // a row further down the list
            scrolled.rows = {whole("E'tam", 2, 5, 16295), whole("LifeCure Medsticks", 7, 1600, 574),
                             whole("Gold", 4, 300, 29000)};
            D::Shot sold = look;
            sold.side = 2;
            D::Shot otherShop = look;
            otherShop.shop = "SCShop_Admin_lt_base_g";
            D::Row opened = whole("Carbon", 7, 24000, 1879);
            opened.perUnit = true;
            D::Row half = whole("Gold", 4, 300, -1);
            half.priceSure = Certainty::Missing;
            D::Shot rest = look;
            rest.rows = {opened, half};
            auto had = std::make_shared<D::Shot>(look);
            had->rows = {whole("[NEON", 2, 23, 15449)};
            auto out = std::make_shared<D::Shot>(*had);
            out->rows = {whole("E'tam", 2, 5, 16295)};
            out->rows[0].removed = true;
            auto elsewhere = std::make_shared<D::Shot>(*had);
            elsewhere->shop = "SCShop_Admin_lt_base_g";
            elsewhere->rows = {whole("LifeCure Medsticks", 7, 1600, 574)};
            auto older = std::make_shared<D::Shot>(*had);
            older->taken = now - 3600;
            older->rows = {whole("LifeCure Medsticks", 7, 1600, 574)};
            const D::Shot fresh = D::newRowsOf(look, {had, out, elsewhere, older}, now - 1800);
            const D::Shot alongTheList = D::steadyRowsOf(scrolled, look);
            const D::Shot afterSale = D::steadyRowsOf(moved, look);
            D::Shot restAgain = rest;
            const bool looks = D::steadyRowsOf(again, look).rows.size() == 3 && afterSale.rows.size() == 2 &&
                               afterSale.rows[0].read == "Neon" && afterSale.rows[1].read == "LifeCure Medsticks" &&
                               alongTheList.rows.size() == 2 && alongTheList.rows[0].read == "E'tam" &&
                               D::steadyRowsOf(sold, look).rows.empty() && D::steadyRowsOf(otherShop, look).rows.empty() &&
                               D::steadyRowsOf(restAgain, rest).rows.empty() &&
                               D::newRowsOf(look, {}, now - 1800).rows.size() == 3 && fresh.rows.size() == 1 &&
                               fresh.rows[0].read == "LifeCure Medsticks" && fresh.shop == look.shop &&
                               fresh.taken == now && fresh.side == 1 && D::newRowsOf(rest, {}, now - 1800).rows.empty() &&
                               D::newRowsOf(sold, {had, out}, now - 1800).rows.size() == 3;
            allPassed &= check(looks, "Bedrock's own looks at a terminal in use: the rows two looks show alike make a "
                                      "picture, however the list moved - those no picture of that shop's list from the "
                                      "last half hour has");
        }
        // ...UEX's commodity decides where both rows have one: a name read
        // another way, or its second line missed, is the same; a list not
        // made out may be either; a list sent or put away at the terminal is
        // read no more there, one not made out with it
        {
            auto whole = [](const char* read, int commodity) {
                D::Row row;
                row.read = read;
                row.commodity = commodity;
                row.level = 7;
                row.stock = 12000;
                row.price = 8585;
                row.levelSure = row.stockSure = row.priceSure = Certainty::Sure;
                return row;
            };
            D::Shot look;
            look.side = 1;
            look.shop = "SCShop_Pyro_RestStop_Rund_Admin";
            look.taken = now;
            look.rows = {whole("Ship Noise", 209), whole("Quantum Fuel", 0), whole("Hydrogen Fuel", 104)};
            auto sent = std::make_shared<D::Shot>(look);
            sent->rows = {whole("Ship Noise Countermeasures", 209), whole("QuantumFuel", 87), whole("Hydrogen", 41)};
            const D::Shot after = D::newRowsOf(look, {sent}, now - 1800);
            D::Shot unknown = look;
            unknown.side = 0;
            D::Shot twice = look;            // LOCAL MARKET VALUE: SELLABLE CARGO over IN DEMAND
            twice.side = 2;
            twice.rows = {whole("Elespo", 0), whole("Elespo", 0), whole("Stims", 70)};
            const std::vector<std::pair<std::string, int>> done = {{"SCShop_Pyro_RestStop_Rund_Admin", 1}};
            const bool byCommodity = after.rows.size() == 1 && after.rows[0].read == "Hydrogen Fuel" &&
                                     D::newRowsOf(unknown, {sent}, now - 1800).rows.size() == 1 &&
                                     D::newRowsOf(twice, {}, now - 1800).rows.size() == 2 &&
                                     D::listDone(done, look.shop, 1) && D::listDone(done, look.shop, 0) &&
                                     !D::listDone(done, look.shop, 2) &&
                                     !D::listDone(done, "SCShop_Admin_lt_base_g", 1) && !D::listDone({}, look.shop, 1);
            allPassed &= check(byCommodity, "Bedrock's own looks: a row UEX's commodity of which was offered is not "
                                            "offered again, read however; a list sent or put away at the terminal is "
                                            "read no more there");
        }
        // The foot of the main window while Bedrock reads a terminal: the
        // rows in the picture of the list being read, out of the length the
        // game's log gives the BUY list (Levski's cargo office, 29 October
        // 2026: 4 of 14) - more rows than that, or no length, the rows
        // alone; once he has left, the one picture waiting with its rows
        {
            using SCMiner::MinerGUI;
            const std::wstring dot = L"  \u00B7  ";
            const std::wstring atTerminal = L"Reading the terminal" + dot;
            const std::wstring scroll = dot + L"Scroll down, then hold still";
            const bool whileReading =
                MinerGUI::datarunnerFootWords(true, 0, 0, 0) == atTerminal + L"Hold the list still for a moment" &&
                MinerGUI::datarunnerFootWords(true, 1, 0, 14) == atTerminal + L"Hold the list still for a moment" &&
                MinerGUI::datarunnerFootWords(true, 1, 4, 14) == atTerminal + L"4 of 14 rows" + scroll &&
                MinerGUI::datarunnerFootWords(true, 1, 24, 0) == atTerminal + L"24 rows" + scroll &&
                MinerGUI::datarunnerFootWords(true, 1, 1, 0) == atTerminal + L"1 row" + scroll &&
                MinerGUI::datarunnerFootWords(true, 1, 14, 14) == atTerminal + L"14 of 14 rows" + dot + L"Click to open" &&
                MinerGUI::datarunnerFootWords(true, 1, 16, 14) == atTerminal + L"16 rows" + scroll &&
                MinerGUI::datarunnerFootWords(true, 3, 4, 14) == atTerminal + L"4 of 14 rows" + scroll;
            allPassed &= check(whileReading, "the foot while a terminal is read: the rows of its list so far, out of the "
                                             "BUY list's length - more than that or no length, the rows alone; the "
                                             "list whole, the way to it");
            const std::wstring oneWaiting = L"1 terminal screenshot to check" + dot;
            const bool afterwards =
                MinerGUI::datarunnerFootWords(false, 1, 4, 14) == oneWaiting + L"4 of 14 rows" + dot + L"Click to open" &&
                MinerGUI::datarunnerFootWords(false, 1, 24, 0) == oneWaiting + L"24 rows" + dot + L"Click to open" &&
                MinerGUI::datarunnerFootWords(false, 1, 0, 0) == oneWaiting + L"Click to open" &&
                MinerGUI::datarunnerFootWords(false, 2, 4, 14) ==
                    L"2 terminal screenshots to check" + dot + L"Click to open" &&
                MinerGUI::datarunnerFootWords(false, 0, 0, 0).empty();
            allPassed &= check(afterwards, "the foot once the terminal is left: the one picture waiting with its rows, "
                                           "two or more counted");
            // the length: the fill of the picture's shop, its BUY list only
            D::Shot buyList;
            buyList.side = 1;
            buyList.shop = "SCShop_Admin_lt_base_g";
            D::Shot otherShop = buyList;
            otherShop.shop = "SCShop_Trdpst_Warehouse_OTLW_Int_A";
            D::Shot noShop = buyList;
            noShop.shop.clear();
            D::Shot market = buyList;
            market.side = 2;
            D::Shot unknown = buyList;
            unknown.side = 0;
            const bool lengths = D::listLength(buyList, first) == 3 && D::listLength(otherShop, first) == 0 &&
                                 D::listLength(noShop, first) == 0 && D::listLength(market, first) == 0 &&
                                 D::listLength(unknown, first) == 0 && D::listLength(buyList, nullptr) == 0;
            allPassed &= check(lengths, "a list's length from the game's log: the wares listed for that shop, its BUY "
                                        "list only");
        }
        // A report UEX stopped at its second row, a duplicate: the first row
        // went, the second is on UEX from the player's report of the last
        // five minutes - both held as sent, so Send goes on with the third
        // and only that. A picture whose report went whole is done.
        {
            D::Shot partly = checked;
            D::Row third = good;
            third.commodity = 30;
            partly.rows.push_back(third);
            partly.state = D::Shot::State::Partly;
            std::vector<D::Sent> onUex;
            for (int i = 0; i < 2; i++) {
                D::Sent s;
                s.terminal = partly.terminal;
                s.side = partly.side;
                s.commodity = partly.rows[i].commodity;
                s.sent = now;
                s.taken = partly.taken;
                onUex.push_back(s);
            }
            const std::vector<const D::Row*> rest = D::rowsToSend(partly, onUex, now);
            D::Shot allThere = partly;
            allThere.rows.pop_back();
            D::Shot done = checked;
            done.state = D::Shot::State::Sent;
            allPassed &= check(rest.size() == 1 && rest[0]->commodity == 30 && D::whyNotSend(partly, onUex, now).empty() &&
                                   D::whyNotSend(allThere, onUex, now).rfind("Nothing to send", 0) == 0 &&
                                   D::whyNotSend(done, {}, now).rfind("Sent - ", 0) == 0,
                               "a report UEX stopped at a duplicate: the rows before it and the duplicate held as "
                               "sent, the rows after it still to go");
        }
        // ...and what goes: the rows as checked, the BUY list's box sizes,
        // the picture's date, a picture at most 1080 tall under UEX's 10 MB
        cv::Mat wide(1440, 5120, CV_8UC3, cv::Scalar(40, 60, 80));
        for (int x = 0; x < wide.cols; x += 64) {
            cv::putText(wide, "15,960", cv::Point(x, 700), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(230, 230, 230), 2);
        }
        checked.jpeg = D::pictureForUex(wide);
        checked.rows[0].boxes = "8,16,24,32";
        const cv::Mat back = cv::imdecode(checked.jpeg, cv::IMREAD_COLOR);
        const std::string body = D::reportBody(checked, {}, now, "4.10.2", false);
        D::Shot sellSide = checked;
        sellSide.side = 2;
        const std::string sellBody = D::reportBody(sellSide, {}, now, "4.10.2", false);
        const bool goes = back.cols == 3840 && back.rows == 1080 && checked.jpeg.size() / 3 * 4 < 10u * 1024 * 1024 &&
                          D::pictureForUex(cv::Mat()).empty() &&
                          body.rfind("{\"id_terminal\":252,\"type\":\"commodity\",\"is_production\":0,\"prices\":["
                                     "{\"id_commodity\":234,\"price_buy\":7681,\"scu_buy\":6000,\"status_buy\":7,"
                                     "\"container_sizes\":\"8,16,24,32\"},{\"id_commodity\":204,\"price_buy\":7681,",
                                     0) == 0 &&
                          body.find("\"game_version\":\"4.10.2\",\"date_added\":" + std::to_string(now - 120) +
                                    ",\"screenshot\":\"/9j/") != std::string::npos &&
                          sellBody.find("container_sizes") == std::string::npos &&
                          sellBody.find("\"price_sell\":7681") != std::string::npos;
        allPassed &= check(goes, "a Datarunner report carries the rows as checked, box sizes on the BUY list only, the "
                                 "picture's date, and the very picture - at most 1080 tall, under UEX's 10 MB");
        // Bedrock's looks at one list put together for one report: the first
        // whole, of a later one the piece with its new cards - a card's
        // height around them - at its own place across, under a dark gap;
        // as UEX gets it, at the size the looks would come at
        {
            const cv::Mat first(720, 1280, CV_8UC3, cv::Scalar(10, 20, 30));
            cv::Mat later(720, 1280, CV_8UC3, cv::Scalar(10, 20, 30));
            cv::rectangle(later, cv::Rect(800, 300, 300, 60), cv::Scalar(200, 180, 160), cv::FILLED);
            cv::Mat montage;
            D::addToMontage(montage, first, {});
            const bool firstWhole = montage.rows == 720 && montage.cols == 1280;
            D::addToMontage(montage, later, {cv::Rect(800, 300, 300, 60)});
            const bool under = montage.rows == 720 + 7 + 120 && montage.cols == 1280 &&
                               montage.at<cv::Vec3b>(720 + 7 + 30 + 10, 810) == cv::Vec3b(200, 180, 160) &&
                               montage.at<cv::Vec3b>(720 + 3, 810) == cv::Vec3b(0, 0, 0) &&
                               montage.at<cv::Vec3b>(720 + 50, 100) == cv::Vec3b(0, 0, 0);
            const cv::Mat asIs = cv::imdecode(D::montageForUex(montage, 720), cv::IMREAD_COLOR);
            const cv::Mat at1080 = cv::imdecode(D::montageForUex(montage, 1440), cv::IMREAD_COLOR);
            const bool sized = asIs.cols == 1280 && asIs.rows == 847 && at1080.cols == 960 && at1080.rows == 635 &&
                               D::montageForUex(cv::Mat(), 1440).empty();
            allPassed &= check(firstWhole && under && sized,
                               "Bedrock's looks at one list in one picture: the first whole, under it the piece of each "
                               "later one with its new cards, at its own place across");
        }
        // Star Citizen can save its screenshots black (seen with HDR on): then
        // the picture Bedrock took the moment the game's came is read instead
        // - only then, and only where it took one. A dark scene is no black one.
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path folder = fs::temp_directory_path(ec) / "bedrock-datarunner-black";
            fs::remove_all(folder, ec);
            fs::create_directories(folder, ec);
            const cv::Mat black(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
            cv::Mat dark(720, 1280, CV_8UC3, cv::Scalar(6, 6, 6));
            cv::putText(dark, "SHOP INVENTORY", cv::Point(200, 300), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                        cv::Scalar(40, 60, 200), 2);
            const fs::path blackFile = folder / "ScreenShot-black.jpg";
            const fs::path darkFile = folder / "ScreenShot-dark.jpg";
            const bool written = cv::imwrite(blackFile.string(), black) && cv::imwrite(darkFile.string(), dark);
            SCMiner::TextLineReader lines;
            lines.loadEmbedded();
            const D::Shot fromBlack = D::readPicture(blackFile, lines, dark);
            const D::Shot noneTaken = D::readPicture(blackFile, lines);
            const D::Shot fromDark = D::readPicture(darkFile, lines, dark);
            const bool blackSeen = D::blackPicture(cv::imread(blackFile.string(), cv::IMREAD_COLOR));
            fs::remove_all(folder, ec);
            allPassed &= check(written && blackSeen && D::blackPicture(black) && !D::blackPicture(dark) &&
                                   !D::blackPicture(cv::Mat()) && fromBlack.own && fromBlack.file == blackFile &&
                                   !fromBlack.black && fromBlack.width == 1280 && !noneTaken.own &&
                                   noneTaken.black && noneTaken.width == 5120 && !fromDark.own && !fromDark.black,
                               "a screenshot the game saved black (seen with HDR on): Bedrock's own picture of the "
                               "moment is read instead, a dark one is read as it is");
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
                                         "{\"id_item\":\"5852\",\"item_name\":\"Aluminum\"},"
                                         "{\"id_item\":5948,\"item_name\":\"Pressurized Ice\"}]}", items, why) &&
                           Mk::itemFor(items, "Ouratite") == 5943 && Mk::itemFor(items, "Aluminium") == 5852 &&
                           Mk::itemFor(items, "Ice") == 5948 && Mk::itemFor(items, "Tin") == 0;
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
        allPassed &= check(reads && forGood.size() == 1 && forGood[0].quality == 960 && forLess.empty() && named,
                           "the UEX marketplace read: figures as numbers or strings, sold out, lapsed and other "
                           "currencies out, quality 0 or 1 none, renewed from the approval, cSCU counted in SCU; "
                           "a buyer for better than the part is none, nor one that names no quality; ice is the "
                           "marketplace's Pressurized Ice (got " +
                               std::to_string(listings.size()) + ")");
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
                               GameTexts::render("x{O2:5,7}", "", owns({5, 7})) == "x" + dot + "All owned" &&
                               GameTexts::render("x{O2:5,7}", "", owns({})) == "x" + dot + blue + "None owned" + blueEnd,
                           "a list says in blue what is missing of it, or that all is owned");
        allPassed &= check(GameTexts::render("h{A3:5,7}", "", owns({5, 7})) == "h" &&
                               GameTexts::render("h{A2:5,7}", "", owns({5, 7})) == "h" + dot + "All owned",
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
                std::strncmp(SCMiner::GAME_TEXT_MARKS[i].text, "\\n\\n", 4) == 0 &&
                std::strstr(SCMiner::GAME_TEXT_MARKS[i].text, "<EM4>BLUEPRINTS</EM4>")) {
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
                               (out.find("=Text\\n\\n<EM4>DIFFICULTY</EM4>") != std::string::npos ||
                                out.find("=Text\\n\\n<EM4>BLUEPRINTS</EM4>") != std::string::npos) &&
                               out.find("<EM4>BLUEPRINTS</EM4>") != std::string::npos &&
                               out.find("\r\nbedrock_game_texts=Written by Bedrock") != std::string::npos,
                           "the game's file comes back line for line, marks added, and Bedrock's line last (" +
                               std::to_string(marked) + " marked)");
        allPassed &= check(GameTexts::isOurs(out) && GameTexts::isOurs("a=b\\n<EM4>BLUEPRINTS</EM4> \xC2\xB7 One of 3") &&
                               !GameTexts::isOurs("a=b <EM4>Potential Blueprints</EM4>"),
                           "Bedrock's file is known as Bedrock's, a pack's is not");
        // ...a contract's description how hard it is and how many enemy ships
        // it puts up at once, above its blueprints, and a haul's title where it
        // goes - the places filled in by the game
        {
            const std::string hauls =
                "Covalex_HaulCargo_AToB_title=~mission(ReputationRank) Rank - Direct Cargo Haul\r\n"
                "RedWind_HaulCargo_AToB_title_01=Haul\r\n"
                "RedWind_HaulCargo_MultiToSingle_title_01=Haul \r\n"
                "Foxwell_ShipWaveAttack_ClearHostiles_Desc_M_001=Text\r\n";
            std::string withRoutes;
            GameTexts::compose(hauls, owns({}), withRoutes);
            allPassed &= check(withRoutes.find("Rank - Direct Cargo Haul \xC2\xB7 ~mission(Location|Name) > "
                                               "~mission(Destination|Name) <EM4>[BP]</EM4>\r\n") != std::string::npos &&
                                   withRoutes.find("RedWind_HaulCargo_AToB_title_01=Haul \xC2\xB7 ~mission(Location|Name) > "
                                                   "~mission(Destination|Name)\r\n") != std::string::npos &&
                                   withRoutes.find("RedWind_HaulCargo_MultiToSingle_title_01=Haul \xC2\xB7 to "
                                                   "~mission(Destination|Name)\r\n") != std::string::npos &&
                                   withRoutes.find("=Text\\n\\n<EM4>DIFFICULTY</EM4> 5.0 of 7 \xC2\xB7 Up to 4 enemy ships at a "
                                                   "time\\n\\n<EM4>BLUEPRINTS</EM4> \xC2\xB7 One of 6") != std::string::npos,
                               "a haul's title says where it goes, a contract's description how hard it is and how many "
                               "enemy ships come at once, above its blueprints");
        }

        // ...and the names: class, size and grade before a component's,
        // tracking signal and size before a missile's - read off the game's
        // own descriptions, whichever way their heads are written - and
        // nothing before anything else. A component's short name keeps as it
        // is, as in StarStrings; a missile's gets its prefix too.
        {
            const std::string items =
                "\xEF\xBB\xBFitem_NameCOOL_AEGS_S01_Bracer=Bracer\r\n"
                "item_NameCOOL_AEGS_S01_Bracer_SCItem=Bracer\r\n"
                "item_DescCOOL_AEGS_S01_Bracer=Item Type: Cooler\\nManufacturer: Aegis Dynamics\\nSize: 1\\nGrade: C\\n"
                "Class: Military\\n\\nA cooler.\r\n"
                "item_Name_SHLD_ASAS_S01_Veil=Veil\r\n"
                "item_Desc_SHLD_ASAS_S01_Veil=Item Type: Shield Generator\\nManufacturer: Ascension Astro\\nSize: 1\\n"
                "Grade: B\\nClass: Stealth\r\n"
                "item_NameCOOL_ACOM_S01_QuickCool=QuikCool\r\n"
                "item_DescCOOL_ACOM_S01_QuickCool=Item Type: Cooler\\nManufacturer: ACOM\\nSize: 1\\nGrade: D\\n"
                "Class:\xC2\xA0"
                "Competition\\n\\nFast.\r\n"
                "item_NameQDMP_WETK_S01_Burke=Burke QD\r\n"
                "item_NameQDMP_WETK_S01_Burke_short=Burke\r\n"
                "item_DescQDMP_WETK_S01_Burke=Manufacturer: Wei-Tek\\nType: Quantum Dampener\\nSize: 1\\nGrade: A\\n"
                "Class: Military\\n\\nA dampener.\r\n"
                "item_NameMISL_S12_IR_BEHR_Executor=EX-T12-IR \"Executor\" Torpedo\r\n"
                "item_NameMISL_S12_IR_BEHR_Executor_Short=EX-T12-IR Torpedo\r\n"
                "item_DescMISL_S12_IR_BEHR_Executor=Manufacturer: Behring\\nTracking Signal: Infared\\nSize: 12\\n\\n"
                "A torpedo.\r\n"
                "item_NameMISL_S01_CS_BEHR_Spearhead_Ground=Spearhead GVI-CS Missile\r\n"
                "item_DescMISL_S01_CS_BEHR_Spearhead_Ground=Manufacturer: Behring\\nTracking Signal: Cross Section\\n"
                "Size: 1 (Ground Vehicle Only)\\n\\nA missile.\r\n"
                "item_NameMISL_S01_CS_FSKI_Spark=Spark I Missile\r\n"
                "item_NameMISL_S01_CS_FSKI_Spark_short=Spark I\r\n"
                "item_DescMISL_S01_CS_FSKI_Spark=Manufacturer: FireStorm Kinetics\\nTracking Signal: Cross Section\\n"
                "Size: 1\\n\\nA missile.\r\n"
                "item_NameGMISL_S02_CS_THCN_StrikeForce=StrikeForce II-G Missile\r\n"
                "item_DescGMISL_S02_CS_THCN_StrikeForce=Manufacturer: Thermyte Concern\\nTracking Signal: Cross Section\\n"
                "Size: 2\\n\\nA missile.\r\n"
                "item_NameMISL_S12_CS_TALN_Calamity=Calamity XII-CS Torpedo\r\n"
                "item_DescMISL_S12_CS_TALN_Calamity=Manufacturer: Talon\\nTracking Signal: Cross Section\\nSize: 12\\n\\n"
                "A torpedo.\r\n"
                "item_NameKSAR_Rifle=Karna\r\n"
                "item_DescKSAR_Rifle=Manufacturer: Kastak Arms\\nItem Type: Assault Rifle\\nClass: Energy (Plasma)\\n\\n"
                "A rifle.\r\n"
                "item_NameSHLD_NONE_S01_Odd=Odd\r\n"
                "item_DescSHLD_NONE_S01_Odd=Item Type: Shield Generator\\nSize: 1\\nGrade: B\\nClass: Unknown\r\n";
            std::string named;
            int names = -1;
            const int marks = GameTexts::compose(items, owns({}), named, false, true, &names);
            auto has = [&named](const std::string& line) { return named.find(line) != std::string::npos; };
            allPassed &= check(marks == 0 && names == 12 &&
                                   has("\xEF\xBB\xBFitem_NameCOOL_AEGS_S01_Bracer=Mil/1/C Bracer\r\n") &&
                                   has("\nitem_NameCOOL_AEGS_S01_Bracer_SCItem=Mil/1/C Bracer\r\n") &&
                                   has("\nitem_Name_SHLD_ASAS_S01_Veil=Sth/1/B Veil\r\n") &&
                                   has("\nitem_NameCOOL_ACOM_S01_QuickCool=Cmp/1/D QuikCool\r\n") &&
                                   has("\nitem_NameQDMP_WETK_S01_Burke=Mil/1/A Burke QD\r\n") &&
                                   has("\nitem_NameQDMP_WETK_S01_Burke_short=Burke\r\n") &&
                                   has("\nitem_NameMISL_S12_IR_BEHR_Executor=[IR12] EX-T12-IR \"Executor\" Torpedo\r\n") &&
                                   has("\nitem_NameMISL_S12_IR_BEHR_Executor_Short=[IR12] EX-T12-IR Torpedo\r\n") &&
                                   has("\nitem_NameMISL_S01_CS_BEHR_Spearhead_Ground=[CS1] Spearhead GVI-CS Missile\r\n") &&
                                   has("\nitem_NameKSAR_Rifle=Karna\r\n") && has("\nitem_NameSHLD_NONE_S01_Odd=Odd\r\n") &&
                                   has("\nitem_DescCOOL_AEGS_S01_Bracer=Item Type: Cooler\\n"),
                               "a component's class, size and grade and a missile's tracking signal and size go before "
                               "their names, off the descriptions, and before nothing else (" +
                                   std::to_string(names) + " named)");
            // ...and a missile's size once, in the prefix: the numeral its
            // name carries it in goes, a size inside a model's code stays
            allPassed &= check(has("\nitem_NameMISL_S01_CS_FSKI_Spark=[CS1] Spark Missile\r\n") &&
                                   has("\nitem_NameMISL_S01_CS_FSKI_Spark_short=[CS1] Spark\r\n") &&
                                   has("\nitem_NameGMISL_S02_CS_THCN_StrikeForce=[CS2] StrikeForce-G Missile\r\n") &&
                                   has("\nitem_NameMISL_S12_CS_TALN_Calamity=[CS12] Calamity-CS Torpedo\r\n"),
                               "a missile's size stands once: Spark I is [CS1] Spark, Calamity XII-CS [CS12] Calamity-CS, "
                               "and EX-T12-IR and GVI-CS keep theirs");
            std::string plain, twice;
            GameTexts::compose(items, owns({}), plain, false, false);
            GameTexts::compose(named, owns({}), twice, false, true);
            allPassed &= check(plain.compare(0, items.size(), items) == 0 &&
                                   twice.find("Mil/1/C Mil/1/C") == std::string::npos &&
                                   twice.find("[CS1] [CS1]") == std::string::npos &&
                                   twice.find("=[CS1] Spark Missile\r\n") != std::string::npos &&
                                   named.find("Untick the options in its Blueprints window") != std::string::npos,
                               "without the names the game's text comes back as it is, and a name gets its prefix once");
        }

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
        allPassed &= check(!leftAlone.written && leftAlone.note.find("Another language file") != std::string::npos &&
                               keptPack.note.find("Another language file") != std::string::npos && slurp(packPlace) == pack &&
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
        allPassed &= check(GameTexts::spellings(channel, steps).empty() && taken.note.rfind("Removed", 0) == 0,
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

    // --- The colours (the settings' gear): the sets made for colour
    //     blindness keep apart what has to be told apart, as someone with
    //     that kind sees it; the file; and nothing makes a window unreadable ---
    {
        namespace C = SCMiner::Colours;
        using C::Vision;
        const C::Scheme& bedrock = C::preset(C::Set::Bedrock);
        char line[200];
        // The simulation itself: white and black stay as they are, and
        // Bedrock's own Epic and Rare - violet and blue - run together for
        // deuteranopia, as the literature has them do. A check that never
        // fails would pass every set.
        bool still = true;
        for (Vision v : {Vision::Protanopia, Vision::Deuteranopia, Vision::Tritanopia}) {
            still &= C::seenWith(C::rgb(255, 255, 255), v) == C::rgb(255, 255, 255) &&
                     C::seenWith(C::rgb(0, 0, 0), v) == C::rgb(0, 0, 0);
        }
        allPassed &= check(still, "colour blindness, simulated: white and black stay white and black");
        {
            C::Role a = C::Legendary, b = C::Legendary;
            const double d = C::worstDifference(bedrock, Vision::Deuteranopia, &a, &b);
            std::snprintf(line, sizeof(line),
                          "Bedrock's own colours: Epic and Rare look alike with deuteranopia (%.1f apart, under 5)", d);
            allPassed &= check(d < 5.0 && a == C::Epic && b == C::Rare, line);
            const double normal = C::worstDifference(bedrock, Vision::Normal);
            std::snprintf(line, sizeof(line), "...and are told apart with normal sight (%.1f, 15 or more)", normal);
            allPassed &= check(normal >= 15.0, line);
            allPassed &= check(C::dimInk(bedrock) == C::rgb(160, 166, 176) && C::faintInk(bedrock) == C::rgb(108, 115, 127),
                               "the quieter inks, worked out from Bedrock's background and words, are Bedrock's own");
        }
        // ...a picture goes through it as single colours do, to the unit
        {
            const C::Rgb samples[4] = {C::rgb(238, 178, 70), C::rgb(171, 139, 214), C::rgb(96, 202, 132), C::rgb(15, 16, 19)};
            uint8_t pixels[16];
            int worst = 0;
            for (Vision v : {Vision::Protanopia, Vision::Deuteranopia, Vision::Tritanopia}) {
                for (int i = 0; i < 4; i++) {
                    pixels[i * 4 + 0] = (uint8_t)C::blueOf(samples[i]);
                    pixels[i * 4 + 1] = (uint8_t)C::greenOf(samples[i]);
                    pixels[i * 4 + 2] = (uint8_t)C::redOf(samples[i]);
                    pixels[i * 4 + 3] = 255;
                }
                C::seenWith(pixels, 4, v);
                for (int i = 0; i < 4; i++) {
                    const C::Rgb one = C::seenWith(samples[i], v);
                    worst = (std::max)({worst, std::abs(pixels[i * 4 + 2] - C::redOf(one)),
                                        std::abs(pixels[i * 4 + 1] - C::greenOf(one)),
                                        std::abs(pixels[i * 4 + 0] - C::blueOf(one))});
                    worst = (std::max)(worst, std::abs(pixels[i * 4 + 3] - 255));
                }
            }
            allPassed &= check(worst <= 1, "the preview's picture is seen as each of its colours is (" +
                                               std::to_string(worst) + " off at most)");
        }
        // Each set: every pair that has to be told apart well apart, and no
        // colour that means something reading as plain text - seen with its
        // own kind of colour blindness and with normal sight; and every
        // coloured word readable on a card (WCAG's 4.5), as that eye sees it
        const struct { C::Set set; Vision vision; const char* name; } sets[3] = {
            {C::Set::Protanopia, Vision::Protanopia, "protanopia"},
            {C::Set::Deuteranopia, Vision::Deuteranopia, "deuteranopia"},
            {C::Set::Tritanopia, Vision::Tritanopia, "tritanopia"},
        };
        const C::Rgb card = C::rgb(24, 26, 30);
        for (const auto& s : sets) {
            const C::Scheme& scheme = C::preset(s.set);
            C::Role a = C::Legendary, b = C::Legendary, plain = C::Legendary;
            const double own = C::worstDifference(scheme, s.vision, &a, &b);
            const double normal = C::worstDifference(scheme, Vision::Normal);
            std::snprintf(line, sizeof(line),
                          "the %s set: every pair apart with %s (%.1f, %s/%s closest) and with normal sight (%.1f)",
                          s.name, s.name, own, C::roleKey(a), C::roleKey(b), normal);
            allPassed &= check(own >= 14.0 && normal >= 14.0, line);
            const double flat = C::plainestDifference(scheme, s.vision, &plain);
            const double flatNormal = C::plainestDifference(scheme, Vision::Normal);
            std::snprintf(line, sizeof(line),
                          "the %s set: nothing that means something reads as plain text with %s (%.1f, %s nearest) "
                          "or with normal sight (%.1f)",
                          s.name, s.name, flat, C::roleKey(plain), flatNormal);
            allPassed &= check(flat >= 14.0 && flatNormal >= 14.0, line);
            double least = 99;
            for (int r = C::Legendary; r <= C::LookedFor; r++) {
                least = (std::min)({least, C::contrast(scheme[r], card),
                                    C::contrast(C::seenWith(scheme[r], s.vision), C::seenWith(card, s.vision))});
            }
            std::snprintf(line, sizeof(line),
                          "the %s set: every coloured word readable on a card, with %s too (contrast %.2f)", s.name,
                          s.name, least);
            allPassed &= check(least >= 4.5, line);
        }
        // ...and a red-green set holds for the other red-green kind too: who
        // has which is not always known
        {
            const double pd = C::worstDifference(C::preset(C::Set::Protanopia), Vision::Deuteranopia);
            const double dp = C::worstDifference(C::preset(C::Set::Deuteranopia), Vision::Protanopia);
            std::snprintf(line, sizeof(line),
                          "the red-green sets hold for each other's kind (%.1f and %.1f, 12 or more)", pd, dp);
            allPassed &= check(pd >= 12.0 && dp >= 12.0, line);
        }
        // A refinery's four workload steps, light to extreme: good, caution,
        // the orange halfway to bad, bad - each apart from the next to
        // normal sight in every set (the word beside them says the step to
        // any eye)
        {
            double least = 99;
            for (C::Set set : {C::Set::Bedrock, C::Set::Protanopia, C::Set::Deuteranopia, C::Set::Tritanopia}) {
                const C::Scheme& s = C::preset(set);
                const C::Rgb steps[4] = {s[C::Good], s[C::Caution], C::halfway(s[C::Caution], s[C::Bad]), s[C::Bad]};
                for (int k = 0; k + 1 < 4; k++) least = (std::min)(least, C::difference(steps[k], steps[k + 1]));
            }
            std::snprintf(line, sizeof(line),
                          "the refinery's workload steps are apart in every set (%.1f, 14 or more), heavy at %s",
                          least, C::hex(C::halfway(bedrock[C::Caution], bedrock[C::Bad])).c_str());
            allPassed &= check(least >= 14.0 && C::halfway(bedrock[C::Caution], bedrock[C::Bad]) == C::rgb(229, 140, 89),
                               line);
        }

        // Nothing a player picks makes a window unreadable: a white
        // background is darkened, words as dark as the window lightened
        {
            const C::Scheme light = C::withColour(bedrock, C::Background, C::rgb(255, 255, 255));
            const C::Scheme dark = C::withColour(bedrock, C::Text, C::rgb(30, 30, 30));
            const C::Scheme both = C::withColour(C::withColour(bedrock, C::Text, C::rgb(60, 60, 60)), C::Background,
                                                 C::rgb(200, 40, 40));
            std::snprintf(line, sizeof(line),
                          "a white background is kept dark (luminance %.3f) and the words on it readable (%.1f : 1)",
                          C::luminance(light[C::Background]), C::contrast(light[C::Text], light[C::Background]));
            allPassed &= check(C::luminance(light[C::Background]) <= C::BACKGROUND_MOST + 1e-9 &&
                                   C::contrast(light[C::Text], light[C::Background]) >= C::TEXT_CONTRAST_LEAST,
                               line);
            std::snprintf(line, sizeof(line), "words as dark as the window are lightened until they read (%.1f : 1)",
                          C::contrast(dark[C::Text], dark[C::Background]));
            allPassed &= check(C::contrast(dark[C::Text], dark[C::Background]) >= C::TEXT_CONTRAST_LEAST &&
                                   dark[C::Background] == bedrock[C::Background],
                               line);
            allPassed &= check(C::contrast(both[C::Text], both[C::Background]) >= C::TEXT_CONTRAST_LEAST &&
                                   C::redOf(both[C::Background]) > C::greenOf(both[C::Background]),
                               "a red background stays red, only darker, and the words still read");
            allPassed &= check(C::withColour(bedrock, C::Epic, bedrock[C::Epic]) == bedrock &&
                                   C::keepBackgroundDark(bedrock[C::Background]) == bedrock[C::Background] &&
                                   C::keepTextReadable(bedrock[C::Text], bedrock[C::Background]) == bedrock[C::Text],
                               "Bedrock's own colours pass as they are");
        }
        // The sliders: every colour of every set comes back from hue,
        // saturation and lightness as it went in
        {
            int off = 0;
            for (int s = 0; s < (int)C::Set::COUNT; s++) {
                const C::Scheme& scheme = C::preset((C::Set)s);
                for (int r = 0; r < C::ROLE_COUNT; r++) {
                    if (C::fromHsl(C::toHsl(scheme[r])) != scheme[r]) off++;
                }
            }
            allPassed &= check(off == 0, "every colour of every set comes back off the sliders as it went on (" +
                                             std::to_string(off) + " off)");
        }
        // colours.txt: there and back; a colour of the player's own kept
        // while another set is in use; and whatever is written into it by
        // hand, something usable comes out
        {
            C::Settings mine = C::defaults();
            mine.own = C::withColour(bedrock, C::Epic, C::rgb(210, 90, 180));
            mine.ownKept = true;
            mine.set = C::Set::Deuteranopia;
            const C::Settings back = C::read(C::write(mine));
            allPassed &= check(back.set == C::Set::Deuteranopia && back.ownKept && back.own == mine.own &&
                                   C::active(back) == C::preset(C::Set::Deuteranopia),
                               "colours.txt: the set in use and the player's own colours, there and back");
            const C::Settings odd = C::read("set=own\nown.epic=#d25ab4\nown.rare=blue\nnonsense\nown.background=#FFFFFF\n"
                                            "set=sepia\n");
            allPassed &= check(odd.set == C::Set::Own && odd.own[C::Epic] == C::rgb(210, 90, 180) &&
                                   odd.own[C::Rare] == bedrock[C::Rare] &&
                                   C::luminance(odd.own[C::Background]) <= C::BACKGROUND_MOST + 1e-9,
                               "colours.txt by hand: what reads is taken, a colour that does not is Bedrock's, "
                               "a white background is kept dark");
            allPassed &= check(C::read("set=own\n").set == C::Set::Bedrock && C::read("").set == C::Set::Bedrock,
                               "colours.txt saying own with no colours of its own, or empty: Bedrock's");
            C::Rgb parsed = 0;
            allPassed &= check(C::parseHex("#eeb246", parsed) && parsed == C::rgb(238, 178, 70) &&
                                   C::parseHex(" EEB246 ", parsed) && !C::parseHex("#EEB24", parsed) &&
                                   !C::parseHex("#GGGGGG", parsed) && C::hex(C::rgb(238, 178, 70)) == "#EEB246",
                               "a colour's figure: #EEB246 or eeb246, six hex digits and nothing else");
        }
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
        allPassed &= check(!DB::carried("install_id.txt") && !DB::carried("uex_key.dat"),
                           "...nor the install's own number or the UEX key: a backup restored elsewhere is another "
                           "install");
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

    // --- One number for the crafted item: each value by what the item is for ---
    {
        // A blueprint's Avg. Q from a quality per material - each a band
        // value of its material - or -2 when the blueprint is not shaped so
        const auto avgOf = [](const std::string& blueprint,
                              const std::vector<std::pair<std::string, int>>& byMaterial) {
            for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
                const auto& bp = SCMiner::CRAFT_BLUEPRINTS[b];
                if (blueprint != bp.name) continue;
                if (bp.count != (int)byMaterial.size() || bp.count > 8) return -2;
                int q[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                for (int s = 0; s < bp.count; s++) {
                    const auto& mat = SCMiner::CRAFT_MATERIALS[SCMiner::CRAFT_SLOTS[bp.first + s].material];
                    int want = -1;
                    for (const auto& [name, quality] : byMaterial) {
                        if (name == mat.name) want = quality;
                    }
                    if (want < 0 || SCMiner::snapToBand(mat, want) != want) return -2;
                    q[s] = want;
                }
                return SCMiner::craftAverageQuality(bp, q, 8);
            }
            return -2;
        };
        // The case a player sent (his tool says 958, by amount): damage 3,
        // shared by Gold and Carinite by how far each moves it, the three
        // recoil values 1 each
        allPassed &= check(avgOf("Parallax Energy Assault Rifle",
                                 {{"Carinite", 959}, {"Gold", 916}, {"Tungsten", 964}}) == 954,
                           "Parallax from Carinite 959, Gold 916 and Tungsten 964 comes out at Avg. Q 954");
        // Pieces and SCU in one recipe, and damage that quality moves 7.5 %
        // against recoil's 40 %: the lenses still count as much as the frame
        allPassed &= check(avgOf("Lumin V SMG", {{"Riccite", 965}, {"Copper", 958}, {"Aphorite", 523}}) == 815,
                           "Lumin V SMG from Riccite 965, Copper 958 and Aphorite 523 comes out at Avg. Q 815");
        // A quantum drive is taken for its speed and fuel, not its Integrity
        allPassed &= check(avgOf("Agni", {{"Agricium", 588}, {"Tungsten", 964}, {"Beryl", 955}}) == 906,
                           "Agni with speed 964 and fuel 955 at Integrity 588 comes out at Avg. Q 906");
        // ...and what each slot counts, as the i beside the quality lists
        // it: Gold and Carinite share the Parallax's damage by how far each
        // moves it, 1 and 2 of its 3
        {
            bool shared = false;
            for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
                const auto& bp = SCMiner::CRAFT_BLUEPRINTS[b];
                if (std::string(bp.name) != "Parallax Energy Assault Rifle" || bp.count != 3) continue;
                SCMiner::CraftSlotCount c[3];
                if (!SCMiner::craftSlotCounts(bp, c)) break;
                double tungsten = -1, gold = -1, carinite = -1;
                for (int s = 0; s < 3; s++) {
                    const std::string mat = SCMiner::CRAFT_MATERIALS[SCMiner::CRAFT_SLOTS[bp.first + s].material].name;
                    if (mat == "Tungsten") tungsten = c[s].total;
                    if (mat == "Gold") gold = c[s].total;
                    if (mat == "Carinite") carinite = c[s].total;
                }
                shared = std::abs(tungsten - 3) < 0.01 && std::abs(gold - 1) < 0.01 && std::abs(carinite - 2) < 0.01;
            }
            allPassed &= check(shared, "Parallax: the Frame counts 3, Gold 1 and Carinite 2 - the two share the damage");
        }
        // A value whose curve is not known hides the number, rather than
        // weigh its slot as if any quality did
        int unknownSeen = 0;
        bool unknownHidden = true;
        for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
            const auto& bp = SCMiner::CRAFT_BLUEPRINTS[b];
            bool unknown = false;
            for (int s = 0; s < bp.count; s++) {
                const auto& slot = SCMiner::CRAFT_SLOTS[bp.first + s];
                for (int k = 0; k < slot.effCount; k++) {
                    if (SCMiner::effectCurveUnknown(SCMiner::CRAFT_EFFECTS[slot.effFirst + k])) unknown = true;
                }
            }
            if (!unknown || bp.count > 8) continue;
            unknownSeen++;
            const int all[8] = {700, 700, 700, 700, 700, 700, 700, 700};
            if (SCMiner::craftAverageQuality(bp, all, 8) != -1) unknownHidden = false;
        }
        allPassed &= check(unknownSeen > 0 && unknownHidden, "a blueprint with a curve not known shows no Avg. Q");
        // ...and as a letter along the game's quality bands, the top two as S
        bool letters = true;
        const std::pair<int, char> edges[] = {{1000, 'S'}, {950, 'S'}, {949, 'A'}, {900, 'A'}, {899, 'B'},
                                              {800, 'B'},  {799, 'C'}, {700, 'C'}, {699, 'D'}, {600, 'D'},
                                              {599, 'E'},  {400, 'E'}, {399, 'F'}, {0, 'F'}};
        for (const auto& [quality, want] : edges) {
            if (SCMiner::craftQualityGrade(quality) != want) letters = false;
        }
        allPassed &= check(letters, "Avg. Q as a letter: S from 950, A from 900, then one band each down to F below 400");
        // ...and a blueprint at one quality throughout is at that quality
        bool flat = true;
        for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
            const int all[8] = {700, 700, 700, 700, 700, 700, 700, 700};
            if (SCMiner::CRAFT_BLUEPRINTS[b].count > 8) continue;
            const int avg = SCMiner::craftAverageQuality(SCMiner::CRAFT_BLUEPRINTS[b], all, 8);
            if (avg != 700 && avg != -1) flat = false;
        }
        allPassed &= check(flat, "a blueprint made of quality 700 throughout averages 700");
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
        allPassed &= check(!bad.empty() && bad.find(L"Line 3") != std::wstring::npos,
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
        {
            // The install's own number: 32 hex characters, made once and kept
            const std::filesystem::path file = std::filesystem::temp_directory_path() / "bedrock_selftest_install_id.txt";
            std::error_code ec;
            std::filesystem::remove(file, ec);
            const std::string first = SCMiner::Updater::installId(file.string());
            const std::string again = SCMiner::Updater::installId(file.string());
            {
                std::ofstream spoilt(file, std::ios::trunc);
                spoilt << "not a number\n";
            }
            const std::string anew = SCMiner::Updater::installId(file.string());
            std::filesystem::remove(file, ec);
            allPassed &= check(SCMiner::Updater::validInstallId(first) && again == first &&
                                   SCMiner::Updater::validInstallId(anew) && anew != first,
                               "the install's number is 32 hex characters, kept, and made anew where the file "
                               "holds none");
            allPassed &= check(!SCMiner::Updater::validInstallId("") &&
                                   !SCMiner::Updater::validInstallId("0123456789ABCDEF0123456789abcdef") &&
                                   !SCMiner::Updater::validInstallId("0123456789abcdef0123456789abcde"),
                               "...and nothing else passes for one");
        }
        allPassed &= check(SCMiner::Updater::isNewer("4.10.0", "4.9.0"),
                           "4.10.0 counts as newer than 4.9.0");
        allPassed &= check(!SCMiner::Updater::isNewer("4.9.0", "4.10.0"),
                           "...and not the other way round");
        allPassed &= check(!SCMiner::Updater::isNewer("4.8.0", "4.8.0"),
                           "the same version is not an update");
        allPassed &= check(SCMiner::Updater::isNewer("4.8.1", "4.8.0"),
                           "...a patch release is");
        // What comes up after an update: 3.0's tour for everyone coming from
        // before it, never on a first start, and not twice
        {
            using SCMiner::Application;
            using W = Application::WhatsNew;
            allPassed &= check(Application::whatsNewAfter("2.7.0", "3.0.0", false) == W::Tour &&
                                   Application::whatsNewAfter("2.4.1", "3.0.0", false) == W::Tour &&
                                   Application::whatsNewAfter("", "3.0.0", false) == W::Tour &&
                                   Application::whatsNewAfter("2.7.0", "3.0.1", false) == W::Tour,
                               "2.7.0 -> 3.0.0 puts up the tour (from 2.4.1, from before the marker, "
                               "straight to a hotfix too)");
            allPassed &= check(Application::whatsNewAfter("3.0.0", "3.0.0", false) == W::Nothing &&
                                   Application::whatsNewAfter("", "3.0.0", true) == W::Nothing &&
                                   Application::whatsNewAfter("3.0.0", "3.0.1", false) == W::Card &&
                                   Application::whatsNewAfter("3.0.1", "3.1.0", false) == W::Card,
                               "...3.0.0 -> 3.0.0 and a first start nothing, 3.0.0 -> 3.0.1 and 3.1 the card");
            allPassed &= check(Application::whatsNewAfter("2.4.0", "2.7.0", false) == W::News &&
                                   Application::whatsNewAfter("2.6.6", "2.6.7", false) == W::Thanks &&
                                   Application::whatsNewAfter("2.6.7", "2.7.0", false) == W::Card,
                               "...and before 3.0 as before: news from before 2.5, 2.6.7's thanks, else the card");
        }
        // The Datarunner's introduction moving on by itself: each step read
        // for its time, 0.8 s to move to the next, the last one staying
        {
            using G = SCMiner::MinerGUI;
            const std::vector<int> reads = {6000, 5000, 7000};
            const G::IntroAt start = G::introAt(0, reads), moving = G::introAt(6400, reads),
                             second = G::introAt(6800, reads), last = G::introAt(12200, reads),
                             end = G::introAt(12600, reads);
            allPassed &= check(start.step == 0 && start.from == -1 && !start.done && start.nextMs == 6000 &&
                                   moving.step == 1 && moving.from == 0 && std::abs(moving.move - 0.5f) < 0.01f &&
                                   second.step == 1 && second.from == -1 && second.nextMs == 11800 &&
                                   last.step == 2 && last.from == 1 && std::abs(last.move - 0.5f) < 0.01f &&
                                   end.step == 2 && end.done && end.nextMs == 0,
                               "the Datarunner's introduction: step 1 read 6 s, halfway to step 2 at 6.4 s, "
                               "step 2 read from 6.8 s, the last reached at 12.6 s and kept (got step " +
                                   std::to_string(moving.step) + " at 6.4 s, " + std::to_string(last.step) +
                                   " at 12.2 s, done " + std::to_string(end.done) + ")");
            allPassed &= check(G::introReadMs(0) == 4500 && G::introReadMs(100) == 5000 && G::introReadMs(1000) == 8000,
                               "...a step read for 4.5 to 8 s by its words' length");
        }
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
        // own laser on a Prospector, more modules than the head has slots,
        // and a ship of three heads at all
        MinerGUI::ShipFit threeHeads;
        for (int i = 0; i < SCMiner::MINING_SHIP_COUNT; i++) {
            if (std::string(SCMiner::MINING_SHIPS[i].id) == "mole") threeHeads.ship = &SCMiner::MINING_SHIPS[i];
        }
        threeHeads.heads.assign(3, SCMiner::MiningLoadout{});
        const bool refused = !MinerGUI::fitTakes(flown, *pitman, {}) &&
                             !MinerGUI::fitTakes(flown, *hofstede, {focus3, rieger3}) &&
                             !MinerGUI::fitTakes(threeHeads, *helix, {}) && flown.heads[0].laser == helix &&
                             threeHeads.heads[0].laser == nullptr;
        allPassed &= check(refused, "a reading no ship of the kind could carry changes no fit: the Golem's own "
                                    "laser on a Prospector, more modules than slots, a MOLE");

        // ...but the Golem's head is not part of the ship: its gimbal takes
        // every S1 head, from the HUD too, and the Pitman it came with back.
        // The Pitman goes on no other ship, so only a Golem is told of it.
        {
            bool everyS1 = true;
            for (int i = 0; i < SCMiner::MINING_LASER_COUNT; i++) {
                const SCMiner::MiningLaser& l = SCMiner::MINING_LASERS[i];
                if (l.size == 1) everyS1 = everyS1 && SCMiner::laserFits(l, *golem);
            }
            MinerGUI::ShipFit golemFit = fits[2];
            const bool swaps = MinerGUI::fitTakes(golemFit, *helix, {focus3}) && golemFit.heads[0].laser == helix &&
                               golemFit.heads[0].modules == Modules{focus3} &&
                               MinerGUI::fitTakes(golemFit, *pitman, {}) && golemFit.heads[0].laser == pitman;
            SCMiner::RockStats rock;
            rock.mass = 9000.0f;
            rock.resistance = 20.0f;
            auto suggests = [&](const SCMiner::MiningShip* asking) {
                for (const SCMiner::LaserSuggestion& p : SCMiner::suggestLasers(rock, 1, 0.0f, asking)) {
                    if (p.laser == pitman) return true;
                }
                return false;
            };
            allPassed &= check(everyS1 && swaps && !SCMiner::laserFits(*pitman, *prospector) && suggests(golem) &&
                                   !suggests(prospector) && !suggests(nullptr),
                               "the Golem takes every S1 head, from its HUD too, and the Pitman back; the Pitman "
                               "goes on no other ship and is suggested to a Golem only");
        }

        // ...and no mount takes a head of another size: each one's record gives
        // the same MinSize as MaxSize - S2 on the MOLE's turrets, S1 on the
        // Prospector's and the Golem's gimbals - so the MOLE's list holds no S1
        // head and none is suggested to it, nor an S2 head to the others
        {
            const SCMiner::MiningShip* mole = SCMiner::shipById("mole");
            bool exact = mole != nullptr;
            int onMole = 0, onProspector = 0, onGolem = 0;
            for (int i = 0; exact && i < SCMiner::MINING_LASER_COUNT; i++) {
                const SCMiner::MiningLaser& l = SCMiner::MINING_LASERS[i];
                const bool m = SCMiner::laserFits(l, *mole);
                const bool p = SCMiner::laserFits(l, *prospector);
                const bool g = SCMiner::laserFits(l, *golem);
                exact = m == (l.size == 2) && p == (l.size == 1 && &l != pitman) && g == (l.size == 1);
                onMole += m ? 1 : 0;
                onProspector += p ? 1 : 0;
                onGolem += g ? 1 : 0;
            }
            SCMiner::RockStats rock;
            rock.mass = 30000.0f;
            rock.resistance = 40.0f;
            bool sized = exact;
            int named = 0;
            for (int pass = 0; sized && pass < 3; pass++) {
                const SCMiner::MiningShip* asking = pass == 0 ? mole : pass == 1 ? prospector : golem;
                for (const SCMiner::LaserSuggestion& s : SCMiner::suggestLasers(rock, asking->size, 0.0f, asking)) {
                    sized = sized && s.laser && SCMiner::laserFits(*s.laser, *asking);
                    named++;
                }
            }
            allPassed &= check(exact && sized && onMole > 0 && onProspector > 0 && named > 0,
                               "a mount takes its own size only: the MOLE " + std::to_string(onMole) +
                                   " S2 heads and no S1, the Prospector " + std::to_string(onProspector) +
                                   " and the Golem " + std::to_string(onGolem) +
                                   " S1 heads, and only those suggested (" + std::to_string(named) + ")");
        }

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
                               SCMiner::jumpDestination("LOC_rs_ext_stan-magnus_jp1") == "Nyx" &&
                               SCMiner::jumpDestination("rs_ext_nyx-castra_jp1") == "Stanton" &&
                               SCMiner::jumpDestination("pyro2").empty(),
                           "a jump point names the far side - Stanton's named for Magnus leads to Nyx, Nyx's named "
                           "for Castra to Stanton");
        {
            // ...and so do the gateways' codes: the Stanton Gateway in Nyx,
            // the Nyx Gateway in Stanton (a terminal opened at each, 31 Oct)
            const auto inNyx = placeByLocationCode("RR_JP_NyxCastra", "");
            const auto inStanton = placeByLocationCode("RR_JP_StantonMagnus", "");
            const auto atPoint = placeByDestination("rs_ext_nyx-castra_jp1", "");
            allPassed &= check(inNyx.name == "Stanton Gateway" && inNyx.system == "Nyx" &&
                                   inStanton.name == "Nyx Gateway" && inStanton.system == "Stanton" &&
                                   atPoint.name == "Stanton Gateway" && atPoint.system == "Nyx",
                               "RR_JP_NyxCastra is the Stanton Gateway in Nyx, RR_JP_StantonMagnus the Nyx Gateway in "
                               "Stanton (got " + inNyx.name + ", " + inStanton.name + ")");
            // From Nyx to Stanton without a terminal on the way, as the log
            // wrote it: after the crossing the ship is in Stanton, at the
            // gateway there - not in a system named for the jump point
            SCMiner::GameState crossing;
            auto put = [&](const char* line) {
                SCMiner::GameState::Event event;
                if (SCMiner::GameState::parse(line, &event)) crossing.apply(event);
            };
            put("<2026-10-31T16:23:14.789Z> [Notice] <Player Selected Quantum Target - Local> [ItemNavigation][CL][31008] | "
                "NOT AUTH | ESPR_Prowler_Utility_910484762854[910484762854]|CSCItemNavigation::OnPlayerSelectedQuantumTarget|"
                "Player has selected point rs_ext_nyx-castra_jp1 as their destination, routing locally "
                "[Team_CGP4][QuantumTravel]");
            put("<2026-10-31T16:23:14.789Z> [Notice] <Calculate Route> [ItemNavigation][CL][31008] | NOT AUTH | "
                "ESPR_Prowler_Utility_910484762854[910484762854]|CSCItemNavigation::CalculateRoute|Projected Start Location "
                "is Nyx System for route to destination rs_ext_nyx-castra_jp1 [Team_CGP4][QuantumTravel]");
            put("<2026-10-31T16:25:14.999Z> [Notice] <Quantum Drive Arrived - Arrived at Final Destination> "
                "[ItemNavigation][CL][31008] | NOT AUTH | ESPR_Prowler_Utility_910484762854[910484762854]|"
                "CSCItemNavigation::OnQuantumDriveArrived|Quantum Drive has arrived at final destination "
                "[Team_CGP4][QuantumTravel]");
            const bool before = crossing.system() == "Nyx" && crossing.place() == "Stanton Gateway";
            const std::string beforeSaid = crossing.system() + ", " + crossing.place();
            put("<2026-10-31T16:28:40.008Z> [Notice] <Calculate Route> [ItemNavigation][CL][31008] | NOT AUTH | "
                "ESPR_Prowler_Utility_910484762854[910484762854]|CSCItemNavigation::CalculateRoute|Projected Start Location "
                "is Nyx Gateway for route to destination ab_collector_gas_Stanton2 [Team_CGP4][QuantumTravel]");
            allPassed &= check(before && crossing.system() == "Stanton" && crossing.place() == "Nyx Gateway",
                               "from Nyx to Stanton: at the jump point still Nyx (got " + beforeSaid +
                                   "), after it Stanton at the Nyx Gateway (got " + crossing.system() + ", " +
                                   crossing.place() + ")");
        }
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
            // ...and an S1 head written under the MOLE, whose turrets take S2
            std::istringstream moleFile("ship=mole\n[mole]\nhead=thcn_helix_s1\nhead=thcn_helix_s2\nhead=shin_hofstede_s2\n");
            const MinerGUI::LoadoutFile kept = MinerGUI::readLoadoutFile(moleFile);
            allPassed &= check(kept.fits.size() == 1 && kept.fits[0].heads.size() == 3 && !kept.fits[0].heads[0].laser &&
                                   kept.fits[0].heads[1].laser == SCMiner::laserById("thcn_helix_s2") &&
                                   kept.fits[0].heads[2].laser == SCMiner::laserById("shin_hofstede_s2"),
                               "...and an S1 head written under the MOLE as well, the other two kept");
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
            // ...and a MOLE turret's: the line over its table names the head
            // the player flies, a laser of size 2. Three turrets' HUDs at 5120 x
            // 1440, cut and put back where they were on a black screen - the
            // Helix II's with a module's lines above it, read and passed over.
            {
                auto turretName = [](const char* line) {
                    const SCMiner::MiningLaser* l = SCMiner::turretLaserByHudLine(line);
                    return std::string(l ? l->name : "-");
                };
                allPassed &= check(turretName("HOFSTEDE-S2 MINING LASER") == "Hofstede-S2 Mining Laser" &&
                                       turretName(" HOFSTEDE-S2 MINING LASER ") == "Hofstede-S2 Mining Laser" &&
                                       turretName("HELIX II MINING LASER") == "Helix II Mining Laser" &&
                                       turretName("IMPACT II MINING LASER") == "Impact II Mining Laser" &&
                                       turretName("HELIX U MINING LASER") == "Helix II Mining Laser" &&
                                       turretName("Laser Power (+25%)") == "-" &&
                                       turretName("C\" RIEGER-C3 MODOLE") == "-" &&
                                       turretName("PITMAN MINING LASER") == "-",
                                   "a turret's laser line names a laser of size 2, and no other line does");
                const struct {
                    const char* file;
                    const wchar_t* resource;
                    int x, y;
                    const char* laser;
                } turrets[] = {
                    {"scanpanel/turret_laser_hofstede.png", L"SELFTEST_TURRET_HOFSTEDE", 1310, 1100, "Hofstede-S2 Mining Laser"},
                    {"scanpanel/turret_laser_helix.png", L"SELFTEST_TURRET_HELIX", 1320, 930, "Helix II Mining Laser"},
                    // ...over bright, rippled sand (8 November 2026), whose ripples
                    // stand out as much as letters do (lightRows)
                    {"scanpanel/turret_laser_helix_sand.jpg", L"SELFTEST_TURRET_HELIX_SAND", 1356, 918,
                     "Helix II Mining Laser"},
                };
                for (const auto& t : turrets) {
                    const cv::Mat cut = testPicture(t.file, t.resource);
                    allPassed &= check(!cut.empty(), std::string("the turret HUD's picture ") + t.file +
                                                         " is beside the build or inside the exe");
                    if (cut.empty() || !reader) continue;
                    cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                    cut.copyTo(screen(cv::Rect(t.x, t.y, cut.cols, cut.rows)));
                    std::string line;
                    const SCMiner::MiningLaser* laser = SCMiner::readTurretLaser(screen, *reader, &line);
                    allPassed &= check(laser && std::string(laser->name) == t.laser,
                                       std::string("...and a turret's HUD reads as ") + t.laser + " (got '" + line + "')");
                }
                if (reader) {
                    const cv::Mat blank(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                    allPassed &= check(SCMiner::readTurretLaser(blank, *reader) == nullptr,
                                       "...and a screen with no turret's HUD names no laser");
                }
            }
        }

        // The LASER RANGE gauge: the figure beside its white triangle, read
        // numeral by numeral, the gauge placed from the scan panel's labels.
        // A cut of the Prospector's at 27 m, put back where it was on a
        // 5120 x 1440 screen with the labels at 3096,699 at size 1.001.
        {
            const cv::Mat gauge = testPicture("scanpanel/gauge_prospector_27.png", L"SELFTEST_GAUGE_PROSPECTOR");
            allPassed &= check(!gauge.empty(), "the Prospector's LASER RANGE picture is beside the build or inside the exe");
            if (!gauge.empty()) {
                const cv::Rect at(2236, 568, gauge.cols, gauge.rows);
                cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                gauge.copyTo(screen(at));
                SCMiner::ScanPanel panel;
                panel.labels = cv::Point(3096, 699);
                panel.labelScale = 1.001;
                panel.labelGlyph = 'L';
                const SCMiner::LaserRangeLook look = SCMiner::readLaserRange(screen, panel);
                allPassed &= check(look.metres == 27, "the Prospector's LASER RANGE gauge reads 27 m (got " +
                                                          std::to_string(look.metres) + ": " + look.note + ")");
                // the triangle without its figure is no reading
                cv::Mat bare(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                gauge.colRange(0, 22).copyTo(bare(cv::Rect(at.x, at.y, 22, at.height)));
                const SCMiner::LaserRangeLook none = SCMiner::readLaserRange(bare, panel);
                // ...nor a figure that stands where another one belongs: the
                // labels 30 units lower put 27 m some ten degrees off its place
                SCMiner::ScanPanel lower = panel;
                lower.labels.y += 30;
                const SCMiner::LaserRangeLook astray = SCMiner::readLaserRange(screen, lower);
                // ...nor a HUD whose gauge is not known: the MOLE's pilot seat
                // has no laser
                SCMiner::ScanPanel seat = panel;
                seat.labelGlyph = 'K';
                const SCMiner::LaserRangeLook pilot = SCMiner::readLaserRange(screen, seat);
                allPassed &= check(none.metres == 0 && astray.metres == 0 && pilot.metres == 0,
                                   "...and nothing for the triangle alone, for 27 m off its place, or beside the MOLE "
                                   "pilot's labels (got " + std::to_string(none.metres) + ", " +
                                       std::to_string(astray.metres) + ", " + std::to_string(pilot.metres) + ")");
            }
        }
        // ...and the current patch's HUD: a red 43 m on blue space - where
        // the most coloured pixels are the ground's - 21 units further left
        // of the labels, here at 3108,696 at size 1.003
        {
            const cv::Mat gauge = testPicture("scanpanel/gauge_prospector_43.png", L"SELFTEST_GAUGE_PROSPECTOR_43");
            allPassed &= check(!gauge.empty(), "the current Prospector HUD's LASER RANGE picture is beside the build or "
                                               "inside the exe");
            if (!gauge.empty()) {
                cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                gauge.copyTo(screen(cv::Rect(2215, 584, gauge.cols, gauge.rows)));
                SCMiner::ScanPanel panel;
                panel.labels = cv::Point(3108, 696);
                panel.labelScale = 1.003;
                panel.labelGlyph = 'L';
                const SCMiner::LaserRangeLook look = SCMiner::readLaserRange(screen, panel);
                SCMiner::ScanPanel lower = panel;
                lower.labels.y += 30;
                const SCMiner::LaserRangeLook astray = SCMiner::readLaserRange(screen, lower);
                allPassed &= check(look.metres == 43 && astray.metres == 0,
                                   "...its red 43 m on blue reads 43, and nothing 30 units off its place (got " +
                                       std::to_string(look.metres) + ", " + std::to_string(astray.metres) + ": " +
                                       look.note + ")");
            }
        }
        // ...and the same HUD as the app sees it on an HDR desktop, tone
        // mapped (DxgiCapture): a green 12 m whose triangle tops out near 190,
        // cut from the app's own scan recording of 30.10.2026 with the labels
        // it found in that look, 3093,642 at size 0.965. Taken for what it is,
        // the triangle is not white enough to be found.
        {
            const cv::Mat gauge = testPicture("scanpanel/gauge_prospector_live12.png", L"SELFTEST_GAUGE_PROSPECTOR_LIVE");
            allPassed &= check(!gauge.empty(), "the tone-mapped LASER RANGE picture is beside the build or inside the exe");
            if (!gauge.empty()) {
                cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                gauge.copyTo(screen(cv::Rect(2256, 498, gauge.cols, gauge.rows)));
                SCMiner::ScanPanel panel;
                panel.labels = cv::Point(3093, 642);
                panel.labelScale = 0.965;
                panel.labelGlyph = 'L';
                const SCMiner::LaserRangeLook look = SCMiner::readLaserRange(screen, panel, true);
                const SCMiner::LaserRangeLook asIs = SCMiner::readLaserRange(screen, panel, false);
                allPassed &= check(look.metres == 12 && asIs.metres == 0,
                                   "...tone mapped, its green 12 m reads 12, and nothing taken as an SDR picture (got " +
                                       std::to_string(look.metres) + ", " + std::to_string(asIs.metres) + ": " +
                                       look.note + ")");
            }
        }
        // the way back from the tone map: SDR white (137) and above is 255,
        // below it the level an SDR capture holds
        {
            const int black = SCMiner::ToneMap::sdrLevel(0), white = SCMiner::ToneMap::sdrLevel(137),
                      above = SCMiner::ToneMap::sdrLevel(190), grey = SCMiner::ToneMap::sdrLevel(63);
            allPassed &= check(black == 0 && white == 255 && above == 255 && grey == 123,
                               "the tone map undone: 0, 137, 190 and 63 come back as 0, 255, 255 and 123 (got " +
                                   std::to_string(black) + ", " + std::to_string(white) + ", " + std::to_string(above) +
                                   ", " + std::to_string(grey) + ")");
        }
        // ...and a MOLE turret seat's, whose figure runs down a near straight
        // line: a red 16 m on rock, the labels at 3436,602 at size 1.468
        {
            const cv::Mat gauge = testPicture("scanpanel/gauge_turret_16.png", L"SELFTEST_GAUGE_TURRET");
            allPassed &= check(!gauge.empty(), "the MOLE turret's LASER RANGE picture is beside the build or inside "
                                               "the exe");
            if (!gauge.empty()) {
                cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                gauge.copyTo(screen(cv::Rect(2180, 488, gauge.cols, gauge.rows)));
                SCMiner::ScanPanel panel;
                panel.labels = cv::Point(3436, 602);
                panel.labelScale = 1.468;
                panel.labelGlyph = 'T';
                const SCMiner::LaserRangeLook look = SCMiner::readLaserRange(screen, panel);
                // the labels 30 units lower put 16 m some 27 units along the
                // line off its place
                SCMiner::ScanPanel lower = panel;
                lower.labels.y += 44;
                const SCMiner::LaserRangeLook astray = SCMiner::readLaserRange(screen, lower);
                // ...and the Prospector's gauge is elsewhere
                SCMiner::ScanPanel prospector = panel;
                prospector.labelGlyph = 'L';
                const SCMiner::LaserRangeLook other = SCMiner::readLaserRange(screen, prospector);
                allPassed &= check(look.metres == 16 && astray.metres == 0 && other.metres == 0,
                                   "...a MOLE turret's red 16 m reads 16, and nothing 30 units off its place or "
                                   "beside a Prospector's labels (got " +
                                       std::to_string(look.metres) + ", " + std::to_string(astray.metres) + ", " +
                                       std::to_string(other.metres) + ": " + look.note + ")");
            }
        }
        // ...and the Golem's, with its long labels at 3216,543 at size
        // 1.242: a green 33 m, the figure leaning 32 degrees down the arc
        {
            const cv::Mat gauge = testPicture("scanpanel/gauge_golem_33.png", L"SELFTEST_GAUGE_GOLEM");
            allPassed &= check(!gauge.empty(), "the Golem's LASER RANGE picture is beside the build or inside the exe");
            if (!gauge.empty()) {
                cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                gauge.copyTo(screen(cv::Rect(2156, 623, gauge.cols, gauge.rows)));
                SCMiner::ScanPanel panel;
                panel.labels = cv::Point(3216, 543);
                panel.labelScale = 1.242;
                panel.labelGlyph = 'H';
                const SCMiner::LaserRangeLook look = SCMiner::readLaserRange(screen, panel);
                // the labels 60 units lower put 33 m where some 10 m belongs
                SCMiner::ScanPanel lower = panel;
                lower.labels.y += 75;
                const SCMiner::LaserRangeLook astray = SCMiner::readLaserRange(screen, lower);
                allPassed &= check(look.metres == 33 && astray.metres == 0,
                                   "...the Golem's green 33 m reads 33, and nothing 60 units off its place (got " +
                                       std::to_string(look.metres) + ", " + std::to_string(astray.metres) + ": " +
                                       look.note + ")");
            }
        }
        // ...but a pale 78 on rock, whose 8 matches the 4's picture at 0.82
        // and its own at 0.67, is not taken for 74 (labels at 3190,546 at
        // size 1.217), nor a 118 whose 8 matches the 4's at 0.87, 0.10
        // ahead of a 3, for 114 (labels at 3202,529 at the same size)
        {
            const cv::Mat gauge = testPicture("scanpanel/gauge_golem_78.png", L"SELFTEST_GAUGE_GOLEM_78");
            const cv::Mat gauge118 = testPicture("scanpanel/gauge_golem_118.png", L"SELFTEST_GAUGE_GOLEM_118");
            allPassed &= check(!gauge.empty() && !gauge118.empty(),
                               "the Golem's pale 78 and 118 are beside the build or inside the exe");
            if (!gauge.empty() && !gauge118.empty()) {
                cv::Mat screen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                gauge.copyTo(screen(cv::Rect(2131, 679, gauge.cols, gauge.rows)));
                SCMiner::ScanPanel panel;
                panel.labels = cv::Point(3190, 546);
                panel.labelScale = 1.217;
                panel.labelGlyph = 'H';
                const SCMiner::LaserRangeLook look = SCMiner::readLaserRange(screen, panel);
                cv::Mat screen2(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                gauge118.copyTo(screen2(cv::Rect(2130, 704, gauge118.cols, gauge118.rows)));
                SCMiner::ScanPanel panel2 = panel;
                panel2.labels = cv::Point(3202, 529);
                const SCMiner::LaserRangeLook look2 = SCMiner::readLaserRange(screen2, panel2);
                allPassed &= check((look.metres == 0 || look.metres == 78) &&
                                       (look2.metres == 0 || look2.metres == 118),
                                   "...and they read 78 and 118 or nothing, never 74 or 114 (got " +
                                       std::to_string(look.metres) + ", " + std::to_string(look2.metres) + ": " +
                                       look.note + "; " + look2.note + ")");
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
                    // The MOLE's turret by its own numerals, and the volume
                    // under its EASY bar with them, in the same numerals:
                    // COMP.  19.35 SCU. Not read before 20 October 2026 - the
                    // turret's rocks came in per cents, not SCU (andydsm).
                    const cv::Mat copper =
                        testPicture("scanpanel/panel_turret_copper.png", L"SELFTEST_TURRET_VOLUME");
                    allPassed &= check(!copper.empty(),
                                       "the MOLE turret's panel with its volume is beside the build or inside the exe");
                    if (!copper.empty()) {
                        cv::Mat towerScreen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                        copper.copyTo(towerScreen(cv::Rect(3395, 515, copper.cols, copper.rows)));
                        SCMiner::ScanPanel tower;
                        for (int look = 0; look < 4 && !tower.found; look++) {
                            tower = SCMiner::ScanPanel();
                            SCMiner::readPanelByShape(towerScreen, tower);
                        }
                        const SCMiner::RockStats& c = tower.stats;
                        allPassed &= check(tower.found && c.mass == 21126.0f && c.resistance == 0.0f &&
                                               std::abs(c.instability - 23.23f) < 0.005f &&
                                               std::abs(c.scu - 19.35f) < 0.005f,
                                           "...and the turret's figures by their shapes, its volume with them: 21126, "
                                           "0 %, 23.23 and 19.35 SCU (got " + std::to_string((int)c.mass) + ", " +
                                               std::to_string((int)c.resistance) + " %, " +
                                               std::to_string(c.instability) + ", " + std::to_string(c.scu) + ")");
                    }
                    // ...and a rock under one SCU, whose volume the turret
                    // writes in thousandths: COMP.  582.28m SCU. Not read
                    // before 21 October 2026 - its parts came in per cents
                    // (andydsm); the m puts every numeral an m's width
                    // further left.
                    const cv::Mat milli =
                        testPicture("scanpanel/panel_turret_milli.png", L"SELFTEST_TURRET_MILLI");
                    allPassed &= check(!milli.empty(),
                                       "the MOLE turret's panel of a rock under one SCU is beside the build or "
                                       "inside the exe");
                    if (!milli.empty()) {
                        cv::Mat towerScreen(1440, 5120, CV_8UC3, cv::Scalar(0, 0, 0));
                        milli.copyTo(towerScreen(cv::Rect(3395, 430, milli.cols, milli.rows)));
                        SCMiner::ScanPanel tower;
                        for (int look = 0; look < 4 && !tower.found; look++) {
                            tower = SCMiner::ScanPanel();
                            SCMiner::readPanelByShape(towerScreen, tower);
                        }
                        const SCMiner::RockStats& c = tower.stats;
                        allPassed &= check(tower.found && c.mass == 745.0f && std::abs(c.scu - 0.58228f) < 0.000005f,
                                           "...and a volume in thousandths: 745 kg and 582.28m SCU, 0.58228 SCU (got " +
                                               std::to_string((int)c.mass) + " kg, " + std::to_string(c.scu) + ")");
                    }
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

        // From the rock back to the signatures: a signature read after a look
        // that found nothing takes the window back at once. In a session of
        // 7 rocks the first signature came 2.5 to 7 s after the last panel,
        // the eight looks took some 10 s.
        {
            SCMiner::RockViewHold hold;
            hold.read();
            const bool early = hold.missed();
            allPassed &= check(!early && hold.signatureRead(),
                               "a signature read after a look that found no panel takes the window back at once");
            hold.read();
            allPassed &= check(!hold.signatureRead(), "...not while the last look read the panel");
            hold.missed();
            hold.seen();
            allPassed &= check(!hold.signatureRead(), "...nor once a look found its labels again");
            hold.read();
            int back = 0;
            for (int i = 1; i <= 10 && back == 0; i++) {
                if (hold.missed()) back = i;
            }
            allPassed &= check(back == 8, "...and without a signature, eight looks that find nothing take it back (" +
                                              std::to_string(back) + ")");
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
                                       needed.because.find("Required") == 0,
                                   "a stock Helix I cannot hold the quantainium, and a Sabir is " +
                                       (needed.because.empty() ? std::string("?") : needed.because));
                bal.powerThrough = [&helixOne](float own) {
                    const SCMiner::LaserOutput o = SCMiner::outputOf(helixOne, 0);
                    return o.atRange * SCMiner::resistanceLetsThrough(own, o.resistance);
                };
                const SCMiner::GadgetAdvice helps = SCMiner::advise(quant, helixOne, &bal);
                allPassed &= check(helps.gadget == sabir && helps.because.find("Recommended") == 0,
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

        // The MOLE's Hofstede-S2, crafted +21.72 %, FLTR-XL and Rieger-C3, off
        // the turret recordings of 17.9.2026: where the bar stood still, or
        // the throttle it fell at and the one it climbed at, at the distance
        // the LASER RANGE pointer showed. Two-sided like the Helix I rocks,
        // with the same 3 points of slack.
        {
            SCMiner::MiningLoadout hof;
            hof.laser = SCMiner::laserById("shin_hofstede_s2");
            hof.craftedPower = 21.72f;
            hof.modules = {SCMiner::moduleById("passive_fltr_mk3"),
                           SCMiner::moduleById("passive_rieger_mk3")};
            const SCMiner::LaserOutput close = SCMiner::outputOf(hof, 0);
            allPassed &= check(hof.laser && std::abs(close.fracture - 6177.0f) < 2.0f &&
                                   std::abs(close.stock - 5075.0f) < 2.0f,
                               "the crafted Hofstede-S2 fit puts out 6,177 (5,075 stock)");
            struct Stood { float mass, green, metres, lo, hi; const char* what; };
            for (const Stood& rock : {Stood{21126.0f, 0.0f, 20.0f, 66.0f, 72.0f, "21,126 kg of copper at 20 m"},
                                      Stood{25175.0f, 0.0f, 51.0f, 78.0f, 82.0f, "25,175 kg of copper at 51 m"},
                                      Stood{14721.0f, 0.0f, 69.0f, 50.0f, 52.0f, "14,721 kg of copper at 69 m"},
                                      Stood{4049.0f, 0.0f, 19.0f, 12.0f, 17.0f, "4,049 kg of copper at 19 m"},
                                      Stood{3493.0f, 0.0f, 54.0f, 11.0f, 13.0f, "3,493 kg of copper at 54 m"},
                                      Stood{10542.0f, 34.0f, 33.0f, 46.0f, 47.2f, "10,542 kg at 34 % green, 33 m"},
                                      Stood{3050.0f, 40.0f, 40.0f, 12.0f, 17.8f, "3,050 kg at 40 % green, 40 m"},
                                      Stood{2380.0f, 26.0f, 86.0f, 10.0f, 11.0f, "2,380 kg of taranite at 26 % green, 86 m"}}) {
                const SCMiner::LaserOutput o = SCMiner::outputOf(hof, rock.metres);
                const float own = SCMiner::rockOwnResistance(rock.green, true, o.resistance / 100.0f);
                const SCMiner::ThrottleAdvice held = SCMiner::throttleFor(
                    rock.mass, hof.laser, o.atRange * SCMiner::resistanceLetsThrough(own, o.resistance));
                allPassed &= check(held.possible && held.toHold >= rock.lo - 3.0f && held.toHold <= rock.hi + 3.0f,
                                   std::string(rock.what) + " held between " + std::to_string((int)std::lround(rock.lo)) +
                                       " and " + std::to_string((int)std::lround(rock.hi)) + " % (model says " +
                                       std::to_string((int)std::lround(held.toHold)) + " %)");
            }
            // ...and the crafting bonus is what makes it so: stock, the
            // 25,175 kg rock would ask 99 %, and it stood at 80
            const SCMiner::ThrottleAdvice asStock = SCMiner::throttleFor(
                25175.0f, hof.laser, close.stock * SCMiner::rangeFalloff(*hof.laser, 51.0f));
            allPassed &= check(asStock.toHold > 82.0f + 10.0f,
                               "...read stock, the 25,175 kg rock would ask " +
                                   std::to_string((int)std::lround(asStock.toHold)) + " % - the crafting bonus counts");
        }

        // The Prospector's Helix I crafted +30.96 %, Focus III and Rieger-C3,
        // off the recordings of 29.10.2026. 29,945 kg of iron at 0 % and
        // 11-12 m charged at 100 %, slowly - what the app says past
        // BREAK_COMFORTABLE. Read stock, the head could not have held it.
        {
            SCMiner::MiningLoadout helix;
            helix.laser = SCMiner::laserById("thcn_helix_s1");
            helix.craftedPower = 30.96f;
            helix.modules = {SCMiner::moduleById("passive_focus_mk3"),
                             SCMiner::moduleById("passive_rieger_mk3")};
            const SCMiner::LaserOutput o = SCMiner::outputOf(helix, 11.5f);
            allPassed &= check(helix.laser && std::abs(o.atRange - 6065.0f) < 2.0f &&
                                   std::abs(o.stock - 4631.0f) < 2.0f,
                               "the crafted Helix I fit puts out 6,065 at 11.5 m (4,631 stock)");
            const SCMiner::ThrottleAdvice iron = SCMiner::throttleFor(29945.0f, helix.laser, o.atRange);
            allPassed &= check(iron.possible && iron.toHold / 100.0f > SCMiner::BREAK_COMFORTABLE,
                               "29,945 kg of iron at 11.5 m charged slowly at 100 % (model says " +
                                   std::to_string((int)std::lround(iron.toHold)) + " %)");
            const SCMiner::ThrottleAdvice ironStock = SCMiner::throttleFor(29945.0f, helix.laser, o.stock);
            allPassed &= check(!ironStock.possible,
                               "...read stock, it would ask " + std::to_string((int)std::lround(ironStock.toHold)) +
                                   " % - the crafting bonus counts");
            // 356 kg of ice 2 m short of the head's 45 m stood in the optimal
            // window at 20-24 %, the beam at 42.6-43.4 m: somewhere in that
            // stretch the straight falloff has to reach the bracket
            const float nearer =
                SCMiner::throttleFor(356.0f, helix.laser, SCMiner::outputOf(helix, 42.6f).atRange).toHold;
            const float further =
                SCMiner::throttleFor(356.0f, helix.laser, SCMiner::outputOf(helix, 43.4f).atRange).toHold;
            allPassed &= check(nearer <= 24.0f && further >= 20.0f,
                               "356 kg of ice at 42.6-43.4 m stood at 20-24 % (model says " +
                                   std::to_string((int)std::lround(nearer)) + "-" +
                                   std::to_string((int)std::lround(further)) + " %)");
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
        // A place in a list opens what else is mined there: Terminus has
        // ship ores, a ROC deposit's gem and hand gems, listed in that order
        // and each family best chance first
        const std::string terminus = "Pyro VI (Terminus)";
        const size_t ship = SCMiner::oresAt(terminus, SCMiner::MiningFamily::Ship).size();
        const size_t roc = SCMiner::oresAt(terminus, SCMiner::MiningFamily::ROC).size();
        const size_t hand = SCMiner::oresAt(terminus, SCMiner::MiningFamily::FPS).size();
        allPassed &= check(ship > 0 && roc > 0 && hand > 0,
                           "Terminus has ship ores, ROC gems and hand gems (" + std::to_string(ship) + ", " +
                               std::to_string(roc) + ", " + std::to_string(hand) + ")");
        const auto card = SCMiner::MinerGUI::minedAt(terminus);
        bool inOrder = card.size() == ship + roc + hand;
        for (size_t i = 1; inOrder && i < card.size(); i++) {
            inOrder = (int)card[i - 1].family < (int)card[i].family ||
                      (card[i - 1].family == card[i].family && card[i - 1].chance >= card[i].chance);
        }
        allPassed &= check(inOrder, "A place lists its ship ores, then ROC gems, then hand gems, best chance first");
        // ...and every place a list names has the listed mineral on its own list
        int unlisted = 0;
        for (const auto& row : SCMiner::ORE_LOCATIONS) {
            const auto there = SCMiner::MinerGUI::minedAt(row.location);
            if (std::none_of(there.begin(), there.end(), [&](const SCMiner::OreLocation& o) {
                    return o.family == row.family && std::strcmp(o.mineral, row.mineral) == 0;
                })) {
                unlisted++;
            }
        }
        allPassed &= check(unlisted == 0, "Every place a list names has that mineral on its own list (" +
                                              std::to_string(unlisted) + " missing)");
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
        // ...and the three the extract leaves out and the terminal shows
        // (gen_refineries.py CORRECTIONS): Lindinium is best at MIC-L5, not
        // at Levski's +4
        {
            auto material = [](const char* name) {
                for (int m = 0; m < SCMiner::REFINERY_MATERIAL_COUNT; m++) {
                    if (std::string(SCMiner::REFINERY_MATERIALS[m]) == name) return m;
                }
                return -1;
            };
            const int lindinium = material("Lindinium (Ore)"), savrilium = material("Savrilium (Ore)"),
                      torite = material("Torite (Ore)");
            int best = -1, bestValue = SCMiner::REFINERY_NONE;
            for (int s = 0; s < SCMiner::REFINERY_COUNT && lindinium >= 0; s++) {
                const int v = SCMiner::refineryYield(s, lindinium);
                if (v != SCMiner::REFINERY_NONE && (bestValue == SCMiner::REFINERY_NONE || v > bestValue)) {
                    bestValue = v;
                    best = s;
                }
            }
            allPassed &= check(icarus >= 0 && lindinium >= 0 && savrilium >= 0 && torite >= 0 &&
                                   SCMiner::refineryYield(icarus, lindinium) == 7 &&
                                   SCMiner::refineryYield(icarus, savrilium) == 6 &&
                                   SCMiner::refineryYield(icarus, torite) == 8 && best == icarus,
                               "MIC-L5 refines Lindinium +7, Savrilium +6, Torite +8 as its terminal does, and is "
                               "the best for Lindinium");
        }
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

    // --- The phone or tablet: Bedrock's page and pictures over the home
    //     network (PhoneServer) - what it reads of a request, where it sends
    //     it, the code to scan, and the server itself, on this machine alone ---
    {
        using SCMiner::PhoneServer;
        PhoneServer::Request r;
        allPassed &= check(PhoneServer::parseRequest("GET /0123456789ab/state?view=window&w=900 HTTP/1.1\r\nHost: x\r\n\r\n",
                                                     r) &&
                               r.method == "GET" && r.path == "/0123456789ab/state" && r.query == "view=window&w=900",
                           "phone: a request's method, path and query are read off its first line");
        allPassed &= check(!PhoneServer::parseRequest("GET\r\n\r\n", r) &&
                               !PhoneServer::parseRequest("GET http://x/ HTTP/1.1\r\n\r\n", r) &&
                               !PhoneServer::parseRequest("hello there\r\n\r\n", r),
                           "phone: what is not a request, or asks for another server, is not taken for one");
        allPassed &= check(PhoneServer::queryValue("view=window&w=900", "w") == "900" &&
                               PhoneServer::queryValue("view=window&w=900", "view") == "window" &&
                               PhoneServer::queryValue("view=window&w=900", "n").empty() &&
                               PhoneServer::queryValue("", "w").empty(),
                           "phone: a value out of the query, and none where there is none");
        const std::string token = "0123456789ab";
        const auto routeOf = [&](const std::string& path) {
            PhoneServer::Request q;
            q.method = "GET";
            q.path = path;
            return PhoneServer::route(q, token);
        };
        allPassed &= check(routeOf("/0123456789ab/") == PhoneServer::Route::Page &&
                               routeOf("/0123456789ab") == PhoneServer::Route::AddSlash &&
                               routeOf("/0123456789ab/state") == PhoneServer::Route::State &&
                               routeOf("/0123456789ab/frame") == PhoneServer::Route::Frame &&
                               routeOf("/0123456789ab/tap") == PhoneServer::Route::Tap,
                           "phone: the page, its state, its pictures and its taps are behind the token");
        allPassed &= check(routeOf("/") == PhoneServer::Route::NotFound &&
                               routeOf("/0123456789ac/") == PhoneServer::Route::NotFound &&
                               routeOf("/0123456789a/") == PhoneServer::Route::NotFound &&
                               routeOf("/0123456789ab/../state") == PhoneServer::Route::NotFound &&
                               routeOf("/favicon.ico") == PhoneServer::Route::NotFound,
                           "phone: without the token, or with a wrong one, nothing is there");
        allPassed &= check(PhoneServer::jsonString("a\"b\\c\nd\x01") == "\"a\\\"b\\\\c\\nd\\u0001\"",
                           "phone: a string escaped for JSON");
        const std::string t1 = PhoneServer::newToken(), t2 = PhoneServer::newToken();
        allPassed &= check(t1.size() == 12 && PhoneServer::tokenOk(t1) && t1 != t2 && !PhoneServer::tokenOk("0123") &&
                               !PhoneServer::tokenOk("0123456789AB") && !PhoneServer::tokenOk("01234567/9ab"),
                           "phone: a token is twelve hex digits, new every time (" + t1 + ")");
        // the code to scan: a size QR codes come in, and the three finder
        // patterns where a camera looks for them
        int n = 0;
        const std::vector<bool> qr = PhoneServer::qrCode("http://192.168.178.20:47820/0123456789ab/", n);
        bool finders = n >= 21 && (n - 17) % 4 == 0 && qr.size() == (size_t)n * (size_t)n;
        for (const auto& corner : {std::pair<int, int>{0, 0}, std::pair<int, int>{n - 7, 0}, std::pair<int, int>{0, n - 7}}) {
            for (int y = 0; finders && y < 7; y++) {
                for (int x = 0; finders && x < 7; x++) {
                    const bool ring = x == 0 || y == 0 || x == 6 || y == 6;
                    const bool core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
                    if (qr[(size_t)(corner.second + y) * (size_t)n + (size_t)(corner.first + x)] != (ring || core)) {
                        finders = false;
                    }
                }
            }
        }
        allPassed &= check(finders, "phone: the code for an address is a QR code (" + std::to_string(n) + " modules a side)");
        // a picture as a PNG, and back as it was
        PhoneServer::Picture picture;
        picture.width = 3;
        picture.height = 2;
        picture.bgra = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 16, 16, 15, 255, 241, 239, 238, 255, 92, 186, 224, 255};
        const std::vector<uint8_t> png = PhoneServer::png(picture);
        const cv::Mat back = png.empty() ? cv::Mat() : cv::imdecode(png, cv::IMREAD_COLOR);
        bool same = !back.empty() && back.cols == 3 && back.rows == 2;
        for (int i = 0; same && i < 6; i++) {
            const cv::Vec3b px = back.at<cv::Vec3b>(i / 3, i % 3);
            same = px[0] == picture.bgra[(size_t)i * 4] && px[1] == picture.bgra[(size_t)i * 4 + 1] &&
                   px[2] == picture.bgra[(size_t)i * 4 + 2];
        }
        allPassed &= check(png.size() > 8 && png[0] == 0x89 && png[1] == 'P' && same,
                           "phone: a picture goes as a PNG, every pixel as it was");

        // the server, listening on this machine alone - which asks Windows
        // for nothing
        PhoneServer server;
        const bool up = server.start(token, 0, true);
        const int port = server.port();
        allPassed &= check(up && port > 0, "phone: the server listens (port " + std::to_string(port) + ")");
        if (up) {
            const std::string page = PhoneServer::fetchLocal(port, "/" + token + "/");
            allPassed &= check(page.rfind("HTTP/1.1 200 OK", 0) == 0 && page.find("<!doctype html>") != std::string::npos &&
                                   page.find("Content-Security-Policy:") != std::string::npos,
                               "phone: the page comes, behind the token");
            const std::string wrong = PhoneServer::fetchLocal(port, "/0123456789ac/");
            allPassed &= check(wrong.rfind("HTTP/1.1 404", 0) == 0 && wrong.find("<!doctype") == std::string::npos,
                               "phone: a wrong token gets nothing");
            const std::string slash = PhoneServer::fetchLocal(port, "/" + token);
            allPassed &= check(slash.rfind("HTTP/1.1 301", 0) == 0 &&
                                   slash.find("Location: /" + token + "/\r\n") != std::string::npos,
                               "phone: the address without its last slash leads to the page");
            allPassed &= check(!server.wanted(PhoneServer::View::Overlay), "phone: nothing is wanted before a page asks");
            const std::string before = PhoneServer::fetchLocal(port, "/" + token + "/state?view=overlay&w=1170");
            int pixels = 0;
            const bool wanted = server.wanted(PhoneServer::View::Overlay, &pixels);
            allPassed &= check(before.find("{\"frame\":0,\"control\":false}") != std::string::npos && wanted &&
                                   pixels == 1170 &&
                                   !server.wanted(PhoneServer::View::Window) && server.watching() == 1,
                               "phone: a page asking for the overlay is looking at it, at its width (" +
                                   std::to_string(pixels) + ")");
            server.setStatus("\"bg\":\"#0F1013\"");
            const bool fresh = server.publish(PhoneServer::View::Overlay, picture);
            const bool again = server.publish(PhoneServer::View::Overlay, picture);
            const std::string state = PhoneServer::fetchLocal(port, "/" + token + "/state?view=overlay&w=1170");
            allPassed &= check(fresh && !again && server.frame(PhoneServer::View::Overlay) == 1 &&
                                   state.find("{\"frame\":1,\"width\":3,\"height\":2,\"control\":false,"
                                              "\"bg\":\"#0F1013\"}") != std::string::npos,
                               "phone: a new picture is a new frame, the same one again is not");
            const std::string frame = PhoneServer::fetchLocal(port, "/" + token + "/frame?view=overlay&n=1");
            const size_t body = frame.find("\r\n\r\n");
            allPassed &= check(frame.rfind("HTTP/1.1 200 OK", 0) == 0 &&
                                   frame.find("Content-Type: image/png") != std::string::npos &&
                                   body != std::string::npos &&
                                   frame.compare(body + 4, std::string::npos, std::string(png.begin(), png.end())) == 0,
                               "phone: the frame is the picture's PNG");
            allPassed &= check(PhoneServer::fetchLocal(port, "/" + token + "/frame?view=window").rfind("HTTP/1.1 404", 0) == 0,
                               "phone: a view not drawn yet has no frame");
            // Taps: only where control is on, only sent (POST), and taken
            // as they came - where on which picture, and a long press as such
            const std::string base = "/" + token + "/tap?view=window&x=120&y=345&n=1";
            const std::string offTap = PhoneServer::fetchLocal(port, base, "POST");
            allPassed &= check(offTap.rfind("HTTP/1.1 403", 0) == 0 && server.takeTaps().empty() &&
                                   state.find("\"control\":false") != std::string::npos,
                               "phone: with control off a tap is turned away, and the page is told");
            server.setControl(true);
            const std::string asked = PhoneServer::fetchLocal(port, base, "GET");
            const std::string onTap = PhoneServer::fetchLocal(port, base, "POST");
            const std::string longTap =
                PhoneServer::fetchLocal(port, "/" + token + "/tap?view=overlay&x=-5&y=40&b=r&n=7", "POST");
            const std::vector<PhoneServer::Tap> taps = server.takeTaps();
            allPassed &= check(asked.rfind("HTTP/1.1 405", 0) == 0 && onTap.rfind("HTTP/1.1 200", 0) == 0 &&
                                   longTap.rfind("HTTP/1.1 200", 0) == 0 && taps.size() == 2 &&
                                   taps[0].view == PhoneServer::View::Window && taps[0].x == 120 && taps[0].y == 345 &&
                                   !taps[0].right && taps[0].frame == 1 && taps[1].view == PhoneServer::View::Overlay &&
                                   taps[1].x == 0 && taps[1].y == 40 && taps[1].right && taps[1].frame == 7 &&
                                   server.takeTaps().empty(),
                               "phone: with control on a tap is taken as sent, and asked for (GET) it is not");
            allPassed &= check(PhoneServer::fetchLocal(port, "/" + token + "/", "POST").rfind("HTTP/1.1 405", 0) == 0 &&
                                   PhoneServer::fetchLocal(port, "/0123456789ac/tap?x=1&y=1", "POST").rfind("HTTP/1.1 404", 0) == 0 &&
                                   PhoneServer::fetchLocal(port, "/" + token + "/state?view=window&w=900")
                                           .find("\"control\":true") != std::string::npos,
                               "phone: the page is not sent to, a tap without the token finds nothing, and the "
                               "page is told control is on");
            for (size_t i = 0; i < PhoneServer::MAX_TAPS + 4; i++) PhoneServer::fetchLocal(port, base, "POST");
            allPassed &= check(server.takeTaps().size() == PhoneServer::MAX_TAPS,
                               "phone: no more than " + std::to_string(PhoneServer::MAX_TAPS) + " taps wait to be taken");
            // ...and stopped, and started again at once on the port it had:
            // a phone keeps its address when the phone is switched off and on
            server.stop();
            allPassed &= check(!server.running() && PhoneServer::fetchLocal(port, "/" + token + "/").empty(),
                               "phone: stopped, nothing answers");
            const bool reopened = server.start(token, port, true);
            allPassed &= check(reopened && server.port() == port,
                               "phone: started again at once, on the same port (" + std::to_string(server.port()) + ")");
            server.stop();
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

    // ---- Plugins: signed folders of Lua, run apart ----
    {
        namespace P = SCMiner::Plugins;
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "bedrock_selftest_plugins";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        auto write = [](const std::filesystem::path& file, const std::string& text) {
            std::filesystem::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
        };
        const std::string probeLua = R"LUA(
title = "PROBE"
local sandbox = io == nil and os == nil and load == nil and dofile == nil and loadfile == nil
    and require == nil and package == nil and debug == nil and string.dump == nil
local data = bedrock.data("things")
local saved = bedrock.load()
local count, old = 0, 0
bedrock.watch("TheCollector", function(line, isOld)
  if isOld then old = old + 1 else count = count + 1 end
  if line:find("LOOP") then while true do end end
  if line:find("HOG") then local t = {} for i = 1, 1e7 do t[i] = string.rep("x", 1048576) .. i end end
  if line:find("FAIL") then error("on purpose") end
end, true)
local n = saved and saved.n or 0
function draw(ui)
  ui.heading("PROBE")
  ui.text(sandbox and "sandbox" or "open")
  ui.text(data.trades[1].wants[1].name .. " " .. data.trades[1].wants[1].amount)
  ui.text("count " .. count .. " old " .. old)
  ui.text("saved " .. n)
  local tab = ui.tabs("tabs", {"ONE", "TWO"}, 1)
  ui.text("tab " .. tab)
  if ui.card("c1", "CARD", {chip = "READY", tone = "good"}) then
    ui.row("left", "right", "bad")
    ui.close()
  end
  local v = ui.counter("k", "Carinite", n)
  if v ~= n then n = v; bedrock.save({n = n, list = {1, 2, 3}, name = "x\"y"}) end
  local b = ui.buttons({{"go", "GO", "primary"}, {"stop", "STOP", "danger"}})
  if b then ui.text("clicked " .. b) end
end
)LUA";
        auto plugin = [&](const std::string& id, const std::string& needs) {
            write(root / id / "plugin.json",
                  "{\"id\":\"" + id + "\",\"name\":\"Probe\",\"version\":\"1.0\",\"bedrock\":\"" + needs + "\"}");
            write(root / id / "main.lua", probeLua);
            write(root / id / "data" / "things.json",
                  "{\"trades\":[{\"id\":\"a\",\"wants\":[{\"name\":\"Carinite\",\"amount\":10}]}]}");
        };
        const P::Key seed = P::testSeed();
        const std::vector<P::Key> keys = {P::publicKeyOf(seed)};
        auto sign = [&](const std::string& id) { write(root / id / "plugin.sig", P::signatureOf(P::manifestOf(root / id), seed)); };
        plugin("probe", "2.0");
        sign("probe");
        plugin("loose", "");
        plugin("newer", "9.0");
        sign("newer");
        plugin("tampered", "");
        sign("tampered");
        write(root / "tampered" / "main.lua", probeLua + "\n-- one line more\n");
        write(root / "broken" / "plugin.json", "{\"id\": not json");
        // a signature: the manifest by the key, nothing else
        const std::string manifest = P::manifestOf(root / "probe");
        P::Key otherSeed = seed;
        otherSeed[0] ^= 1;
        const std::string signature = P::signatureOf(manifest, seed);
        // ...and one made by ssh-keygen -Y sign -n bedrock-plugin -O hashalg=sha512 (a throwaway key)
        const P::Key sshKey = {0x22, 0xb1, 0x5a, 0xe9, 0x65, 0xf9, 0x7f, 0xa3, 0x8f, 0x5a, 0x36, 0xb5, 0x03, 0xf0, 0x46, 0xbd,
                               0x73, 0x91, 0xd7, 0x13, 0x5b, 0x8b, 0x53, 0x16, 0x98, 0x1c, 0xde, 0x01, 0x25, 0x01, 0xac, 0xb1};
        const std::string sshSignature = "-----BEGIN SSH SIGNATURE-----\n"
                                 "U1NIU0lHAAAAAQAAADMAAAALc3NoLWVkMjU1MTkAAAAgIrFa6WX5f6OPWja1A/BGvXOR1x\n"
                                 "Nbi1MWmBzeASUBrLEAAAAOYmVkcm9jay1wbHVnaW4AAAAAAAAABnNoYTUxMgAAAFMAAAAL\n"
                                 "c3NoLWVkMjU1MTkAAABAPY/vx4W56U2l4dugVFhEjVZH3ouSrc/ysRTDQUe6HVJcFITi2Y\n"
                                 "Q4+6Uy+yaeOTqxu4hJ3JorBxKwElHmywfqAw==\n"
                                 "-----END SSH SIGNATURE-----\n";
        const bool fromSsh = P::signedBy("0123abcd  main.lua\n", sshSignature, {sshKey}) &&
                             !P::signedBy("0123abcd  main.lua\n", sshSignature, keys) &&
                             !P::signedBy("0123abcd  main.lub\n", sshSignature, {sshKey});
        const bool signs = manifest.find("  main.lua\n") != std::string::npos &&
                           manifest.find("  data/things.json\n") != std::string::npos &&
                           manifest.find("plugin.sig") == std::string::npos &&
                           signature.rfind("-----BEGIN SSH SIGNATURE-----\n", 0) == 0 &&
                           P::signedBy(manifest, signature, keys) &&
                           !P::signedBy(manifest, signature, {P::publicKeyOf(otherSeed)}) &&
                           !P::signedBy(manifest + "x", signature, keys) && !P::signedBy(manifest, "", keys) &&
                           !P::signedBy(manifest, std::string(128, 'g'), keys) && fromSsh;
        allPassed &= check(signs, "a plugin's manifest: every file but plugin.sig by its SHA-512; an SSH signature "
                                  "of it passes with the signing key alone and for that manifest alone, "
                                  "one by ssh-keygen too");
        const std::vector<P::Info> found = P::discover(root, keys, false, "2.7.0");
        auto info = [&](const std::string& id) {
            for (const P::Info& i : found) {
                if (i.id == id) return i;
            }
            return P::Info();
        };
        const bool findings = found.size() == 5 && info("probe").trusted && info("probe").problem.empty() &&
                              info("probe").name == "Probe" && info("probe").needs == "2.0" &&
                              info("loose").problem.rfind("NOT SIGNED", 0) == 0 &&
                              info("newer").trusted && info("newer").problem.rfind("NEEDS BEDROCK 9.0", 0) == 0 &&
                              info("tampered").problem.rfind("FILES CHANGED", 0) == 0 &&
                              info("broken").problem.rfind("BROKEN", 0) == 0 &&
                              P::discover(root, keys, true, "2.7.0")[1].id == "loose" &&
                              P::discover(root, keys, true, "2.7.0")[1].problem.empty();
        allPassed &= check(findings, "plugin folders: signed runs; unsigned, changed after signing, broken or for a "
                                     "newer Bedrock does not - a developer may run his own unsigned");
        // running: the language without the world, its data, its state,
        // the log lines it watches, its window and what the player did in it
        P::Plugin probe(info("probe"), root / "state" / "probe.json");
        const bool started = probe.start() && probe.running() && probe.title() == "PROBE";
        P::UiState state;
        auto texts = [](const std::vector<P::UiItem>& items) {
            std::string all;
            for (const P::UiItem& item : items) {
                if (item.kind == P::UiItem::Kind::Text) all += item.text + "|";
            }
            return all;
        };
        const std::vector<P::UiItem> first = probe.draw(state);
        probe.logLine("<x> TheCollector old", true);
        probe.logLine("<x> TheCollector new", false);
        probe.logLine("<x> nothing of it", false);
        const bool wanted = probe.wants("a TheCollector b", false) && !probe.wants("a b", false);
        state.picks["tabs"] = 2;
        state.open.insert("c1");
        state.clicked = "k:+";
        const std::vector<P::UiItem> second = probe.draw(state);
        state.clicked = "go";
        const std::vector<P::UiItem> third = probe.draw(state);
        const std::vector<P::UiItem> fourth = probe.draw(state);
        std::ifstream savedFile(root / "state" / "probe.json", std::ios::binary);
        const std::string saved((std::istreambuf_iterator<char>(savedFile)), std::istreambuf_iterator<char>());
        savedFile.close();
        bool rowInCard = false;
        for (size_t i = 0; i + 2 < second.size(); i++) {
            rowInCard |= second[i].kind == P::UiItem::Kind::Card && second[i].open && second[i].chip == "READY" &&
                         second[i].tone == P::UiItem::Tone::Good && second[i + 1].kind == P::UiItem::Kind::Row &&
                         second[i + 1].value == "right" && second[i + 2].kind == P::UiItem::Kind::CardEnd;
        }
        bool counted = false;
        for (const P::UiItem& item : second) counted |= item.kind == P::UiItem::Kind::Counter && item.value == "1";
        const bool runs = started && first.size() == 10 && first[0].kind == P::UiItem::Kind::Heading &&
                          texts(first) == "sandbox|Carinite 10|count 0 old 0|saved 0|tab 1|" && wanted &&
                          texts(second) == "sandbox|Carinite 10|count 1 old 1|saved 0|tab 2|" && rowInCard && counted &&
                          texts(third).find("saved 1|") != std::string::npos &&
                          texts(third).find("clicked go|") != std::string::npos &&
                          texts(fourth).find("clicked") == std::string::npos && state.clicked.empty() &&
                          saved.find("\"n\":1") != std::string::npos && saved.find("[1,2,3]") != std::string::npos &&
                          saved.find("\"x\\\"y\"") != std::string::npos;
        allPassed &= check(runs, "a plugin runs: no io, os, load or bytecode; its data and saved state; the log "
                                 "lines it watches, old and new; its window with tabs, cards, counters and buttons, "
                                 "a click counting once");
        // ...and stops alone: a loop that never ends, memory without end,
        // an error; saved state read again on the next start
        P::Plugin again(info("probe"), root / "state" / "probe.json");
        again.start();
        const bool reloaded = texts(again.draw(state)).find("saved 1|") != std::string::npos;
        const auto t0 = std::chrono::steady_clock::now();
        again.logLine("TheCollector LOOP", false);
        const double loopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const bool looped = !again.running() && again.error().find("took too long") != std::string::npos && loopMs < 2000 &&
                            again.draw(state).empty();
        P::Plugin hog(info("probe"), root / "state" / "probe.json");
        hog.start();
        hog.logLine("TheCollector HOG", false);
        const bool hogged = !hog.running() && hog.error().find("64 MB") != std::string::npos;
        P::Plugin failing(info("probe"), root / "state" / "probe.json");
        failing.start();
        failing.logLine("TheCollector FAIL", false);
        const bool failed = !failing.running() && failing.error().find("on purpose") != std::string::npos;
        P::Plugin loose(info("loose"), root / "state" / "loose.json");
        const bool refused = !loose.start() && loose.error().rfind("NOT SIGNED", 0) == 0;
        allPassed &= check(reloaded && looped && hogged && failed && refused,
                           "a plugin stops alone: after " + std::to_string(static_cast<int>(loopMs)) +
                               " ms of a loop, past 64 MB, at an error - and one not signed does not start");
        std::filesystem::remove_all(root, ec);

        // ---- what the server offers, and packages ----
        {
            std::vector<P::Offer> offers;
            std::string why;
            const bool read = P::readIndex(
                R"({"plugins":[{"id":"wikelo","name":"Wikelo","version":"1.0.0","bedrock":"2.8.0","description":"d",)"
                R"("url":"wikelo-1.0.0.bplugin"},{"id":"Bad Id","version":"1","url":"x"},{"id":"nourl","version":"1"}]})",
                offers, why);
            const std::string index = "https://the-forsaken.org/bedrock/plugins/index.json";
            allPassed &= check(read && offers.size() == 1 && offers[0].id == "wikelo" && offers[0].needs == "2.8.0" &&
                                   P::packageUrl(index, offers[0].url) ==
                                       "https://the-forsaken.org/bedrock/plugins/wikelo-1.0.0.bplugin" &&
                                   P::packageUrl(index, "https://example.org/p.bplugin") == "https://example.org/p.bplugin" &&
                                   P::packageUrl(index, "http://example.org/p.bplugin").empty() &&
                                   P::packageUrl(index, "/p.bplugin").empty() &&
                                   P::indexUrlBeside("https://the-forsaken.org/bedrock/bedrock-latest.txt") == index &&
                                   P::indexUrlBeside("").empty() && !P::readIndex("{\"plugins\": 5}", offers, why),
                               "the server's list of plugins: plain ids with a version and a package, the package "
                               "beside the list or at an https address of its own");
        }
        {
            const std::filesystem::path src = root / "src" / "probe2";
            write(src / "plugin.json", "{\"id\":\"probe2\",\"name\":\"Probe two\",\"version\":\"1.0\"}");
            write(src / "main.lua", "title = \"PROBE\\226\\128\\148TWO\"\nfunction draw(ui) ui.text(\"x\\ty\\r\\n\") end\n");
            write(src / "data" / "things.json", "{\"a\": \"\\u00e9\\u00df\"}\r\n");
            std::string why;
            const std::string package = P::pack(src, seed, why);
            const std::filesystem::path installed = root / "installed";
            std::string id;
            const bool in = !package.empty() && P::unpack(package, installed, keys, false, id, why);
            std::ifstream original(src / "main.lua", std::ios::binary), copied(installed / "probe2" / "main.lua", std::ios::binary);
            const std::string a((std::istreambuf_iterator<char>(original)), std::istreambuf_iterator<char>());
            const std::string b((std::istreambuf_iterator<char>(copied)), std::istreambuf_iterator<char>());
            original.close();
            copied.close();
            const std::vector<P::Info> there = P::discover(installed, keys, false, "2.7.0");
            // ...a second time, a new version over the old one
            write(src / "plugin.json", "{\"id\":\"probe2\",\"name\":\"Probe two\",\"version\":\"1.1\"}");
            const std::string newer = P::pack(src, seed, why);
            const bool over = P::unpack(newer, installed, keys, false, id, why);
            const std::vector<P::Info> after = P::discover(installed, keys, false, "2.7.0");
            int leftOver = 0;
            for (const auto& e : std::filesystem::directory_iterator(installed, ec)) leftOver += e.path().filename().string()[0] == '.';
            // ...and what does not go in
            std::string tampered = newer;
            const size_t at = tampered.find("PROBE");
            if (at != std::string::npos) tampered[at] = 'Q';
            std::string bad1, bad2, bad3, bad4;
            const bool notTampered = !P::unpack(tampered, root / "t1", keys, false, id, bad1);
            const bool notOther = !P::unpack(newer, root / "t2", {P::publicKeyOf(otherSeed)}, false, id, bad2);
            std::string sneaky = newer;
            const size_t name = sneaky.find("\"data/things.json\"");
            if (name != std::string::npos) sneaky.replace(name, 18, "\"../evil.json\"");
            const bool notOutside = !P::unpack(sneaky, root / "t3", keys, true, id, bad3) &&
                                    !std::filesystem::exists(root / "evil.json");
            write(src / "tool.exe", "MZ");
            const bool notPacked = P::pack(src, seed, bad4).empty();
            allPassed &= check(in && id == "probe2" && a == b && there.size() == 1 && there[0].trusted &&
                                   there[0].problem.empty() && over && after.size() == 1 && after[0].version == "1.1" &&
                                   after[0].trusted && leftOver == 0 && notTampered && notOther && notOutside && notPacked,
                               "a package: packed and signed, unpacked byte for byte and replacing the version before; "
                               "one changed, signed by another key, reaching out of its folder or holding a program "
                               "does not go in (" + bad1 + "; " + bad2 + "; " + bad3 + "; " + bad4 + ")");
        }
        {
            // the older logs once, oldest first, and the live log's lines
            // held back until they are read
            const std::filesystem::path game = root / "game", plugins = root / "hosted", state = root / "hoststate";
            write(game / "logbackups" / "a.log", "<2026-10-01T10:00:00.000Z> MARK A1\n<2026-10-01T10:00:01.000Z> other\n"
                                                 "<2026-10-01T10:00:02.000Z> MARK A2\r\n");
            write(game / "logbackups" / "b.log", "<2026-10-02T10:00:00.000Z> MARK B1\n  MARK notaline\n");
            const auto now = std::filesystem::file_time_type::clock::now();
            std::filesystem::last_write_time(game / "logbackups" / "a.log", now - std::chrono::hours(48), ec);
            std::filesystem::last_write_time(game / "logbackups" / "b.log", now - std::chrono::hours(24), ec);
            write(plugins / "rec" / "plugin.json", "{\"id\":\"rec\",\"name\":\"Rec\",\"version\":\"1\"}");
            write(plugins / "rec" / "main.lua",
                  "local got = {}\n"
                  "bedrock.watch('MARK', function(line, old) got[#got + 1] = (old and 'o:' or 'l:') .. line:match('MARK (%w+)') ..\n"
                  "  ':' .. tostring(bedrock.time(line) % 60000) end, true)\n"
                  "function draw(ui) ui.text(table.concat(got, ',')) end\n");
            auto seen = [&](P::Host& host) {
                P::UiState st;
                const auto items = host.draw("rec", st);
                return items.empty() ? std::string("?") : items[0].text;
            };
            std::string first, second, third;
            int all = 0;
            {
                P::Host host(plugins, state, "2.7.0", keys, true);
                host.setGameFolder(game);
                host.load();
                host.liveLine("<2026-10-03T10:00:00.000Z> MARK L1");
                host.readingHistory("rec", nullptr, &all);
                for (int i = 0; i < 10 && host.readingHistory("rec"); i++) host.tick(50);
                first = seen(host);
            }
            {
                // started again: what was read stays read; a new backup is read
                write(game / "logbackups" / "c.log", "<2026-10-04T10:00:00.000Z> MARK C1\n");
                P::Host host(plugins, state, "2.7.0", keys, true);
                host.setGameFolder(game);
                host.load();
                for (int i = 0; i < 10 && host.readingHistory("rec"); i++) host.tick(50);
                host.liveLine("<2026-10-04T11:00:00.000Z> MARK L2");
                second = seen(host);
                std::string why;
                const bool gone = host.remove("rec", why) && host.found().empty() && !std::filesystem::exists(plugins / "rec");
                third = gone ? "gone" : why;
            }
            allPassed &= check(all == 2 && first == "o:A1:0,o:A2:2000,o:B1:0,l:L1:0" && second == "o:C1:0,l:L2:0" &&
                                   third == "gone",
                               "the older logs are read once, oldest first and before the live log's lines; a plugin "
                               "started again reads the backups new since; a plugin removed is gone (" + first + " / " +
                                   second + ")");
        }
        std::filesystem::remove_all(root, ec);
    }

    // ---- The game's settings files and SC Blackbox's backups of them ----
    {
        namespace G = SCMiner::GameFiles;
        namespace P = SCMiner::Plugins;
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "bedrock_selftest_gamefiles";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        auto write = [](const std::filesystem::path& file, const std::string& text) {
            std::filesystem::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
        };
        auto readBack = [](const std::filesystem::path& file) {
            std::ifstream in(file, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        using K = G::Kind;
        const bool named =
            G::kindOf("client\\0\\Profiles\\default\\actionmaps.xml") == K::Keybinds &&
            G::kindOf("client\\1\\profiles\\DEFAULT\\attributes.xml") == K::Settings &&
            G::kindOf("client\\0\\controls\\mappings\\layout_Pilot_exported.xml") == K::Layout &&
            G::kindOf("client\\0\\CustomCharacters\\Pilot.chf") == K::Character &&
            G::kindOf("client\\0\\Profiles\\default\\profile.xml") == K::None &&
            G::kindOf("client\\..\\Profiles\\default\\actionmaps.xml") == K::None &&
            G::kindOf("client\\0\\controls\\mappings\\..\\..\\x.xml") == K::None &&
            G::kindOf("client/0/Profiles/default/actionmaps.xml") == K::None &&
            G::kindOf("C:\\x\\client\\0\\Profiles\\default\\actionmaps.xml") == K::None &&
            G::kindOf("client\\0\\CustomCharacters\\Pilot.xml") == K::None &&
            G::kindOf("client\\0\\controls\\mappings\\a\\b.xml") == K::None && G::kindOf("") == K::None &&
            G::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
            G::sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        allPassed &= check(named, "the game's settings files by name: keybinds, settings, layouts and characters under "
                                  "a client folder, nothing else and no way out of it; SHA-256");

        // a game folder: LIVE and PTU are channels, Other is not
        const std::filesystem::path live = root / "game" / "LIVE", user = live / "user";
        const std::string am = "client\\0\\Profiles\\default\\actionmaps.xml",
                          at = "client\\0\\Profiles\\default\\attributes.xml",
                          lay = "client\\0\\controls\\mappings\\layout_Pilot_exported.xml",
                          chf = "client\\0\\CustomCharacters\\Pilot.chf";
        write(user / "client" / "0" / "Profiles" / "default" / "actionmaps.xml",
              "<ActionMaps><ActionProfiles profileName=\"default\"/></ActionMaps>");
        write(user / "client" / "0" / "Profiles" / "default" / "attributes.xml", "<Attributes Version=\"1\"/>");
        write(user / "client" / "0" / "Profiles" / "default" / "profile.xml", "<Profile/>");
        write(user / "client" / "0" / "controls" / "mappings" / "layout_Pilot_exported.xml", "<ActionMaps/>");
        write(user / "client" / "0" / "CustomCharacters" / "Pilot.chf", std::string("\x01\x02" "chf"));
        write(root / "game" / "PTU" / "Data.p4k", "");
        std::filesystem::create_directories(root / "game" / "Other" / "user", ec);
        const std::vector<G::Channel> chans = G::channels(live);
        const G::Set set = G::read(user);
        const G::Hashes h = G::hashesOf(set);
        const std::vector<std::string> names = G::names(user);
        allPassed &= check(chans.size() == 2 && chans[0].name == "LIVE" && chans[1].name == "PTU" && set.size() == 4 &&
                               names.size() == 4 && names[0] == lay && names[1] == chf && names[3] == at &&
                               set.count(lay) && set.at(chf).data == std::string("\x01\x02" "chf") &&
                               h.all == "c349083686a3cc6da159cbd278de268b7031406b6ffa98a9ac88636e368ab385" &&
                               h.keybinds == "682341efe0028d141e3afbc843eaf8b8c9c49249748ae513251acba7da15b2b5" &&
                               h.characters == "d7ff7b4b1bb5b2734a8d8a21b6d1b95e45f4fbda9bf65457fa91f08814ca4922" &&
                               !G::stamp(user).empty(),
                           "a game folder's channels, LIVE first; its files read and hashed as SC Blackbox does - "
                           "all but the characters, the keybinds alone (" + h.all.substr(0, 8) + ")");

        // backups: written, listed, read, kept, removed
        const std::filesystem::path backups = root / "backups";
        G::Backup info;
        info.reason = "change";
        info.summary = "2 keybinds";
        info.detail = "Fire \xE2\x86\x92 Shoot";
        info.binds = 12;
        std::string why1, why2, why3;
        const long long before = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string id1 = G::writeBackup(backups, "LIVE", user, set, info, why1);
        std::filesystem::create_directories(backups / "LIVE" / ".incomplete-left", ec);
        const std::string id2 = G::writeBackup(backups, "LIVE", user, set, info, why2);
        const bool cleaned = !std::filesystem::exists(backups / "LIVE" / ".incomplete-left");
        std::vector<G::Backup> listed = G::backups(backups, "LIVE");
        const G::Set again = G::backupFiles(backups, "LIVE", id1, why3);
        bool sameFiles = again.size() == set.size();
        for (const auto& [rel, f] : set) {
            sameFiles &= again.count(rel) && again.at(rel).data == f.data && again.at(rel).time == f.time;
        }
        const std::string infoText = readBack(backups / "LIVE" / id1 / "backup-info.txt");
        const bool written = id1.size() == 19 && id1[10] == '_' && !id2.empty() && id2 != id1 && cleaned &&
                             listed.size() == 2 && listed[0].id == id2 && listed[1].id == id1 &&
                             listed[1].reason == "change" && listed[1].detail == info.detail && listed[1].binds == 12 &&
                             !listed[1].kept && listed[1].allHash == h.all && listed[1].keybindHash == h.keybinds &&
                             listed[1].characters == 1 && listed[1].counted && listed[1].created >= before - 1000 &&
                             listed[1].created <= before + 60000 && sameFiles &&
                             infoText.rfind("# SC Blackbox.", 0) == 0 && infoText.find("\r\nchannel=LIVE\r\n") != std::string::npos &&
                             infoText.find("\r\nsummary=2 keybinds\r\n") != std::string::npos &&
                             !std::filesystem::exists(backups / "LIVE" / id1 / "user" / "client" / "0" / "Profiles" /
                                                      "default" / "profile.xml");
        std::string why4, why5, why6, why7;
        const bool marked = G::markBackup(backups, "LIVE", id1, true, why4);
        listed = G::backups(backups, "LIVE");
        const bool kept = marked && listed.size() == 2 && listed[1].kept && listed[1].counted && listed[1].binds == 12;
        const bool removed = G::removeBackup(backups, "LIVE", id2, why5) && !std::filesystem::exists(backups / "LIVE" / id2) &&
                             !G::removeBackup(backups, "LIVE", "..", why6) && !G::removeBackup(backups, "LIVE", "nope", why7) &&
                             G::backups(backups, "LIVE").size() == 1;
        // ...and one SC Blackbox wrote
        write(backups / "LIVE" / "2026-10-29_15-47-24" / "backup-info.txt",
              "# SC Blackbox. The game files are in the \"user\" folder next to this file.\r\n"
              "created=2026-10-29T15:47:24.5681841+01:00\r\nchannel=LIVE\r\nsource=X\r\nreason=while-playing\r\n"
              "summary=1 game setting\r\nkept=0\r\nbinds=211\r\nallhash=a\r\nkeybindhash=b\r\n");
        write(backups / "LIVE" / "2026-10-29_15-47-24" / "user" / "client" / "0" / "Profiles" / "default" /
                  "actionmaps.xml", "<ActionMaps/>");
        listed = G::backups(backups, "LIVE");
        std::string why8;
        const G::Set theirs = G::backupFiles(backups, "LIVE", "2026-10-29_15-47-24", why8);
        const bool foreign = listed.size() == 2 && listed[1].id == "2026-10-29_15-47-24" &&
                             listed[1].created == 1793285244568LL && !listed[1].counted && listed[1].characters == 0 &&
                             listed[1].binds == 211 && theirs.size() == 1 && theirs.count(am);
        allPassed &= check(written && kept && removed && foreign,
                           "backups in SC Blackbox's form: written whole under the time's name, a second beside it, "
                           "what a write left half done cleared, listed newest first, read back byte for byte with "
                           "their times, kept, removed - and one of SC Blackbox's read (" + why1 + why2 + why3 + why4 +
                               why5 + ")");

        // files put back: whole, read-only kept so, nothing else touched
        const std::filesystem::path amFile = user / "client" / "0" / "Profiles" / "default" / "actionmaps.xml";
        std::filesystem::permissions(amFile, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove, ec);
#ifdef _WIN32
        SetFileAttributesW(amFile.wstring().c_str(), GetFileAttributesW(amFile.wstring().c_str()) | FILE_ATTRIBUTE_READONLY);
#endif
        std::string why9, why10;
        const int n = G::write(user, {{am, "<ActionMaps version=\"2\"/>"}, {"client\\0\\CustomCharacters\\Scout.chf", "c2"}},
                               why9);
        const std::filesystem::perms after = std::filesystem::status(amFile, ec).permissions();
        const bool stillReadOnly = (after & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
        std::filesystem::permissions(amFile, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);
#ifdef _WIN32
        SetFileAttributesW(amFile.wstring().c_str(), GetFileAttributesW(amFile.wstring().c_str()) & ~FILE_ATTRIBUTE_READONLY);
#endif
        const bool refused = G::write(user, {{"client\\0\\..\\..\\evil.xml", "x"}}, why10) < 0 &&
                             !std::filesystem::exists(user / "evil.xml");
        allPassed &= check(n == 2 && readBack(amFile) == "<ActionMaps version=\"2\"/>" && stillReadOnly &&
                               readBack(user / "client" / "0" / "CustomCharacters" / "Pilot.chf") ==
                                   std::string("\x01\x02" "chf") &&
                               readBack(user / "client" / "0" / "CustomCharacters" / "Scout.chf") == "c2" && refused,
                           "files put back into the game's folder: each whole, a read-only one left read-only, the "
                           "others as they were; a name out of the folder refused (" + why10 + ")");

        // a plugin that asks for them, and one that does not
        write(root / "hosted" / "gf" / "plugin.json",
              "{\"id\":\"gf\",\"name\":\"GF\",\"version\":\"1\",\"access\":[\"game-files\"]}");
        write(root / "hosted" / "gf" / "main.lua", R"LUA(
local ticks = 0
bedrock.every(1, function() ticks = ticks + 1 end)
function draw(ui)
  local names = {}
  for _, c in ipairs(bedrock.game.channels()) do names[#names + 1] = c.name end
  local files = bedrock.game.files("LIVE")
  local n = 0
  for _ in pairs(files) do n = n + 1 end
  local h = bedrock.hashes(files)
  local id = bedrock.backups.write("LIVE", files, {reason = "manual", summary = "s", binds = 3})
  local list = bedrock.backups.list("LIVE")
  local back = bedrock.backups.files("LIVE", id)
  local am = "client\\0\\Profiles\\default\\actionmaps.xml"
  ui.text(table.concat(names, ",") .. "|" .. n .. "|" .. list[1].reason .. "|" .. tostring(list[1].binds) .. "|" ..
          (list[1].allhash == h.all and "same" or "other") .. "|" .. (back[am].data == files[am].data and "back" or "lost") ..
          "|" .. ticks)
  local ok, why = bedrock.game.restore("LIVE", {["client\\0\\Profiles\\default\\attributes.xml"] = "<Attributes Version=\"2\"/>"})
  ui.text(tostring(ok) .. "|" .. tostring(why))
  ui.text(pcall(bedrock.backups.write, "LIVE", {["client\\0\\..\\x.xml"] = "x"}, {}) and "took it" or "refused it")
  local none, nowhy = bedrock.backups.list("NOPE")
  ui.text(tostring(none) .. "|" .. tostring(nowhy))
end
)LUA");
        write(root / "hosted" / "plain" / "plugin.json", "{\"id\":\"plain\",\"name\":\"Plain\",\"version\":\"1\"}");
        write(root / "hosted" / "plain" / "main.lua",
              "function draw(ui) ui.text(tostring(bedrock.game) .. '|' .. tostring(bedrock.backups) .. '|' .. "
              "tostring(bedrock.hashes) .. '|' .. type(bedrock.every)) end\n");
        std::string seenGf, seenPlain;
        {
            P::Host host(root / "hosted", root / "hoststate", "2.7.0", P::trustedKeys(), true);
            host.setGameFolder(live);
            host.setBackupRoot(root / "pluginbackups");
            host.load();
            const bool ran = host.runTimers(true);
            P::UiState st;
            for (const auto& item : host.draw("gf", st)) seenGf += item.text + "/";
            P::UiState st2;
            for (const auto& item : host.draw("plain", st2)) seenPlain += item.text + "/";
            if (!ran) seenGf += "no timer";
        }
        const std::string restored = G::gameRunning() ? "nil|Star Citizen is running/" : "1|nil/";
        allPassed &= check(seenGf == "LIVE,PTU|5|manual|3|same|back|1/" + restored + "refused it/nil|no such channel/" &&
                               seenPlain == "nil|nil|nil|function/" &&
                               std::filesystem::exists(root / "pluginbackups" / "LIVE"),
                           "a plugin with game-files access: channels, files, hashes, backups written and read back, "
                           "files put back unless the game runs, a bad name refused, its timer run; one without "
                           "has none of it (" + seenGf + " / " + seenPlain + ")");
        std::filesystem::remove_all(root, ec);
    }

    std::cout << "\n=== Self-test " << (allPassed ? "PASSED" : "FAILED") << " ===\n" << std::endl;
    return allPassed ? 0 : 1;
}
