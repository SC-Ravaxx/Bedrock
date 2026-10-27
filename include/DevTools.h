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

// --refineryreplay <folder> <station> <first frame's UTC> <seconds a frame>
// [every]: a recording cut into frames played through the order book the way
// the app reads it - every <every>-th frame, a reading a second - and the
// book printed at the end
int runRefineryReplay(const std::string& dir, const std::string& station, long long start, double frameSeconds,
                      int every);

// --datarunner <pictures, or a folder>: the commodity terminal's list read
// off each picture as a UEX Datarunner report reads it - every card's name,
// stock level, stock and price, each with how sure the reader is of it
// (S sure, C check, M missing), and which list it is (side 1 BUY, 2 LOCAL
// MARKET VALUE). A picture with no cards shows no commodity terminal.
// (SCMINER_COMMODITY_TRACE: every line of text read)
int runDatarunner(const std::vector<std::string>& files);

// --plugin-check <plugin folder> <game folder> [state folder]: a plugin run
// with no window - started (signed or not), fed the logbackups of the game
// folder as older logs and its Game.log as the live one, then its window
// printed as text: every tab, every card open. The state it saves goes to
// the state folder, a fresh one in the temp folder where none is given.
int runPluginCheck(const std::string& folder, const std::string& gameFolder, const std::string& stateFolder);

// --pack-plugin <plugin folder> <package>: the folder packed and signed into
// one file, for the server's list (PluginHost.h). The key is the seed in
// BEDROCK_PLUGIN_SEED (64 hex figures), put there by whoever holds it - by
// `op run`, so it never stands in a file or a command line; without it the
// test key signs, and the package runs in a developer's build alone.
int runPackPlugin(const std::string& folder, const std::string& out);
