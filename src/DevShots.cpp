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
#include "WhereToBuy.h"
#include "Datarunner.h"
#include "TextLineReader.h"
#include "PluginHost.h"
#include "GameFiles.h"
#include "resource.h"
#include <ctime>
#include <memory>

#include <Windows.h>
#include <opencv2/opencv.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
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

// UEX's shops of the mining gear for the Gear page's pictures: items_prices
// of categories 28 (gadgets), 29 (laser heads) and 30 (modules) as UEX
// answered on 8 November 2026, cut down to the items Bedrock knows - the
// terminals once, then each item with its shops as "terminal:price" pairs -
// and read back through UexShops::read, the way the app reads the answer
struct ShotTerminal {
    const char* terminal;
    const char* system;
    const char* planet;
    const char* orbit;
    const char* moon;
    const char* station;
    const char* city;
    const char* outpost;
};
struct ShotGear {
    int category;
    const char* item;
    const char* shops;
};
static const ShotTerminal SHOT_TERMINALS[] = {
    {"Dumper's Depot - Area 18", "Stanton", "ArcCorp", "ArcCorp", nullptr, nullptr, "Area 18", nullptr},
    {"Shubin Interstellar - Commons - New Babbage", "Stanton", "MicroTech", "MicroTech", nullptr, nullptr, "New Babbage", nullptr},
    {"Tammany and Sons - Metro Center - Lorville", "Stanton", "Hurston", "Hurston", nullptr, nullptr, "Lorville", nullptr},
    {"Refinery Shop - ARC-L1", "Stanton", "ArcCorp", "ArcCorp Lagrange Point 1", nullptr, "ARC-L1 Wide Forest Station", nullptr, nullptr},
    {"Refinery Shop - ARC-L2", "Stanton", "ArcCorp", "ArcCorp Lagrange Point 2", nullptr, "ARC-L2 Lively Pathway Station", nullptr, nullptr},
    {"Refinery Shop - ARC-L4", "Stanton", "ArcCorp", "ArcCorp Lagrange Point 4", nullptr, "ARC-L4 Faint Glen Station", nullptr, nullptr},
    {"Refinery Shop - MIC-L2", "Stanton", "MicroTech", "microTech Lagrange Point 2", nullptr, "MIC-L2 Long Forest Station", nullptr, nullptr},
    {"Refinery Shop - MIC-L1", "Stanton", "MicroTech", "microTech Lagrange Point 1", nullptr, "MIC-L1 Shallow Frontier Station", nullptr, nullptr},
    {"Refinery Shop - MIC-L5", "Stanton", "MicroTech", "microTech Lagrange Point 5", nullptr, "MIC-L5 Modern Icarus Station", nullptr, nullptr},
    {"Refinery Shop - Terra Gateway (Stanton)", "Stanton", nullptr, "Terra Gateway (Stanton system)", nullptr, "Terra Gateway (Stanton)", nullptr, nullptr},
    {"Refinery Shop - HUR-L1", "Stanton", "Hurston", "Hurston Lagrange Point 1", nullptr, "HUR-L1 Green Glade Station", nullptr, nullptr},
    {"Refinery Shop - CRU-L1", "Stanton", "Crusader", "Crusader Lagrange Point 1", nullptr, "CRU-L1 Ambitious Dream Station", nullptr, nullptr},
    {"Refinery Shop - HUR-L2", "Stanton", "Hurston", "Hurston Lagrange Point 2", nullptr, "HUR-L2 Faithful Dream Station", nullptr, nullptr},
    {"Refinery Shop - Pyro Gateway (Stanton)", "Stanton", nullptr, "Pyro Gateway (Stanton system)", nullptr, "Pyro Gateway (Stanton)", nullptr, nullptr},
    {"Dumper's Depot - GrimHEX", "Stanton", "Crusader", "Crusader", "Yela", "Green Imperial Housing Exchange", nullptr, nullptr},
    {"Refinery Shop - Nyx Gateway (Stanton)", "Stanton", nullptr, "Nyx Gateway (Stanton system)", nullptr, "Nyx Gateway (Stanton)", nullptr, nullptr},
    {"Refinery Shop - Ruin Station", "Pyro", "Terminus", "Terminus", nullptr, "Ruin Station", nullptr, nullptr},
    {"Refinery Shop - Checkmate", "Pyro", "Monox", "Monox Lagrange Point 4", nullptr, "Checkmate Station", nullptr, nullptr},
    {"Refinery Shop - Orbituary", "Pyro", "Bloom", "Bloom", nullptr, "Orbituary", nullptr, nullptr},
    {"Shop Terminal - Yang's Place", "Pyro", "Monox", "Monox", nullptr, nullptr, nullptr, "Yang's Place"},
    {"Shop Terminal - Ostler's Claim", "Pyro", "Monox", "Monox", nullptr, nullptr, nullptr, "Ostler's Claim"},
    {"Shop Terminal - Bueno Ravine", "Pyro", "Bloom", "Bloom", nullptr, nullptr, nullptr, "Bueno Ravine"},
    {"Refinery Shop - Stanton Gateway (Pyro)", "Pyro", nullptr, "Stanton Gateway (Pyro system)", nullptr, "Stanton Gateway (Pyro)", nullptr, nullptr},
    {"Shop Terminal - Carver's Ridge", "Pyro", "Bloom", "Bloom", nullptr, nullptr, nullptr, "Carver's Ridge"},
    {"Shop Terminal - Stag's Rut", "Pyro", "Pyro I", "Pyro I", nullptr, nullptr, nullptr, "Stag's Rut"},
    {"Tools & Supplies - Levski", "Nyx", nullptr, "Delamar", nullptr, nullptr, "Levski", nullptr},
    {"Refinery Shop - Levski", "Nyx", nullptr, "Delamar", nullptr, nullptr, "Levski", nullptr},
    {"Refinery Shop - Stanton Gateway (Nyx)", "Nyx", nullptr, "Stanton Gateway (Nyx system)", nullptr, "Stanton Gateway (Nyx)", nullptr, nullptr},
    {"Refinery Shop - Pyro Gateway (Nyx)", "Nyx", nullptr, "Pyro Gateway (Nyx system)", nullptr, "Pyro Gateway (Nyx)", nullptr, nullptr},
    {"Refinery Shop - Nyx Gateway (Pyro)", "Pyro", nullptr, "Nyx Gateway (Pyro system)", nullptr, "Nyx Gateway (Pyro)", nullptr, nullptr},
    {"Tools & Supplies - People's Service Station Alpha", "Nyx", nullptr, "People's Service Station Alpha", nullptr, "People's Service Station Alpha", nullptr, nullptr},
    {"Tools & Supplies - People's Service Station Theta", "Nyx", nullptr, "People's Service Station Theta", nullptr, "People's Service Station Theta", nullptr, nullptr},
    {"Tools & Supplies - People's Service Station Lambda", "Nyx", nullptr, "People's Service Station Lambda", nullptr, "People's Service Station Lambda", nullptr, nullptr},
    {"Tools & Supplies - People's Service Station Delta", "Nyx", nullptr, "People's Service Station Delta", nullptr, "People's Service Station Delta", nullptr, nullptr},
};
static const ShotGear SHOT_GEAR[] = {
    {28, "BoreMax", "25:12750 26:12750 30:12750 31:12750 32:12750 33:12750"},
    {28, "Okunis",
     "1:23250 3:23250 4:23250 6:23250 7:23250 9:23250 10:23250 13:22088 15:23250 18:23250 22:23250 "
     "26:23250 27:23250 28:23250 29:23250"},
    {28, "OptiMax",
     "1:27625 4:27625 5:27625 9:27265 11:27625 12:27625 13:26244 15:27625 16:27625 17:27625 25:27625 "
     "28:27625 30:27625 31:27625 32:27625 33:27625"},
    {28, "Sabir",
     "0:13815 1:13815 2:13125 3:13815 4:13815 5:13815 6:13815 7:13815 8:13815 9:13815 10:13815 11:13815 "
     "12:13815 13:13125 14:13815 15:13815 16:13815 18:13815 19:13815 20:13815 21:13815 22:13815 23:13815 "
     "24:13815 25:13815 26:13815 27:13815 28:13815 29:13815 30:13815 31:13815 32:13815 33:13815"},
    {28, "Stalwart", "26:20190"},
    {28, "Waveshift",
     "0:19125 2:18169 6:19125 8:19125 11:19125 12:19125 14:19125 16:19125 17:19125 19:19125 20:19125 "
     "21:19125 23:19125 24:19125"},
    {29, "Arbor MH1 Mining Laser", "0:11363 1:11363 2:10795 7:11363 11:11363 14:11363"},
    {29, "Arbor MH2 Mining Laser", "1:21450 2:20378 5:21450 8:21450 14:21450"},
    {29, "Helix I Mining Laser",
     "0:58000 1:58000 2:55100 4:58000 6:58000 7:58000 9:58000 11:58000 12:58000 13:55100 14:58000 15:58000 "
     "16:58000 28:58000"},
    {29, "Helix II Mining Laser",
     "0:108000 1:108000 2:102600 6:108000 7:108000 8:108000 10:108000 11:108000 18:108000 22:108000 "
     "26:108000 27:108000 29:108000"},
    {29, "Hofstede-S1 Mining Laser", "0:12750 1:12750 2:12113 8:12750 10:12750 11:12750 17:12750 26:12750 27:12750"},
    {29, "Hofstede-S2 Mining Laser",
     "1:22750 2:21613 3:22750 4:22750 5:22750 6:22750 7:22750 9:22750 12:22750 13:21613 14:22750 15:22750 "
     "16:22750 28:22750"},
    {29, "Impact I Mining Laser", "0:57750 1:57750 2:54863 3:57750 8:57750 14:57750 16:57750"},
    {29, "Impact II Mining Laser",
     "0:57750 1:57750 2:54863 4:57750 5:57750 9:57750 10:57750 11:57750 12:57750 13:54863 15:57750 "
     "17:57750 26:57750 27:57750 28:57750"},
    {29, "Klein-S1 Mining Laser", "1:10150 26:10150"},
    {29, "Klein-S2 Mining Laser", "1:20150 26:20150"},
    {29, "Lancet MH1 Mining Laser",
     "1:23500 2:22325 3:23500 4:23500 5:23500 6:23500 7:23500 9:23500 11:23500 12:23500 13:22325 15:23500 "
     "16:23500 18:23500 22:23500 28:23500 29:23500"},
    {29, "Lancet MH2 Mining Laser",
     "0:94000 1:94000 2:89300 7:94000 8:94000 10:94000 14:94000 18:94000 22:94000 26:94000 27:94000 "
     "29:94000"},
    {30, "Brandt Module", "1:2250 2:2138 3:2250 11:2250 16:2250 17:2250 18:2250 22:2250 29:2250"},
    {30, "FLTR Module",
     "3:6000 4:6000 5:6000 6:6000 7:6000 8:6000 9:6000 11:6000 13:5700 15:6000 16:6000 17:6000 28:6000"},
    {30, "FLTR-L Module",
     "1:18000 4:18000 8:18000 9:18000 11:18000 12:18000 13:17100 15:18000 18:18000 22:18000 28:18000 "
     "29:18000"},
    {30, "FLTR-XL Module",
     "1:60000 3:60000 4:60000 5:60000 6:60000 7:60000 9:60000 10:60000 13:57000 15:60000 16:60000 26:60000 "
     "27:60000 28:60000"},
    {30, "Focus II Module",
     "1:12000 3:12000 4:12000 5:12000 6:12000 9:12000 10:12000 11:12000 12:12000 13:11400 15:12000 "
     "26:12000 27:12000 28:12000"},
    {30, "Focus III Module", "1:40000 5:40000 6:40000 7:40000 8:40000 10:40000 11:40000 12:40000 26:40000 27:40000"},
    {30, "Focus Module",
     "0:4000 3:4000 4:4000 5:4000 7:4000 8:4000 9:4000 10:4000 11:4000 12:4000 13:3800 14:4000 15:4000 "
     "18:4000 22:4000 26:4000 27:4000 28:4000 29:4000"},
    {30, "Forel Module",
     "1:1950 3:1950 4:1950 5:1950 6:1950 7:1950 8:1950 9:1950 10:1950 11:1950 12:1950 13:1853 15:1950 "
     "26:1950 27:1950 28:1950"},
    {30, "Lifeline Module",
     "1:800 2:760 3:800 4:800 5:800 6:800 7:800 8:800 9:800 10:800 11:800 12:800 13:760 15:800 18:800 "
     "22:800 26:800 27:800 28:800 29:800"},
    {30, "Optimum Module", "1:3000 3:3000 5:3000 7:3000 8:3000 12:3000 16:3000 17:3000"},
    {30, "Rieger Module", "26:5700"},
    {30, "Rieger-C2 Module", "26:17100"},
    {30, "Rieger-C3 Module", "26:57000"},
    {30, "Rime Module",
     "1:1100 3:1100 4:1100 5:1100 6:1100 7:1100 8:1100 9:1100 10:1100 11:1100 12:1100 13:1045 15:1100 "
     "16:1100 18:1100 22:1100 26:1100 27:1100 28:1100 29:1100"},
    {30, "Stampede Module",
     "1:1400 3:1400 4:1400 5:1400 6:1400 7:1400 8:1400 9:1400 10:1400 11:1400 12:1400 13:1330 15:1400 "
     "16:1400 26:1400 27:1400 28:1400"},
    {30, "Surge Module", "26:1400"},
    {30, "Torpid Module",
     "1:1950 3:1950 4:1950 5:1950 6:1950 7:1950 8:1950 9:1950 10:1950 11:1950 12:1950 13:1853 15:1950 "
     "18:1950 22:1950 26:1950 27:1950 28:1950 29:1950"},
    {30, "Torrent II Module",
     "1:12000 3:12000 4:12000 6:12000 7:12000 8:12000 9:12000 10:12000 11:12000 13:11400 15:12000 26:15000 "
     "27:12000 28:12000"},
    {30, "Torrent III Module",
     "1:40000 3:40000 4:40000 5:40000 7:40000 8:40000 9:40000 11:40000 12:40000 13:38000 15:40000 17:40000 "
     "28:40000"},
    {30, "Torrent Module", "0:4000 3:4000 5:4000 6:4000 7:4000 10:4000 12:4000 17:4000 26:4000 27:4000"},
    {30, "Vaux Module",
     "0:2000 1:2000 3:2000 4:2000 5:2000 6:2000 7:2000 9:2000 11:2000 12:2000 13:1900 14:2000 15:2000 "
     "16:2000 18:2000 22:2000 28:2000 29:2000"},
    {30, "Vaux-C2 Module",
     "0:6000 3:6000 4:6000 5:6000 7:6000 8:6000 9:6000 10:6000 11:6000 13:5700 14:6000 15:6000 17:6000 "
     "26:6000 27:6000 28:6000"},
    {30, "Vaux-C3 Module", "0:20000 3:20000 6:20000 7:20000 8:20000 10:20000 12:20000 26:20000 27:20000"},
    {30, "XTR Module",
     "0:2000 1:2000 4:2000 7:2000 8:2000 9:2000 10:2000 11:2000 12:2000 13:1900 14:2000 15:2000 26:2000 "
     "27:2000 28:2000"},
    {30, "XTR-L Module",
     "0:6000 3:6000 4:6000 5:6000 8:6000 9:6000 10:6000 11:6000 12:6000 13:5700 14:6000 15:6000 16:6000 "
     "26:6000 27:6000 28:6000"},
    {30, "XTR-XL Module",
     "0:20000 3:20000 5:20000 7:20000 8:20000 10:20000 11:20000 12:20000 14:20000 16:20000 26:20000 "
     "27:20000"},
};

