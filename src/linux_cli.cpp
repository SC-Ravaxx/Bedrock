// bedrock-read - Bedrock's core as a native Linux tool.
//
// What it can do, and each of these is the same code path the Windows build
// takes at that point:
//
//   bedrock-read <image.png> ...     read the signature out of a picture
//   bedrock-read -v <number> ...     say what a number can stand for
//   bedrock-read --windows           list the windows it can see
//   bedrock-read --shot <file.png>   save a picture of the game's window
//   bedrock-read --live              watch the game and report what it reads
//
// SCMINER_SYSTEM, SCMINER_PLACE and SCMINER_GROUND narrow the answer, exactly
// as they do for the Windows build's --analyze.
#include "GlyphReader.h"
#include "DevSwitch.h"
#include "SignatureAnalyzer.h"
#include "AppPaths.h"
#include "DevTools.h"
#include "LinuxCapture.h"
#include "ScreenCapture.h"

#include <opencv2/opencv.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

void applyScope(SCMiner::SignatureAnalyzer& analyzer) {
    if (const char* sys = DEV_ENV("SCMINER_SYSTEM")) analyzer.setSystemFilter(sys);
    if (const char* ground = DEV_ENV("SCMINER_GROUND")) {
        using Ground = SCMiner::SignatureAnalyzer::Ground;
        analyzer.setGround(std::string(ground) == "space"     ? Ground::Space
                           : std::string(ground) == "surface" ? Ground::Surface
                                                              : Ground::Unknown);
    }
    if (const char* place = DEV_ENV("SCMINER_PLACE")) {
        analyzer.setPlaceFilter(place);
        std::cout << "at " << place
                  << (analyzer.placeFilter().empty() ? "  - the tables know no such place" : "")
                  << "\n";
    }
}

void report(SCMiner::SignatureAnalyzer& analyzer, int value) {
    auto results = analyzer.analyzeSignature(value);
    std::vector<SCMiner::AnalysisResult> exact;
    for (const auto& r : results)
        if (r.difference < 1.0) exact.push_back(r);

    if (!analyzer.scopeName().empty() && analyzer.outOfPlace()) {
        std::cout << "   nothing " << analyzer.scopeName()
                  << " is known for explains this - what follows is from elsewhere\n";
    }
    // What the window puts on top: the first row, a near miss included when
    // nothing exact is at the place - the same line Bedrock.exe --analyze
    // prints
    if (!results.empty()) {
        const auto& top = results.front();
        std::cout << "   top row: " << top.name
                  << (top.clusterSize > 1 ? " x" + std::to_string(top.clusterSize) : "")
                  << "  " << static_cast<int>(top.probability + 0.5) << " %"
                  << (top.inSelectedSystem ? "  [here]" : "  [not here]")
                  << (top.difference >= 1.0 ? "  (near miss, " + std::to_string(static_cast<int>(top.difference)) + " off)" : "")
                  << "\n";
    }
    std::cout << value << " -> ";
    if (!exact.empty()) {
        for (size_t i = 0; i < exact.size(); i++) {
            if (i) std::cout << ", ";
            std::cout << exact[i].name;
        }
        std::cout << "\n";
        return;
    }
    const auto constellations = analyzer.decomposeAll(value, 6);
    if (constellations.empty()) {
        std::cout << "NO MATCHES\n";
        return;
    }
    std::cout << constellations.size()
              << (constellations.size() == 1 ? " CONSTELLATION: " : " CONSTELLATIONS: ");
    for (size_t i = 0; i < constellations.size(); i++) {
        if (i) std::cout << ", ";
        for (size_t t = 0; t < constellations[i].terms.size(); t++) {
            if (t) std::cout << " + ";
            std::cout << constellations[i].terms[t].count << "x "
                      << constellations[i].terms[t].name;
        }
    }
    std::cout << "\n";
}

