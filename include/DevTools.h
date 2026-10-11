#pragma once
// The two measurements the readers are tuned against, on both platforms:
// the signature pipeline over saved frames, and the rock-panel bench over a
// recording cut into frames. They live here rather than in main.cpp (the
// Windows program) because a Linux build has to run the very same code
// over the very same frames before anything about it is believed.
#include <opencv2/core.hpp>

#include <optional>
#include <string>
#include <vector>

namespace SCMiner {
class TextLineReader;
}

// What the app knew when it took a snapshot, from the .txt beside the
// PNG - the aim box and the search area, both relative to the picture
struct SnapshotGeometry { cv::Rect aim; cv::Rect area; };
std::optional<SnapshotGeometry> sidecarGeometry(const std::string& pngFile);

// The second reader the panel reader is given: the one the program carries,
// loaded once, or null where it could not be
SCMiner::TextLineReader* panelLines();

// --panelbench <folder> <out.csv> [shard shards]: every frame of a folder
// through readScanPanel, one CSV row each
int runPanelBench(const std::string& dir, const std::string& outCsv, int shard, int shards);

// --pipeline <files...>: locate and read the signature marker in each saved
// frame the way the live scan does, one line each
int runPipelineOnFiles(const std::vector<std::string>& files);

// --refinery <pictures, or a folder> [--draw <dir>]: the refinery terminal
// read off each picture - the work-order bars found, then every panel's
// fields - one block each; --draw writes the pictures with the bars and the
// places read marked
int runRefinery(const std::vector<std::string>& files, const std::string& drawDir);

// --refineryshare <picture> <service address>: one saved picture of a
// refinery terminal read as the app reads it, and what it says of the
// station's workload sent to the service as the app would - taken when the
// picture was written - with the service's answer printed
int runRefineryShare(const std::string& picture, const std::string& service);

// --commodity <pictures, or a folder> [--wanted <commodity>]: the commodity
// terminal's SHOP INVENTORY read off each picture - every card's name, stock
// level, stock and price, or the one commodity's card alone, as a report to
// UEX reads it (SCMINER_COMMODITY_TRACE: every line of text read)
int runCommodity(const std::vector<std::string>& files, const std::string& wanted);

// --tradecard <folder> <commodity> <price a SCU> <second of the trade>: the
// pictures of a folder taken a second apart, the first at second 0, read as
// a report to UEX reads them for a purchase at that second - which card
// was found, in which picture
int runTradeCard(const std::string& folder, const std::string& commodity, double perScu, double tradeSecond);

// --session <folder>: the pictures of a folder, a second apart, read as a
// visit to a terminal's list - every picture whole - and the reports to UEX
// it would make, before UEX's prices are held against them
int runSession(const std::string& folder);

// --refineryreplay <folder> <station> <first frame's UTC> <seconds a frame>
// [every]: a recording cut into frames played through the order book the way
// the app reads it - every <every>-th frame, a reading a second - and the
// book printed at the end
int runRefineryReplay(const std::string& dir, const std::string& station, long long start, double frameSeconds,
                      int every);