static std::shared_ptr<const SCMiner::UexShops> shotGearShops() {
    auto out = std::make_shared<SCMiner::UexShops>();
    for (const int category : {28, 29, 30}) {
        std::string list = "{\"status\":\"ok\",\"data\":[";
        bool first = true;
        for (const ShotGear& item : SHOT_GEAR) {
            if (item.category != category) continue;
            std::istringstream pairs(item.shops);
            std::string pair;
            while (pairs >> pair) {
                const size_t colon = pair.find(':');
                const ShotTerminal& t = SHOT_TERMINALS[std::stoi(pair.substr(0, colon))];
                list += first ? "{" : ",{";
                first = false;
                list += "\"item_name\":\"" + std::string(item.item) + "\",\"price_buy\":" + pair.substr(colon + 1) +
                        ",\"date_modified\":1794096000";
                const std::pair<const char*, const char*> fields[] = {
                    {"terminal_name", t.terminal}, {"star_system_name", t.system}, {"planet_name", t.planet},
                    {"orbit_name", t.orbit},       {"moon_name", t.moon},          {"space_station_name", t.station},
                    {"city_name", t.city},         {"outpost_name", t.outpost}};
                for (const auto& field : fields) {
                    if (field.second) list += ",\"" + std::string(field.first) + "\":\"" + field.second + "\"";
                }
                list += "}";
            }
        }
        list += "]}";
        std::string why;
        if (!out->read(list, why)) std::cerr << "shot gear shops: " << why << std::endl;
    }
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
    // (an ore with a long name: the market's heading runs into its count)
    static const Row quantainium[] = {
        {true, 2200000, "scu", 900, "WTB Quantainium Q900+", "", "Forge_Seeker", 1, 0},
        {true, 1600000, "scu", 700, "Buying Quantainium, any quality", "", "Old_Ledger", 12, 0},
        {false, 2400000, "scu", 985, "Quantainium Q985 - refined, waiting at CRU-L1", "CRU-L1", "Vela_Mining", 2, 0},
        {false, 1900000, "scu", 920, "Quantainium 920", "Levski", "Delamar_Rocks", 7, 0},
        {false, 1750000, "scu", 870, "Quantainium Q870, bulk", "Orison", "Cloudview_Ore", 15, 4},
    };
    const Row* rows = nullptr;
    size_t count = 0;
    if (ore == "Ouratite") { rows = ouratite; count = sizeof(ouratite) / sizeof(ouratite[0]); }
    if (ore == "Hephaestanite") { rows = hephaestanite; count = sizeof(hephaestanite) / sizeof(hephaestanite[0]); }
    if (ore == "Hadanite") { rows = hadanite; count = sizeof(hadanite) / sizeof(hadanite[0]); }
    if (ore == "Quantainium") { rows = quantainium; count = sizeof(quantainium) / sizeof(quantainium[0]); }
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

// A commodity terminal's rows for the Datarunner's card: a real picture's,
// read as the app reads it, where SCMINER_DATARUNNER_SHOT names one (the
// picture stays where it is and goes nowhere); else drawn here, a dark card
// with the game's lines on it. Every value read, UEX's name beside it.
static std::vector<SCMiner::Datarunner::Row> shotDatarunnerRows(const std::vector<std::pair<std::string, int>>& names) {
    namespace D = SCMiner::Datarunner;
    using SCMiner::Certainty;
    std::vector<D::Row> rows;
    if (const char* path = std::getenv("SCMINER_DATARUNNER_SHOT")) {
        static SCMiner::TextLineReader reader;
        static const bool loaded = reader.loadEmbedded();
        if (loaded) rows = D::readPicture(std::filesystem::path(path), reader).rows;
        std::cout << "  Datarunner rows from " << path << ": " << rows.size() << std::endl;
    }
    if (rows.empty()) {
        struct Drawn { const char* name; int level; int stock; int price; };
        static const Drawn DRAWN[] = {{"HEPHAESTANITE", 7, 1050, 4167}, {"IRON", 2, 4, 2850},
                                      {"ALUMINUM", 4, 509, 3128},       {"ARGON", 7, 24000, 316},
                                      {"SHIP AMMUNITION - SIZE 1", 7, 12000, 6868},
                                      {"SHIP AMMUNITION - SIZE 2", 7, 12000, 7126},
                                      {"SHIP AMMUNITION - SIZE 3", 7, 12000, 7384}};
        for (const Drawn& d : DRAWN) {
            cv::Mat card(68, 420, CV_8UC3, cv::Scalar(52, 40, 26));
            cv::rectangle(card, cv::Rect(6, 8, 4, 52), cv::Scalar(70, 190, 230), cv::FILLED);
            cv::putText(card, d.name, cv::Point(18, 28), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(235, 235, 235), 1, cv::LINE_AA);
            cv::putText(card, SCMiner::commodityLevelWords(d.level), cv::Point(18, 52), cv::FONT_HERSHEY_SIMPLEX, 0.38,
                        cv::Scalar(70, 190, 230), 1, cv::LINE_AA);
            char line[48];
            std::snprintf(line, sizeof(line), "%d SCU", d.stock);
            cv::putText(card, line, cv::Point(300, 28), cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(235, 235, 235), 1, cv::LINE_AA);
            std::snprintf(line, sizeof(line), "%d/SCU", d.price);
            cv::putText(card, line, cv::Point(300, 52), cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(235, 235, 235), 1, cv::LINE_AA);
            D::Row row;
            row.read = d.name;
            row.level = d.level;
            row.stock = d.stock;
            row.price = d.price;
            row.snippet = card;
            rows.push_back(row);
        }
    }
    for (D::Row& row : rows) {
        SCMiner::Certainty sure = Certainty::Missing;
        row.commodity = D::matchCommodity(row.read, names, {}, row.name, sure);
        row.nameSure = row.levelSure = row.stockSure = row.priceSure = Certainty::Sure;
        if (row.commodity == 0) row.nameSure = Certainty::Missing;
        if (row.level < 1) row.levelSure = Certainty::Missing;
        if (row.stock < 0) row.stockSure = Certainty::Missing;
        if (row.price < 0) row.priceSure = Certainty::Missing;
    }
    return rows;
}

// ---- Blackbox: a made-up game folder and backups ------------------------------
// LIVE with five backups over a week - a layout saved by hand and kept,
// keybinds and settings changed, a character added - and PTU, whose keybinds
// fell back to the game's (a reset). Made-up binds, names and characters,
// nothing from a player's PC.
struct ShotBind {
    const char* map;
    const char* action;
    const char* input;
};

static const char* const SHOT_JOYSTICK = " T.16000M  {B10A044F-0000-0000-0000-504944564944}";

static std::string shotActionMaps(const std::vector<ShotBind>& binds, const char* indent) {
    std::string s, map;
    for (const ShotBind& b : binds) {
        if (map != b.map) {
            if (!map.empty()) s += std::string(indent) + "</actionmap>\n";
            s += std::string(indent) + "<actionmap name=\"" + b.map + "\">\n";
            map = b.map;
        }
        s += std::string(indent) + " <action name=\"" + b.action + "\">\n";
        s += std::string(indent) + "  <rebind input=\"" + b.input + "\"/>\n";
        s += std::string(indent) + " </action>\n";
    }
    if (!map.empty()) s += std::string(indent) + "</actionmap>\n";
    return s;
}

static std::string shotKeybinds(const std::vector<ShotBind>& binds) {
    return std::string("<ActionMaps>\n") +
           " <ActionProfiles version=\"1\" optionsVersion=\"2\" rebindVersion=\"2\" profileName=\"default\">\n"
           "  <options type=\"keyboard\" instance=\"1\" Product=\"Keyboard  {6F1D2B61-D5A0-11CF-BFC7-444553540000}\"/>\n"
           "  <options type=\"joystick\" instance=\"1\" Product=\"" + SHOT_JOYSTICK + "\"/>\n"
           "  <modifiers />\n" +
           shotActionMaps(binds, "  ") + " </ActionProfiles>\n</ActionMaps>\n";
}

static std::string shotLayout(const std::string& name, const std::vector<ShotBind>& binds) {
    return "<ActionMaps version=\"1\" optionsVersion=\"2\" rebindVersion=\"2\" profileName=\"" + name + "\">\n" +
           " <CustomisationUIHeader label=\"" + name + "\" description=\"\" image=\"\">\n" +
           "  <devices>\n   <keyboard instance=\"1\"/>\n   <mouse instance=\"1\"/>\n   <joystick instance=\"1\"/>\n"
           "  </devices>\n </CustomisationUIHeader>\n"
           " <options type=\"joystick\" instance=\"1\" Product=\"" + SHOT_JOYSTICK + "\"/>\n <modifiers />\n" +
           shotActionMaps(binds, " ") + "</ActionMaps>\n";
}

static std::string shotSettings(const std::vector<std::pair<const char*, const char*>>& values) {
    std::string s = "<Attributes Version=\"35\">\n";
    for (const auto& v : values) s += std::string(" <Attr name=\"") + v.first + "\" value=\"" + v.second + "\"/>\n";
    return s + "</Attributes>\n";
}

static std::string shotCharacter(const std::string& name, int seed) {
    std::string b(4096, '\0');
    b[0] = 1;
    b.replace(4, name.size(), name);
    for (int i = 64; i < 4096; i++) b[i] = static_cast<char>((i * 7 + seed) & 0xFF);
    return b;
}

// `binds` with these set: a bind changed where its map and action are there,
// else added after its map's last
static std::vector<ShotBind> shotChanged(std::vector<ShotBind> binds, const std::vector<ShotBind>& changes) {
    for (const ShotBind& c : changes) {
        auto same = std::find_if(binds.begin(), binds.end(), [&](const ShotBind& b) {
            return !std::strcmp(b.map, c.map) && !std::strcmp(b.action, c.action);
        });
        if (same != binds.end()) {
            same->input = c.input;
            continue;
        }
        size_t at = binds.size();
        for (size_t i = 0; i < binds.size(); i++) {
            if (!std::strcmp(binds[i].map, c.map)) at = i + 1;
        }
        binds.insert(binds.begin() + static_cast<std::ptrdiff_t>(at), c);
    }
    return binds;
}

struct BlackboxShot {
    std::filesystem::path game;      // LIVE's folder
    std::filesystem::path backups;
    std::string liveNewest, liveKept;
};

// Writes the game folder (<base>/StarCitizen/LIVE and PTU) and the backups
// (<base>/backups), each backup's age counted back from `now`
static BlackboxShot blackboxShotFolders(const std::filesystem::path& base, long long now) {
    namespace GF = SCMiner::GameFiles;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    const std::vector<ShotBind> mine = {
        {"spaceship_movement", "v_strafe_up", "js1_hat1_up"},
        {"spaceship_movement", "v_strafe_down", "js1_hat1_down"},
        {"spaceship_movement", "v_strafe_left", "js1_button13"},
        {"spaceship_movement", "v_strafe_right", "js1_button12"},
        {"spaceship_movement", "v_strafe_forward", "kb1_r"},
        {"spaceship_weapons", "v_weapon_preset_attack", "js1_button2"},
        {"spaceship_weapons", "v_weapon_gimbals_state_toggle", "kb1_lalt+g"},
        {"spaceship_missiles", "v_weapon_toggle_launch_missile", "js1_button2"},
        {"spaceship_missiles", "v_weapon_cycle_missile_fwd", "js1_button3"},
        {"spaceship_targeting", "v_target_toggle_lock_index_1", "js1_button4"},
        {"spaceship_targeting", "v_target_unlock", "js1_button5"},
        {"spaceship_power", "v_power_toggle", "kb1_ralt+u"},
        {"spaceship_power", "v_power_toggle_shields", "kb1_ralt+o"},
        {"spaceship_power", "v_power_toggle_weapons", "kb1_ralt+p"},
        {"spaceship_mining", "v_toggle_mining_laser_fire", "js1_button6"},
        {"spaceship_mining", "v_toggle_mining_laser_type", "js1_button7"},
        {"spaceship_mining", "v_increase_mining_throttle", "js1_button8"},
        {"spaceship_mining", "v_decrease_mining_throttle", "js1_button9"},
        {"seat_general", "v_toggle_mining_mode", "kb1_n"},
        {"seat_general", "v_eject", "kb1_ralt+e"},
        {"seat_general", "v_emergency_exit", "kb1_ralt+x"},
        {"spaceship_hud", "v_starmap", "kb1_f3"},
        {"spaceship_hud", "visor_wipe", "kb1_v"},
        {"spaceship_radar", "v_invoke_ping", "js1_button10"},
    };
    std::vector<std::pair<const char*, const char*>> settings = {
        {"AudioMasterVolume", "1"}, {"AudioMusicVolume", "0.8"}, {"FOV", "90"},
        {"HeadtrackingToggle", "0"}, {"MouseSensitivity", "0.5"}, {"SpeedThrottleDefaultFixedSpeed", "0"},
    };
    const std::string keybinds = "client\\0\\Profiles\\default\\actionmaps.xml";
    const std::string attributes = "client\\0\\Profiles\\default\\attributes.xml";
    const std::string layout = "client\\0\\controls\\mappings\\layout_T16000M_exported.xml";
    const std::string pilot = "client\\0\\CustomCharacters\\Pilot.chf";
    const std::string scout = "client\\0\\CustomCharacters\\Scout.chf";
    using Files = std::map<std::string, std::string>;

    std::vector<Files> live(5);
    live[0] = {{keybinds, shotKeybinds(mine)}, {attributes, shotSettings(settings)}, {pilot, shotCharacter("Pilot", 3)}};
    live[1] = live[0];
    live[1][layout] = shotLayout("T16000M", mine);
    const std::vector<ShotBind> mine3 = shotChanged(mine, {{"spaceship_mining", "v_increase_mining_throttle", "js1_hat1_right"},
                                                           {"spaceship_mining", "v_decrease_mining_throttle", "js1_hat1_left"},
                                                           {"seat_general", "v_toggle_mining_mode", "kb1_lalt+m"}});
    settings[1].second = "0.4";
    live[2] = live[1];
    live[2][keybinds] = shotKeybinds(mine3);
    live[2][attributes] = shotSettings(settings);
    live[3] = live[2];
    live[3][scout] = shotCharacter("Scout", 11);
    const std::vector<ShotBind> mine5 = shotChanged(mine3, {{"spaceship_power", "v_power_toggle_shields", "kb1_ralt+s"},
                                                            {"spaceship_targeting", "v_target_remove_all_pins", "js1_button11"}});
    settings[2].second = "100";
    live[4] = live[3];
    live[4][keybinds] = shotKeybinds(mine5);
    live[4][attributes] = shotSettings(settings);
    settings[1].second = "0.8";
    settings[2].second = "90";
    std::vector<ShotBind> reset;
    for (const ShotBind& b : mine) {
        if (!std::strcmp(b.action, "v_strafe_forward") || !std::strcmp(b.action, "v_power_toggle")) reset.push_back(b);
    }
    const std::vector<Files> ptu = {{{keybinds, shotKeybinds(mine)}, {attributes, shotSettings(settings)}},
                                    {{keybinds, shotKeybinds(reset)}, {attributes, shotSettings(settings)}}};

    auto put = [&](const std::filesystem::path& user, const Files& files) {
        for (const auto& f : files) {
            std::string rel = f.first;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            const std::filesystem::path p = user / rel;
            std::filesystem::create_directories(p.parent_path(), ec);
            std::ofstream(p, std::ios::binary) << f.second;
        }
    };
    BlackboxShot out;
    out.game = base / "StarCitizen" / "LIVE";
    out.backups = base / "backups";
    put(out.game / "user", live.back());
    put(base / "StarCitizen" / "PTU" / "user", ptu.back());
    // played last: LIVE 25 minutes ago, PTU before it
    const auto written = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(out.game / "user/client/0/Profiles/default/actionmaps.xml",
                                     written - std::chrono::minutes(25), ec);
    std::filesystem::last_write_time(base / "StarCitizen/PTU/user/client/0/Profiles/default/actionmaps.xml",
                                     written - std::chrono::minutes(50), ec);

    // what Blackbox itself wrote into each backup-info.txt (its diff of the
    // files against the backup before)
    struct Made {
        const char* channel;
        const Files* files;
        long long age;
        const char *reason, *summary, *detail, *flag;
        bool kept;
        int binds;
    };
    const long long minute = 60 * 1000, hour = 60 * minute, day = 24 * hour;
    const Made made[] = {
        {"LIVE", &live[0], 6 * day + 2 * hour, "start", "First backup", "", "", false, 24},
        {"LIVE", &live[1], 2 * day + 5 * hour, "manual", "1 layout", "Layout \"T16000M\" (new)", "", true, 24},
        {"LIVE", &live[2], day + 3 * hour, "game-exit", "3 keybinds, 1 game setting",
         "Decrease Mining Laser Power \xC2\xB7 T.16000M: Button 9 \xE2\x86\x92 Hat 1 left   (+3 more)", "", false, 24},
        {"LIVE", &live[3], 3 * hour + 10 * minute, "change", "1 character", "Character \"Scout\" (new)", "", false, 24},
        {"LIVE", &live[4], 25 * minute, "while-playing", "2 keybinds, 1 game setting",
         "Toggle Power - Shields \xC2\xB7 Keyboard: Right Alt + O \xE2\x86\x92 Right Alt + S   (+2 more)", "", false, 25},
        {"PTU", &ptu[0], 3 * day, "start", "First backup", "", "", false, 24},
        {"PTU", &ptu[1], 50 * minute, "while-playing", "22 keybinds",
         "Map \xC2\xB7 Keyboard: F3 \xE2\x86\x92 F2 (default)   (+21 more)", "reset", false, 2},
    };
    for (const Made& m : made) {
        const long long at = now - m.age;
        const std::time_t t = static_cast<std::time_t>(at / 1000);
        std::tm local{}, utc{};
        localtime_s(&local, &t);
        gmtime_s(&utc, &t);
        char id[32], created[48];
        std::strftime(id, sizeof id, "%Y-%m-%d_%H-%M-%S", &local);
        std::strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%S", &utc);
        const std::filesystem::path folder = out.backups / m.channel / id;
        put(folder / "user", *m.files);
        GF::Set set;
        int characters = 0;
        for (const auto& f : *m.files) {
            set[f.first].data = f.second;
            if (GF::kindOf(f.first) == GF::Kind::Character) characters++;
        }
        const GF::Hashes h = GF::hashesOf(set);
        char ms[16];
        std::snprintf(ms, sizeof ms, ".%03d0000Z", static_cast<int>(at % 1000));
        std::ofstream info(folder / "backup-info.txt", std::ios::binary);
        info << "# SC Blackbox. The game files are in the \"user\" folder next to this file.\r\n"
             << "created=" << created << ms << "\r\nchannel=" << m.channel << "\r\nsource=\r\nreason=" << m.reason
             << "\r\nrestoredfrom=\r\nrestoredparts=\r\nsummary=" << m.summary << "\r\ndetail=" << m.detail
             << "\r\nflag=" << m.flag << "\r\nkept=" << (m.kept ? "1" : "0") << "\r\nbinds=" << m.binds
             << "\r\nallhash=" << h.all << "\r\nkeybindhash=" << h.keybinds << "\r\ncharacters=" << characters << "\r\n";
        if (!std::strcmp(m.channel, "LIVE")) {
            out.liveNewest = id;
            if (m.kept) out.liveKept = id;
        }
    }
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
        else if (direct && target == gui.settingsHandle()) gui.paintSettingsTo(mem);
        else if (direct && target == gui.pluginsWindow()) gui.paintPluginsTo(mem);
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
    // What a phone or tablet is sent of a view (MinerGUI::phonePicture), as
    // wide as it stands on its screen
    auto shootPhone = [&](SCMiner::PhoneServer::View view, int devicePixels, const std::string& name) {
        const auto from = std::chrono::steady_clock::now();
        const SCMiner::PhoneServer::Picture picture = gui.phonePictureForShot(view, devicePixels);
        const auto drawn = std::chrono::steady_clock::now();
        if (picture.width <= 0 || picture.height <= 0) {
            std::cout << name << ": nothing drawn" << std::endl;
            return;
        }
        const std::vector<uint8_t> png = SCMiner::PhoneServer::png(picture);
        const auto packed = std::chrono::steady_clock::now();
        const cv::Mat bgra(picture.height, picture.width, CV_8UC4, const_cast<uint8_t*>(picture.bgra.data()));
        cv::Mat bgr;
        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
        save(bgr, name, true);
        // ...and what it costs: the drawing, on the window's thread, and the
        // PNG the device is sent
        const auto ms = [](auto a, auto b) {
            return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
        };
        std::cout << "    drawn in " << ms(from, drawn) << " ms, PNG " << png.size() / 1024 << " KB in " << ms(drawn, packed)
                  << " ms" << std::endl;
    };


    // The Plugins window, alone: BEDROCK_SHOT_PLUGINS names a folder of
    // plugins (the repository's plugins\), copied into the run's data folder
    // and run there with a state of the run's own - Wikelo with things in
    // stock, a trade or two done, one accepted. Nothing else is shot.
    if (const char* from = DEV_ENV("BEDROCK_SHOT_PLUGINS")) {
        namespace P = SCMiner::Plugins;
        std::filesystem::remove_all(data / "plugins", ec);
        std::filesystem::create_directories(data / "plugins", ec);
        for (const auto& entry : std::filesystem::directory_iterator(from, ec)) {
            if (!std::filesystem::exists(entry.path() / "plugin.json")) continue;   // the repository's tools and all
            std::filesystem::copy(entry.path(), data / "plugins" / entry.path().filename(),
                                  std::filesystem::copy_options::recursive, ec);
            if (ec) std::cout << "plugin not copied: " << ec.message() << std::endl;
        }
        std::filesystem::remove_all(data / "plugin_data", ec);
        const long long now = (long long)std::time(nullptr) * 1000;
        const long long hour = 3600LL * 1000, day = 24 * hour;
        {
            std::filesystem::create_directories(data / "plugin_data", ec);
            std::ofstream out(data / "plugin_data" / "wikelo.json", std::ios::binary);
            out << "{\"since\":" << now - 30 * day << ",\"session\":" << now - 2 * hour
                << ",\"inv\":{\"Monolith Armor Legs\":1,\"Monolith Armor Arms\":1,\"Monolith Armor Core\":1,"
                   "\"Monolith Helmet\":1,\"Vanduul Metal\":6,\"Saldynium (Ore)\":25,\"Carinite (Pure)\":2,"
                   "\"Antium Core\":1,\"Antium Helmet\":1,\"Antium Legs\":1,\"Antium Arms\":1,\"Wikelo Favor\":3,"
                   "\"Vendetta HMG\":1,\"ASD Secure Drive\":4,\"Zenith Laser Sniper Rifle\":1}"
                << ",\"done\":{\"TheCollector_apar_hmg\":{\"n\":1,\"last\":" << now - 8 * day
                << "},\"TheCollector_Favours_CouncilScrip\":{\"n\":3,\"last\":" << now - 8 * day
                << "},\"TheCollector_Intro\":{\"n\":1,\"last\":" << now - 120 * day << "}}"
                << ",\"missions\":{\"a3087271-8cb9-4af0-8519-d02f49806953\":{\"trade\":\"TheCollector_GreyHeavyCombatArmour\","
                   "\"title\":\"Monolith Armor Very Smooth\",\"accepted\":" << now - 90 * 60 * 1000 << ",\"seen\":" << now - 2 * hour
                << ",\"parts\":{\"dropoff_0\":{\"name\":\"Monolith Armor Legs\",\"need\":1,\"handed\":" << now - 40 * 60 * 1000
                << "},\"dropoff_1\":{\"name\":\"Monolith Armor Arms\",\"need\":1,\"handed\":" << now - 40 * 60 * 1000
                << "},\"dropoff_2\":{\"name\":\"Monolith Armor Core\",\"need\":1},\"dropoff_3\":{\"name\":\"Monolith Helmet\","
                   "\"need\":1},\"dropoff_4\":{\"name\":\"Monolith Backpack\",\"need\":1},\"dropoff_5\":{\"name\":\"Vanduul Metal\","
                   "\"need\":10},\"dropoff_6\":{\"name\":\"Saldynium (Ore)\",\"need\":25}}},"
                   "\"4abc0769-bf25-4d0a-820b-148de0bb3f56\":{\"trade\":\"TheCollector_apar_hmg\",\"title\":\"Too Much Gun\","
                   "\"accepted\":" << now - 8 * day - hour << ",\"ended\":\"complete\",\"endedAt\":" << now - 8 * day
                << ",\"parts\":{}}}}";
        }
        // ...and Blackbox on a made-up game folder and backups of the run's own
        const BlackboxShot blackbox = blackboxShotFolders(data / "blackbox", now);
        P::Host host(data / "plugins", data / "plugin_data", BEDROCK_VERSION, P::trustedKeys(), true);
        host.setGameFolder(blackbox.game);
        host.setBackupRoot(blackbox.backups);
        host.load();
        // its first look at the files, as when Bedrock starts: nothing new to
        // back up, PTU's reset noticed
        host.runTimers(true);
        std::string clickNext;   // a click in a plugin's window, for its next draw
        gui.setPluginCallbacks(
            [&](const std::string& id, P::UiState& state) {
                SCMiner::MinerGUI::PluginView view;
                P::Plugin* p = host.plugin(id);
                if (!p) return view;
                if (!clickNext.empty()) {
                    // answered, and the window drawn once more, as after a real one
                    state.clicked = clickNext;
                    clickNext.clear();
                    host.draw(id, state);
                }
                view.items = host.draw(id, state);
                view.title = p->title();
                view.subtitle = p->subtitle();
                return view;
            },
            [](const std::string&, const std::string&) {});
        std::vector<SCMiner::MinerGUI::PluginEntry> list;
        for (const auto& info : host.found()) {
            SCMiner::MinerGUI::PluginEntry e;
            e.id = info.id;
            e.name = info.name;
            e.version = info.version;
            e.description = info.description;
            e.installed = true;
            e.problem = info.problem;
            const P::Plugin* p = host.plugin(info.id);
            e.running = p && p->running();
            if (p && !e.running && info.problem.empty()) e.error = p->error();
            e.offered = info.version;
            list.push_back(e);
            std::cout << "plugin " << info.id << ": " << (e.running ? "running" : e.problem + e.error) << std::endl;
        }
        gui.setPlugins(list, "");
        gui.openPlugin("");
        pump();
        if (gui.pluginsWindow()) shoot(gui.pluginsWindow(), "plugins_list.png", true);
        gui.pluginStateForShot("wikelo", {"t:TheCollector_GreyHeavyCombatArmour"}, {}, {});
        gui.openPlugin("wikelo");
        pump();
        if (gui.pluginsWindow()) shoot(gui.pluginsWindow(), "plugin_wikelo_trades.png", true);
        gui.pluginStateForShot("wikelo", {}, {{"filter", 1}}, {{"find", "antium"}});
        pump();
        if (gui.pluginsWindow()) shoot(gui.pluginsWindow(), "plugin_wikelo_search.png", true);
        gui.pluginStateForShot("wikelo", {}, {{"tab", 2}, {"items", 1}}, {});
        pump();
        if (gui.pluginsWindow()) shoot(gui.pluginsWindow(), "plugin_wikelo_inventory.png", true);
        gui.pluginStateForShot("wikelo", {"m:" + std::to_string(now - 90 * 60 * 1000) + ":TheCollector_GreyHeavyCombatArmour"},
                               {{"tab", 3}}, {});
        pump();
        if (gui.pluginsWindow()) shoot(gui.pluginsWindow(), "plugin_wikelo_log.png", true);
        gui.togglePlugins();
        pump();
        // ...and the main window, the plugin on its rail
        shoot(gui.windowHandle(), "plugins_header.png");
        // ...and the list and Wikelo as pages of the main window, as the
        // rail opens them
        using Rail = SCMiner::MinerGUI::RailItem;
        gui.railPickForShot(Rail::Plugins);
        pump();
        shoot(gui.windowHandle(), "main_page_plugins.png");
        gui.pluginStateForShot("wikelo", {"t:TheCollector_GreyHeavyCombatArmour"}, {}, {});
        gui.railPickForShot(Rail::Plugin, "wikelo");
        pump();
        shoot(gui.windowHandle(), "main_page_wikelo.png");
        // ...and a trade whose parts have long names, on the page's 480 px
        gui.pluginStateForShot("wikelo", {"t:TheCollector_Vehicle_Medium_F7_MK2"}, {}, {{"find", "navy f7"}});
        pump();
        shoot(gui.windowHandle(), "main_page_wikelo_long.png");
        // Blackbox: LIVE's backups, the newest open
        if (host.plugin("blackbox")) {
            const std::string newest = "b:" + blackbox.liveNewest, kept = "b:" + blackbox.liveKept;
            gui.pluginStateForShot("blackbox", {newest}, {{"channel", 1}}, {});
            gui.railPickForShot(Rail::Plugin, "blackbox");
            pump();
            shoot(gui.windowHandle(), "main_page_blackbox.png");
            // ...what changed in it
            clickNext = "show:" + blackbox.liveNewest;
            gui.pluginStateForShot("blackbox", {newest}, {{"channel", 1}}, {});
            pump();
            shoot(gui.windowHandle(), "main_page_blackbox_changes.png");
            clickNext = "show:" + blackbox.liveNewest;
            gui.pluginStateForShot("blackbox", {newest}, {{"channel", 1}}, {});
            // ...restoring the kept one: the part as first picked, before the button
            clickNext = "restore:" + blackbox.liveKept;
            gui.pluginStateForShot("blackbox", {kept}, {{"channel", 1}}, {});
            pump();
            shoot(gui.windowHandle(), "main_page_blackbox_restore.png");
            clickNext = "cancel:" + blackbox.liveKept;
            gui.pluginStateForShot("blackbox", {kept}, {{"channel", 1}}, {});
            // ...and PTU, whose keybinds were reset
            gui.pluginStateForShot("blackbox", {}, {{"channel", 2}}, {});
            pump();
            shoot(gui.windowHandle(), "main_page_blackbox_reset.png");
        }
        gui.railPickForShot(Rail::Scan);
        pump();
        return 0;
    }
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
    // ...and a route plotted through the Aaron Halo: the band to leave
    // quantum travel in, between the place and the signature, until the
    // next scan
    gui.setPlace("Stanton - ARC-L1", "", true);
    gui.setRoute("ARC-L1", "CRU-L4");
    pump();
    shoot(gui.windowHandle(), "main_halo.png");
    gui.setRoute("", "");
    gui.setPlace("Pyro - Monox", analyzer.placeFilter());
    pump();
    // ...and the three matches the place rules out, opened from the line
    // they fold into
    gui.matchesOpenForShot(true);
    pump();
    shoot(gui.windowHandle(), "main_matches_open.png");
    gui.matchesOpenForShot(false);
    pump();
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
    // The settings, behind the gear: Bedrock's own colours; how someone with
    // each kind of colour blindness sees them, and the set made for them;
    // the main window painted with one of the sets; and a colour of one's own
    {
        using namespace SCMiner;
        // the box that shares rock scans, as a release shows it: this build
        // carries no service's address
        gui.setRockShare(true, true);
        gui.openSettingsForShot();
        pump();
        shoot(gui.settingsHandle(), "settings.png", true);
        shoot(gui.windowHandle(), "main_settings_open.png");
        gui.settingsForShot(-1, Colours::Rare, (int)Colours::Vision::Deuteranopia);
        pump();
        shoot(gui.settingsHandle(), "settings_bedrock_deuteranopia.png", true);
        const char* const kinds[3] = {"protanopia", "deuteranopia", "tritanopia"};
        for (int k = 1; k <= 3; k++) {
            gui.settingsForShot(k, Colours::Good, k);
            pump();
            shoot(gui.settingsHandle(), std::string("settings_") + kinds[k - 1] + ".png", true);
        }
        gui.settingsForShot((int)Colours::Set::Deuteranopia, Colours::LookedFor, 0);
        pump();
        shoot(gui.windowHandle(), "main_deuteranopia.png");
        gui.settingsForShot((int)Colours::Set::Bedrock, Colours::Legendary, 0);
        gui.settingsColourForShot(Colours::Epic, 0xD25AB4);
        pump();
        shoot(gui.settingsHandle(), "settings_own.png", true);
        gui.setRockShare(false, true);
        pump();
        shoot(gui.settingsHandle(), "settings_rocks_off.png", true);
        gui.setRockShare(true, true);
        // ...and the overlay let see more of the game, in the preview at its
        // real size; put back after
        gui.settingsForShot((int)Colours::Set::Bedrock, Colours::Legendary, 0);
        gui.setOverlayOpacityForShot(65);
        pump();
        shoot(gui.settingsHandle(), "settings_overlay_65.png", true);
        gui.setOverlayOpacityForShot(92);
        // ...and the phone switched on: its code and its address, with a
        // device watching. It listens on this machine alone here, and says
        // it is at a home network's address, the same in every picture.
        // Then what the device is sent of the window, at a phone's width.
        if (gui.phoneForShot("192.168.178.20")) {
            PhoneServer::fetchLocal(gui.phonePort(), "/" + gui.phoneToken() + "/state?view=window&w=1170");
            pump();
            shoot(gui.settingsHandle(), "settings_phone.png", true);
            shootPhone(PhoneServer::View::Window, 1170, "phone_window.png");
            shootPhone(PhoneServer::View::Window, 1640, "phone_window_tablet.png");
            // ...and used from there: a tap on the ores field opens their
            // list on the phone's picture, a tap off it shuts it again
            int tx = 0, ty = 0;
            shootPhone(PhoneServer::View::Window, 1170, "phone_window_before_tap.png");
            if (gui.phoneSpotForShot(PhoneServer::View::Window, "looking", tx, ty)) {
                std::cout << "phone tap on the ores field at " << tx << "," << ty << std::endl;
                gui.phoneTapForShot(PhoneServer::View::Window, tx, ty);
                pump();
                shootPhone(PhoneServer::View::Window, 1170, "phone_window_tapped.png");
                gui.phoneTapForShot(PhoneServer::View::Window, 6, 6);
                pump();
                shootPhone(PhoneServer::View::Window, 1170, "phone_window_tapped_off.png");
            } else {
                std::cout << "phone_window_tapped.png: no ores field to tap" << std::endl;
            }
            // ...and what works at the PC alone is not in it: the ? card's
            // links are words there, the ship's crafted bonus a figure, and
            // a card the PC answers - the Datarunner's - leaves the page
            // under it
            gui.openAboutForShot();
            pump();
            shootPhone(PhoneServer::View::Window, 1170, "phone_window_about.png");
            gui.openAboutForShot();   // (shut again)
            gui.showLoadoutForShot(true);
            pump();
            shootPhone(PhoneServer::View::Window, 1170, "phone_window_loadout.png");
            gui.showLoadoutForShot(false);
            gui.railPickForShot(SCMiner::MinerGUI::RailItem::Datarunner);
            pump();
            shootPhone(PhoneServer::View::Window, 1170, "phone_window_pc_card.png");
            gui.railPickForShot(SCMiner::MinerGUI::RailItem::Scan);
            pump();
            // BEDROCK_SHOT_PHONE_HOLD=30 keeps it up that many seconds more,
            // drawing for a device as it does, for a browser on this machine
            // to be pointed at the address it prints
            if (const char* hold = DEV_ENV("BEDROCK_SHOT_PHONE_HOLD")) {
                const int seconds = std::atoi(hold);
                std::cout << "phone: http://127.0.0.1:" << gui.phonePort() << "/" << gui.phoneToken() << "/ for "
                          << seconds << " s" << std::endl;
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                while (std::chrono::steady_clock::now() < until) pump();
            }
            gui.phoneOffForShot();
        } else {
            std::cout << "settings_phone.png: the phone did not start" << std::endl;
        }
        gui.hideSettingsForShot();
        pump();
    }
    gui.toggleOverlayMode();
    pump();
    shoot(gui.windowHandle(), "overlay.png");
    gui.setOverlayTextForShot(1.5f);
    pump();
    shoot(gui.windowHandle(), "overlay_text_150_matches.png");
    gui.setOverlayTextForShot(1.0f);
    pump();
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
    gui.setPlace("Nyx - Open space", "", true);
    scanned(28000, 96);
    pump();
    shoot(gui.windowHandle(), "main_space.png");
    // 12,855 is Aluminium x3 and nothing else. In Stanton its list has
    // Lagrange C, five points long, and from Pyro the belt with a sentence
    // for a note: rows that take a second line. Then back to open space at
    // Nyx for what follows.
    analyzer.setSystemFilter("STANTON");
    gui.setCurrentSystem("Stanton");
    gui.setPlace("Stanton - Open space", "", true);
    scanned(12855, 97);
    pump();
    shoot(gui.windowHandle(), "main_long_places.png");
    analyzer.setSystemFilter("NYX");
    gui.setCurrentSystem("NYX");
    gui.setPlace("Nyx - Open space", "", true);
    scanned(28000, 96);
    pump();
    // The rock view: what the window shows while the game's scan panel is up.
    // The figures are the ones off the screenshot in tests/scanpanel - a Tin
    // rock of 22,593, read at 28.4 m by a Helix I with Focus III and
    // Rieger-C3. Here the MOLE's Helix II carries the same two; it reaches
    // twice as far, so the rock stands at 57 m, where it is at 55 % as the
    // Helix I was at 28.4. The advice is to close in rather than to fit
    // anything, which is the case worth having a picture of.
    {
                // A ship picked, so the header shows the ship row rather than the
        // notice - and the loadout card has something to be about.
        gui.pickShipForShot("mole");
        // A MOLE fitted the way one actually gets fitted: a forgiving wide
        // head, a hard one, and a Helix II with Focus III and Rieger-C3 -
        // S2 heads all, as its turrets take no other size.
        SCMiner::MiningLoadout fit;
        fit.laser = SCMiner::laserById("thcn_helix_s2");
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
        // ...and the screenshot's Helix I, for the Prospector further down
        SCMiner::MiningLoadout helixOne = fit;
        helixOne.laser = SCMiner::laserById("thcn_helix_s1");

        SCMiner::RockStats rock;
        rock.ore = "TIN (ORE)";
        rock.mass = 22593;
        rock.resistance = 0;
        rock.instability = 3.52f;
        rock.scu = 30.21f;
        rock.distance = 57.0f;
        gui.updateRock(rock);
        pump();
        shoot(gui.windowHandle(), "rock_far.png");

        // ...and the same rock once the ship has closed in
        rock.distance = 12.0f;
        gui.updateRock(rock);
        pump();
        shoot(gui.windowHandle(), "rock_near.png");

        // ...and in a MOLE's turret, the player's own head known by its HUD
        // (setTurretLaser) and not: the MOLE fitted as it was flown - two
        // crafted Helix II with three Rieger-C3 each, a crafted Hofstede-S2
        // with FLTR-XL and Rieger-C3 - and two Stilerons it was at, one at
        // 48 m from the Hofstede's turret, one at 27 m from a Helix II's
        {
            const SCMiner::MiningModule* rieger = SCMiner::moduleById("passive_rieger_mk3");
            SCMiner::MiningLoadout helixTwo;
            helixTwo.laser = SCMiner::laserById("thcn_helix_s2");
            helixTwo.craftedPower = 28.84f;
            helixTwo.modules = {rieger, rieger, rieger};
            SCMiner::MiningLoadout hofstede;
            hofstede.laser = SCMiner::laserById("shin_hofstede_s2");
            hofstede.craftedPower = 21.72f;
            hofstede.modules = {SCMiner::moduleById("passive_fltr_mk3"), rieger};
            gui.setHeadForShot(0, helixTwo);
            gui.setHeadForShot(1, hofstede);
            gui.setHeadForShot(2, helixTwo);
            SCMiner::RockStats stileron;
            stileron.ore = "STILERON (ORE)";
            stileron.mass = 4290;
            stileron.resistance = 44;
            stileron.resistanceAdjusted = true;
            stileron.instability = 168.75f;
            stileron.scu = 6.32f;
            stileron.distance = 48.0f;
            gui.updateRock(stileron);
            pump();
            shoot(gui.windowHandle(), "rock_turret_48_unknown.png");
            gui.setTurretLaser(hofstede.laser);
            pump();
            shoot(gui.windowHandle(), "rock_turret_48_hofstede.png");
            gui.setTurretLaser(nullptr);
            stileron.mass = 5337;
            stileron.instability = 209.92f;
            stileron.scu = 7.86f;
            stileron.distance = 27.0f;
            gui.updateRock(stileron);
            pump();
            shoot(gui.windowHandle(), "rock_turret_27_unknown.png");
            gui.setTurretLaser(helixTwo.laser);
            pump();
            shoot(gui.windowHandle(), "rock_turret_27_helix.png");
            gui.setTurretLaser(nullptr);
            gui.setHeadForShot(0, fit);
            gui.setHeadForShot(1, wide);
            gui.setHeadForShot(2, hard);
            gui.updateRock(rock);
            pump();
        }

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
            // ...the same on a phone, the overlay and the window both, while
            // the overlay is what is on screen
            shootPhone(SCMiner::PhoneServer::View::Overlay, 1170, "phone_overlay_rock.png");
            shootPhone(SCMiner::PhoneServer::View::Window, 1170, "phone_window_rock.png");
            // ...and for the web page, on a phone and on a tablet standing
            // upright (1640 is one at twice its size), at no distance: Bedrock
            // does not read the distance in the game yet, so the page's
            // pictures show none
            {
                SCMiner::RockStats site = rock;
                site.distance = 0;
                gui.updateRock(site);
                gui.setRockContents(holds);
                pump();
                shootPhone(SCMiner::PhoneServer::View::Overlay, 1170, "site_phone_overlay.png");
                shootPhone(SCMiner::PhoneServer::View::Window, 1170, "site_phone_window.png");
                shootPhone(SCMiner::PhoneServer::View::Overlay, 1640, "site_tablet_overlay.png");
                shootPhone(SCMiner::PhoneServer::View::Window, 1640, "site_tablet_window.png");
                gui.updateRock(rock);
                gui.setRockContents(holds);
                pump();
            }
            // ...and LIVE pressed on the phone's overlay, and pressed again
            {
                int lx = 0, ly = 0;
                if (gui.phoneForShot("192.168.178.20") &&
                    gui.phoneSpotForShot(SCMiner::PhoneServer::View::Overlay, "live", lx, ly)) {
                    gui.phoneTapForShot(SCMiner::PhoneServer::View::Overlay, lx, ly);
                    pump();
                    shootPhone(SCMiner::PhoneServer::View::Overlay, 1170, "phone_overlay_live_tapped.png");
                    gui.phoneTapForShot(SCMiner::PhoneServer::View::Overlay, lx, ly);
                    pump();
                } else {
                    std::cout << "phone_overlay_live_tapped.png: no LIVE to tap" << std::endl;
                }
                gui.phoneOffForShot();
            }
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
            // ...grown and shrunk by its grip
            gui.setOverlayZoomForShot(0.75f);
            pump();
            shoot(gui.windowHandle(), "overlay_zoom_75.png");
            gui.setOverlayZoomForShot(1.5f);
            pump();
            shoot(gui.windowHandle(), "overlay_zoom_150.png");
            gui.setOverlayZoomForShot(1.0f);
            pump();
            // ...and only its text bigger (Ctrl+plus in the overlay): as wide
            // as before, taller only by the bigger lines
            gui.setOverlayTextForShot(1.25f);
            pump();
            shoot(gui.windowHandle(), "overlay_text_125.png");
            gui.setOverlayTextForShot(1.5f);
            pump();
            shoot(gui.windowHandle(), "overlay_text_150.png");
            gui.setOverlayTextForShot(0.8f);
            pump();
            shoot(gui.windowHandle(), "overlay_text_80.png");
            gui.setOverlayTextForShot(1.0f);
            pump();
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
            // ...and in the rock view what a player on UEX pays for that 975,
            // under the list in the full window and in the overlay, its ores
            // one to a row and on one line
            {
                const long long now = (long long)std::time(nullptr);
                gui.setUexPrices(true, nullptr, 0, "");
                gui.setUexMarket("Hephaestanite", shotListings("Hephaestanite", now), now - 60, "");
                pump();
                shoot(gui.windowHandle(), "rock_contents_market.png");
                gui.toggleOverlayMode();
                pump();
                shoot(gui.windowHandle(), "overlay_rock_contents_market.png");
                gui.setOverlayOreRows(false);
                pump();
                shoot(gui.windowHandle(), "overlay_rock_contents_market_line.png");
                gui.setOverlayOreRows(true);
                gui.toggleOverlayMode();
                pump();
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
        // ...and for the web page at no distance: Bedrock does not read the
        // distance in the game yet, so the page's pictures show none
        {
            SCMiner::RockStats site = rock;
            site.distance = 0;
            gui.updateRock(site);
            pump();
            shoot(gui.windowHandle(), "site_rock_hard.png");
            gui.updateRock(rock);
            pump();
        }

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
        // ...and for the web page: the whole MOLE on that rock, no gadget on
        // it and no distance (see site_rock_hard.png)
        {
            SCMiner::RockStats site = rock;
            site.distance = 0;
            gui.updateRock(site);
            pump();
            shoot(gui.windowHandle(), "site_rock_wont_hold.png");
        }

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
        gui.setHeadForShot(0, helixOne);
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
            gui.setPlace("Nyx - Open space", "", true);
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

        // ...and the same head with two of the same module, which a head
        // with the slots for them takes: the multipliers stack, so 1.25 x
        // 1.25 is a real fit.
        {
            SCMiner::MiningLoadout stacked;
            stacked.laser = SCMiner::laserById("thcn_helix_s2");
            stacked.modules = {SCMiner::moduleById("passive_rieger_mk3"),
                               SCMiner::moduleById("passive_rieger_mk3")};
            gui.setHeadForShot(0, stacked);
            pump();
            shoot(gui.windowHandle(), "loadout_stacked.png");
            // ...and Brandt beside Stampede, which cut each other off: the
            // card says so under the list, and which of the two counts
            SCMiner::MiningLoadout cutOff;
            cutOff.laser = SCMiner::laserById("thcn_helix_s2");
            cutOff.modules = {SCMiner::moduleById("active_brandt"), SCMiner::moduleById("active_stampede")};
            gui.setHeadForShot(0, cutOff);
            pump();
            shoot(gui.windowHandle(), "loadout_cut_off.png");
            const SCMiner::LaserOutput both = SCMiner::outputOf(stacked, 0);
            std::cout << "  two Rieger-C3 on a Helix II: " << both.fracture
                      << " fracture power" << std::endl;
            gui.setHeadForShot(0, fit);
            pump();
        }

        gui.showLoadoutForShot(false);
        pump();

        // The Gear page, as the rail opens it: the MOLE as its player fits it
        // - a Helix II with three Rieger-C3 on the left, a Hofstede-S2 in
        // the middle, a Helix II on the right - at Checkmate in Pyro, the
        // shops UEX listed on 8 November 2026 (shotGearShops)
        {
            using Rail = SCMiner::MinerGUI::RailItem;
            const SCMiner::MiningModule* rieger = SCMiner::moduleById("passive_rieger_mk3");
            SCMiner::MiningLoadout portHead;
            portHead.laser = SCMiner::laserById("thcn_helix_s2");
            portHead.modules = {rieger, rieger, rieger};
            SCMiner::MiningLoadout middleHead;
            middleHead.laser = SCMiner::laserById("shin_hofstede_s2");
            SCMiner::MiningLoadout starboardHead;
            starboardHead.laser = SCMiner::laserById("thcn_helix_s2");
            gui.setHeadForShot(0, portHead);
            gui.setHeadForShot(1, middleHead);
            gui.setHeadForShot(2, starboardHead);
            gui.setWhereAt("Checkmate", "Pyro");
            const int helixTwo = (int)(SCMiner::laserById("thcn_helix_s2") - SCMiner::MINING_LASERS);
            const SCMiner::MiningModule* focusThree = SCMiner::moduleById("passive_focus_mk3");
            const int fltrXl = (int)(SCMiner::moduleById("passive_fltr_mk3") - SCMiner::MINING_MODULES);
            const int boreMax = (int)(SCMiner::gadgetById("thcn_boremax") - SCMiner::MINING_GADGETS);
            // the lists not there yet - and UEX asked for them, the page being up
            gui.railPickForShot(Rail::Gear);
            pump();
            std::cout << "  the Gear page wants UEX's lists: " << (gui.gearListsWanted() ? "yes" : "NO") << std::endl;
            shoot(gui.windowHandle(), "gear_loading.png");
            // ...not to be had, and none kept
            gui.setGearShops(nullptr, "Server responded 503");
            pump();
            shoot(gui.windowHandle(), "gear_unreachable.png");
            // ...and with them: the heads the MOLE takes, the two it carries
            // framed green - nothing to fit here, that is Edit ship's
            gui.setGearShops(shotGearShops(), "");
            gui.gearStateForShot(0, "");
            pump();
            shoot(gui.windowHandle(), "gear_heads.png");
            int fitPieces = 0;
            for (int kind : {1, 2, 3, 4, 8}) {
                for (int value = 0; value < 64; value++) {
                    fitPieces += gui.clickGearForShot(kind, value) ? 1 : 0;
                }
            }
            std::cout << "  something to fit on the Gear page: " << (fitPieces ? "YES" : "no") << std::endl;
            // ...the fitted head's shops unfolded by a click on its line: the
            // green frame round its row alone, the shops in a panel under it
            const bool unfolded = gui.clickGearForShot(5, helixTwo);
            pump();
            shoot(gui.windowHandle(), "gear_heads_open.png");
            // ...folded again by a click on its row, and by one on Less
            const bool rowClick = gui.clickGearForShot(9, helixTwo);
            pump();
            const bool foldedByRow = rowClick && !gui.clickGearForShot(6, helixTwo);
            const bool unfoldedAgain = gui.clickGearForShot(5, helixTwo);
            pump();
            const bool lessClick = unfoldedAgain && gui.clickGearForShot(6, helixTwo);
            pump();
            const bool foldedByLess = lessClick && !gui.clickGearForShot(9, helixTwo);
            std::cout << "  the Helix II's shops unfolded by a click on its line: " << (unfolded ? "yes" : "NO")
                      << ", folded by a click on its row: " << (foldedByRow ? "yes" : "NO")
                      << ", by one on Less: " << (foldedByLess ? "yes" : "NO") << std::endl;
            // ...Edit ship over the page
            gui.showLoadoutForShot(true);
            pump();
            shoot(gui.windowHandle(), "gear_edit_ship.png");
            gui.showLoadoutForShot(false);
            // the modules: the two the left head carries framed green, the
            // Rieger-C3 with its count
            portHead.modules = {rieger, rieger, focusThree};
            gui.setHeadForShot(0, portHead);
            gui.gearStateForShot(1, "");
            pump();
            std::cout << "  the modules: " << gui.gearHeadForShot() << std::endl;
            shoot(gui.windowHandle(), "gear_modules.png");
            // ...three notches of the wheel further down
            SendMessageW(gui.windowHandle(), WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(short)(-3 * WHEEL_DELTA)), 0);
            pump();
            shoot(gui.windowHandle(), "gear_modules_scrolled.png");
            // ...a module's shops unfolded
            gui.gearStateForShot(1, "");
            pump();
            const bool moduleUnfolded = gui.clickGearForShot(5, fltrXl);
            pump();
            std::cout << "  the FLTR-XL's shops unfolded: " << (moduleUnfolded ? "yes" : "NO") << std::endl;
            shoot(gui.windowHandle(), "gear_modules_open.png");
            gui.clickGearForShot(9, fltrXl);
            pump();
            // the gadgets: prices and shops alone, no button - and BoreMax
            // on the rock in the rock view leaves them as they are
            gui.setFittedGadget(SCMiner::gadgetById("thcn_boremax"));
            gui.gearStateForShot(2, "");
            pump();
            const bool button = gui.clickGearForShot(7, boreMax);
            std::cout << "  gadgets: " << (button ? "A BUTTON" : "no button") << std::endl;
            shoot(gui.windowHandle(), "gear_gadgets.png");
            // ...the pointer on BoreMax's i: what it does over the list -
            // then on Waveshift's, the last
            const int waveshift = (int)(SCMiner::gadgetById("grin_waveshift") - SCMiner::MINING_GADGETS);
            const bool boreInfo = gui.pointGearForShot(10, boreMax);
            pump();
            shoot(gui.windowHandle(), "gear_gadgets_info.png");
            const bool waveInfo = gui.pointGearForShot(10, waveshift);
            pump();
            shoot(gui.windowHandle(), "gear_gadgets_info_last.png");
            gui.pointGearForShot(-1, 0);
            pump();
            std::cout << "  a gadget's i under the pointer: " << (boreInfo && waveInfo ? "yes" : "NO I") << std::endl;
            gui.setFittedGadget(nullptr);
            // a ship of one head: no heads to pick between
            gui.pickShipForShot("prospector");
            gui.gearStateForShot(0, "");
            pump();
            shoot(gui.windowHandle(), "gear_prospector.png");
            // ...and back the way the pictures after these expect it
            gui.railPickForShot(Rail::Scan);
            gui.setGearShops(nullptr, "");
            gui.setWhereAt("", "");
            gui.pickShipForShot("mole");
            gui.setHeadForShot(0, fit);
            gui.setHeadForShot(1, wide);
            gui.setHeadForShot(2, hard);
            pump();
        }
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
        // ...and as a new ship's card or a click on the title opens it: the
        // name marked, and a letter typed over it whole
        gui.startRenameForShot();
        pump();
        shoot(gui.windowHandle(), "loadout_name_marked.png");
        for (const wchar_t c : std::wstring(L"Big")) SendMessageW(gui.windowHandle(), WM_CHAR, (WPARAM)c, 0);
        pump();
        std::cout << "  the marked name typed over with Big: "
                  << (gui.nameEntryForShot() == L"Big" ? "Big" : "NOT REPLACED") << std::endl;
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
    // a key pasted - whether to send refinery jobs
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
        // ...and No said: the account kept, no refinery jobs sent
        account.note = "Account connected, reporting off";
        gui.setUexAccount(account);
        gui.openUexWelcomeForShot(1);
        pump();
        shoot(gui.windowHandle(), "uex_welcome_kept.png");
        // ...and refinery jobs going, the last one taken; then UEX took
        // nothing from the account - then the same in the narrowest window
        account.on = true;
        account.since = (long long)std::time(nullptr) - 3600;
        account.note = "Refinery job at Levski: accepted by UEX";
        account.noteAt = (long long)std::time(nullptr) - 40;
        gui.setUexAccount(account);
        gui.openUexWelcomeForShot(1);
        pump();
        shoot(gui.windowHandle(), "uex_reported.png");
        account.accountTrouble = "datarunner";
        account.note = "Refinery job at Levski: UEX doesn't take reports from this account";
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
        gui.openUexWelcomeForShot(0);
        gui.setUexAccount(SCMiner::MinerGUI::UexAccount());
        pump();
    }
    // The UEX Datarunner: its part of the UEX card, off and then on; the
    // foot of the main window with pictures waiting; and the card - a
    // picture read whole, one with values to check and missing and a figure
    // being typed, a terminal that is one of two with its list down, a
    // commodity's list, a cargo box picked, sent, and turned down with the
    // way to sign up; the narrowest window last
    {
        namespace D = SCMiner::Datarunner;
        using SCMiner::Certainty;
        const long long now = (long long)std::time(nullptr);
        SCMiner::MinerGUI::UexAccount account;
        account.hasToken = true;
        account.keyEnd = "7f3a";
        gui.setUexAccount(account);
        gui.openUexWelcomeForShot(1);
        pump();
        shoot(gui.windowHandle(), "uex_datarunner_off.png");
        account.datarunner = true;
        account.datarunnerSince = now - 60;
        account.note = "Datarunner on";
        account.noteAt = now - 5;
        gui.setUexAccount(account);
        pump();
        shoot(gui.windowHandle(), "uex_datarunner_on.png");
        gui.openUexWelcomeForShot(0);
        gui.setUexAccount(SCMiner::MinerGUI::UexAccount());
        pump();

        auto names = std::make_shared<std::vector<std::pair<std::string, int>>>(std::vector<std::pair<std::string, int>>{
            {"Hephaestanite", 57}, {"Iron", 39}, {"Aluminum", 1}, {"Argon", 105}, {"Ship Ammunition - Size 1", 201},
            {"Ship Ammunition - Size 2", 202}, {"Ship Ammunition - Size 3", 203}, {"Gold", 30}, {"Hydrogen", 33},
            {"Hydrogen Fuel", 120}, {"Iodine", 35}, {"Titanium", 24}, {"Tin", 25}});
        auto terminals = std::make_shared<std::vector<std::pair<int, std::string>>>(std::vector<std::pair<int, std::string>>{
            {18, "CBD - Central Business District - Lorville"}, {51, "Admin - L19 Residences - Metro Center - Lorville"},
            {252, "Admin - Pyro Gateway (Stanton)"}, {436, "Admin - Checkmate"}});
        const std::vector<D::Row> read = shotDatarunnerRows(*names);
        // ...and as the player completes them: a level the picture cut off
        // at its foot, picked (only the picture with doubts keeps it missing)
        std::vector<D::Row> rows = read;
        for (D::Row& row : rows) {
            if (row.level >= 1) continue;
            row.level = 7;
            row.levelSure = Certainty::Sure;
        }
        auto shotOf = [&](int id) {
            auto shot = std::make_shared<D::Shot>();
            shot->id = id;
            shot->file = "ScreenShot_" + std::to_string(id) + ".jpg";
            shot->taken = now - 120;
            shot->width = 5120;
            shot->height = 1440;
            shot->channel = "LIVE";
            shot->side = 1;
            shot->sideSure = true;
            shot->shop = "SCShop_Admin_lt_base_g";
            shot->place = "Pyro Gateway (Stanton)";
            shot->terminals = {{252, "Admin - Pyro Gateway (Stanton)"}};
            shot->terminal = 252;
            shot->rows = rows;
            shot->matched = true;
            return shot;
        };
        auto good = shotOf(1);
        // read with doubts: a price and a level to check, a name UEX does
        // not have, a stock not made out
        auto doubts = shotOf(2);
        doubts->rows = read;
        if (doubts->rows.size() >= 5) {
            doubts->rows[1].priceSure = Certainty::Check;
            doubts->rows[2].levelSure = Certainty::Check;
            doubts->rows[3].commodity = 0;
            doubts->rows[3].read = "ARG0N1";
            doubts->rows[3].nameSure = Certainty::Missing;
            doubts->rows[4].stock = -1;
            doubts->rows[4].stockSure = Certainty::Missing;
            doubts->rows[0].removed = true;
        }
        // at a city with two terminals, neither picked
        auto city = shotOf(3);
        city->place = "Lorville";
        city->shop = "SCShop_Admin_hur";
        city->terminals = {{18, "CBD - Central Business District - Lorville"},
                           {51, "Admin - L19 Residences - Metro Center - Lorville"}};
        city->terminal = 0;
        // a cargo box picked in the game: one box's price on the picture
        auto box = shotOf(4);
        if (!box->rows.empty()) {
            box->rows[0].perUnit = true;
            box->rows[0].price = 1879;
            box->rows[0].priceSure = Certainty::Missing;
        }
        auto stateOf = [&](const std::vector<std::shared_ptr<D::Shot>>& shots, bool signUp) {
            SCMiner::MinerGUI::DatarunnerState state;
            for (const auto& shot : shots) {
                state.shots.push_back(shot);
                state.blocked.push_back(D::whyNotSend(*shot, {}, now));
                state.holds.push_back(std::vector<int>(shot->rows.size(), 0));
                state.versions.push_back("4.10.2");
            }
            state.commodities = names;
            state.terminals = terminals;
            state.signUp = signUp;
            return state;
        };
        const std::vector<std::shared_ptr<D::Shot>> waiting = {good, doubts, city, box};
        gui.setDatarunner(stateOf(waiting, false));
        pump();
        shoot(gui.windowHandle(), "main_datarunner_waiting.png");
        // ...its page, as the rail opens it (its introduction put away: it
        // has pictures of its own further on)
        gui.railPickForShot(SCMiner::MinerGUI::RailItem::Datarunner);
        gui.closeDatarunnerIntroForShot();
        pump();
        shoot(gui.windowHandle(), "main_page_datarunner.png");
        gui.openDatarunnerForShot(0);
        pump();
        shoot(gui.windowHandle(), "datarunner_ready.png");
        gui.openDatarunnerForShot(1);
        pump();
        shoot(gui.windowHandle(), "datarunner_check.png");
        gui.openDatarunnerForShot(1, 7, 1, L"716");
        pump();
        shoot(gui.windowHandle(), "datarunner_typing.png");
        gui.openDatarunnerForShot(1, 8, 3);
        pump();
        shoot(gui.windowHandle(), "datarunner_commodity_list.png");
        gui.openDatarunnerForShot(1, 5, 2);
        pump();
        shoot(gui.windowHandle(), "datarunner_level_list.png");
        gui.openDatarunnerForShot(2);
        pump();
        shoot(gui.windowHandle(), "datarunner_terminal.png");
        gui.openDatarunnerForShot(2, 9, -1);
        pump();
        shoot(gui.windowHandle(), "datarunner_terminal_list.png");
        gui.openDatarunnerForShot(3);
        pump();
        shoot(gui.windowHandle(), "datarunner_cargo_box.png");
        // sent, then turned down - and an account UEX takes nothing from yet
        auto sent = shotOf(5);
        sent->state = D::Shot::State::Sent;
        sent->ids = std::vector<std::string>(sent->rows.size(), "964307");
        auto refused = shotOf(6);
        refused->state = D::Shot::State::Refused;
        refused->note = "Already reported in the last 5 minutes";
        auto notRunner = shotOf(7);
        notRunner->state = D::Shot::State::Refused;
        notRunner->note = "UEX does not accept reports from this account";   // UexReports::verdict's words
        gui.setDatarunner(stateOf({sent, refused}, false));
        gui.openDatarunnerForShot(0);
        pump();
        shoot(gui.windowHandle(), "datarunner_sent.png");
        gui.openDatarunnerForShot(1);
        pump();
        shoot(gui.windowHandle(), "datarunner_refused.png");
        gui.setDatarunner(stateOf({notRunner}, true));
        gui.openDatarunnerForShot(0);
        pump();
        shoot(gui.windowHandle(), "datarunner_sign_up.png");
        // the narrowest, shortest window: the rows scroll
        gui.setDatarunner(stateOf(waiting, false));
        RECT was = {};
        GetWindowRect(gui.windowHandle(), &was);
        SetWindowPos(gui.windowHandle(), nullptr, 0, 0, 1, 1, SWP_NOMOVE | SWP_NOZORDER);
        gui.openDatarunnerForShot(1);
        pump();
        shoot(gui.windowHandle(), "datarunner_narrow.png");
        SetWindowPos(gui.windowHandle(), nullptr, 0, 0, was.right - was.left, was.bottom - was.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        gui.openDatarunnerForShot(-1);
        gui.setDatarunner(SCMiner::MinerGUI::DatarunnerState());
        pump();
        // ...and the rail's page with no picture waiting - its introduction
        // first, as the rail opens it: as it opens, the third step being
        // read, at the end with the box ticked, in the narrowest window and
        // in a tall one; then put away, the page under it
        gui.railPickForShot(SCMiner::MinerGUI::RailItem::Datarunner);
        gui.datarunnerIntroForShot(0, false);
        pump();
        shoot(gui.windowHandle(), "datarunner_intro.png");
        gui.datarunnerIntroForShot(2, false);
        pump();
        shoot(gui.windowHandle(), "datarunner_intro_step3.png");
        gui.datarunnerIntroForShot(-1, true);
        pump();
        shoot(gui.windowHandle(), "datarunner_intro_end.png");
        {
            RECT size = {};
            GetWindowRect(gui.windowHandle(), &size);
            SetWindowPos(gui.windowHandle(), nullptr, 0, 0, 1, 1, SWP_NOMOVE | SWP_NOZORDER);
            gui.datarunnerIntroForShot(2, false);
            pump();
            shoot(gui.windowHandle(), "datarunner_intro_narrow.png");
            SetWindowPos(gui.windowHandle(), nullptr, 0, 0, size.right - size.left, 1400, SWP_NOMOVE | SWP_NOZORDER);
            gui.datarunnerIntroForShot(2, false);
            pump();
            shoot(gui.windowHandle(), "datarunner_intro_tall.png");
            SetWindowPos(gui.windowHandle(), nullptr, 0, 0, size.right - size.left, size.bottom - size.top,
                         SWP_NOMOVE | SWP_NOZORDER);
        }
        gui.closeDatarunnerIntroForShot();
        pump();
        shoot(gui.windowHandle(), "main_page_datarunner_empty.png");
        gui.railPickForShot(SCMiner::MinerGUI::RailItem::Scan);
        pump();
        // The foot while Bedrock reads a terminal by itself, as at Levski's
        // cargo office on 29 October 2026: no row held still yet; 4 of the
        // 14 wares the log listed for its BUY list; 24 rows of LOCAL MARKET
        // VALUE, which the log does not list; the BUY list whole; and the
        // picture left to check once the player has gone
        if (!rows.empty()) {
            auto listOf = [&](int id, int side, size_t count) {
                auto shot = shotOf(id);
                shot->automatic = true;
                shot->side = side;
                shot->rows.clear();
                for (size_t i = 0; i < count; i++) shot->rows.push_back(rows[i % rows.size()]);
                return shot;
            };
            auto readingOf = [&](const std::vector<std::shared_ptr<D::Shot>>& shots, const std::vector<int>& lengths,
                                 int reading) {
                SCMiner::MinerGUI::DatarunnerState state = stateOf(shots, false);
                state.lengths = lengths;
                state.reading = reading >= 0;
                state.readingShot = (std::max)(reading, 0);
                return state;
            };
            const auto buy = listOf(11, 1, 4);
            const auto sell = listOf(12, 2, 24);
            const auto whole = listOf(13, 1, 14);
            gui.setDatarunner(readingOf({}, {}, 0));
            pump();
            shoot(gui.windowHandle(), "main_datarunner_reading.png");
            gui.setDatarunner(readingOf({buy}, {14}, buy->id));
            pump();
            shoot(gui.windowHandle(), "main_datarunner_reading_rows.png");
            gui.setDatarunner(readingOf({buy, sell}, {14, 0}, sell->id));
            pump();
            shoot(gui.windowHandle(), "main_datarunner_reading_sell.png");
            gui.setDatarunner(readingOf({whole}, {14}, whole->id));
            pump();
            shoot(gui.windowHandle(), "main_datarunner_reading_all.png");
            gui.setDatarunner(readingOf({buy}, {14}, -1));
            pump();
            shoot(gui.windowHandle(), "main_datarunner_left.png");
            gui.setDatarunner(SCMiner::MinerGUI::DatarunnerState());
            pump();
        }
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
    // ...and 2.6.7's thank-you: the same window, one page of its own
    gui.showThanksForShot();
    pump();
    if (gui.newsHandle()) shoot(gui.newsHandle(), "news_thanks.png", true);
    gui.hideNewsForShot();
    pump();
    // ...and 3.0's tour: the same window, every page of it
    gui.showTourForShot();
    for (int page = 0; page < SCMiner::MinerGUI::TOUR_PAGES; page++) {
        gui.tourPageForShot(page);
        pump();
        if (gui.newsHandle()) shoot(gui.newsHandle(), "tour_" + std::to_string(page + 1) + ".png", true);
    }
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
    // ...and the ground's two families: all their gems, each place by its
    // likeliest, a gem a line by its best buyer
    gui.openOreForShot("Hadanite");
    gui.pointInPlannerForShot(2, "Hand minables", true);
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_gems.png", true);
    gui.openOreForShot("Beradom");
    gui.pointInPlannerForShot(2, "ROC deposits", true);
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_gems_roc.png", true);
    // ...a place of theirs: what is mined there, its gems under the ship's ores
    gui.pointInPlannerForShot(10, "Pyro IV", true);
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_place_from_roc.png", true);
    // ...a family's arrow: the list of its gems over the ores, the pointer
    // on one; then that one picked, its places
    {
        gui.pointInPlannerForShot(2, "Hand minables", true);
        const bool arrow = gui.pointInPlannerForShot(13, "Hand minables", true);
        const bool listed = gui.pointInPlannerForShot(14, "Hadanite", false);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_gems_menu.png", true);
        gui.pointInPlannerForShot(14, "Hadanite", true);
        const bool shut = !gui.pointInPlannerForShot(14, "Hadanite", false);
        gui.pointInPlannerForShot(-1, "", false);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_gems_pick.png", true);
        // ...and Stanton's ROC deposits, the list under its row if it fits
        gui.pointInPlannerForShot(1, "STANTON", true);
        gui.pointInPlannerForShot(13, "ROC deposits", true);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_gems_menu_stanton.png", true);
        gui.pointInPlannerForShot(1, "ALL", true);
        pump();
        std::cout << "ore finder gems: " << (arrow ? "arrow" : "NO ARROW") << ", "
                  << (listed ? "Hadanite listed" : "HADANITE NOT LISTED") << ", "
                  << (shut ? "list shut on the pick" : "LIST STILL OPEN") << std::endl;
    }
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
    // ...and an ore with a long name, whose heading and count take two lines,
    // gone again after (the rock view would quote its buyers), and the
    // planner back on the gem it was on
    gui.setUexMarket("Quantainium", shotListings("Quantainium", shotNow), shotNow - 60, "");
    gui.openUexTabForShot("Quantainium");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_market_long.png", true);
    gui.setUexMarket("Quantainium", std::make_shared<std::vector<SCMiner::UexListing>>(), shotNow, "");
    gui.openUexTabForShot("Hand minables", "Hadanite");
    gui.openUexTabForShot(nullptr);
    gui.setUexThreads(nullptr);
    // ...and the UEX account: what it gives and the key, then whether to send
    // refinery jobs, then on - a made-up name for UEX's answer, never the
    // player's
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
        account.on = true;
        account.since = shotNow - 3 * 86400;
        account.user = "DataRunner";
        account.note = "Refinery job at Levski: accepted by UEX";
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
    // ...and the Aaron Halo's: under its ores the way in, band by band, for
    // the route plotted from ARC-L1 to CRU-L4; the list of places to go to
    // open over it (those a route from ARC-L1 runs through the belt to); and
    // a route plotted that does not (ARC-L1 to ArcCorp), for which the
    // nearest place through band 5 stands: HUR-L5, nine bands - the route
    // passes over band 1
    gui.setRoute("ARC-L1", "CRU-L4");
    gui.openHaloForShot("", "", 0);
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_halo.png", true);
    gui.openHaloForShot("", "", 2);
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_halo_to.png", true);
    gui.setRoute("ARC-L1", "ArcCorp");
    gui.openHaloForShot("", "", 0);
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_halo_suggest.png", true);
    gui.openHaloForShot("", "", 0);
    gui.setRoute("", "");
    pump();
    // ...and a place in an ore's list under the pointer, then clicked: what
    // else is mined there, the ground's gems under the ship's ores, in the
    // system on the tabs. Then back by the ore on the left.
    {
        const std::string terminus = "Pyro VI (Terminus)";
        gui.openOreForShot("Stileron");
        gui.pointInPlannerForShot(1, "PYRO", true);
        pump();
        const bool listed = gui.pointInPlannerForShot(10, terminus, false);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_hover.png", true);
        gui.pointInPlannerForShot(10, terminus, true);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_place_gems.png", true);
        const bool opened = !gui.pointInPlannerForShot(10, terminus, false);
        gui.pointInPlannerForShot(0, "Stileron", true);
        const bool back = gui.pointInPlannerForShot(10, terminus, false);
        gui.pointInPlannerForShot(-1, "", false);
        pump();
        std::cout << "ore finder place: " << (listed ? "listed" : "NOT LISTED") << ", "
                  << (opened ? "its card opened" : "CARD NOT OPENED") << ", "
                  << (back ? "back by the ore" : "NOT BACK") << std::endl;
        // ...and a gem of the place clicked: the places of that gem, in the
        // system on the tabs (before kind 11, the family's list)
        gui.pointInPlannerForShot(10, terminus, true);
        pump();
        if (!gui.pointInPlannerForShot(11, "Feynmaline", true)) gui.pointInPlannerForShot(2, "ROC deposits", true);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_place_gem_pick.png", true);
        // ...and a place whose name leaves no room beside it for how many
        gui.openOreForShot("Savrilium");
        gui.pointInPlannerForShot(10, "Breaker Stations Large Geode", true);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_place_long.png", true);
    }
    // ...and an ore with the longest rows there are: Lagrange C names five
    // points, the Pyro belt a sentence. Their small words take a second
    // line rather than running under the figure.
    gui.openOreForShot("Aluminium");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_long.png", true);
    // ...and the places whose surface asks for a suit: Feynmaline's, three
    // hot and Terminus cold; then Aberdeen's card
    gui.openOreForShot("Feynmaline");
    pump();
    if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_suits.png", true);
    {
        const bool listed = gui.pointInPlannerForShot(10, "Aberdeen", true);
        pump();
        if (gui.plannerHandle()) shoot(gui.plannerHandle(), "planner_place_suit.png", true);
        std::cout << "ore finder suits: " << (listed ? "Aberdeen's card" : "ABERDEEN NOT LISTED") << std::endl;
    }
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
    // ...with the refineries' workloads beside them, as the player and the
    // service saw them, and the box that shares them ticked
    {
        const long long now = (long long)std::time(nullptr);
        std::map<std::string, SCMiner::MinerGUI::RefineryWorkload> seen;
        seen["Checkmate"] = {103, 1, now - 40, 1, true};
        seen["Levski"] = {42804, 3, now - 26 * 60, 3, false};
        seen["Pyro Gateway (Nyx)"] = {96, 0, now - 3 * 3600, 2, false};
        seen["Orbituary"] = {148, 2, now - 7 * 60, 2, false};
        seen["HUR-L1 Green Glade Station"] = {-1, 1, now - 12 * 60, 1, false};
        // ...and readings hours and a day old, which keep their colour
        seen["ARC-L1 Wide Forest Station"] = {299, 2, now - 6 * 3600, 1, false};
        seen["Ruin Station"] = {90, 0, now - 22 * 3600, 1, false};
        gui.setRefineryWorkloads(seen);
        gui.setRefineryShare(true, true);
        pump();
        if (gui.refineryHandle()) shoot(gui.refineryHandle(), "refinery_workloads.png", true);
        // ...sorted by price instead: the lowest surcharge first, then the
        // fewest aUEC per SCU refined - a capacity read alone counts by its
        // step - and the same with nothing ticked
        seen["MIC-L5 Modern Icarus Station"] = {62, 0, now - 50 * 60, 1, false};
        seen["Stanton Gateway (Nyx)"] = {30, -1, now - 5 * 60, 1, false};
        seen["CRU-L1 Ambitious Dream Station"] = {304, -1, now - 2 * 3600, 1, false};
        gui.setRefineryWorkloads(seen);
        gui.setRefineryCheapestForShot(true);
        pump();
        if (gui.refineryHandle()) shoot(gui.refineryHandle(), "refinery_cheapest.png", true);
        gui.pickForShot(false);
        pump();
        if (gui.refineryHandle()) shoot(gui.refineryHandle(), "refinery_cheapest_empty.png", true);
        gui.pickForShot();
        gui.setRefineryCheapestForShot(false);
        gui.setRefineryWorkloads({});
        gui.setRefineryShare(false, false);
    }
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
        // ...the calendar code of a running one unfolded for the phone, by
        // clicks on the buttons: the one at Levski, then the one at Checkmate,
        // which folds the first - one at a time - and once more, which folds it
        {
            const bool first = gui.openRefineryCalendarForShot(1);
            pump();
            const bool second = gui.openRefineryCalendarForShot(2);
            pump();
            if (second) shoot(gui.refineryHandle(), "refinery_orders_qr.png", true);
            const bool folded = !gui.openRefineryCalendarForShot(2);
            pump();
            std::cout << "refinery calendar code: " << (first ? "unfolded" : "NOT UNFOLDED") << ", "
                      << (second ? "the other unfolded instead" : "NOT UNFOLDED") << ", "
                      << (folded ? "folded" : "NOT FOLDED") << std::endl;
        }
        // ...where the box in the header still takes a click: along its row
        // from the right edge in, until it answers
        {
            bool answered = false, nowOn = true;
            gui.setRefineryShare(true, true);
            gui.setRefineryShareCallback([&](bool on) { answered = true; nowOn = on; });
            pump();
            RECT rc{};
            GetClientRect(gui.refineryHandle(), &rc);
            for (int x = rc.right - 4; x > rc.right / 2 && !answered; x -= 6) {
                SendMessageW(gui.refineryHandle(), WM_LBUTTONDOWN, 0, MAKELPARAM(x, 28));
            }
            pump();
            std::cout << "refinery share box over the work orders: "
                      << (answered ? (nowOn ? "clicked, on" : "clicked, off") : "NO ANSWER") << std::endl;
            gui.setRefineryShareCallback(nullptr);
            gui.setRefineryShare(false, false);
            pump();
        }
        // ...and the main window's Refinery button with the finished ones
        // counted on it: three of the four done
        orders[1].due = now - 30;
        gui.setRefineryOrders(orders);
        pump();
        shoot(gui.windowHandle(), "main_refinery_done.png");
        // ...two running at one station read under the same number, as at
        // Checkmate on 8 November 2026: the first one's Calendar unfolds the
        // code under the first, not the other
        {
            std::vector<SCMiner::RefineryOrder> two(2);
            two[0].station = "Checkmate";
            two[0].number = 2;
            two[0].parts = {{"Stileron (Ore)", 747, 253}};
            two[0].method = "Pyrometric Chromalysis";
            two[0].cost = 10474;
            two[0].seconds = 1854;
            two[0].placed = now - 31 * 60;
            two[0].due = now + 30 * 60;
            two[0].seen = now - 30 * 60;
            two[1].station = "Checkmate";
            two[1].number = 2;
            two[1].parts = {{"Stileron (Ore)", 330, 2662}, {"Stileron (Ore)", 517, 279}, {"Stileron (Ore)", 681, 275}};
            two[1].method = "Pyrometric Chromalysis";
            two[1].cost = 133210;
            two[1].seconds = 15240;
            two[1].placed = now - 31 * 60 - 26;
            two[1].due = now + 4 * 3600 + 12 * 60;
            two[1].seen = now - 30 * 60;
            gui.setRefineryOrders(two);
            pump();
            const bool first = gui.openRefineryCalendarForShot(0);
            pump();
            shoot(gui.refineryHandle(), "refinery_orders_qr_same_number.png", true);
            const bool other = gui.openRefineryCalendarForShot(1);
            pump();
            std::cout << "refinery calendar code, two under one number: "
                      << (first ? "the first's under the first" : "NOT UNDER THE FIRST") << ", "
                      << (other ? "then the other's under the other" : "NOT UNDER THE OTHER") << std::endl;
            gui.openRefineryCalendarForShot(1);
            pump();
        }
        // ...orders of up to six parts, as quotes shared on 9 November 2026
        // had them: a row for each part, the best quality first, and the
        // calendar code under the last one's rows
        {
            std::vector<SCMiner::RefineryOrder> many(4);
            many[0].station = "MIC-L5 Modern Icarus Station";
            many[0].number = 1;
            many[0].parts = {{"Riccite (Ore)", 325, 94}, {"Riccite (Ore)", 671, 14}, {"Aluminum (Ore)", 783, 18},
                             {"Aslarite (Raw)", 287, 89}, {"Aslarite (Raw)", 602, 7}, {"Agricium (Ore)", 588, 7}};
            many[0].method = "Ferron Exchange";
            many[0].cost = 1165;
            many[0].seconds = 3312;
            many[0].placed = now - 3312 - 25 * 60;
            many[0].due = many[0].placed + 3312;
            many[0].seen = many[0].placed + 5;
            many[1].station = "MIC-L5 Modern Icarus Station";
            many[1].number = 2;
            many[1].parts = {{"Titanium (Ore)", 295, 210}, {"Titanium (Ore)", 784, 348}};
            many[1].method = "Pyrometric Chromalysis";
            many[1].cost = 1335;
            many[1].seconds = 3524;
            many[1].placed = now - 15 * 60;
            many[1].due = many[1].placed + 3524;
            many[1].seen = many[1].placed + 5;
            many[2].station = "Levski";
            many[2].number = 1;
            many[2].parts = {{"Lindinium (Ore)", 305, 568}, {"Lindinium (Ore)", 618, 122}, {"Tungsten (Ore)", 662, 80},
                             {"Savrilium (Ore)", 322, 446}, {"Savrilium (Ore)", 580, 100}};
            many[2].method = "Dinyx Solventation";
            many[2].cost = 9664;
            many[2].seconds = 48060;
            many[2].placed = now - 3600;
            many[2].due = many[2].placed + 48060;
            many[2].seen = many[2].placed + 5;
            many[3].station = "ARC-L1 Wide Forest Station";
            many[3].number = 1;
            many[3].parts = {{"Iron (Ore)", 325, 375}, {"Iron (Ore)", 874, 268},
                             {"Quantainium (Raw)", 344, 887}, {"Quantainium (Raw)", 514, 131},
                             {"Laranite (Raw)", 298, 724}, {"Laranite (Raw)", 858, 166}};
            many[3].method = "Dinyx Solventation";
            many[3].cost = 12938;
            many[3].seconds = 49920;
            many[3].placed = now - 600;
            many[3].due = many[3].placed + 49920;
            many[3].seen = many[3].placed + 5;
            gui.setRefineryOrders(many);
            pump();
            shoot(gui.refineryHandle(), "refinery_orders_parts.png", true);
            const bool unfolded = gui.openRefineryCalendarForShot(3);
            pump();
            if (unfolded) shoot(gui.refineryHandle(), "refinery_orders_parts_qr.png", true);
            gui.openRefineryCalendarForShot(3);
            pump();
        }
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
    // ...with the boxes for the game's texts ticked, and what a write said
    gui.setGameTexts(true, "in the game from its next start (written 20:31)");
    gui.setGameNames(true);
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprints_game_texts.png", true);
    gui.setGameTexts(false, "");
    gui.setGameNames(false);
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
    // Avg. Q: the case a player sent (his tool says 958), then one that
    // mixes pieces and SCU - two Aphorite lenses that barely move the gun
    gui.openQualityForShot("Parallax Energy Assault Rifle", {{"Carinite", 959}, {"Gold", 916}, {"Tungsten", 964}});
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_quality.png", true);
    // ...its calculation unfolded: Gold and Carinite share the damage
    gui.showQualityInfoForShot(true);
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_quality_info_shared.png", true);
    gui.showQualityInfoForShot(false);
    gui.openQualityForShot("Lumin V SMG", {{"Riccite", 965}, {"Copper", 958}, {"Aphorite", 523}});
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_quality_mixed.png", true);
    gui.showQualityInfoForShot(true);
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_quality_info.png", true);
    gui.showQualityInfoForShot(false);
    gui.openQualityForShot("Parallax \"Shock Trooper\" Energy Assault Rifle",
                           {{"Carinite", 959}, {"Gold", 916}, {"Tungsten", 964}});
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_quality_long.png", true);
    // ...and a quantum drive, its speed and fuel high and its Integrity low
    gui.openQualityForShot("Agni", {{"Agricium", 588}, {"Tungsten", 964}, {"Beryl", 955}});
    pump();
    if (gui.craftingHandle()) shoot(gui.craftingHandle(), "blueprint_quality_qdrive.png", true);
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
            // ...and its ranks open under its pill in the rank line, with
            // the way back to the logs
            gui.grindRankMenuForShot(i);
            pump();
            if (gui.grindHandle()) shoot(gui.grindHandle(), "grind_rank_menu.png", true);
            gui.grindRankMenuForShot(-1);
            pump();
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