// The program carries its templates (EmbeddedData, on Linux too). A
// glyphs folder, where there is one - beside the program, in the working
// directory, or wherever BEDROCK_GLYPHS says - replaces them, exactly as
// a folder beside the exe does on Windows.
std::string findGlyphs() {
    if (const char* env = std::getenv("BEDROCK_GLYPHS")) return env;
    const std::filesystem::path beside = SCMiner::AppPaths::program() / "glyphs";
    if (std::filesystem::is_directory(beside)) return beside.string();
    if (std::filesystem::is_directory("glyphs")) return "glyphs";
    return {};
}

bool loadReader(SCMiner::GlyphReader& reader) {
    const std::string dir = findGlyphs();
    const int loaded = reader.loadTemplates(dir.empty() ? "./glyphs" : dir);
    if (loaded <= 0) {
        std::cerr << "No glyph templates - none inside the program, and no glyphs folder.\n";
        return false;
    }
    std::cout << loaded << " templates from " << (dir.empty() ? std::string("inside the program") : dir)
              << (reader.hasMfdTemplates() ? "  (with the instrument-panel set)" : "") << "\n";
    return true;
}

int listWindows() {
    SCMiner::LinuxCapture capture;
    if (!capture.ok()) {
        std::cerr << capture.lastError() << "\n";
        return 2;
    }
    const auto all = capture.windows();
    std::cout << all.size() << " window(s):\n";
    for (const auto& w : all) {
        std::cout << "  0x" << std::hex << w.id << std::dec
                  << "  " << w.width << "x" << w.height
                  << (w.viable ? "  " : "  (too small) ")
                  << "process=" << (w.process.empty() ? "?" : w.process)
                  << "  class=" << (w.wmClass.empty() ? "?" : w.wmClass)
                  << "  \"" << w.title << "\"\n";
    }
    const auto game = capture.gameWindow();
    std::cout << "\ngame window: ";
    if (game) {
        std::cout << "0x" << std::hex << game->id << std::dec << "  " << game->width << "x"
                  << game->height << "  \"" << game->title << "\"\n";
    } else {
        std::cout << "none found\n";
    }
    return game ? 0 : 1;
}

// One capture, located and read - the same three steps the live loop takes on
// Windows: take the picture, find the plate in it, read the digits.
int readFrame(SCMiner::LinuxCapture& capture, SCMiner::ScreenCapture& locator,
              SCMiner::GlyphReader& reader, SCMiner::SignatureAnalyzer& analyzer,
              bool quiet) {
    const cv::Mat frame = capture.captureWindow();
    if (frame.empty()) {
        if (!quiet) std::cout << "no picture: " << capture.lastError() << "\n";
        return -1;
    }
    const auto labels = locator.locateSignatureLabels(frame, 6);
    if (labels.empty()) {
        if (!quiet) std::cout << frame.cols << "x" << frame.rows << ": no marker in view\n";
        return -1;
    }
    for (const auto& label : labels) {
        const cv::Rect safe = label & cv::Rect(0, 0, frame.cols, frame.rows);
        if (safe.width <= 0 || safe.height <= 0) continue;
        SCMiner::GlyphReader::Reading reading;
        const auto row = locator.rowOf(label);
        const std::optional<int> value =
            reader.read(frame(safe), &reading, {}, nullptr, {}, row ? &*row : nullptr);
        if (!value || !SCMiner::GlyphReader::goodEnough(reading)) continue;
        std::cout << *value << (reading.mfd ? "  [instrument panel]" : "")
                  << "   pin " << (int)(reading.pinScore * 100) << "%\n";
        report(analyzer, *value);
        return *value;
    }
    if (!quiet) std::cout << labels.size() << " candidate(s), none read cleanly\n";
    return -1;
}

int live() {
    SCMiner::LinuxCapture capture;
    if (!capture.ok()) {
        std::cerr << capture.lastError() << "\n";
        return 2;
    }
    SCMiner::GlyphReader reader;
    if (!loadReader(reader)) return 2;
    SCMiner::SignatureAnalyzer analyzer;
    applyScope(analyzer);
    SCMiner::ScreenCapture locator;

    const auto game = capture.gameWindow();
    if (!game) {
        std::cerr << "No game window found. Run --windows to see what is up.\n";
        return 1;
    }
    std::cout << "watching \"" << game->title << "\" (" << game->width << "x" << game->height
              << ")  -  Ctrl+C to stop\n\n";

    std::signal(SIGINT, onSignal);
    int last = -1;
    while (!g_stop) {
        const int value = readFrame(capture, locator, reader, analyzer, true);
        if (value > 0 && value != last) last = value;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::cout << "\nstopped.\n";
    return 0;
}

int shot(const std::string& path) {
    SCMiner::LinuxCapture capture;
    if (!capture.ok()) {
        std::cerr << capture.lastError() << "\n";
        return 2;
    }
    const cv::Mat frame = capture.captureWindow();
    if (frame.empty()) {
        std::cerr << "nothing captured: " << capture.lastError() << "\n";
        return 1;
    }
    if (!cv::imwrite(path, frame)) {
        std::cerr << "could not write " << path << "\n";
        return 1;
    }
    std::cout << "wrote " << path << "  (" << frame.cols << "x" << frame.rows << ", via "
              << (capture.lastSource() == SCMiner::LinuxCapture::Source::Composite
                      ? "the Composite pixmap"
                      : "the window directly")
              << ")\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "bedrock-read <image.png> ...     read the signature out of a picture\n"
                     "bedrock-read -v <number> ...     say what a number can stand for\n"
                     "bedrock-read --windows           list the windows it can see\n"
                     "bedrock-read --shot <file.png>   save a picture of the game's window\n"
                     "bedrock-read --live              watch the game and report readings\n"
                     "bedrock-read --pipeline <frames> locate and read the marker in saved frames\n"
                     "bedrock-read --panelbench <dir> <out.csv>  read the rock panel in every frame\n";
        return 1;
    }

    const std::string first = argv[1];
    if (first == "--windows") return listWindows();
    // The two benches, as the Windows build runs them (DevTools.h)
    if (first == "--pipeline") return runPipelineOnFiles(std::vector<std::string>(argv + 2, argv + argc));
    if (first == "--panelbench") {
        if (argc < 4) { std::cerr << "--panelbench needs a folder and an output file\n"; return 1; }
        return runPanelBench(argv[2], argv[3], 0, 1);
    }
    if (first == "--live") return live();
    if (first == "--shot") {
        if (argc < 3) { std::cerr << "--shot needs a file name\n"; return 1; }
        return shot(argv[2]);
    }

    SCMiner::SignatureAnalyzer analyzer;
    applyScope(analyzer);

    if (first == "-v") {
        for (int i = 2; i < argc; i++) {
            try {
                report(analyzer, std::stoi(argv[i]));
            } catch (const std::exception&) {
                std::cout << argv[i] << " -> not a number\n";
            }
        }
        return 0;
    }

    SCMiner::GlyphReader reader;
    if (!loadReader(reader)) return 2;
    std::cout << "\n";

    int failures = 0;
    for (int i = 1; i < argc; i++) {
        const cv::Mat image = cv::imread(argv[i], cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cout << argv[i] << ": not readable\n";
            failures++;
            continue;
        }
        SCMiner::GlyphReader::Reading reading;
        const std::optional<int> value = reader.read(image, &reading);
        std::cout << std::filesystem::path(argv[i]).filename().string() << " ("
                  << image.cols << "x" << image.rows << "): ";
        if (!value) {
            std::cout << "nothing read"
                      << (reading.glyphCount
                              ? "  (" + std::to_string(reading.glyphCount) + " digit shapes found)"
                              : "")
                      << "\n";
            failures++;
            continue;
        }
        std::cout << *value << (reading.mfd ? "  [instrument panel]" : "")
                  << "   pin " << (int)(reading.pinScore * 100) << "%"
                  << (SCMiner::GlyphReader::goodEnough(reading) ? "" : "  (below the bar)")
                  << "\n";
        report(analyzer, *value);
        std::cout << "\n";
    }
    return failures ? 1 : 0;
}
