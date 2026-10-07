#include <iostream>
#include "DevSwitch.h"
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include <map>
#include <array>
#include <functional>
#include <cstring>
#include <Windows.h>
#include <opencv2/opencv.hpp>
#include "Application.h"
#include "GameState.h"
#include "GamePlaces.h"
#include "ScanPanel.h"
#include "PanelShapes.h"
#include "TextLineReader.h"
#include "LoadoutReader.h"
#include "RockContents.h"
#include "MiningHardware.h"
#include "DevTools.h"
#include "DevShots.h"
#include "SelfTest.h"
#include "OCREngine.h"
#include "MiningLocations.h"
#include "AppPaths.h"
#include "Updater.h"
#include "resource.h"
#include "MiningLocations.h"
#include "RefineryData.h"
#include "RefiningData.h"
#include "CraftingData.h"
#include "BlueprintLibrary.h"
#include "GameTexts.h"
#include "NoteBook.h"
#include "DataBackup.h"

namespace {

// The second reader, for the tools that read a rock panel: the same
// reader the app gives readScanPanel, loaded once and only when a tool
// actually asks for it. nullptr when the exe carries no model.

// Returns the folder containing the executable
std::string getExecutableDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string exePath(path);
    size_t lastSlash = exePath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return exePath.substr(0, lastSlash);
    }
    return ".";
}

// Attaches to the parent console (when started from a terminal) or creates
// one (forceAlloc). Does nothing if stdout is already redirected to a file/pipe.
void setupConsole(bool forceAlloc) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE &&
        GetFileType(out) != FILE_TYPE_UNKNOWN) {
        return;  // stdout is redirected - keep it
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!forceAlloc || !AllocConsole()) {
            return;
        }
    }

    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
}


// Prints the exact matches for given values, in the same shape SCMDB's
// signature identifier reports them - for cross-checking the table.
#ifdef BEDROCK_DEV_TOOLS
int runAnalyze(const std::vector<std::string>& values) {
    SCMiner::SignatureAnalyzer analyzer;
    // What the app knows while it runs, so the same question can be asked
    // here: SCMINER_SYSTEM=PYRO and SCMINER_PLACE="Pyro II (Monox)" narrow
    // the candidates the way standing at Monox does.
    if (const char* sys = DEV_ENV("SCMINER_SYSTEM")) analyzer.setSystemFilter(sys);
    // ...and SCMINER_GROUND=space|surface says what is under the ship
    if (const char* ground = DEV_ENV("SCMINER_GROUND")) {
        using Ground = SCMiner::SignatureAnalyzer::Ground;
        analyzer.setGround(std::string(ground) == "space" ? Ground::Space
                           : std::string(ground) == "surface" ? Ground::Surface : Ground::Unknown);
    }
    if (const char* place = DEV_ENV("SCMINER_PLACE")) {
        analyzer.setPlaceFilter(place);
        std::cout << "at " << place
                  << (analyzer.placeFilter().empty() ? " - the tables know no such place" : "")
                  << std::endl;
    }

    for (const auto& text : values) {
        int value = 0;
        try {
            value = std::stoi(text);
        } catch (const std::exception&) {
            std::cout << text << " -> not a number" << std::endl;
            continue;
        }

        auto results = analyzer.analyzeSignature(value);
        std::vector<SCMiner::AnalysisResult> exact;
        for (const auto& r : results) {
            if (r.difference < 1.0) exact.push_back(r);
        }
        // What the window puts on top: the first row, a near miss included
        // when nothing exact is at the place (see the sort in analyzeSignature)
        if (!results.empty()) {
            const auto& top = results.front();
            std::cout << "   top row: " << top.name
                      << (top.clusterSize > 1 ? " x" + std::to_string(top.clusterSize) : "")
                      << "  " << static_cast<int>(top.probability + 0.5) << " %"
                      << (top.inSelectedSystem ? "  [here]" : "  [not here]")
                      << (top.difference >= 1.0 ? "  (near miss, " + std::to_string(static_cast<int>(top.difference)) + " off)" : "")
                      << std::endl;
        }
        if (!analyzer.scopeName().empty()) {
            if (analyzer.outOfPlace()) {
                std::cout << "   nothing " << analyzer.scopeName()
                          << " is known for explains this - what follows is from elsewhere" << std::endl;
            }
            for (const auto& r : exact) {
                std::cout << "   " << (r.inSelectedSystem ? "[here] " : "[not here] ") << r.name << std::endl;
            }
        }

        std::cout << value << " -> ";
        if (exact.empty()) {
            const auto constellations = analyzer.decomposeAll(value, 6);
            if (constellations.empty()) {
                std::cout << "NO MATCHES";
            } else {
                std::cout << constellations.size()
                          << (constellations.size() == 1 ? " CONSTELLATION: " : " CONSTELLATIONS: ");
                for (size_t i = 0; i < constellations.size(); i++) {
                    if (i) std::cout << ", ";
                    for (size_t t = 0; t < constellations[i].terms.size(); t++) {
                        if (t) std::cout << " + ";
                        std::cout << constellations[i].terms[t].count << "x " << constellations[i].terms[t].name;
                    }
                }
            }
        } else {
            std::cout << exact.size() << (exact.size() == 1 ? " MATCH: " : " MATCHES: ");
            for (size_t i = 0; i < exact.size(); i++) {
                if (i) std::cout << ", ";
                // A combination already carries its own counts in the name
                if (exact[i].clusterSize > 0 && !exact[i].combination) std::cout << exact[i].clusterSize << "x ";
                std::cout << exact[i].name;
            }
        }
        std::cout << std::endl;
    }

    return 0;
}

// What the SCAN RESULTS search makes of a picture, and a copy of it with
// every rule the search considered drawn on. The panel slides with the rock,
// so there is no crop to check it against - only whether the structure it
// looks for is there and whether anything else in the frame fakes it.
//
//   Bedrock.exe --scanpanel shot.png [more.png ...]
//
// Writes <name>_panel.png beside each input.
// Grabs the game's own viewport and looks for the panel in it, so the search
// can be measured against the HUD the player is actually looking at rather
// than against a test frame from another version with another colour. Writes
// the capture and what the search made of it beside the exe.
int runScanPanelLive(const std::string& outDir) {
    // A countdown, because whoever starts this is looking at a console and
    // the thing to be measured is behind it. Desktop Duplication hands back
    // what is actually on the screen, so the game has to be the thing on it.
    for (int left = 6; left > 0; left--) {
        std::cout << "Switch to Star Citizen - capturing in " << left << " ... " << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    std::cout << "                                          " << std::flush;

    SCMiner::ScreenCapture capture;
    cv::Rect area(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                  GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (auto viewport = SCMiner::ScreenCapture::gameViewport()) {
        area = *viewport;
        std::cout << "Game viewport : " << area.width << "x" << area.height
                  << " at " << area.x << "," << area.y << std::endl;
    } else {
        std::cout << "Game viewport : not found - Star Citizen does not seem to be up."
                  << std::endl;
        std::cout << "                Measuring the whole desktop instead." << std::endl;
    }

    SCMiner::CaptureRegion region;
    region.x = area.x;
    region.y = area.y;
    region.width = area.width;
    region.height = area.height;
    cv::Mat frame = capture.captureRegion(region);
    if (frame.empty()) {
        std::cout << "Capture       : FAILED - nothing came back" << std::endl;
        return 1;
    }
    std::cout << "Capture       : " << frame.cols << "x" << frame.rows << std::endl;

    SCMiner::GlyphReader glyphs;
    glyphs.loadTemplates("./glyphs");
    glyphs.loadMfdTemplates("./glyphs");
    SCMiner::OCREngine ocr;
    SCMiner::OCRConfig cfg;
    const SCMiner::ScanPanel panel = ocr.initialize(cfg)
        ? SCMiner::readScanPanel(frame, ocr, &glyphs, panelLines())
        : SCMiner::findScanPanel(frame);
    std::cout << "Glyph fonts   : " << glyphs.templateCount() << " canopy, "
              << glyphs.templateCount(true) << " panel" << std::endl;
    std::cout << "Search        : " << panel.note << std::endl;
    if (panel.found) {
        std::cout << "Panel         : " << panel.box.x << "," << panel.box.y << "  "
                  << panel.box.width << "x" << panel.box.height
                  << "   of the viewport: x " << (double)panel.box.x / frame.cols
                  << "  y " << (double)panel.box.y / frame.rows
                  << "  w " << (double)panel.box.width / frame.cols
                  << "  h " << (double)panel.box.height / frame.rows << std::endl;
    } else {
        std::cout << "Panel         : NOT FOUND" << std::endl;
    }

    const std::string raw = outDir + "/scanpanel_live.png";
    const std::string shown = outDir + "/scanpanel_live_search.png";
    cv::imwrite(raw, frame);
    cv::imwrite(shown, SCMiner::drawScanPanelSearch(frame, panel));
    std::cout << "Written       : " << raw << std::endl;
    std::cout << "                " << shown << std::endl;
    return panel.found ? 0 : 1;
}

// Harvests panel digits into a template set.
//
//   Bedrock.exe --panelglyphs <dir> <shot.png> <mass> <res> <inst> [more...]
//
// The figures are given because the picture cannot say what it shows: the
// whole point is that the reader gets them wrong. What it writes is one PNG
// per glyph named <digit>_<tag>.png, and those become the shapes the panel
// reader matches against - the HUD's own font, including its slashed zero,
// which is what general OCR keeps reading as an 8.
int runPanelGlyphs(const std::vector<std::string>& args) {
    if (args.size() < 5) {
        std::cout << "usage: --panelglyphs <dir> <shot.png> <mass> <res> <inst> [shot mass res inst ...]"
                  << std::endl;
        return 1;
    }
    const std::string dir = args[0];
    std::filesystem::create_directories(dir);

    int written = 0, rowsDone = 0;
    for (size_t at = 1; at + 3 < args.size(); at += 4) {
        const std::string file = args[at];
        const std::string truth[3] = {args[at + 1], args[at + 2], args[at + 3]};
        cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) {
            std::cout << file << ": cannot read" << std::endl;
            continue;
        }
        // readScanPanel, not findScanPanel. The first is the pair of rules
        // that a rock's figures can actually be read out of; the second is
        // merely the best-matched pair, which can sit around the composition
        // list and fill the harvest with the letters of "(ORE)" and a line of
        // percentages.
        SCMiner::GlyphReader glyphs;
        glyphs.loadTemplates("./glyphs");
        SCMiner::OCREngine ocr;
        SCMiner::OCRConfig cfg;
        const bool ready = ocr.initialize(cfg);
        const SCMiner::ScanPanel panel = ready ? SCMiner::readScanPanel(frame, ocr, &glyphs, panelLines())
                                               : SCMiner::findScanPanel(frame);
        if (!panel.found || panel.figuresRead < 3) {
            std::cout << file << ": no panel read (" << panel.note << ")" << std::endl;
            continue;
        }

        // the rows the reader itself finds, in the order the panel writes
        // them: the ore's name, then mass, resistance, instability
        const std::vector<cv::Rect> rows = SCMiner::panelValueRows(frame, panel);
        if (rows.size() < 4) {
            std::cout << file << ": " << rows.size() << " rows between the rules, need 4"
                      << std::endl;
            continue;
        }
        const std::string tag = std::filesystem::path(file).stem().string();
        for (int which = 0; which < 3; which++) {
            const std::vector<SCMiner::PanelGlyph> cut =
                SCMiner::cutPanelGlyphs(frame, rows[which + 1]);

            std::string digits;
            for (char c : truth[which]) {
                if (c >= '0' && c <= '9') digits += c;
            }
            std::cout << tag << " row " << which << ": " << cut.size() << " glyphs, "
                      << digits.size() << " digits given";
            if (cut.size() != digits.size()) {
                // Written anyway, unlabelled, because a count that does not
                // match is exactly what has to be looked at: too many means
                // the glyphs are breaking apart, too few means they are
                // running together, and the pictures say which.
                std::cout << "  - unlabelled" << std::endl;
                for (size_t i = 0; i < cut.size(); i++) {
                    cv::Mat out;
                    cut[i].shape.convertTo(out, CV_8U, 255.0);
                    cv::imwrite(dir + "/x_" + tag + "_r" + std::to_string(which) + "_" +
                                    std::to_string(i) + ".png", out);
                }
                continue;
            }
            std::cout << std::endl;
            rowsDone++;
            for (size_t i = 0; i < cut.size(); i++) {
                cv::Mat out;
                cut[i].shape.convertTo(out, CV_8U, 255.0);
                const std::string name = dir + "/" + digits[i] + "_" + tag + "_r" +
                                         std::to_string(which) + "_" + std::to_string(i) + ".png";
                if (cv::imwrite(name, out)) written++;
            }
        }
    }
    std::cout << "\n" << written << " glyphs from " << rowsDone << " rows into " << dir << std::endl;
    return written > 0 ? 0 : 1;
}

// Every picture in a folder through the live panel reader, one CSV line each.
//
// --textmap <in> <out> <x> <y> <w> <h>: a strip of four, for tuning the panel
// reader - the crop, the old brightness structure, the stroke map, and the
// stroke map cut at a level.
int runTextMap(const std::string& in, const std::string& out, int x, int y, int w, int h) {
    const cv::Mat frame = cv::imread(in, cv::IMREAD_COLOR);
    if (frame.empty()) return 1;
    const cv::Rect roi = cv::Rect(x, y, w, h) & cv::Rect(0, 0, frame.cols, frame.rows);
    const cv::Mat crop = frame(roi).clone();

    std::vector<cv::Mat> planes;
    cv::split(crop, planes);
    const cv::Mat high = cv::max(planes[0], cv::max(planes[1], planes[2]));
    cv::Mat tophat, old;
    cv::morphologyEx(high, tophat, cv::MORPH_TOPHAT,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(1, 20)));
    cv::threshold(tophat, old, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);

    const cv::Mat stroke = SCMiner::hudStrokeMap(crop, 4);
    cv::Mat boosted = stroke * 3, cut;
    cv::threshold(stroke, cut, 40, 255, cv::THRESH_BINARY);

    std::vector<cv::Mat> tiles = {crop};
    for (const cv::Mat& g : {old, boosted, cut}) {
        cv::Mat c;
        cv::cvtColor(g, c, cv::COLOR_GRAY2BGR);
        tiles.push_back(c);
    }
    cv::Mat strip;
    cv::hconcat(tiles, strip);
    return cv::imwrite(out, strip) ? 0 : 1;
}

// --panelharvest <frames> <labels.csv> <out>: glyph samples of the panel's
// own face out of frames whose figures are known, and a class atlas of them.
// Only rows whose glyph count matches the known figure exactly are used, so a
// sample is never labelled by guesswork.
int runPanelHarvest(const std::string& framesDir, const std::string& labelsCsv,
                    const std::string& outDir) {
    std::ifstream in(labelsCsv);
    if (!in.is_open()) return 1;
    std::string line;
    std::getline(in, line);   // header
    struct Label { std::string file, mass, res, inst; int head, body; bool ok; };
    std::vector<Label> labels;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() < 10) continue;
        labels.push_back({f[0], f[2], f[3], f[5], std::atoi(f[8].c_str()), std::atoi(f[9].c_str()),
                          f[7] == "1"});
    }

    std::map<char, std::vector<cv::Mat>> samples;
    int rowsTried = 0, rowsMatched = 0, frames = 0;
    for (const Label& l : labels) {
        if (!l.ok) continue;
        const cv::Mat frame = cv::imread(framesDir + "/" + l.file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        SCMiner::ScanPanel panel;
        for (const SCMiner::ScanPanel& c : SCMiner::scanPanelCandidates(frame, 20)) {
            if (c.found && std::abs(c.headRule - l.head) <= 2 && std::abs(c.bodyRule - l.body) <= 2) {
                panel = c;
                break;
            }
        }
        if (!panel.found) continue;
        frames++;
        const std::vector<cv::Rect> rows = SCMiner::panelValueRows(frame, panel);
        if (rows.size() < 4) continue;
        const SCMiner::PanelInk ink = SCMiner::panelInk(frame, panel.box);
        const std::string expected[3] = {l.mass, l.res + "%", l.inst};
        for (int k = 0; k < 3; k++) {
            rowsTried++;
            const std::vector<SCMiner::FaceGlyph> glyphs = SCMiner::faceGlyphs(ink, rows[k + 1]);
            const std::string& want = expected[k];
            if (glyphs.size() != want.size()) continue;
            bool fits = true;
            for (size_t i = 0; i < want.size(); i++) {
                if ((want[i] == '.') != glyphs[i].stop) fits = false;
            }
            if (!fits) continue;
            rowsMatched++;
            for (size_t i = 0; i < want.size(); i++) {
                if (!glyphs[i].stop) samples[want[i]].push_back(glyphs[i].shape.clone());
            }
        }
    }
    std::cout << frames << " frames, " << rowsMatched << " of " << rowsTried
              << " rows matched their figure" << std::endl;

    // A mean per class, and how the samples fare against the means
    std::map<char, cv::Mat> means;
    for (auto& entry : samples) {
        cv::Mat sum = cv::Mat::zeros(SCMiner::FACE_H, SCMiner::FACE_W, CV_32F);
        for (const cv::Mat& m : entry.second) sum += m;
        sum /= cv::norm(sum);
        means[entry.first] = sum;
    }
    int right = 0, total = 0;
    std::map<std::string, int> confusions;
    for (auto& entry : samples) {
        for (const cv::Mat& m : entry.second) {
            char best = '?';
            double bestScore = -2;
            for (auto& mean : means) {
                const double score = m.dot(mean.second);
                if (score > bestScore) { bestScore = score; best = mean.first; }
            }
            total++;
            if (best == entry.first) right++;
            else confusions[std::string(1, entry.first) + "->" + best]++;
        }
    }
    for (auto& entry : samples) std::cout << "'" << entry.first << "' " << entry.second.size() << "  ";
    std::cout << std::endl << "against the class means: " << right << " of " << total << std::endl;
    for (auto& c : confusions) std::cout << "  " << c.first << " x" << c.second << std::endl;

    // Atlas: each class mean eight times its size
    std::vector<cv::Mat> tiles;
    for (auto& mean : means) {
        cv::Mat shown;
        cv::normalize(mean.second, shown, 0, 255, cv::NORM_MINMAX);
        shown.convertTo(shown, CV_8U);
        cv::resize(shown, shown, cv::Size(), 8, 8, cv::INTER_NEAREST);
        cv::copyMakeBorder(shown, shown, 4, 4, 4, 4, cv::BORDER_CONSTANT, cv::Scalar(80));
        tiles.push_back(shown);
    }
    if (!tiles.empty()) {
        cv::Mat atlas;
        cv::hconcat(tiles, atlas);
        cv::imwrite(outDir + "/face_atlas.png", atlas);
    }

    // The means as a header, in the order of the classes
    std::ofstream h(outDir + "/PanelFace.h", std::ios::trunc);
    h << "// Generated by --panelharvest from a recording whose figures were read by eye.\n";
    h << "#pragma once\n\nnamespace SCMiner {\n\n";
    h << "struct PanelFaceTemplate {\n    char glyph;\n    float shape[" << SCMiner::FACE_H * SCMiner::FACE_W << "];\n};\n\n";
    h << "inline const PanelFaceTemplate PANEL_FACE[] = {\n";
    for (auto& mean : means) {
        h << "    {'" << mean.first << "', {";
        for (int i = 0; i < SCMiner::FACE_H * SCMiner::FACE_W; i++) {
            char num[24];
            std::snprintf(num, sizeof(num), "%.4ff", mean.second.at<float>(i / SCMiner::FACE_W, i % SCMiner::FACE_W));
            h << (i ? "," : "") << num;
        }
        h << "}},\n";
    }
    h << "};\n\ninline constexpr int PANEL_FACE_COUNT = " << means.size() << ";\n\n}  // namespace SCMiner\n";
    std::cout << "wrote " << outDir << "/PanelFace.h and face_atlas.png" << std::endl;
    return 0;
}

// --facedebug <in> <out.png>: every candidate panel of one frame, with the
// rows the face reader found in it, the glyphs it cut and what it read, drawn
// over the brightness structure three times its size.
int runFaceDebug(const std::string& in, const std::string& out) {
    const cv::Mat frame = cv::imread(in, cv::IMREAD_COLOR);
    if (frame.empty()) return 1;
    std::vector<cv::Mat> strips;
    int index = 0;
    for (const cv::Rect& region : SCMiner::figureRegions(frame)) {
        SCMiner::ScanPanel probe;
        const bool read = SCMiner::readFiguresIn(frame, region, probe);
        {
            const SCMiner::PanelInk ink = SCMiner::panelInk(frame, region);
            cv::imwrite(out + "." + std::to_string(index) + ".ink.png", ink.ink);
        }
        cv::Mat shown = frame(region).clone();
        for (const auto& line : SCMiner::faceLinesIn(frame, region)) {
            cv::Rect r(line.first.x - region.x, line.first.y - region.y, line.first.width, line.first.height);
            cv::rectangle(shown, r, cv::Scalar(255, 128, 0), 1);
            std::cout << "   #" << index << " line y " << line.first.y << " h " << line.first.height << ": " << line.second << std::endl;
            cv::putText(shown, line.second, cv::Point(2, r.y + r.height - 2), cv::FONT_HERSHEY_PLAIN, 0.9,
                        cv::Scalar(0, 0, 255), 1);
        }
        cv::putText(shown, "#" + std::to_string(index) + (read ? " READ" : " -"), cv::Point(2, 14),
                    cv::FONT_HERSHEY_PLAIN, 1.1, read ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 128, 255), 1);
        std::cout << "#" << index << " " << region << " " << (read ? probe.note : "-") << std::endl;
        strips.push_back(shown);
        if (++index >= 16) break;
    }
    if (strips.empty()) return 1;
    int width = 0, height = 0;
    for (const cv::Mat& m : strips) {
        width += m.cols;
        height = (std::max)(height, m.rows);
    }
    cv::Mat sheet(height, width, CV_8UC3, cv::Scalar(40, 40, 40));
    int x = 0;
    for (const cv::Mat& m : strips) {
        m.copyTo(sheet(cv::Rect(x, 0, m.cols, m.rows)));
        x += m.cols;
    }
    return cv::imwrite(out, sheet) ? 0 : 1;
}

// --faceharvest <frames> <labels.csv> <out>: the face reader's own glyphs,
// labelled from known figures. A word is matched to a figure by how many
// glyphs it has and where its full stop is - never by what the reader made of
// it, so frames the reader gets wrong teach it too. Shapes are clustered into
// a few per glyph and written as the table the reader uses.
int runFaceHarvest2(const std::string& framesDir, const std::string& labelsCsv, const std::string& outDir) {
    std::ifstream in(labelsCsv);
    if (!in.is_open()) return 1;
    std::string line;
    std::getline(in, line);
    struct Label { std::string file, mass, res, inst; };
    std::vector<Label> labels;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() >= 6) labels.push_back({f[0], f[2], f[3] + "%", f[5]});
    }

    // does a word fit a figure: same glyph count, stops where the figure has them
    auto fits = [](const SCMiner::FaceTokenInfo& t, const std::string& want) {
        if (t.text.size() != want.size()) return false;
        for (size_t i = 0; i < want.size(); i++) {
            if ((want[i] == '.') != (t.text[i] == '.')) return false;
        }
        return true;
    };

    std::map<char, std::vector<cv::Mat>> samples;
    std::map<char, std::vector<std::string>> sampleFrame;
    int frames = 0, matched = 0;
    for (const Label& l : labels) {
        const cv::Mat frame = cv::imread(framesDir + "/" + l.file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        frames++;
        bool done = false;
        for (const cv::Rect& region : SCMiner::figureRegions(frame)) {
            const std::vector<SCMiner::FaceLineInfo> lines = SCMiner::faceLinesDetailed(frame, region);
            // (line, token) of each figure
            auto find = [&](size_t from, size_t to, const std::string& want, size_t& li, size_t& ti) {
                for (size_t a = from; a < lines.size() && a <= to; a++) {
                    const auto& toks = lines[a].tokens;
                    for (size_t b = 1; b < toks.size(); b++) {
                        if (toks[b - 1].text.size() >= 3 && fits(toks[b], want)) {
                            li = a;
                            ti = b;
                            return true;
                        }
                    }
                }
                return false;
            };
            for (size_t i = 0; i < lines.size() && !done; i++) {
                size_t mi = 0, mt = 0, ri = 0, rt = 0, ii = 0, it = 0;
                if (!find(i, i, l.mass, mi, mt)) continue;
                if (!find(mi + 1, mi + 3, l.res, ri, rt)) continue;
                if (!find(ri + 1, ri + 3, l.inst, ii, it)) continue;
                const auto& m = lines[mi].tokens[mt];
                const auto& r = lines[ri].tokens[rt];
                const auto& n = lines[ii].tokens[it];
                const int glyph = (std::max)(4, m.box.height * 3 / 4);
                const int spread = (std::max)({m.box.x, r.box.x, n.box.x}) - (std::min)({m.box.x, r.box.x, n.box.x});
                if (spread > glyph * 2) continue;
                for (const auto* pair : {&m, &r, &n}) {
                    const std::string& want = pair == &m ? l.mass : pair == &r ? l.res : l.inst;
                    for (size_t g = 0; g < want.size(); g++) {
                        if (want[g] == '.' || pair->shapes[g].empty()) continue;
                        samples[want[g]].push_back(pair->shapes[g].clone());
                        sampleFrame[want[g]].push_back(l.file);
                    }
                }
                matched++;
                done = true;
            }
            if (done) break;
        }
    }
    std::cout << frames << " frames, figures matched in " << matched << std::endl;

    // Hold out every fifth frame to see how the table does on shapes it was
    // not made from
    auto heldOut = [](const std::string& file) {
        return (std::atoi(file.substr(3, 4).c_str()) % 5) == 0;
    };
    std::map<char, std::vector<cv::Mat>> centres;
    for (auto& entry : samples) {
        std::vector<cv::Mat> train;
        for (size_t i = 0; i < entry.second.size(); i++) {
            if (!heldOut(sampleFrame[entry.first][i])) train.push_back(entry.second[i]);
        }
        if (train.empty()) continue;
        const int k = (std::max)(1, (std::min)(8, (int)train.size() / 12));
        cv::Mat data((int)train.size(), SCMiner::FACE_W * SCMiner::FACE_H, CV_32F);
        for (size_t i = 0; i < train.size(); i++) train[i].reshape(1, 1).copyTo(data.row((int)i));
        cv::Mat labelsK, c;
        cv::kmeans(data, k, labelsK, cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 50, 1e-4),
                   5, cv::KMEANS_PP_CENTERS, c);
        for (int j = 0; j < c.rows; j++) {
            cv::Mat centre = c.row(j).reshape(1, SCMiner::FACE_H).clone();
            centre -= cv::mean(centre)[0];
            const double len = cv::norm(centre);
            if (len > 1e-6) centres[entry.first].push_back(centre / len);
        }
    }

    // How the held-out shapes fare, at a few confidence levels
    struct Result { char truth; double score, margin; char read; };
    std::vector<Result> results;
    for (auto& entry : samples) {
        for (size_t i = 0; i < entry.second.size(); i++) {
            if (!heldOut(sampleFrame[entry.first][i])) continue;
            std::map<char, double> bestOf;
            for (auto& cls : centres) {
                for (const cv::Mat& c : cls.second) {
                    const double sc = entry.second[i].dot(c);
                    auto f = bestOf.find(cls.first);
                    if (f == bestOf.end() || sc > f->second) bestOf[cls.first] = sc;
                }
            }
            char best = '?';
            double b1 = -2, b2 = -2;
            for (auto& e : bestOf) {
                if (e.second > b1) { b2 = b1; b1 = e.second; best = e.first; }
                else if (e.second > b2) b2 = e.second;
            }
            results.push_back({entry.first, b1, b1 - b2, best});
        }
    }
    for (double minScore : {0.60, 0.65, 0.70, 0.75}) {
        for (double minMargin : {0.00, 0.02, 0.04, 0.06, 0.10}) {
            int accepted = 0, right = 0;
            for (const Result& r : results) {
                if (r.score >= minScore && r.margin >= minMargin) {
                    accepted++;
                    if (r.read == r.truth) right++;
                }
            }
            std::cout << "score>=" << minScore << " margin>=" << minMargin << ": accepted " << accepted
                      << " of " << results.size() << ", wrong " << (accepted - right) << std::endl;
        }
    }
    for (auto& entry : samples) std::cout << "'" << entry.first << "' " << entry.second.size() << "  ";
    std::cout << std::endl;

    std::ofstream h(outDir + "/PanelFace.h", std::ios::trunc);
    h << "// Generated by --faceharvest from a recording whose figures were read by eye:\n";
    h << "// several shapes per glyph, clustered from the face reader's own cuts.\n";
    h << "#pragma once\n\nnamespace SCMiner {\n\n";
    h << "struct PanelFaceTemplate {\n    char glyph;\n    float shape[" << SCMiner::FACE_H * SCMiner::FACE_W << "];\n};\n\n";
    h << "inline const PanelFaceTemplate PANEL_FACE[] = {\n";
    int count = 0;
    for (auto& cls : centres) {
        for (const cv::Mat& c : cls.second) {
            h << "    {'" << cls.first << "', {";
            for (int i = 0; i < SCMiner::FACE_H * SCMiner::FACE_W; i++) {
                char num[24];
                std::snprintf(num, sizeof(num), "%.4ff", c.at<float>(i / SCMiner::FACE_W, i % SCMiner::FACE_W));
                h << (i ? "," : "") << num;
            }
            h << "}},\n";
            count++;
        }
    }
    h << "};\n\ninline constexpr int PANEL_FACE_COUNT = " << count << ";\n\n}  // namespace SCMiner\n";
    std::cout << "wrote " << count << " shapes to " << outDir << "/PanelFace.h" << std::endl;
    return 0;
}

// --facewhy <frames> <labels.csv> <out.csv> [shard shards]: for every labelled
// frame, whether the figures turn up anywhere in what the face reader cut -
// as a word of their own, inside a longer word, or as a word of the right
// shape it was not sure of - so that misses can be told apart.
int runFaceWhy(const std::string& framesDir, const std::string& labelsCsv, const std::string& outCsv,
               int shard, int shards) {
    std::ifstream in(labelsCsv);
    if (!in.is_open()) return 1;
    std::string line;
    std::getline(in, line);
    std::ofstream out(outCsv, std::ios::trunc);
    out << "file,regions,accepted,m_exact,r_exact,i_exact,m_sub,r_sub,i_sub,m_fit,r_fit,i_fit" << std::endl;
    int row = 0;
    while (std::getline(in, line)) {
        if (shards > 1 && (row++ % shards) != shard) continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() < 6) continue;
        const std::string want[3] = {f[2], f[3] + "%", f[5]};
        const cv::Mat frame = cv::imread(framesDir + "/" + f[0], cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        bool exact[3] = {false, false, false}, sub[3] = {false, false, false}, fit[3] = {false, false, false};
        bool accepted = false;
        const std::vector<cv::Rect> regions = SCMiner::figureRegions(frame);
        for (const cv::Rect& region : regions) {
            SCMiner::ScanPanel probe;
            if (SCMiner::readFiguresIn(frame, region, probe)) accepted = true;
            for (const auto& l : SCMiner::faceLinesDetailed(frame, region)) {
                std::string joined;
                for (const auto& t : l.tokens) {
                    joined += t.text;
                    for (int k = 0; k < 3; k++) {
                        if (t.text == want[k]) exact[k] = true;
                        if (t.text.size() == want[k].size()) {
                            bool same = true;
                            for (size_t c = 0; c < want[k].size(); c++) {
                                const bool a = want[k][c] == '.', b = t.text[c] == '.';
                                const bool ok = a == b && (t.text[c] == '?' || t.text[c] == want[k][c]);
                                if (!ok) same = false;
                            }
                            if (same) fit[k] = true;
                        }
                    }
                }
                for (int k = 0; k < 3; k++) {
                    if (joined.find(want[k]) != std::string::npos) sub[k] = true;
                }
            }
        }
        out << f[0] << "," << regions.size() << "," << accepted;
        for (bool b : exact) out << "," << b;
        for (bool b : sub) out << "," << b;
        for (bool b : fit) out << "," << b;
        out << std::endl;
    }
    return 0;
}

// --shapeharvest <frames> <labels.csv> <out>: the panel as it looks, averaged
// over every frame the face reader got right - the labels, the numerals in
// grey rather than ink, and where everything sits relative to the labels.
int runShapeHarvest(const std::string& framesDir, const std::string& labelsCsv, const std::string& outDir) {
    std::ifstream in(labelsCsv);
    if (!in.is_open()) return 1;
    std::string line;
    std::getline(in, line);
    struct Label { std::string file, want[3]; };
    std::vector<Label> labels;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() >= 6) labels.push_back({f[0], {f[2], f[3] + "%", f[5]}});
    }
    struct Row {
        cv::Rect label, value;
        std::vector<cv::Rect> glyphs;
        std::string text;
    };
    struct Sample {
        std::string file;
        Row rows[3];
    };
    std::vector<Sample> samples;
    for (const Label& l : labels) {
        const cv::Mat frame = cv::imread(framesDir + "/" + l.file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        for (const cv::Rect& region : SCMiner::figureRegions(frame)) {
            SCMiner::ScanPanel probe;
            if (!SCMiner::readFiguresIn(frame, region, probe)) continue;
            if (std::fabs(probe.stats.mass - std::stof(l.want[0])) > 0.5f) continue;
            if (std::fabs(probe.stats.resistance - std::stof(l.want[1])) > 0.5f) continue;
            if (std::fabs(probe.stats.instability - std::stof(l.want[2])) > 0.006f) continue;
            Sample sample;
            sample.file = l.file;
            bool ok[3] = {false, false, false};
            for (const auto& fl : SCMiner::faceLinesDetailed(frame, region)) {
                for (size_t k = 1; k < fl.tokens.size(); k++) {
                    for (int r = 0; r < 3; r++) {
                        if (ok[r] || fl.tokens[k].text != l.want[r]) continue;
                        for (size_t j = k; j-- > 0;) {
                            if (fl.tokens[j].text.size() < 3) continue;
                            sample.rows[r] = {fl.tokens[j].box, fl.tokens[k].box, fl.tokens[k].boxes, fl.tokens[k].text};
                            ok[r] = true;
                            break;
                        }
                    }
                }
            }
            if (ok[0] && ok[1] && ok[2] && sample.rows[0].value.y < sample.rows[1].value.y &&
                sample.rows[1].value.y < sample.rows[2].value.y) {
                samples.push_back(sample);
            }
            break;
        }
    }
    std::cout << samples.size() << " frames read right" << std::endl;
    if (samples.empty()) return 1;

    auto median = [](std::vector<int> v) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::vector<int> digitH, advance, labelH[3], labelW[3], valueDx[3], baseDy[3], labelTopDy[3];
    for (const Sample& sm : samples) {
        for (int r = 0; r < 3; r++) {
            const Row& row = sm.rows[r];
            for (size_t g = 0; g < row.text.size(); g++) {
                const bool digit = row.text[g] >= '0' && row.text[g] <= '9';
                if (digit) digitH.push_back(row.glyphs[g].height);
                if (g > 0 && digit && row.text[g - 1] >= '0' && row.text[g - 1] <= '9') {
                    advance.push_back((row.glyphs[g].x + row.glyphs[g].width / 2) -
                                      (row.glyphs[g - 1].x + row.glyphs[g - 1].width / 2));
                }
            }
            labelH[r].push_back(row.label.height);
            labelW[r].push_back(row.label.width);
            valueDx[r].push_back(row.value.x - sm.rows[0].label.x);
            baseDy[r].push_back((row.value.y + row.value.height) - (sm.rows[0].label.y + sm.rows[0].label.height));
            labelTopDy[r].push_back(row.label.y - sm.rows[0].label.y);
        }
    }
    std::vector<int> digitW, stopGap, stopW, stopH, stopBase, pctDx, pctW, pctH, firstDx, pctBase;
    for (const Sample& sm : samples) {
        for (int r = 0; r < 3; r++) {
            const Row& row = sm.rows[r];
            const int base = row.value.y + row.value.height;
            for (size_t g = 0; g < row.text.size(); g++) {
                const char c = row.text[g];
                const cv::Rect& b = row.glyphs[g];
                auto centre = [&](size_t k) { return row.glyphs[k].x + row.glyphs[k].width / 2; };
                if (c >= '0' && c <= '9') digitW.push_back(b.width);
                if (g == 0) firstDx.push_back(centre(0) - row.value.x);
                if (c == '.' && g > 0 && g + 1 < row.text.size()) {
                    stopGap.push_back(centre(g + 1) - centre(g - 1));
                    stopW.push_back(b.width);
                    stopH.push_back(b.height);
                    stopBase.push_back(b.y + b.height - base);
                }
                if (c == '%' && g > 0) {
                    pctDx.push_back(centre(g) - centre(g - 1));
                    pctW.push_back(b.width);
                    pctH.push_back(b.height);
                    pctBase.push_back(b.y + b.height - base);
                }
            }
        }
    }
    const int Hd = median(digitH);
    std::ofstream geo(outDir + "/geometry.txt", std::ios::trunc);
    auto put = [&](const std::string& name, int v) {
        std::cout << name << " " << v << std::endl;
        geo << name << " " << v << std::endl;
    };
    put("samples", (int)samples.size());
    put("digit_height", Hd);
    put("digit_advance", median(advance));
    put("digit_w", median(digitW));
    put("first_centre_dx", median(firstDx));
    put("stop_gap", median(stopGap));
    put("stop_w", median(stopW));
    put("stop_h", median(stopH));
    put("stop_base", median(stopBase));
    put("pct_dx", median(pctDx));
    put("pct_w", median(pctW));
    put("pct_h", median(pctH));
    put("pct_base", median(pctBase));
    for (int r = 0; r < 3; r++) {
        const std::string n = std::to_string(r);
        put("label_w_" + n, median(labelW[r]));
        put("label_h_" + n, median(labelH[r]));
        put("label_top_dy_" + n, median(labelTopDy[r]));
        put("value_dx_" + n, median(valueDx[r]));
        put("value_base_dy_" + n, median(baseDy[r]));
    }

    // Glyphs: grey, from the darkest channel, centred on the glyph and sat on
    // its row's line
    auto darkest = [](const cv::Mat& bgr) {
        std::vector<cv::Mat> planes;
        cv::split(bgr, planes);
        return cv::Mat(cv::min(cv::min(planes[0], planes[1]), planes[2]));
    };
    std::map<char, cv::Mat> sums;
    std::map<char, int> counts;
    std::map<int, cv::Mat> labelSums;
    std::map<int, int> labelCounts;
    cv::Mat panelSum;
    int panelCount = 0;
    const int lw[3] = {median(labelW[0]), median(labelW[1]), median(labelW[2])};
    const int lh[3] = {median(labelH[0]), median(labelH[1]), median(labelH[2])};
    const int panelW = median(valueDx[2]) + Hd * 8, panelH = median(baseDy[2]) + Hd * 5;
    for (const Sample& sm : samples) {
        const cv::Mat frame = cv::imread(framesDir + "/" + sm.file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        cv::Mat grey;
        darkest(frame).convertTo(grey, CV_32F);
        const cv::Rect bounds(0, 0, grey.cols, grey.rows);
        for (int r = 0; r < 3; r++) {
            const Row& row = sm.rows[r];
            const int base = row.value.y + row.value.height;
            for (size_t g = 0; g < row.text.size(); g++) {
                const char c = row.text[g];
                const cv::Rect& b = row.glyphs[g];
                if (c != '.' && std::abs(b.height - Hd) > 1) continue;
                const int w = c == '%' ? Hd + Hd / 2 + 4 : c == '.' ? Hd / 2 + 4 : Hd + 4;
                const int cx = b.x + b.width / 2;
                const cv::Rect patch(cx - w / 2, base - Hd - 2, w, Hd + 4);
                if ((patch & bounds) != patch) continue;
                if (sums[c].empty()) sums[c] = cv::Mat::zeros(patch.size(), CV_32F);
                sums[c] += grey(patch);
                counts[c]++;
            }
            if (std::abs(row.label.width - lw[r]) <= 1 && std::abs(row.label.height - lh[r]) <= 1) {
                const cv::Rect patch(row.label.x - 3, row.label.y - 3, lw[r] + 6, lh[r] + 6);
                if ((patch & bounds) == patch) {
                    if (labelSums[r].empty()) labelSums[r] = cv::Mat::zeros(patch.size(), CV_32F);
                    labelSums[r] += grey(patch);
                    labelCounts[r]++;
                }
            }
        }
        const cv::Rect whole(sm.rows[0].label.x - 10, sm.rows[0].label.y - Hd * 3, panelW, panelH);
        if ((whole & bounds) == whole) {
            if (panelSum.empty()) panelSum = cv::Mat::zeros(whole.size(), CV_32F);
            panelSum += grey(whole);
            panelCount++;
        }
    }
    auto write = [&](const std::string& name, const cv::Mat& sum, int count) {
        cv::Mat mean = sum / (std::max)(1, count), shown;
        cv::normalize(mean, shown, 0, 255, cv::NORM_MINMAX);
        shown.convertTo(shown, CV_8U);
        cv::imwrite(outDir + "/" + name + ".png", shown);
        cv::Mat big;
        cv::resize(shown, big, cv::Size(), 8, 8, cv::INTER_NEAREST);
        cv::imwrite(outDir + "/" + name + "_x8.png", big);
        cv::FileStorage fs(outDir + "/" + name + ".yml", cv::FileStorage::WRITE);
        fs << "mean" << mean << "count" << count;
    };
    for (auto& e : sums) {
        const std::string name = e.first == '%' ? "pct" : e.first == '.' ? "stop" : std::string("d") + e.first;
        write(name, e.second, counts[e.first]);
        std::cout << "'" << e.first << "' " << counts[e.first] << "  ";
    }
    std::cout << std::endl;
    for (auto& e : labelSums) {
        write("label" + std::to_string(e.first), e.second, labelCounts[e.first]);
        std::cout << "label " << e.first << ": " << labelCounts[e.first] << std::endl;
    }
    if (panelCount) write("panel", panelSum, panelCount);
    std::cout << "panel " << panelCount << std::endl;
    return 0;
}

// --labelsearch <panel.yml> <frames...>: where the label column of the
// averaged panel matches best in each frame, and how long the search takes.
int runLabelSearch(const std::string& yml, const std::vector<std::string>& files) {
    cv::FileStorage fs(yml, cv::FileStorage::READ);
    cv::Mat mean;
    fs["mean"] >> mean;
    if (mean.empty()) return 1;
    const cv::Rect column(7, 30, 48, (std::min)(70, mean.rows - 30));
    const cv::Mat templ = mean(column).clone();
    for (const std::string& file : files) {
        const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<cv::Mat> planes;
        cv::split(frame, planes);
        cv::Mat grey;
        cv::Mat(cv::min(cv::min(planes[0], planes[1]), planes[2])).convertTo(grey, CV_32F);
        for (double scale : {1.0, 0.5}) {
            const auto t1 = std::chrono::steady_clock::now();
            cv::Mat g = grey, t = templ;
            if (scale != 1.0) {
                cv::resize(grey, g, cv::Size(), scale, scale, cv::INTER_AREA);
                cv::resize(templ, t, cv::Size(), scale, scale, cv::INTER_AREA);
            }
            cv::Mat result;
            cv::matchTemplate(g, t, result, cv::TM_CCOEFF_NORMED);
            const auto t2 = std::chrono::steady_clock::now();
            std::cout << file.substr(file.size() - 11) << " scale " << scale << " "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count() << " ms:";
            for (int k = 0; k < 4; k++) {
                double best;
                cv::Point at;
                cv::minMaxLoc(result, nullptr, &best, nullptr, &at);
                std::cout << "  (" << (int)(at.x / scale) << "," << (int)(at.y / scale) << ") " << std::fixed
                          << std::setprecision(3) << best;
                cv::rectangle(result, cv::Rect(at.x - t.cols, at.y - t.rows, t.cols * 2, t.rows * 2), cv::Scalar(-1), -1);
            }
            std::cout << std::endl;
        }
    }
    return 0;
}

// --shapebook <shapes dir> <PanelShapes.h>: the averaged panel cut into what
// the shape reader compares against - the label column and one grey glyph
// per numeral and the per cent sign - written as a header.
int runShapeBook(const std::string& dir, const std::string& outHeader) {
    auto loadMean = [&](const std::string& name) {
        cv::FileStorage fs(dir + "/" + name + ".yml", cv::FileStorage::READ);
        cv::Mat m;
        if (fs.isOpened()) fs["mean"] >> m;
        return m;
    };
    std::ofstream h(outHeader, std::ios::trunc);
    h << "// Generated by --shapebook from the panel averaged over every frame of a\n";
    h << "// recording at 1440 lines the face reader read right. Grey, from the darkest\n";
    h << "// of the three channels, at that recording's size.\n";
    h << "#pragma once\n\nnamespace SCMiner {\n\n";
    h << "struct PanelShape {\n    char glyph;       // 'L' for the label column\n    int width, height;\n    const float* grey;\n};\n\n";
    auto dump = [&](const std::string& name, const cv::Mat& m) {
        h << "inline const float " << name << "[] = {";
        for (int y = 0; y < m.rows; y++) {
            for (int x = 0; x < m.cols; x++) {
                char num[24];
                std::snprintf(num, sizeof(num), "%.1ff", m.at<float>(y, x));
                h << ((y | x) ? "," : "") << num;
            }
        }
        h << "};\n";
    };
    const cv::Mat panel = loadMean("panel");
    if (panel.empty()) return 1;
    const cv::Mat labels = panel(cv::Rect(7, 30, 48, (std::min)(70, panel.rows - 30))).clone();
    dump("SHAPE_LABELS", labels);
    struct Cut { char glyph; std::string file; int width; };
    const std::vector<Cut> cuts = {{'0', "d0", 9}, {'1', "d1", 9}, {'2', "d2", 9}, {'3', "d3", 9}, {'4', "d4", 9},
                                   {'5', "d5", 9}, {'6', "d6", 9}, {'7', "d7", 9}, {'8', "d8", 9}, {'9', "d9", 9},
                                   {'%', "pct", 15}};
    std::vector<std::string> entries;
    entries.push_back("    {'L', " + std::to_string(labels.cols) + ", " + std::to_string(labels.rows) + ", SHAPE_LABELS},");
    for (const Cut& c : cuts) {
        const cv::Mat m = loadMean(c.file);
        if (m.empty()) return 1;
        const int centre = m.cols / 2;
        const cv::Mat cut = m(cv::Rect(centre - c.width / 2, 0, c.width, m.rows)).clone();
        const std::string name = "SHAPE_" + (c.glyph == '%' ? std::string("PCT") : std::string("D") + c.glyph);
        dump(name, cut);
        entries.push_back(std::string("    {'") + c.glyph + "', " + std::to_string(cut.cols) + ", " +
                          std::to_string(cut.rows) + ", " + name + "},");
    }
    // The label column of a panel that writes its labels out, when it was cut
    const cv::Mat longLabels = loadMean("long_labels");
    if (!longLabels.empty()) {
        dump("SHAPE_LONG_LABELS", longLabels);
        entries.push_back("    {'M', " + std::to_string(longLabels.cols) + ", " + std::to_string(longLabels.rows) +
                          ", SHAPE_LONG_LABELS},");
    }
    // Two fixed words further down, whose distance from the labels pins the
    // panel's size far better than the labels alone: COMP. and SCU. The
    // averaged area starts 10 left of the labels' edge and 40 above the MASS
    // label's top.
    const cv::Mat comp = loadMean("comp");
    if (!comp.empty()) {
        const cv::Mat compWord = comp(cv::Rect(7, 155, 56, 21)).clone();     // labels + (-3, 115)
        const cv::Mat scuWord = comp(cv::Rect(198, 154, 35, 17)).clone();    // labels + (188, 114)
        dump("SHAPE_COMP", compWord);
        dump("SHAPE_SCU", scuWord);
        entries.push_back("    {'C', " + std::to_string(compWord.cols) + ", " + std::to_string(compWord.rows) +
                          ", SHAPE_COMP},");
        entries.push_back("    {'U', " + std::to_string(scuWord.cols) + ", " + std::to_string(scuWord.rows) +
                          ", SHAPE_SCU},");
    }
    h << "\ninline const PanelShape PANEL_SHAPES[] = {\n";
    for (const std::string& e : entries) h << e << "\n";
    h << "};\n\ninline constexpr int PANEL_SHAPE_COUNT = " << entries.size() << ";\n\n}  // namespace SCMiner\n";
    std::cout << "wrote " << entries.size() << " shapes" << std::endl;
    return 0;
}

// --digittrain <frames> <labels.csv> <out dir>: numerals cut where the shape
// reader placed them, named from the figures read by eye, and a linear
// classifier fitted to them - checked by fitting to one recording and
// reading the other, both ways round, before the one fitted to both is
// written out as PanelDigits.h.
int runDigitTrain(const std::string& framesDir, const std::string& labelsCsv, const std::string& outDir) {
    std::ifstream in(labelsCsv);
    if (!in.is_open()) return 1;
    std::string line;
    std::getline(in, line);
    struct Sample {
        cv::Mat features;    // 1 x D, CV_32F, normalised, with a bias
        int digit;
        int part;            // 0 first recording, 1 second
        bool centre;         // the unmoved cut
        std::string file;
    };
    std::vector<Sample> samples;
    const int W = SCMiner::PANEL_SHAPES[1].width, H = SCMiner::PANEL_SHAPES[1].height, D = W * H + 1;
    auto featuresOf = [&](const cv::Mat& patch) {
        cv::Mat f;
        patch.convertTo(f, CV_32F);
        f = f.reshape(1, 1).clone();
        f -= cv::mean(f)[0];
        const double n = cv::norm(f);
        if (n > 1e-6) f /= n;
        cv::Mat out(1, D, CV_32F);
        f.copyTo(out.colRange(0, D - 1));
        out.at<float>(0, D - 1) = 1.0f;
        return out;
    };
    int frames = 0, rowsUsed = 0, rowsSkipped = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() < 6) continue;
        const std::string want[3] = {f[2], f[3] + "%", f[5]};
        const cv::Mat frame = cv::imread(framesDir + "/" + f[0], cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        std::string texts[3];
        std::vector<std::vector<cv::Mat>> patches[3];
        if (!SCMiner::shapeSamplesOf(frame, texts, patches, 1)) continue;
        frames++;
        for (int r = 0; r < 3; r++) {
            const std::string& t = texts[r];
            bool same = t.size() == want[r].size() && t.find('?') == std::string::npos;
            for (size_t i = 0; same && i < t.size(); i++) {
                const bool a = t[i] == '.' || t[i] == '%', b = want[r][i] == '.' || want[r][i] == '%';
                if (a != b || (a && t[i] != want[r][i])) same = false;
            }
            if (!same) {
                rowsSkipped++;
                continue;
            }
            rowsUsed++;
            size_t g = 0;
            for (size_t i = 0; i < t.size(); i++) {
                if (t[i] == '.') continue;
                if (g >= patches[r].size()) break;
                const std::vector<cv::Mat>& around = patches[r][g++];
                if (want[r][i] < '0' || want[r][i] > '9') continue;
                for (size_t k = 0; k < around.size(); k++) {
                    samples.push_back({featuresOf(around[k]), want[r][i] - '0', f[0][1] == '1' ? 0 : 1,
                                       k == around.size() / 2, f[0]});
                }
            }
        }
    }
    std::cout << frames << " frames, rows used " << rowsUsed << ", skipped " << rowsSkipped << ", cuts "
              << samples.size() << std::endl;
    {
        std::ofstream dump(outDir + "/digits.bin", std::ios::binary | std::ios::trunc);
        const int count = (int)samples.size();
        dump.write((const char*)&count, sizeof(count));
        dump.write((const char*)&D, sizeof(D));
        for (const Sample& sm : samples) {
            dump.write((const char*)sm.features.ptr<float>(0), sizeof(float) * D);
            const int meta[3] = {sm.digit, sm.part, sm.centre ? 1 : 0};
            dump.write((const char*)meta, sizeof(meta));
            const int frameNo = std::atoi(sm.file.substr(3, 4).c_str());
            dump.write((const char*)&frameNo, sizeof(frameNo));
        }
    }

    auto fit = [&](int part, double lambda) {
        int n = 0;
        for (const Sample& sm : samples) n += (part < 0 || sm.part == part) ? 1 : 0;
        cv::Mat X(n, D, CV_32F), Y = cv::Mat::zeros(n, 10, CV_32F);
        int i = 0;
        for (const Sample& sm : samples) {
            if (part >= 0 && sm.part != part) continue;
            sm.features.copyTo(X.row(i));
            Y.at<float>(i, sm.digit) = 1.0f;
            i++;
        }
        cv::Mat A = X.t() * X, B = X.t() * Y, Wt;
        A += cv::Mat::eye(D, D, CV_32F) * lambda;
        cv::solve(A, B, Wt, cv::DECOMP_CHOLESKY);
        return cv::Mat(Wt.t());     // 10 x D
    };
    auto test = [&](const cv::Mat& model, int part, const std::string& name) {
        std::map<int, int> wrongAt, acceptedAt;
        const double leads[] = {0.0, 0.1, 0.2, 0.3, 0.4};
        int total = 0;
        std::map<std::string, int> confusions;
        for (const Sample& sm : samples) {
            if (sm.part != part || !sm.centre) continue;
            total++;
            const cv::Mat scores = model * sm.features.t();
            int best = 0, second = -1;
            for (int c = 1; c < 10; c++) {
                if (scores.at<float>(c) > scores.at<float>(best)) {
                    second = best;
                    best = c;
                } else if (second < 0 || scores.at<float>(c) > scores.at<float>(second)) {
                    second = c;
                }
            }
            const double lead = scores.at<float>(best) - scores.at<float>(second);
            for (int k = 0; k < 5; k++) {
                if (lead < leads[k]) continue;
                acceptedAt[k]++;
                if (best != sm.digit) wrongAt[k]++;
            }
            if (best != sm.digit) confusions[std::to_string(sm.digit) + "->" + std::to_string(best)]++;
        }
        std::cout << name << ": " << total << " numerals";
        for (int k = 0; k < 5; k++) {
            std::cout << "  lead>=" << leads[k] << " took " << acceptedAt[k] << " wrong " << wrongAt[k];
        }
        std::cout << std::endl << "   ";
        for (const auto& c : confusions) std::cout << " " << c.first << " x" << c.second;
        std::cout << std::endl;
    };
    // the shapes themselves, as they read now, for comparison
    {
        std::vector<cv::Mat> templ;
        for (int k = 1; k <= 10; k++) {
            cv::Mat t = cv::Mat(SCMiner::PANEL_SHAPES[k].height, SCMiner::PANEL_SHAPES[k].width, CV_32F,
                                const_cast<float*>(SCMiner::PANEL_SHAPES[k].grey)).clone();
            templ.push_back(featuresOf(t));
        }
        cv::Mat model(10, D, CV_32F);
        for (int k = 0; k < 10; k++) {
            templ[k].copyTo(model.row(k));
            model.at<float>(k, D - 1) = 0.0f;
        }
        test(model, 0, "shapes on the first");
        test(model, 1, "shapes on the second");
    }
    for (double lambda : {0.3, 3.0, 30.0}) {
        test(fit(0, lambda), 1, "fit first, read second, lambda " + std::to_string(lambda));
        test(fit(1, lambda), 0, "fit second, read first, lambda " + std::to_string(lambda));
    }
    const cv::Mat all = fit(-1, 3.0);
    std::ofstream h(outDir + "/PanelDigits.h", std::ios::trunc);
    h << "// Generated by --digittrain: a linear classifier for the panel's numerals,\n";
    h << "// fitted to grey cuts of them (normalised, then a bias) named from figures\n";
    h << "// read by eye. One row of weights per numeral, 0 to 9.\n";
    h << "#pragma once\n\nnamespace SCMiner {\n\n";
    h << "inline constexpr int PANEL_DIGIT_FEATURES = " << D << ";\n\n";
    h << "inline const float PANEL_DIGIT_WEIGHTS[10][" << D << "] = {\n";
    for (int c = 0; c < 10; c++) {
        h << "    {";
        for (int d = 0; d < D; d++) {
            char num[24];
            std::snprintf(num, sizeof(num), "%.5ff", all.at<float>(c, d));
            h << (d ? "," : "") << num;
        }
        h << "},\n";
    }
    h << "};\n\n}  // namespace SCMiner\n";
    std::cout << "wrote " << outDir << "/PanelDigits.h" << std::endl;
    return 0;
}

// --glyphpair <frames...>: how alike every two glyphs the shape reader placed
// in one frame are - the same numeral twice in one frame should be more alike
// than two different ones.
int runGlyphPair(const std::vector<std::string>& files) {
    for (const std::string& file : files) {
        const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        std::string texts[3];
        std::vector<std::vector<cv::Mat>> patches[3];
        if (!SCMiner::shapeSamplesOf(frame, texts, patches, 0)) continue;
        std::vector<cv::Mat> cuts;
        std::vector<std::string> names;
        for (int r = 0; r < 3; r++) {
            size_t g = 0;
            for (size_t i = 0; i < texts[r].size(); i++) {
                if (texts[r][i] == '.') continue;
                if (g >= patches[r].size()) break;
                cv::Mat f;
                patches[r][g++][0].convertTo(f, CV_32F);
                f = f.reshape(1, 1).clone();
                f -= cv::mean(f)[0];
                f /= (std::max)(1e-6, cv::norm(f));
                cuts.push_back(f);
                names.push_back(std::string(1, "mri"[r]) + std::to_string(i) + texts[r][i]);
            }
        }
        std::cout << file.substr(file.size() - 11) << "  " << texts[0] << " " << texts[1] << " " << texts[2] << std::endl;
        std::cout << "      ";
        for (const std::string& n : names) std::cout << std::setw(5) << n;
        std::cout << std::endl;
        for (size_t a = 0; a < cuts.size(); a++) {
            std::cout << std::setw(6) << names[a];
            for (size_t b = 0; b < cuts.size(); b++) {
                std::cout << std::setw(5) << (int)std::lround(cuts[a].dot(cuts[b]) * 100);
            }
            std::cout << std::endl;
        }
    }
    return 0;
}

// --glyphmean <frames dir> <out.yml> <row> <glyph> <length> <file...>: the
// average grey cut of one glyph of one figure over many frames - the glyph
// counted without full stops, the figure only when it has `length` glyphs.
int runGlyphMean(const std::string& dir, const std::string& out, int row, int glyph, int length,
                 const std::vector<std::string>& files) {
    cv::Mat sum;
    int n = 0;
    for (const std::string& file : files) {
        const cv::Mat frame = cv::imread(dir + "/" + file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        std::string texts[3];
        std::vector<std::vector<cv::Mat>> patches[3];
        if (!SCMiner::shapeSamplesOf(frame, texts, patches, 0)) continue;
        if ((int)patches[row].size() != length || glyph >= length) continue;
        cv::Mat f;
        patches[row][glyph][0].convertTo(f, CV_32F);
        f -= cv::mean(f)[0];
        f /= (std::max)(1e-6, cv::norm(f));
        if (sum.empty()) sum = cv::Mat::zeros(f.size(), CV_32F);
        sum += f;
        n++;
    }
    if (n == 0) return 1;
    sum -= cv::mean(sum)[0];
    sum /= (std::max)(1e-6, cv::norm(sum));
    cv::FileStorage fs(out, cv::FileStorage::WRITE);
    fs << "mean" << sum << "count" << n;
    std::cout << n << " cuts into " << out << std::endl;
    return 0;
}

// --glyphcompare <a.yml> <b.yml> ...: how alike every two averaged cuts are
int runGlyphCompare(const std::vector<std::string>& files) {
    std::vector<cv::Mat> means;
    for (const std::string& f : files) {
        cv::FileStorage fs(f, cv::FileStorage::READ);
        cv::Mat m;
        fs["mean"] >> m;
        means.push_back(m);
    }
    for (size_t a = 0; a < means.size(); a++) {
        std::cout << std::setw(40) << files[a].substr(files[a].find_last_of("/\\") + 1);
        for (size_t b = 0; b < means.size(); b++) {
            std::cout << std::setw(6) << std::fixed << std::setprecision(3) << means[a].dot(means[b]);
        }
        std::cout << std::endl;
    }
    return 0;
}

// --gametexts <channel folder> <out file> [owned rows]: the global.ini that
// the box in the Blueprints window would put into the game, written where
// asked instead - nothing in the game's folder is touched, user.cfg neither.
// The owned blueprints are the library's, or, given a file of CRAFT_BLUEPRINTS
// rows (one number a line), those: that is how the output is compared byte
// for byte with the reference gen_game_texts.py's tokens make in Python.
int runGameTexts(const std::string& channel, const std::string& outFile, const std::string& ownedFile) {
    std::vector<bool> owned(SCMiner::CRAFT_BLUEPRINT_COUNT, false);
    if (!ownedFile.empty()) {
        std::ifstream in(ownedFile);
        int row = 0;
        while (in >> row) {
            if (row >= 0 && row < SCMiner::CRAFT_BLUEPRINT_COUNT) owned[row] = true;
        }
    } else {
        SCMiner::BlueprintLibrary library;
        library.load();
        for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) owned[b] = library.owned(b);
    }
    std::string text, note;
    int marked = 0;
    const auto t0 = std::chrono::steady_clock::now();
    if (!SCMiner::GameTexts::build(channel, owned, text, marked, note)) {
        std::cout << "not built: " << note << std::endl;
        return 1;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::ofstream out(outFile, std::ios::binary);
    out.write(text.data(), (std::streamsize)text.size());
    std::cout << marked << " texts marked, " << text.size() << " bytes, " << ms << " ms -> " << outFile << std::endl;
    return out.good() ? 0 : 1;
}

// --locatecrop <x> <y> <w> <h> <frames...>: the locator over the whole search
// area against the locator over what captureSignatureCandidates hands it while
// a marker is locked - the aim box (x y w h, in the frame) and the marker, each
// with 150 px about it. Every label the whole-area look finds is taken as the
// lock in turn and has to come back from its crop; the cost of both is set
// side by side. Frames: the app's own recording (debug/recording/scan_N.mkv).
int runLocateCrop(const cv::Rect& aim, const std::vector<std::string>& files) {
    SCMiner::ScreenCapture capture;
    constexpr int MARGIN = 150;      // NEAR_SWEEP_MARGIN
    constexpr int CANDIDATES = 12;   // SCAN_CANDIDATES
    auto ms = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    int frames = 0, labels = 0, again = 0, crops = 0;
    double wholeMs = 0, cropMs = 0, cropArea = 0;
    for (const std::string& file : files) {
        const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        const cv::Rect all(0, 0, frame.cols, frame.rows);
        auto t0 = std::chrono::steady_clock::now();
        const auto whole = capture.locateSignatureLabels(frame, CANDIDATES);
        wholeMs += ms(t0);
        frames++;
        const cv::Rect aimArea =
            cv::Rect(aim.x - MARGIN, aim.y - MARGIN, aim.width + 2 * MARGIN, aim.height + 2 * MARGIN) & all;
        for (const cv::Rect& label : whole) {
            const cv::Rect lockArea(label.x - MARGIN, label.y - MARGIN, label.width + 2 * MARGIN,
                                    label.height + 2 * MARGIN);
            const cv::Rect box = (aimArea | lockArea) & all;
            t0 = std::chrono::steady_clock::now();
            const auto local = capture.locateSignatureLabels(frame(box), CANDIDATES);
            cropMs += ms(t0);
            cropArea += (double)box.area() / all.area();
            crops++;
            bool back = false;
            for (cv::Rect r : local) {
                r.x += box.x;
                r.y += box.y;
                if (std::abs(r.x - label.x) <= 3 && std::abs(r.y - label.y) <= 3 &&
                    std::abs(r.width - label.width) <= 4 && std::abs(r.height - label.height) <= 4) {
                    back = true;
                }
            }
            labels++;
            if (back) {
                again++;
            } else {
                std::cout << "  not found again: " << file << "  label " << label.x << "," << label.y << " "
                          << label.width << "x" << label.height << "  crop " << box.x << "," << box.y << " "
                          << box.width << "x" << box.height << std::endl;
            }
        }
    }
    if (frames == 0) return 1;
    std::cout << frames << " frames, " << labels << " labels: " << again << " found again in their crop" << std::endl;
    std::cout << std::fixed << std::setprecision(1) << "  whole area " << wholeMs / frames << " ms a look, crop "
              << (crops ? cropMs / crops : 0.0) << " ms (" << (crops ? 100.0 * cropArea / crops : 0.0)
              << " % of the area on average)" << std::endl;
    return again == labels ? 0 : 2;
}

// --backupcheck <backup> <folder>: a backup put back the way the Restore
// button puts it back - into <folder>, never the player's own data - and
// that folder backed up again the way the Back up button would today. Says
// what came back and what was left out, whether every file came back byte
// for byte, and what the backup made today weighs.
int runBackupCheck(const std::string& backupFile, const std::string& folder) {
    namespace fs = std::filesystem;
    namespace DB = SCMiner::DataBackup;
    std::ifstream in(backupFile, std::ios::binary);
    if (!in.is_open()) {
        std::cout << "cannot read " << backupFile << std::endl;
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<DB::Entry> entries;
    std::string error, aside;
    if (!DB::read(text, entries, error)) {
        std::cout << "refused: " << error << std::endl;
        return 1;
    }
    size_t kept = 0, keptBytes = 0;
    for (const auto& e : entries) {
        const bool carried = DB::carried(e.path);
        std::cout << (carried ? "  put back " : "  left out ") << std::setw(10) << e.bytes.size() << "  "
                  << e.path << std::endl;
        if (carried) {
            kept++;
            keptBytes += e.bytes.size();
        }
    }
    if (!DB::restore(fs::path(folder), entries, aside, error)) {
        std::cout << "restore refused: " << error << std::endl;
        return 1;
    }
    int same = 0, differ = 0;
    for (const auto& e : entries) {
        if (!DB::carried(e.path)) continue;
        std::ifstream back(fs::path(folder) / fs::path(e.path), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
        if (bytes == e.bytes) same++;
        else {
            differ++;
            std::cout << "  DIFFERS: " << e.path << std::endl;
        }
    }
    const std::string again = DB::write(DB::collect(fs::path(folder)));
    std::cout << entries.size() << " files in the backup (" << text.size() << " bytes), " << kept
              << " put back (" << keptBytes << " bytes), " << same << " byte for byte, " << differ
              << " not; what was in the folder went to " << DB::ASIDE << "/" << aside << std::endl;
    std::cout << "backed up again today: " << again.size() << " bytes" << std::endl;
    return differ == 0 ? 0 : 2;
}

// --payouts <channel folder>: the game's logs read the way the library reads
// them - into a fresh library, nothing loaded, nothing saved - and what the
// contracts that paid nothing add: each blueprint assumed owned and from
// which contract, and each contract the rule failed for. The player's own
// library is not touched.
int runPayouts(const std::string& channel) {
    namespace fs = std::filesystem;
    std::vector<std::pair<fs::file_time_type, fs::path>> logs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(channel) / "logbackups", ec)) {
        if (entry.path().extension() == ".log") logs.push_back({fs::last_write_time(entry.path(), ec), entry.path()});
    }
    std::sort(logs.begin(), logs.end());
    const fs::path live = fs::path(channel) / "Game.log";
    if (fs::exists(live, ec)) logs.push_back({fs::last_write_time(live, ec), live});

    SCMiner::BlueprintLibrary library;
    SCMiner::ContractScan scan;
    int ofNow = 0;
    for (const auto& [when, path] : logs) {
        (void)when;
        std::ifstream in(path, std::ios::binary);
        std::string line;
        scan.reset();
        bool counted = false;
        while (std::getline(in, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            library.feedLine(line, scan);
            if (!counted && scan.ruleHolds()) {
                counted = true;
                ofNow++;
            }
        }
        library.finishFile(scan);
    }
    std::cout << logs.size() << " logs, " << ofNow << " of them from 4.10 on; " << library.ownedCount()
              << " blueprints owned by them, " << library.assumedCount() << " of those assumed" << std::endl;
    for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
        const std::string source = library.sourceOf(b);
        if (source.rfind("assumed: ", 0) != 0) continue;
        std::cout << "  assumed  " << library.stamps()[b].substr(0, 16) << "  " << SCMiner::CRAFT_BLUEPRINTS[b].name
                  << "  <- " << source.substr(9) << std::endl;
    }
    for (const auto& title : library.doubted()) std::cout << "  doubted  " << title << std::endl;
    return 0;
}

void printStandings(std::vector<SCMiner::Standing> all);

// --trades <channel folder>: what was bought and sold at the commodity
// terminals, read out of the game's logs the way the library reads them -
// into a fresh book, nothing loaded, nothing saved. What, how much, for how
// much, at which shop and where; nothing of the player's own names.
#ifdef _WIN32
// --hidecheck: whether a window kept out of screen captures
// (WDA_EXCLUDEFROMCAPTURE, as the overlay is) is left out of the picture
// Bedrock takes of the screen - Desktop Duplication, or GDI where that is
// not to be had. A red square at the foot of the main screen for a moment:
// as it is, kept out, and kept out while translucent like the overlay; and
// as it is with Bedrock's own windows blacked out - on its own, under a
// green stand-in for the game's window (class CryENGINE), and over it.
int runHideCheck() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = CreateSolidBrush(RGB(255, 0, 0));
    wc.lpszClassName = L"BedrockCaptureCheck";
    RegisterClassW(&wc);
    const int size = 200;
    const int x = GetSystemMetrics(SM_CXSCREEN) - size - 100, y = GetSystemMetrics(SM_CYSCREEN) - size - 100;
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"",
                             WS_POPUP | WS_VISIBLE, x, y, size, size, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return 1;
    auto settle = [](int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        MSG msg;
        while (std::chrono::steady_clock::now() < until) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(10);
        }
    };
    SCMiner::ScreenCapture capture;
    SCMiner::CaptureRegion region;
    region.x = x + 20;
    region.y = y + 20;
    region.width = size - 40;
    region.height = size - 40;
    region.autoDetect = false;
    auto look = [&](const char* what) {
        settle(400);
        const cv::Mat shot = capture.captureRegion(region);
        const cv::Scalar mean = shot.empty() ? cv::Scalar() : cv::mean(shot);
        const bool red = mean[2] > 200 && mean[1] < 60 && mean[0] < 60;
        const bool green = mean[1] > 200 && mean[2] < 60 && mean[0] < 60;
        const bool black = mean[0] < 5 && mean[1] < 5 && mean[2] < 5;
        std::cout << what << ": "
                  << (red ? "the red square" : green ? "the game (green)" : black ? "black" : "what is under it") << "  (B "
                  << (int)mean[0] << " G " << (int)mean[1] << " R " << (int)mean[2] << ", via "
                  << (capture.lastSource() == SCMiner::ScreenCapture::Source::Gdi ? "GDI" : "Desktop Duplication")
                  << ")" << std::endl;
    };
    SCMiner::ScreenCapture::maskOwnWindows = false;
    look("as it is              ");
    const BOOL set = SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE);
    look(set ? "kept out of captures   " : "kept out (call FAILED) ");
    SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) | WS_EX_LAYERED);
    SetLayeredWindowAttributes(w, 0, 235, LWA_ALPHA);
    look("...and translucent     ");
    SetWindowDisplayAffinity(w, WDA_NONE);
    SCMiner::ScreenCapture::maskOwnWindows = true;
    look("as it is, own windows blacked out");
    // A stand-in for the game's window, round the square: what is under the
    // game is not in the picture, and must not black the game out
    WNDCLASSW gc{};
    gc.lpfnWndProc = DefWindowProcW;
    gc.hInstance = wc.hInstance;
    gc.hbrBackground = CreateSolidBrush(RGB(0, 255, 0));
    gc.lpszClassName = L"CryENGINE";
    RegisterClassW(&gc);
    HWND g = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, gc.lpszClassName, L"",
                             WS_POPUP | WS_VISIBLE, x - 40, y - 40, size + 80, size + 80, nullptr, nullptr,
                             wc.hInstance, nullptr);
    if (g) {
        SetWindowPos(w, g, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        look("under the game's window  ");
        SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        look("over the game's window   ");
        DestroyWindow(g);
    }
    DestroyWindow(w);
    return 0;
}
#endif

int runTrades(const std::string& channel) {
    namespace fs = std::filesystem;
    std::vector<std::pair<fs::file_time_type, fs::path>> logs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(channel) / "logbackups", ec)) {
        if (entry.path().extension() == ".log") logs.push_back({fs::last_write_time(entry.path(), ec), entry.path()});
    }
    std::sort(logs.begin(), logs.end());
    const fs::path live = fs::path(channel) / "Game.log";
    if (fs::exists(live, ec)) logs.push_back({fs::last_write_time(live, ec), live});

    SCMiner::TradeBook book;
    int with = 0;
    for (const auto& [when, path] : logs) {
        (void)when;
        std::ifstream in(path, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!SCMiner::TradeBook::hasRequest(data)) continue;
        with++;
        book.replay(data);
    }
    int refused = 0;
    for (const SCMiner::Trade& trade : book.trades()) {
        const char* name = SCMiner::TradeBook::nameOf(trade.resource);
        char text[160];
        std::snprintf(text, sizeof(text), "%s  %-6s %9.2f SCU  %-26s %12.0f aUEC  %9.0f / SCU  ", trade.stamp.substr(0, 19).c_str(),
                      trade.sold ? "sold" : "bought", trade.scu, name[0] ? name : trade.resource.c_str(), trade.total,
                      trade.perScu());
        std::cout << text << trade.shop << "  -  " << (trade.place.empty() ? "?" : trade.place)
                  << (trade.refused ? "  (refused)" : "") << std::endl;
        if (trade.refused) refused++;
    }
    std::cout << logs.size() << " logs, " << with << " with a trade in them: " << book.count() << " requests, " << refused
              << " of them refused" << std::endl;
    return 0;
}

// --uexprobe <folder>: the Ore Finder's prices the way the app gets them -
// the lists kept in <folder>, fetched where they are too old - and what came
// of it: how old, what went wrong, and the best five for a few ores
int runUexProbe(const std::string& folder) {
    const auto started = std::chrono::steady_clock::now();
    const SCMiner::Application::UexOutcome out = SCMiner::Application::loadUex(folder);
    const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started).count();
    std::cout << "took " << ms << " ms; prices "
              << (out.prices ? std::to_string(((long long)std::time(nullptr) - out.fetchedAt) / 60) + " min old"
                             : std::string("none"))
              << (out.note.empty() ? std::string() : "; note: " + out.note)
              << (out.placesNote.empty() ? std::string() : "; " + out.placesNote)
              << (out.tokenRefused ? "; UEX turned the app token away, asked without it" : "") << std::endl;
    if (!out.prices) return 1;
    std::vector<std::string> ores;
    for (const auto& m : SCMiner::MINERAL_SIGNATURES) ores.push_back(m.name);
    std::cout << "sells highest: " << out.prices->mostValuable(ores) << std::endl;
    // ...and the marketplace, for an ore players pay for quality
    std::cout << SCMiner::Application::uexMarketForProbe(folder, "Ouratite");
    for (const auto& ore : ores) {
        const auto& offers = out.prices->offersFor(ore);
        std::cout << ore << ": " << offers.size() << std::endl;
        for (const auto& o : offers) {
            std::cout << "    " << (long long)o.price << "  " << o.terminal << "  |  " << o.place << "  |  "
                      << ((long long)std::time(nullptr) - o.reported) / 3600 << " h" << std::endl;
        }
    }
    return 0;
}

// --uexreportprobe <folder> <channel folder>: reporting to UEX, without
// reporting anything - UEX's lists kept in <folder>, every trade of the
// channel's logs put to them (which terminal, which commodity, or why none),
// and one price report sent as a test (is_production 0) with a made-up key:
// UEX's answer says the request got as far as the key
int runUexReportProbe(const std::string& folder, const std::string& channel) {
    namespace fs = std::filesystem;
    const auto directory = SCMiner::Application::loadUexDirectoryForProbe(folder);
    if (!directory) {
        std::cout << "UEX's lists could not be had" << std::endl;
        return 1;
    }
    std::vector<std::pair<fs::file_time_type, fs::path>> logs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(channel) / "logbackups", ec)) {
        if (entry.path().extension() == ".log") logs.push_back({fs::last_write_time(entry.path(), ec), entry.path()});
    }
    std::sort(logs.begin(), logs.end());
    const fs::path live = fs::path(channel) / "Game.log";
    if (fs::exists(live, ec)) logs.push_back({fs::last_write_time(live, ec), live});
    SCMiner::TradeBook book;
    for (const auto& [when, path] : logs) {
        (void)when;
        std::ifstream in(path, std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (SCMiner::TradeBook::hasRequest(data)) book.replay(data);
    }
    int reportable = 0, all = 0;
    int firstTerminal = 0, firstCommodity = 0;
    for (const SCMiner::Trade& trade : book.trades()) {
        if (trade.refused) continue;
        all++;
        std::string why;
        const std::string name = SCMiner::TradeBook::nameOf(trade.resource);
        const int terminal = directory->tradeTerminal(trade.shop, trade.place, why);
        const int commodity = name.empty() ? 0 : directory->commodity(name);
        if (terminal && commodity) {
            reportable++;
            if (!firstTerminal) {
                firstTerminal = terminal;
                firstCommodity = commodity;
            }
        }
        std::cout << trade.stamp.substr(0, 16) << "  " << (trade.sold ? "sold  " : "bought") << "  "
                  << (name.empty() ? trade.resource : name) << "  " << trade.shop << " | "
                  << (trade.place.empty() ? "?" : trade.place) << "  ->  "
                  << (terminal ? directory->terminalName(terminal) + " (" + std::to_string(terminal) + ")" : "none: " + why)
                  << (commodity ? "" : "  [UEX lists no such commodity]") << std::endl;
    }
    std::cout << reportable << " of " << all << " trades could be reported" << std::endl;
    if (!firstTerminal) return 0;
    // one test report: a made-up key, the first reportable trade's terminal and commodity
    const std::string body = SCMiner::UexReports::priceReport(firstTerminal, firstCommodity, true, 1, 0, "", false);
    SCMiner::HttpAnswer answer;
    const std::string token = SCMiner::Application::uexTokenForProbe();
    std::vector<std::string> headers = {"secret-key: " + std::string(40, '0'), "Content-Type: application/json"};
    if (!token.empty()) headers.push_back("Authorization: Bearer " + token);
    const bool sent = SCMiner::httpsPost("https://api.uexcorp.uk/2.0/data_submit", headers, body, answer);
    std::cout << "test report (is_production 0, a made-up key): "
              << (sent ? "HTTP " + std::to_string(answer.status) + ", " + answer.body.substr(0, 200) : answer.why)
              << std::endl;
    return 0;
}

// --uexshotprobe <picture> <out.jpg>: the picture a price report would
// carry, made of a saved game frame the way Bedrock makes it of the screen,
// written out to look at - with its size as UEX gets it
int runUexShotProbe(const std::string& picture, const std::string& out) {
    const cv::Mat frame = cv::imread(picture, cv::IMREAD_UNCHANGED);
    if (frame.empty()) {
        std::cout << "could not read " << picture << std::endl;
        return 1;
    }
    const std::vector<unsigned char> jpeg = SCMiner::Application::uexPictureJpeg(frame);
    std::ofstream(out, std::ios::binary).write((const char*)jpeg.data(), (std::streamsize)jpeg.size());
    const cv::Mat back = cv::imdecode(jpeg, cv::IMREAD_COLOR);
    const size_t sent = SCMiner::UexReports::base64(jpeg.data(), jpeg.size()).size();
    std::cout << frame.cols << "x" << frame.rows << " -> " << back.cols << "x" << back.rows << ", JPEG "
              << jpeg.size() / 1024 << " KB, " << sent / 1024 << " KB as sent (UEX takes up to 10 MB)" << std::endl;
    return 0;
}

// --ranks <channel folder>: the player's rank with each outfit, read out of
// the game's logs the way the library reads them - into a fresh library,
// nothing loaded, nothing saved. Per outfit and ladder only; nothing of the
// player's own names. For comparing with what they know their ranks to be.
int runRanks(const std::string& channel) {
    namespace fs = std::filesystem;
    std::vector<std::pair<fs::file_time_type, fs::path>> logs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(channel) / "logbackups", ec)) {
        if (entry.path().extension() == ".log") logs.push_back({fs::last_write_time(entry.path(), ec), entry.path()});
    }
    std::sort(logs.begin(), logs.end());
    const fs::path live = fs::path(channel) / "Game.log";
    if (fs::exists(live, ec)) logs.push_back({fs::last_write_time(live, ec), live});

    SCMiner::BlueprintLibrary library;
    SCMiner::ContractScan scan;
    std::map<std::string, int> envs;
    for (const auto& [when, path] : logs) {
        (void)when;
        std::ifstream in(path, std::ios::binary);
        std::string line;
        scan.reset();
        while (std::getline(in, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            library.feedLine(line, scan);
        }
        envs[scan.env().empty() ? "(no header)" : scan.env()]++;
        library.finishFile(scan);
    }
    int accepted = 0, completed = 0, unknown = 0, before = 0;
    for (const auto& [key, e] : library.contracts()) {
        (void)key;
        (e.completed ? completed : accepted)++;
        if (!SCMiner::Reputation::contract(e.contract)) unknown++;
        if (e.version < library.countsFrom()) before++;
    }
    std::cout << logs.size() << " logs:";
    for (const auto& [env, n] : envs) std::cout << " " << n << " " << env;
    std::cout << "; the player's own contracts: " << accepted << " taken, " << completed << " finished, " << before
              << " of them before " << SCMiner::Reputation::versionName(library.countsFrom()) << ", " << unknown
              << " this build does not know; " << library.ownedCount() << " blueprints owned by them" << std::endl;
    printStandings(library.standings());
    return 0;
}

void printStandings(std::vector<SCMiner::Standing> all) {
    std::sort(all.begin(), all.end(), [](const SCMiner::Standing& a, const SCMiner::Standing& b) {
        const int c = std::strcmp(SCMiner::REP_FACTIONS[a.faction].name, SCMiner::REP_FACTIONS[b.faction].name);
        return c != 0 ? c < 0 : a.ladder < b.ladder;
    });
    for (const auto& s : all) {
        const SCMiner::RepLadder& l = SCMiner::REP_LADDERS[s.ladder];
        std::cout << "  " << std::left << std::setw(34) << SCMiner::REP_FACTIONS[s.faction].name << std::setw(16) << l.name
                  << std::setw(28) << SCMiner::REP_RANKS[l.first + s.rank].name << std::right << std::setw(8)
                  << s.reputation << "  ";
        if (s.anchorRank >= 0) {
            std::cout << "from a " << SCMiner::REP_RANKS[l.first + s.anchorRank].name << " contract taken "
                      << s.anchorStamp.substr(0, 10);
        } else {
            std::cout << "counted since the wipe";
        }
        std::cout << ", +" << s.gained << " from " << s.completions << " finished";
        if (s.proven >= 0) std::cout << "; proven " << SCMiner::REP_RANKS[l.first + s.proven].name;
        if (s.set) std::cout << "; set " << s.setStamp.substr(0, 16);
        std::cout << std::endl;
    }
}

// --rankcheck <data folder> <channel folder>: the library as the app keeps it,
// in a folder of its own - a copy of the player's, never theirs - read
// against the game's logs to the end, the way a first start of this version
// does: the old logs once more, the other game's lines out, the contracts
// kept. Says what left the library and what the ranks are.
int runRankCheck(const std::string& dataFolder, const std::string& channel) {
    namespace fs = std::filesystem;
    SCMiner::AppPaths::useDataFolder(fs::path(dataFolder));
    SCMiner::BlueprintLibrary library;
    library.load();
    std::vector<int> had;
    for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
        if (library.owned(b)) had.push_back(b);
    }
    const int archivedBefore = library.archived();
    library.setGameLog(fs::path(channel) / "Game.log");
    const auto start = std::chrono::steady_clock::now();
    while (library.historyPending()) library.poll();
    library.poll();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << library.historyRead() << " logs read in " << std::fixed << std::setprecision(1) << seconds
              << " s; logs checked: " << (library.logsChecked() ? "yes" : "no") << "; owned " << had.size() << " -> "
              << library.ownedCount() << " (" << library.foreignDropped() << " only the other game's logs had)"
              << "; evidence lines " << archivedBefore << " -> " << library.archived() << "; contracts kept "
              << library.contractsKept() << std::endl;
    for (int b : had) {
        if (!library.owned(b)) std::cout << "  no longer owned: " << SCMiner::CRAFT_BLUEPRINTS[b].name << std::endl;
    }
    for (int b = 0; b < SCMiner::CRAFT_BLUEPRINT_COUNT; b++) {
        if (library.owned(b) && std::find(had.begin(), had.end(), b) == had.end()) {
            std::cout << "  owned now: " << SCMiner::CRAFT_BLUEPRINTS[b].name << " (" << library.sourceOf(b) << ")"
                      << std::endl;
        }
    }
    printStandings(library.standings());
    return 0;
}

// --truthcheck <frames> <labels.csv>: every rock's figures read once more,
// from glyphs averaged over all of its frames - a check on labels read by
// eye (such as 40.83 typed for 40.03).
int runTruthCheck(const std::string& framesDir, const std::string& labelsCsv) {
    std::ifstream in(labelsCsv);
    if (!in.is_open()) return 1;
    std::string line;
    std::getline(in, line);
    struct Rock {
        std::string want[3];
        std::vector<cv::Mat> sums[3];
        int counts[3] = {0, 0, 0};
    };
    std::map<std::string, Rock> rocks;
    std::vector<std::string> order;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() < 6) continue;
        if (!rocks.count(f[1])) order.push_back(f[1]);
        Rock& rock = rocks[f[1]];
        rock.want[0] = f[2];
        rock.want[1] = f[3] + "%";
        rock.want[2] = f[5];
        const cv::Mat frame = cv::imread(framesDir + "/" + f[0], cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        std::string texts[3];
        std::vector<std::vector<cv::Mat>> patches[3];
        if (!SCMiner::shapeSamplesOf(frame, texts, patches, 0)) continue;
        for (int r = 0; r < 3; r++) {
            size_t glyphs = 0;
            for (char c : rock.want[r]) glyphs += c == '.' ? 0 : 1;
            if (patches[r].size() != glyphs) continue;
            if (rock.sums[r].empty()) rock.sums[r].assign(glyphs, cv::Mat());
            for (size_t g = 0; g < glyphs; g++) {
                cv::Mat p;
                patches[r][g][0].convertTo(p, CV_32F);
                p -= cv::mean(p)[0];
                p /= (std::max)(1e-6, cv::norm(p));
                if (rock.sums[r][g].empty()) rock.sums[r][g] = cv::Mat::zeros(p.size(), CV_32F);
                if (rock.sums[r][g].size() == p.size()) rock.sums[r][g] += p;
            }
            rock.counts[r]++;
        }
    }
    std::vector<cv::Mat> refs;
    for (int k = 1; k <= 10; k++) {
        cv::Mat t = cv::Mat(SCMiner::PANEL_SHAPES[k].height, SCMiner::PANEL_SHAPES[k].width, CV_32F,
                            const_cast<float*>(SCMiner::PANEL_SHAPES[k].grey)).clone();
        t -= cv::mean(t)[0];
        t /= (std::max)(1e-6, cv::norm(t));
        refs.push_back(t);
    }
    for (const std::string& id : order) {
        Rock& rock = rocks[id];
        std::cout << id << std::endl;
        for (int r = 0; r < 3; r++) {
            std::cout << "   " << "mri"[r] << " truth " << rock.want[r] << " from " << rock.counts[r] << " frames:";
            size_t g = 0;
            for (char c : rock.want[r]) {
                if (c == '.') {
                    std::cout << " .";
                    continue;
                }
                if (g >= rock.sums[r].size() || rock.sums[r][g].empty()) {
                    std::cout << " -";
                    g++;
                    continue;
                }
                cv::Mat m = rock.sums[r][g++];
                m -= cv::mean(m)[0];
                m /= (std::max)(1e-6, cv::norm(m));
                int best = -1, second = -1;
                std::vector<double> sc(10);
                for (int k = 0; k < 10; k++) {
                    sc[k] = m.size() == refs[k].size() ? m.dot(refs[k]) : -1;
                    if (best < 0 || sc[k] > sc[best]) {
                        second = best;
                        best = k;
                    } else if (second < 0 || sc[k] > sc[second]) {
                        second = k;
                    }
                }
                if (c == '%') {
                    std::cout << " %";
                    continue;
                }
                std::cout << " " << best << (best == c - '0' ? "" : "!") << "(" << std::fixed << std::setprecision(3)
                          << sc[best] << " vs " << second << " " << sc[second] << ")";
            }
            std::cout << std::endl;
        }
    }
    return 0;
}

// --digiteval <digits.bin>: classifiers for the numerals, each fitted to one
// recording and tried on the other.
int runDigitEval(const std::string& bin, const std::string& outHeader) {
    std::ifstream in(bin, std::ios::binary);
    if (!in.is_open()) return 1;
    int count = 0, D = 0;
    in.read((char*)&count, sizeof(count));
    in.read((char*)&D, sizeof(D));
    struct Sample {
        cv::Mat f;          // 1 x (D-1), normalised
        int digit, part, frame;
        bool centre;
    };
    std::vector<Sample> all(count);
    for (Sample& sm : all) {
        cv::Mat row(1, D, CV_32F);
        in.read((char*)row.ptr<float>(0), sizeof(float) * D);
        int meta[3];
        in.read((char*)meta, sizeof(meta));
        in.read((char*)&sm.frame, sizeof(sm.frame));
        sm.f = row.colRange(0, D - 1).clone();
        sm.digit = meta[0];
        sm.part = meta[1];
        sm.centre = meta[2] != 0;
    }
    std::cout << all.size() << " cuts" << std::endl;

    // a classifier: exemplars with their numeral
    struct Model {
        cv::Mat exemplars;     // N x (D-1)
        std::vector<int> digits;
    };
    auto classify = [](const Model& m, const cv::Mat& f, double& lead) {
        const cv::Mat scores = m.exemplars * f.t();
        double best[10];
        for (double& b : best) b = -2;
        for (int i = 0; i < scores.rows; i++) {
            const double sc = scores.at<float>(i);
            if (sc > best[m.digits[i]]) best[m.digits[i]] = sc;
        }
        int top = 0, second = 1;
        if (best[1] > best[0]) std::swap(top, second);
        for (int c = 2; c < 10; c++) {
            if (best[c] > best[top]) {
                second = top;
                top = c;
            } else if (best[c] > best[second]) {
                second = c;
            }
        }
        lead = best[top] - best[second];
        return top;
    };
    auto evaluate = [&](const Model& m, int part, const std::string& name) {
        const double leads[] = {0.0, 0.005, 0.01, 0.02, 0.03, 0.05};
        int took[6] = {0}, wrong[6] = {0}, total = 0;
        std::map<std::string, int> conf;
        for (const Sample& sm : all) {
            if (sm.part != part || !sm.centre) continue;
            total++;
            double lead = 0;
            const int c = classify(m, sm.f, lead);
            for (int k = 0; k < 6; k++) {
                if (lead < leads[k]) continue;
                took[k]++;
                if (c != sm.digit) wrong[k]++;
            }
            if (c != sm.digit) conf[std::to_string(sm.digit) + ">" + std::to_string(c)]++;
        }
        std::cout << name << " (" << m.exemplars.rows << " exemplars) on " << total << ":";
        for (int k = 0; k < 6; k++) std::cout << "  " << leads[k] << ":" << took[k] << "/" << wrong[k];
        std::cout << std::endl << "      ";
        for (const auto& c : conf) std::cout << " " << c.first << "x" << c.second;
        std::cout << std::endl;
    };
    auto means = [&](int part, int perClass, bool jittered) {
        Model m;
        for (int d = 0; d < 10; d++) {
            std::vector<cv::Mat> rows;
            for (const Sample& sm : all) {
                if ((part >= 0 && sm.part != part) || sm.digit != d) continue;
                if (!jittered && !sm.centre) continue;
                rows.push_back(sm.f);
            }
            if (rows.empty()) continue;
            cv::Mat data;
            cv::vconcat(rows, data);
            const int k = (std::min)(perClass, data.rows);
            cv::Mat centres;
            if (k == 1) {
                cv::reduce(data, centres, 0, cv::REDUCE_AVG);
            } else {
                cv::Mat labels;
                cv::kmeans(data, k, labels, cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 40, 1e-4),
                           3, cv::KMEANS_PP_CENTERS, centres);
            }
            for (int i = 0; i < centres.rows; i++) {
                cv::Mat c = centres.row(i).clone();
                c -= cv::mean(c)[0];
                c /= (std::max)(1e-6, cv::norm(c));
                m.exemplars.push_back(c);
                m.digits.push_back(d);
            }
        }
        return m;
    };
    if (outHeader.size() > 4 && outHeader.substr(outHeader.size() - 4) == ".bin") {
        // <name>.bin: two files, <name>.first.bin and <name>.second.bin, each
        // clustered from one recording only
        for (int part = 0; part < 2; part++) {
            cv::setRNGSeed(16092026);
            const Model m = means(part, 32, DEV_ENV("SCMINER_CENTRED") == nullptr);
            const std::string path = outHeader.substr(0, outHeader.size() - 4) + (part == 0 ? ".first.bin" : ".second.bin");
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            const int count = m.exemplars.rows;
            out.write((const char*)&count, sizeof(count));
            for (int i = 0; i < count; i++) {
                const int digit = m.digits[i];
                out.write((const char*)&digit, sizeof(digit));
                out.write((const char*)m.exemplars.ptr<float>(i), sizeof(float) * (D - 1));
            }
            std::cout << "wrote " << count << " shapes to " << path << std::endl;
        }
        return 0;
    }
    if (!outHeader.empty()) {
        cv::setRNGSeed(16092026);
        const Model m = means(-1, 32, true);
        std::ofstream h(outHeader, std::ios::trunc);
        h << "// Generated by --digiteval: the panel's numerals as grey shapes, 32 of each," << std::endl;
        h << "// clustered from cuts (to a fraction of a pixel, and half a pixel either" << std::endl;
        h << "// way) of two recordings, named from figures read by eye and checked" << std::endl;
        h << "// against the glyphs averaged over every frame of a rock. Normalised; the" << std::endl;
        h << "// nearest one names the numeral." << std::endl;
        h << "#pragma once" << std::endl << std::endl << "namespace SCMiner {" << std::endl << std::endl;
        h << "struct PanelDigitShape {" << std::endl << "    int digit;" << std::endl << "    float grey["
          << (D - 1) << "];" << std::endl << "};" << std::endl << std::endl;
        h << "inline const PanelDigitShape PANEL_DIGITS[] = {" << std::endl;
        for (int i = 0; i < m.exemplars.rows; i++) {
            h << "    {" << m.digits[i] << ", {";
            for (int d = 0; d < D - 1; d++) {
                char num[24];
                std::snprintf(num, sizeof(num), "%.4ff", m.exemplars.at<float>(i, d));
                h << (d ? "," : "") << num;
            }
            h << "}}," << std::endl;
        }
        h << "};" << std::endl << std::endl << "inline constexpr int PANEL_DIGIT_COUNT = " << m.exemplars.rows << ";"
          << std::endl << std::endl << "}  // namespace SCMiner" << std::endl;
        std::cout << "wrote " << m.exemplars.rows << " shapes to " << outHeader << std::endl;
        return 0;
    }
    for (int k : {1, 4, 12, 32}) {
        for (bool jit : {false, true}) {
            const std::string tag = "k" + std::to_string(k) + (jit ? " jittered" : " centred");
            evaluate(means(0, k, jit), 1, "fit first, read second, " + tag);
            evaluate(means(1, k, jit), 0, "fit second, read first, " + tag);
        }
    }
    return 0;
}

// --steadybench <frames> <out.csv> <step> <prefix>: the frames of one
// recording in order, every `step`-th one looked at the way the app looks,
// and for every frame what the app would be showing at that moment - the last
// settled rock, until eight looks in a row found no panel. Written in the
// columns of --panelbench, so the same score reads both.
int runSteadyBench(const std::string& dir, const std::string& outCsv, int step, const std::string& prefix) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".jpg") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    SCMiner::OCREngine ocr;
    SCMiner::OCRConfig cfg;
    // without tessdata, as every install runs: the shapes and the second
    // reader alone
    if (!ocr.initialize(cfg)) std::cout << "no tessdata - reading by the shapes alone" << std::endl;
    SCMiner::GlyphReader glyphs;
    SCMiner::throttlePanelSearch(true);
    std::ofstream out(outCsv, std::ios::trunc);
    out << "file,found,figures,ore,mass,res,res_printed,inst,scu,box_x,box_y,box_w,box_h,head,body,ms,note" << std::endl;
    auto quoted = [](std::string text) {
        for (char& c : text) {
            if (c == '"') c = '\'';
        }
        return "\"" + text + "\"";
    };
    SCMiner::PanelSteadier steadier;
    SCMiner::RockStats shown;
    bool showing = false;
    int misses = 0;
    std::string lastNote;
    int lastMs = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (i % (size_t)step == 0) {
            const cv::Mat frame = cv::imread(files[i], cv::IMREAD_COLOR);
            const auto started = std::chrono::steady_clock::now();
            SCMiner::ScanPanel look;
            if (!frame.empty()) look = SCMiner::readScanPanel(frame, ocr, &glyphs, panelLines());
            SCMiner::RockStats rock;
            bool settled = false;
            if (look.found) {
                misses = 0;
                settled = steadier.add(look, rock);
                if (settled) {
                    shown = rock;
                    showing = true;
                }
            } else if (++misses >= 8) {
                steadier.clear();
                showing = false;
            }
            lastMs = (int)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            lastNote = std::string(look.found ? (settled ? "settled" : "unsettled") : "no panel") + " after " +
                       std::to_string(steadier.looks()) + " looks; " + look.note;
        }
        char figures[160];
        std::snprintf(figures, sizeof(figures), "%.0f,%.0f,%s,%.2f,%.2f", shown.mass, shown.resistance,
                      shown.resistanceAdjusted ? "colour" : "white", shown.instability, shown.scu);
        out << std::filesystem::path(files[i]).filename().string() << "," << (showing ? 1 : 0) << ","
            << (showing ? 4 : 0) << "," << quoted(showing ? shown.ore : "") << "," << figures << ",0,0,0,0,0,0,"
            << lastMs << "," << quoted(lastNote) << std::endl;
    }
    return 0;
}

// --contents <pictures, or a folder [shard shards]>: the rock's composition
// list as the app would read it - the panel found, then every line's share
// and quality, the names by the rock's kind or read, and whether it holds
int runContents(const std::vector<std::string>& files) {
    SCMiner::OCREngine ocr;
    SCMiner::OCRConfig cfg;
    if (!ocr.initialize(cfg)) std::cout << "no tessdata - reading by the shapes alone" << std::endl;
    SCMiner::GlyphReader glyphs;
    SCMiner::TextLineReader* lines = panelLines();
    if (!lines) return 1;
    int panels = 0, whole = 0;
    for (const std::string& file : files) {
        const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        const SCMiner::ScanPanel panel = SCMiner::readScanPanel(frame, ocr, &glyphs, lines);
        std::cout << std::filesystem::path(file).filename().string() << ": ";
        if (!panel.found || panel.labels.x < 0) {
            std::cout << "no panel" << std::endl;
            continue;
        }
        panels++;
        SCMiner::RockContents contents;
        SCMiner::readRockContents(frame, panel, panel.stats.ore, *lines, contents);
        if (contents.whole) whole++;
        std::cout << "'" << panel.stats.ore << "' " << panel.stats.mass << " kg, labels " << panel.labelGlyph << " at "
                  << panel.labels.x << "," << panel.labels.y << " x" << panel.labelScale << " |";
        for (const SCMiner::RockPartRead& part : contents.parts) {
            std::cout << " [" << part.shareText << " -> " << part.percent << " % | "
                      << (part.inert ? std::string("inert") : part.ore.empty() ? "?" + part.nameText : part.ore) << " | "
                      << part.qualityText << " -> " << part.quality << "]";
        }
        std::cout << " | " << (contents.whole ? "WHOLE" : "not whole") << " sum " << contents.sum << "; " << contents.note
                  << "; " << (int)contents.ms << " ms | scu " << panel.stats.scu << std::endl;
    }
    std::cout << files.size() << " pictures, " << panels << " with a panel, " << whole << " lists whole" << std::endl;
    return 0;
}

// --compmean <frames dir> <out prefix> <files...>: the panel below the
// figures - the volume's line - averaged over frames, aligned on the labels
int runCompMean(const std::string& dir, const std::string& out, const std::vector<std::string>& files) {
    cv::Mat sum;
    int n = 0;
    for (const std::string& file : files) {
        const cv::Mat frame = cv::imread(dir + "/" + file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        cv::Point labels;
        double scale = 0, score = 0;
        if (!SCMiner::findPanelLabels(frame, labels, scale, score) || std::abs(scale - 1.0) > 0.02) continue;
        const cv::Rect area(labels.x - 10, labels.y - 40, 270, 190);
        if ((area & cv::Rect(0, 0, frame.cols, frame.rows)) != area) continue;
        std::vector<cv::Mat> planes;
        cv::split(frame(area), planes);
        cv::Mat grey;
        cv::Mat(cv::min(cv::min(planes[0], planes[1]), planes[2])).convertTo(grey, CV_32F);
        if (sum.empty()) sum = cv::Mat::zeros(grey.size(), CV_32F);
        sum += grey;
        n++;
    }
    if (n == 0) return 1;
    cv::Mat mean = sum / n, shown, big;
    cv::normalize(mean, shown, 0, 255, cv::NORM_MINMAX);
    shown.convertTo(shown, CV_8U);
    cv::resize(shown, big, cv::Size(), 6, 6, cv::INTER_NEAREST);
    cv::imwrite(out + "_x6.png", big);
    cv::FileStorage fs(out + ".yml", cv::FileStorage::WRITE);
    fs << "mean" << mean << "count" << n;
    std::cout << n << " frames; area starts 10 left of the labels and 40 above the MASS label" << std::endl;
    return 0;
}

// --turretharvest <truth.csv> <out dir>: the MOLE turret's numerals and where
// they stand, off frames whose figures the face reader read (file,mass,res,inst).
// The turret writes its figures in a font of its own, wider and rounder than
// the Prospector's. The face reader finds each glyph's box; the boxes against
// the label column the shape reader found give the places, and the glyphs cut
// out and averaged give the shapes - each in the ink its row is written in:
// the mass white, the resistance green, the instability red or white.
int runTurretHarvest(const std::string& truthCsv, const std::string& outDir, char wantGlyph = 'T', int wantInk = 4) {
    std::ifstream in(truthCsv);
    if (!in.is_open()) return 1;
    std::string line;
    std::getline(in, line);
    struct Truth {
        std::string file, text[3];
    };
    std::vector<Truth> truths;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(cell);
        if (f.size() >= 4) truths.push_back({f[0], {f[1], f[2] + "%", f[3]}});
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    SCMiner::throttlePanelSearch(false);

    struct Glyph {
        char c;
        int row;             // 0 mass, 1 resistance, 2 instability, 3 COMP., 4 SCU
        cv::Rect box;        // pixels
    };
    struct Sample {
        std::string file;
        cv::Point labels;
        double scale;
        std::vector<Glyph> glyphs;
    };
    std::vector<Sample> samples;
    int tried = 0, noLabels = 0;
    for (const Truth& t : truths) {
        const cv::Mat frame = cv::imread(t.file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        tried++;
        SCMiner::ScanPanel shape;
        bool turret = false;
        for (int look = 0; look < 4 && !turret; look++) {
            shape = SCMiner::ScanPanel();
            SCMiner::readPanelByShape(frame, shape);
            turret = shape.labels.x >= 0 && shape.ink == wantInk && shape.labelGlyph == wantGlyph;
        }
        if (!turret) {
            noLabels++;
            continue;
        }
        const double s = shape.labelScale;
        const cv::Rect region = cv::Rect((int)std::lround(shape.labels.x - 60 * s), (int)std::lround(shape.labels.y - 80 * s),
                                         (int)std::lround(330 * s), (int)std::lround(260 * s)) &
                                cv::Rect(0, 0, frame.cols, frame.rows);
        Sample sample{t.file, shape.labels, s, {}};
        bool found[5] = {false, false, false, false, false};
        for (const auto& fl : SCMiner::faceLinesDetailed(frame, region)) {
            for (const auto& tok : fl.tokens) {
                int row = -1;
                for (int r = 0; r < 3; r++) {
                    if (!found[r] && tok.text == t.text[r]) row = r;
                }
                if (row < 0 && !found[3] && (tok.text == "COMP." || tok.text == "COMP")) row = 3;
                if (row < 0 && !found[4] && tok.text == "SCU") row = 4;
                if (row < 0) continue;
                if (row <= 2 && tok.boxes.size() != tok.text.size()) continue;
                if (row <= 2) {
                    for (size_t g = 0; g < tok.text.size(); g++) sample.glyphs.push_back({tok.text[g], row, tok.boxes[g]});
                } else {
                    sample.glyphs.push_back({row == 3 ? 'C' : 'U', row, tok.box});
                }
                found[row] = true;
            }
        }
        if (found[0] && found[1] && found[2]) samples.push_back(sample);
    }
    std::cout << tried << " frames, " << noLabels << " without turret labels, " << samples.size()
              << " with all three figures found" << std::endl;
    if (samples.empty()) return 1;

    // the places, in label units from the labels' left edge and the MASS label's top
    auto median = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::vector<double> digitH, digitW, advance, stopGap, pctDx, pctW, pctH, stopBase, firstCentre;
    std::vector<double> valueDx[3], bottom[3], compCx, compCy, compW, compH, scuCx, scuCy, scuW, scuH;
    for (const Sample& sm : samples) {
        const double s = sm.scale;
        auto ux = [&](double px) { return (px - sm.labels.x) / s; };
        auto uy = [&](double py) { return (py - sm.labels.y) / s; };
        for (int r = 0; r < 3; r++) {
            std::vector<const Glyph*> row;
            for (const Glyph& g : sm.glyphs) {
                if (g.row == r) row.push_back(&g);
            }
            if (row.empty()) continue;
            std::vector<double> bottoms;
            for (const Glyph* g : row) {
                if (g->c >= '0' && g->c <= '9') {
                    digitH.push_back(g->box.height / s);
                    digitW.push_back(g->box.width / s);
                    bottoms.push_back(uy(g->box.y + g->box.height));
                }
            }
            const double base = median(bottoms);
            bottom[r].push_back(base);
            valueDx[r].push_back(ux(row[0]->box.x));
            firstCentre.push_back(ux(row[0]->box.x + row[0]->box.width / 2.0) - ux(row[0]->box.x));
            auto centre = [&](size_t k) { return ux(row[k]->box.x + row[k]->box.width / 2.0); };
            for (size_t k = 1; k < row.size(); k++) {
                const char c = row[k]->c, p = row[k - 1]->c;
                if (c >= '0' && c <= '9' && p >= '0' && p <= '9') advance.push_back(centre(k) - centre(k - 1));
                if (c == '%') {
                    pctDx.push_back(centre(k) - centre(k - 1));
                    pctW.push_back(row[k]->box.width / s);
                    pctH.push_back(row[k]->box.height / s);
                }
                if (p == '.' && k >= 2) {
                    stopGap.push_back(centre(k) - centre(k - 2));
                    stopBase.push_back(uy(row[k - 1]->box.y + row[k - 1]->box.height) - base);
                }
            }
        }
        for (const Glyph& g : sm.glyphs) {
            if (g.row == 3) {
                compCx.push_back(ux(g.box.x + g.box.width / 2.0));
                compCy.push_back(uy(g.box.y + g.box.height / 2.0));
                compW.push_back(g.box.width / s);
                compH.push_back(g.box.height / s);
            }
            if (g.row == 4) {
                scuCx.push_back(ux(g.box.x + g.box.width / 2.0));
                scuCy.push_back(uy(g.box.y + g.box.height / 2.0));
                scuW.push_back(g.box.width / s);
                scuH.push_back(g.box.height / s);
            }
        }
    }
    std::ofstream geo(outDir + "/geometry.txt", std::ios::trunc);
    auto put = [&](const std::string& name, double v, size_t n) {
        std::cout << name << " " << v << "  (" << n << ")" << std::endl;
        geo << name << " " << v << "  (" << n << ")" << std::endl;
    };
    put("digit_height", median(digitH), digitH.size());
    put("digit_width", median(digitW), digitW.size());
    put("digit_advance", median(advance), advance.size());
    put("first_centre", median(firstCentre), firstCentre.size());
    put("stop_gap", median(stopGap), stopGap.size());
    put("stop_base", median(stopBase), stopBase.size());
    put("pct_dx", median(pctDx), pctDx.size());
    put("pct_width", median(pctW), pctW.size());
    put("pct_height", median(pctH), pctH.size());
    for (int r = 0; r < 3; r++) {
        put("value_dx_" + std::to_string(r), median(valueDx[r]), valueDx[r].size());
        put("bottom_" + std::to_string(r), median(bottom[r]), bottom[r].size());
    }
    put("comp_cx", median(compCx), compCx.size());
    put("comp_cy", median(compCy), compCy.size());
    put("comp_w", median(compW), compW.size());
    put("comp_h", median(compH), compH.size());
    put("scu_cx", median(scuCx), scuCx.size());
    put("scu_cy", median(scuCy), scuCy.size());
    put("scu_w", median(scuW), scuW.size());
    put("scu_h", median(scuH), scuH.size());

    // the shapes: a cell about each glyph, cut at the label size, in its row's ink
    const double Hd = median(digitH), Wd = median(digitW);
    constexpr int ABOVE = 2, BELOW = 2;
    const int cellH = (int)std::lround(Hd) + ABOVE + BELOW;
    const int cellW = (int)std::lround(Wd) + 2;
    const int pctCellW = (int)std::lround(median(pctW)) + 2;
    auto inkOf = [](const cv::Mat& bgr, int which) {
        std::vector<cv::Mat> planes;
        cv::split(bgr, planes);
        cv::Mat out;
        if (which == 0) out = cv::min(cv::min(planes[0], planes[1]), planes[2]);
        else if (which == 1) out = cv::max(cv::max(planes[0], planes[1]), planes[2]);
        else if (which == 2) out = planes[1];
        else cv::subtract(planes[2], planes[0], out);
        cv::Mat f;
        out.convertTo(f, CV_32F);
        return f;
    };
    auto cut = [](const cv::Mat& grey, const cv::Point2d& at, double scale, int w, int h) {
        const cv::Matx23d map(scale, 0, at.x + 0.5 * scale - 0.5, 0, scale, at.y + 0.5 * scale - 0.5);
        cv::Mat out;
        cv::warpAffine(grey, out, map, cv::Size(w, h), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_REPLICATE);
        return out;
    };
    std::map<char, cv::Mat> sums;
    std::map<char, int> counts;
    for (const Sample& sm : samples) {
        const cv::Mat frame = cv::imread(sm.file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        const double s = sm.scale;
        const cv::Mat inks[4] = {inkOf(frame, 0), inkOf(frame, 2), inkOf(frame, 1), inkOf(frame, 4)};
        for (int r = 0; r < 3; r++) {
            std::vector<double> bottoms;
            for (const Glyph& g : sm.glyphs) {
                if (g.row == r && g.c >= '0' && g.c <= '9') bottoms.push_back(g.box.y + g.box.height);
            }
            const double base = median(bottoms);
            for (const Glyph& g : sm.glyphs) {
                if (g.row != r || g.c == '.') continue;
                const int w = g.c == '%' ? pctCellW : cellW;
                const double cx = g.box.x + g.box.width / 2.0;
                const cv::Point2d at(cx - w * s / 2.0, base - (Hd + ABOVE) * s);
                cv::Mat patch = cut(inks[r], at, s, w, cellH);
                cv::Scalar mean, dev;
                cv::meanStdDev(patch, mean, dev);
                if (dev[0] < 1e-3) continue;
                patch = (patch - mean[0]) / dev[0];
                if (sums[g.c].empty()) sums[g.c] = cv::Mat::zeros(patch.size(), CV_32F);
                sums[g.c] += patch;
                counts[g.c]++;
            }
        }
        // COMP. in the labels' orange, SCU in white: cut as a cell about the word
        for (const Glyph& g : sm.glyphs) {
            if (g.row < 3) continue;
            const int w = (int)std::lround(g.box.width / s) + 4, h = (int)std::lround(g.box.height / s) + 4;
            const cv::Point2d at(g.box.x + g.box.width / 2.0 - w * s / 2.0, g.box.y + g.box.height / 2.0 - h * s / 2.0);
            if (g.row == 3 && !compCx.empty() && std::abs(g.box.width / s - median(compW)) > 3) continue;
            if (g.row == 4 && !scuCx.empty() && std::abs(g.box.width / s - median(scuW)) > 3) continue;
            const int wantW = (int)std::lround(g.row == 3 ? median(compW) : median(scuW)) + 4;
            const int wantH = (int)std::lround(g.row == 3 ? median(compH) : median(scuH)) + 4;
            const cv::Point2d at2(g.box.x + g.box.width / 2.0 - wantW * s / 2.0, g.box.y + g.box.height / 2.0 - wantH * s / 2.0);
            cv::Mat patch = cut(g.row == 3 ? inks[3] : inks[0], at2, s, wantW, wantH);
            (void)at;
            (void)w;
            (void)h;
            cv::Scalar mean, dev;
            cv::meanStdDev(patch, mean, dev);
            if (dev[0] < 1e-3) continue;
            patch = (patch - mean[0]) / dev[0];
            char key = g.row == 3 ? 'C' : 'U';
            if (sums[key].empty()) sums[key] = cv::Mat::zeros(patch.size(), CV_32F);
            if (sums[key].size() != patch.size()) continue;
            sums[key] += patch;
            counts[key]++;
        }
    }
    std::ofstream inc(outDir + "/turret_shapes.inc", std::ios::trunc);
    inc << "// the MOLE turret's font, harvested by --turretharvest off " << samples.size() << " frames" << std::endl;
    for (auto& kv : sums) {
        cv::Mat m = kv.second / counts[kv.first];
        double lo, hi;
        cv::minMaxLoc(m, &lo, &hi);
        m = (m - lo) * (230.0 / (std::max)(1e-6, hi - lo)) + 12.0;
        const std::string name = kv.first == '%' ? "PCT" : kv.first == 'C' ? "COMP" : kv.first == 'U' ? "SCU"
                                                                                                   : std::string(1, kv.first);
        inc << "inline const float SHAPE_TURRET_" << name << "[] = {";
        for (int i = 0; i < m.rows * m.cols; i++) {
            char num[16];
            std::snprintf(num, sizeof(num), "%.1ff", m.at<float>(i / m.cols, i % m.cols));
            inc << (i ? "," : "") << num;
        }
        inc << "};  // " << m.cols << " x " << m.rows << ", " << counts[kv.first] << " glyphs" << std::endl;
        cv::Mat view;
        m.convertTo(view, CV_8U);
        cv::resize(view, view, cv::Size(), 8, 8, cv::INTER_NEAREST);
        cv::imwrite(outDir + "/glyph_" + name + ".png", view);
        std::cout << "shape " << name << " " << m.cols << "x" << m.rows << " from " << counts[kv.first] << std::endl;
    }
    return 0;
}

// --grabtiming <w> <h> <times>: what one capture of a w x h piece of the
// game's viewport (the screen's middle without the game) costs - the glance
// captures its own small piece on every scan
int runGrabTiming(int w, int h, int times) {
    SCMiner::ScreenCapture capture;
    cv::Rect area(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                  GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (auto viewport = SCMiner::ScreenCapture::gameViewport()) area = *viewport;
    SCMiner::CaptureRegion region;
    region.x = area.x + (area.width - w) / 2;
    region.y = area.y + (area.height - h) / 2;
    region.width = w;
    region.height = h;
    region.autoDetect = false;
    cv::Mat frame = capture.captureRegion(region);   // the first builds the duplication
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < times; i++) frame = capture.captureRegion(region);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / times;
    std::cout << w << "x" << h << ": " << ms << " ms a grab, " << (frame.empty() ? "nothing came back" : "ok")
              << std::endl;
    return 0;
}

// --paneltiming <frame> <times>: how long a look costs, read in full and by
// the shapes alone, on one frame looked at again and again (as the app does)
int runPanelTiming(const std::string& file, int times) {
    const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
    if (frame.empty()) return 1;
    SCMiner::OCREngine ocr;
    SCMiner::OCRConfig cfg;
    if (!ocr.initialize(cfg)) return 1;
    SCMiner::GlyphReader glyphs;
    SCMiner::throttlePanelSearch(true);
    auto clock = [](const std::function<void()>& work, int n) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) work();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
    };
    // the first look searches every size; the ones after know where to look
    SCMiner::ScanPanel first;
    const double firstMs = clock([&] { first = SCMiner::readScanPanel(frame, ocr, &glyphs, panelLines()); }, 1);
    std::cout << "first look " << firstMs << " ms, found " << first.found << std::endl;
    // the glance, where the first look found the panel
    if (const SCMiner::PanelSpot spot = SCMiner::lastPanelSpot(); spot.known) {
        const cv::Rect window = SCMiner::panelSpotWindow(spot, frame.size());
        double score = 0;
        const double glanceMs = clock([&] { score = SCMiner::glanceAtPanelSpot(spot, frame(window)); }, times);
        std::cout << "glance " << glanceMs << " ms, score " << score << " against " << SCMiner::glanceBar(spot)
                  << ", window " << window.width << "x" << window.height << std::endl;
    }
    SCMiner::ScanPanel p;
    SCMiner::reportPanelStages(std::cout, 1);
    std::cout << "shapes alone " << clock([&] { SCMiner::readPanelByShape(frame, p); }, times) << " ms" << std::endl;
    SCMiner::reportPanelStages(std::cout, times);
    std::cout << "whole look " << clock([&] { p = SCMiner::readScanPanel(frame, ocr, &glyphs, panelLines()); }, times) << " ms" << std::endl;
    SCMiner::reportPanelStages(std::cout, times);
    cv::Mat none = frame.clone();
    none.setTo(cv::Scalar(20, 20, 20));
    std::cout << "no panel on screen " << clock([&] { p = SCMiner::readScanPanel(none, ocr, &glyphs, panelLines()); }, times) << " ms" << std::endl;
    return 0;
}

// --shapeprobe <pictures...>: the shape reader on single pictures, each
// looked at fresh, with what it found and read
int runShapeProbe(const std::vector<std::string>& files) {
    for (const std::string& file : files) {
        const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) {
            std::cout << file << ": cannot read" << std::endl;
            continue;
        }
        cv::Point labels;
        double scale = 0, score = 0;
        SCMiner::findPanelLabels(frame, labels, scale, score);
        SCMiner::ScanPanel panel;
        const bool sure = SCMiner::readPanelByShape(frame, panel);
        const SCMiner::RockStats& r = panel.stats;
        std::cout << file << " (" << frame.cols << "x" << frame.rows << "): labels " << score << " at " << labels
                  << " x" << scale << std::endl;
        std::cout << "   found " << panel.found << " sure " << sure << " mass " << r.mass << " res " << r.resistance
                  << (r.resistanceAdjusted ? " colour" : " white") << " inst " << r.instability << " scu " << r.scu
                  << std::endl;
        std::cout << "   " << panel.note << std::endl;
    }
    return 0;
}

// --cutlong <frame> <x> <y> <font size> <out.yml>: the label column of a
// panel that writes its labels out - MASS:, RESISTANCE:, INSTABILITY: - cut
// from one picture and brought to the size the other shapes were cut at.
// x and y are the labels' left edge and the MASS label's top in the picture,
// to a fraction of a pixel; the font size is the picture's against 1440 lines.
int runCutLong(const std::string& file, double x, double y, double size, const std::string& out) {
    const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
    if (frame.empty()) return 1;
    std::vector<cv::Mat> planes;
    cv::split(frame, planes);
    cv::Mat darkest;
    cv::Mat(cv::min(cv::min(planes[0], planes[1]), planes[2])).convertTo(darkest, CV_32F);
    // at size 1: from 3 before the labels' edge and 3 above the MASS label's
    // top, 118 wide (the colon of INSTABILITY: ends at 115) and 73 tall
    constexpr int W = 118, H = 73;
    const double left = x - 3 * size, top = y - 3 * size;
    const cv::Matx23d map(size, 0, left + 0.5 * size - 0.5, 0, size, top + 0.5 * size - 0.5);
    cv::Mat cut;
    cv::warpAffine(darkest, cut, map, cv::Size(W, H), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_REPLICATE);
    cv::FileStorage fs(out, cv::FileStorage::WRITE);
    fs << "mean" << cut << "count" << 1;
    cv::Mat shown, big;
    cv::normalize(cut, shown, 0, 255, cv::NORM_MINMAX);
    shown.convertTo(shown, CV_8U);
    cv::resize(shown, big, cv::Size(), 6, 6, cv::INTER_NEAREST);
    cv::imwrite(out + "_x6.png", big);
    std::cout << "cut " << W << "x" << H << " into " << out << std::endl;
    return 0;
}

// --firing <frames dir> <prefix> <out.csv> <max.png> [dump dir] [angle]: for
// measuring what breaks a rock. Every frame of a recording whose name starts
// with <prefix>: the rock off the panel, the throttle off "NN% LASER IN", how
// far the charge bar is filled and how far out the beam reaches, off the LASER
// RANGE gauge left of the crosshair: a white pointer rides its arc with the
// beam's length in whole metres in small figures beside it - green, yellow,
// orange or red with the band the beam is in - and the yellow MAX label
// stands still. The pointer's place against that label is
// the length too, finer than whole metres. The mining HUD is not fixed on the
// screen - it sits nearer or further with the camera - so every cut is placed
// from where the panel's labels were found and how big, against a reference
// frame where the labels stood at 3086,695 at size 1. <max.png> is the MAX
// label cut from that frame; a dump dir gets each frame's levelled figures
// for checking by eye. Where the bar holds still at a throttle, the rock gives
// back what the laser puts in.
namespace {
const cv::Point FIRING_REF_LABELS(3086, 695);

// The cut of a place given at size 1 against the labels, brought to size 1.
cv::Mat firingCut(const cv::Mat& frame, cv::Point labels, double scale, cv::Rect ref) {
    const cv::Rect src(
        (int)std::lround(labels.x + (ref.x - FIRING_REF_LABELS.x) * scale),
        (int)std::lround(labels.y + (ref.y - FIRING_REF_LABELS.y) * scale),
        (int)std::lround(ref.width * scale), (int)std::lround(ref.height * scale));
    if ((src & cv::Rect(0, 0, frame.cols, frame.rows)) != src) return cv::Mat();
    cv::Mat out;
    cv::resize(frame(src), out, ref.size(), 0, 0, scale > 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    return out;
}

cv::Mat yellowness(const cv::Mat& bgr) {
    cv::Mat out(bgr.size(), CV_8U);
    for (int y = 0; y < bgr.rows; y++) {
        const cv::Vec3b* px = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; x++) out.at<uchar>(y, x) = (uchar)std::max(0, std::min(px[x][1], px[x][2]) - px[x][0]);
    }
    return out;
}
}  // namespace

int runFiring(const std::string& dir, const std::string& prefix, const std::string& outCsv,
              const std::string& maxPng, const std::string& dump, double angle) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".jpg") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    const cv::Mat maxLabel = cv::imread(maxPng, cv::IMREAD_COLOR);
    if (maxLabel.empty()) {
        std::cout << "cannot read " << maxPng << std::endl;
        return 1;
    }
    const cv::Mat maxTemplate = yellowness(maxLabel);
    SCMiner::OCREngine ocr;
    SCMiner::OCRConfig cfg;
    if (!ocr.initialize(cfg)) return 1;
    // the whole range of sizes only every fourth frame, as the app looks
    SCMiner::throttlePanelSearch(true);
    if (!dump.empty()) std::filesystem::create_directories(dump, ec);
    std::ofstream out(outCsv, std::ios::trunc);
    out << "file,found,mass,res,res_printed,inst,labels_x,labels_y,scale,anchor_age,throttle,charge_top,charge_deg,"
           "throttle_top,throttle_deg,charge_px,max_score,max_x,max_y,pointer_dx,pointer_dy,pointer_deg,pointer_area,colour_px,digits,others"
        << std::endl;
    // the places at size 1 against the labels, all off the reference frame
    const cv::Rect THROTTLE(2945, 840, 110, 38);
    const cv::Rect CHARGE(2780, 560, 230, 480);
    const cv::Rect GAUGE(2130, 460, 270, 230);
    cv::Point labels(-1, -1);
    double scale = 0;
    int anchorAge = 1000;
    for (const std::string& file : files) {
        const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        SCMiner::ScanPanel panel;
        SCMiner::readPanelByShape(frame, panel);
        if (panel.found && panel.labelScale > 0) {
            labels = panel.labels;
            scale = panel.labelScale;
            anchorAge = 0;
        } else {
            anchorAge++;
        }
        const SCMiner::RockStats& r = panel.stats;
        out << std::filesystem::path(file).filename().string() << "," << (panel.found ? 1 : 0) << "," << r.mass
            << "," << r.resistance << "," << (r.resistanceAdjusted ? "colour" : "white") << "," << r.instability
            << "," << labels.x << "," << labels.y << "," << scale << "," << anchorAge;
        if (scale <= 0 || anchorAge > 40) {
            out << ",-1,-1,0,-1,0,0,0,-1,-1,0,0,0,0,0,,0" << std::endl;
            continue;
        }

        // the throttle: white digits, right-aligned against the per cent sign
        int throttle = -1;
        const cv::Mat thr = firingCut(frame, labels, scale, THROTTLE);
        if (!thr.empty()) {
            std::vector<cv::Mat> planes;
            cv::split(thr, planes);
            cv::Mat low = cv::min(cv::min(planes[0], planes[1]), planes[2]), big, bin;
            cv::resize(low, big, cv::Size(), 3, 3, cv::INTER_CUBIC);
            cv::threshold(big, bin, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
            const std::string text = ocr.recognizeLine(bin, "0123456789%");
            const size_t pct = text.find('%');
            if (pct != std::string::npos && pct > 0) {
                size_t start = pct;
                while (start > 0 && std::isdigit((unsigned char)text[start - 1])) start--;
                if (start < pct) throttle = std::atoi(text.substr(start, pct - start).c_str());
                if (throttle > 100) throttle = -1;
            }
        }

        // The CHARGE LEVEL arc carries two green bars up from its foot, both
        // riding one circle: the throttle bar outside - as long as the
        // throttle is set - and the charge fill some 20 px inside it. Told
        // apart by their radius from the circle's centre, fitted on the
        // throttle bar at full throttle: centre -157.9,197.0 and radius 344.0
        // in this 230 x 480 cut, to a third of a pixel. Each bar's top is the
        // highest row of the run up from the foot, in rows and as the angle
        // round the centre - the foot lies at about 45 degrees below the
        // horizontal, the top of the arc above it.
        const double ARC_CX = -157.9, ARC_CY = 197.0, ARC_R = 344.0;
        int top = -1, throttleTop = -1, pixels = 0;
        double chargeDeg = 0, throttleDeg = 0;
        const cv::Mat arc = firingCut(frame, labels, scale, CHARGE);
        if (!arc.empty()) {
            std::vector<int> inner(arc.rows, 0), outer(arc.rows, 0);
            for (int y = 0; y < arc.rows; y++) {
                const cv::Vec3b* px = arc.ptr<cv::Vec3b>(y);
                for (int x = 0; x < arc.cols; x++) {
                    // Green over bright rock washes out: the thin throttle bar
                    // comes to 190,250,120 there, green only 55 above red, and
                    // its dashed foot to 25 above. Inside the narrow bands of
                    // the two bars nothing else is green at all.
                    const int b = px[x][0], g = px[x][1], r2 = px[x][2];
                    if (!(g >= 120 && g - (std::max)(r2, b) >= 20)) continue;
                    // the HUD leans a little with the camera, so the bars
                    // stray a few pixels off the fitted circle
                    const double d = std::hypot(x - ARC_CX, y - ARC_CY) - ARC_R;
                    if (std::abs(d) <= 8.0) outer[y]++;
                    else if (d >= -30.0 && d <= -11.0) inner[y]++;
                }
                pixels += inner[y];
            }
            // A bar grows from the foot, at row 441 of the cut: green that
            // starts higher up - the OPTIMAL window's box sits at about the
            // charge bar's radius - is no bar, and an empty bar is none.
            const auto topOf = [&](const std::vector<int>& rows) {
                int y = (int)rows.size() - 1;
                while (y >= 0 && rows[y] == 0) y--;           // up to the foot
                if (y < 415) return -1;
                int found = -1, gap = 0;
                for (; y >= 0; y--) {
                    if (rows[y] > 0) {
                        found = y;
                        gap = 0;
                    } else if (++gap > 8) {
                        break;
                    }
                }
                return found;
            };
            const auto degreesAt = [&](int row) {
                const double dy = row - ARC_CY;
                return std::atan2(dy, std::sqrt(std::max(0.0, ARC_R * ARC_R - dy * dy))) * 180.0 / 3.14159265358979;
            };
            const int chargeRow = topOf(inner), throttleRow = topOf(outer);
            if (chargeRow >= 0) {
                top = CHARGE.y + chargeRow;
                chargeDeg = degreesAt(chargeRow);
            }
            if (throttleRow >= 0) {
                throttleTop = CHARGE.y + throttleRow;
                throttleDeg = degreesAt(throttleRow);
            }
        }

        // the gauge: the MAX label, then the pointer against it
        double maxScore = 0, maxX = -1, maxY = -1, pdx = 0, pdy = 0, pdeg = 0;
        int pointerArea = 0, greenBeside = 0, others = 0;
        std::string digits;
        const cv::Mat gauge = firingCut(frame, labels, scale, GAUGE);
        if (!gauge.empty()) {
            // MAX stands at 2180,593 in the reference frame - 50,133 in this
            // cut - and the anchor is good to a few pixels, so it is looked
            // for only thereabouts: OPTIMAL above it is yellow lettering too
            const cv::Rect window = cv::Rect(28 - 30, 120 - 30, maxTemplate.cols + 60, maxTemplate.rows + 60) & cv::Rect(0, 0, gauge.cols, gauge.rows);
            cv::Mat match;
            cv::matchTemplate(yellowness(gauge(window)), maxTemplate, match, cv::TM_CCOEFF_NORMED);
            cv::Point where;
            cv::minMaxLoc(match, nullptr, &maxScore, nullptr, &where);
            maxX = window.x + where.x + maxTemplate.cols / 2.0;
            maxY = window.y + where.y + maxTemplate.rows / 2.0;
            // the figures take the band's colour, so colour itself marks them
            cv::Mat white(gauge.size(), CV_8U, cv::Scalar(0)), coloured(gauge.size(), CV_8U, cv::Scalar(0)),
                chroma(gauge.size(), CV_8U, cv::Scalar(0));
            for (int y = 0; y < gauge.rows; y++) {
                const cv::Vec3b* px = gauge.ptr<cv::Vec3b>(y);
                for (int x = 0; x < gauge.cols; x++) {
                    const int b = px[x][0], g = px[x][1], r2 = px[x][2];
                    const int lo = std::min(b, std::min(g, r2)), hi = std::max(b, std::max(g, r2));
                    if (lo >= 190 && hi - lo <= 50) white.at<uchar>(y, x) = 255;
                    if (hi >= 140 && hi - lo >= 70) coloured.at<uchar>(y, x) = 255;
                    chroma.at<uchar>(y, x) = (uchar)(hi - lo);
                }
            }
            // The pointer: a filled white triangle with coloured figures to
            // its right, riding a circle of radius 143.2 round a centre 190.3
            // right of and 65.6 below the MAX label - fitted through fifteen
            // readings between 8 and 40 m, to a pixel. Anything white off that
            // circle - the THRUST figures, a rock's label - is not it.
            const double POINTER_CX = 190.3, POINTER_CY = 65.6, POINTER_R = 143.2;
            cv::Mat cc, stats, centroids;
            const int n = cv::connectedComponentsWithStats(white, cc, stats, centroids, 8);
            int best = -1, bestGreen = -1;
            cv::Rect bestBox;
            for (int i = 1; i < n; i++) {
                const int w = stats.at<int>(i, cv::CC_STAT_WIDTH), h = stats.at<int>(i, cv::CC_STAT_HEIGHT);
                const int area = stats.at<int>(i, cv::CC_STAT_AREA);
                if (area < 18 || area > 240 || w < 6 || w > 24 || h < 5 || h > 20) continue;
                const double fill = (double)area / (w * h);
                if (fill < 0.30 || fill > 0.85) continue;
                const double dx = centroids.at<double>(i, 0) - maxX, dy = centroids.at<double>(i, 1) - maxY;
                if (dx < 30 || dx > 130 || dy < -100 || dy > 50) continue;
                if (std::abs(std::hypot(dx - POINTER_CX, dy - POINTER_CY) - POINTER_R) > 6.0) continue;
                // ...and between the ends of its scale, 0 m at -130 and 45 m at -165 degrees
                const double deg = std::atan2(dy - POINTER_CY, dx - POINTER_CX) * 180.0 / 3.14159265358979;
                if (deg > -128.0 || deg < -167.0) continue;
                const cv::Rect box(stats.at<int>(i, cv::CC_STAT_LEFT), stats.at<int>(i, cv::CC_STAT_TOP), w, h);
                const cv::Rect beside = cv::Rect(box.x + box.width + 2, box.y - 10, 44, box.height + 20) & cv::Rect(0, 0, gauge.cols, gauge.rows);
                const int greenHere = beside.area() > 0 ? cv::countNonZero(coloured(beside)) : 0;
                others++;
                if (greenHere > bestGreen) {
                    bestGreen = greenHere;
                    best = i;
                    bestBox = box;
                }
            }
            if (best > 0) {
                pdx = centroids.at<double>(best, 0) - maxX;
                pdy = centroids.at<double>(best, 1) - maxY;
                pdeg = std::atan2(pdy - POINTER_CY, pdx - POINTER_CX) * 180.0 / 3.14159265358979;
                pointerArea = stats.at<int>(best, cv::CC_STAT_AREA);
                greenBeside = bestGreen;
                // the figures: turned level, blown up, read
                const cv::Rect cut = cv::Rect(bestBox.x + bestBox.width - 2, bestBox.y - 14, 56, bestBox.height + 30) & cv::Rect(0, 0, gauge.cols, gauge.rows);
                cv::Mat level, big, bin;
                const cv::Point2f centre(cut.width / 2.0f, cut.height / 2.0f);
                cv::warpAffine(chroma(cut), level, cv::getRotationMatrix2D(centre, angle, 1.0), cut.size(), cv::INTER_CUBIC, cv::BORDER_CONSTANT, cv::Scalar(0));
                cv::resize(level, big, cv::Size(), 4, 4, cv::INTER_CUBIC);
                cv::threshold(big, bin, 60, 255, cv::THRESH_BINARY_INV);
                cv::copyMakeBorder(bin, bin, 12, 12, 12, 12, cv::BORDER_CONSTANT, cv::Scalar(255));
                digits = ocr.recognizeLine(bin, "0123456789");
                digits.erase(std::remove_if(digits.begin(), digits.end(), [](unsigned char c) { return !std::isdigit(c); }), digits.end());
                if (!dump.empty()) {
                    const std::string stem = std::filesystem::path(file).stem().string();
                    cv::imwrite(dump + "/" + stem + "_lvl.png", bin);
                    cv::Mat raw;
                    cv::resize(gauge(cut), raw, cv::Size(), 4, 4, cv::INTER_NEAREST);
                    cv::imwrite(dump + "/" + stem + "_raw.png", raw);
                }
            }
        }
        out << "," << throttle << "," << top << "," << chargeDeg << "," << throttleTop << "," << throttleDeg << "," << pixels
            << "," << maxScore << "," << maxX << "," << maxY << ","
            << pdx << "," << pdy << "," << pdeg << "," << pointerArea << "," << greenBeside << "," << digits << "," << others << std::endl;
    }
    return 0;
}

int runScanPanel(const std::vector<std::string>& files) {
    std::cout << "=== SCAN RESULTS panel ===" << std::endl;
    SCMiner::GlyphReader glyphs;
    glyphs.loadTemplates("./glyphs");
    glyphs.loadMfdTemplates("./glyphs");
    std::cout << "glyph fonts: " << glyphs.templateCount() << " canopy, "
              << glyphs.templateCount(true) << " panel" << std::endl;
    SCMiner::OCREngine ocr;
    SCMiner::OCRConfig cfg;
    const bool ocrReady = ocr.initialize(cfg);
    if (!ocrReady) std::cout << "(no tessdata - the shapes read alone, as on a fresh install)" << std::endl;
    int found = 0;
    for (const std::string& file : files) {
        cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) {
            std::cout << file << ": cannot read" << std::endl;
            continue;
        }
        // read either way: an engine that did not start answers with nothing,
        // and the shapes do not ask it
        const SCMiner::ScanPanel panel = SCMiner::readScanPanel(frame, ocr, &glyphs, panelLines());
        std::cout << "\n" << file << "  " << frame.cols << "x" << frame.rows << std::endl;
        std::cout << "  " << panel.note << std::endl;
        if (panel.found) {
            found++;
            std::cout << "  box " << panel.box.x << "," << panel.box.y << "  "
                      << panel.box.width << "x" << panel.box.height
                      << "   of the viewport: x " << (double)panel.box.x / frame.cols
                      << "  y " << (double)panel.box.y / frame.rows
                      << "  w " << (double)panel.box.width / frame.cols
                      << "  h " << (double)panel.box.height / frame.rows << std::endl;
            std::cout << "  read: mass " << panel.stats.mass << "  res " << panel.stats.resistance
                      << (panel.stats.resistanceAdjusted ? " % in colour (the laser is in it)"
                                                         : " % white (the rock's own)")
                      << "  inst " << panel.stats.instability << "  scu " << panel.stats.scu
                      << "  ore " << panel.stats.ore << std::endl;
        }
        const size_t dot = file.find_last_of('.');
        const std::string out = (dot == std::string::npos ? file : file.substr(0, dot)) + "_panel.png";
        cv::imwrite(out, SCMiner::drawScanPanelSearch(frame, panel));
        std::cout << "  drawn to " << out << std::endl;
    }
    std::cout << "\n" << found << " of " << files.size() << " frames had a panel" << std::endl;
    return found == (int)files.size() ? 0 : 1;
}

// Draws the located labels onto a screenshot so the detector can be checked
// against a real full-screen capture (--locate shot.png).
int runLocateOnFiles(const std::vector<std::string>& files) {
    // The locator verifies its candidates with the pin templates, so they have
    // to be loaded here too - the app does it at startup
    SCMiner::GlyphReader reader;
    reader.loadTemplates("./glyphs");
    SCMiner::ScreenCapture capture;
    int failures = 0;

    for (const auto& file : files) {
        cv::Mat image = cv::imread(file, cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cout << file << " -> could not load image" << std::endl;
            failures++;
            continue;
        }

        auto labels = capture.locateSignatureLabels(image, 12);
        std::cout << file << " -> " << labels.size() << " label(s)";

        cv::Mat marked = image.clone();
        for (size_t i = 0; i < labels.size(); i++) {
            std::cout << (i == 0 ? ": " : ", ") << labels[i].x << "," << labels[i].y
                      << " " << labels[i].width << "x" << labels[i].height;
            cv::rectangle(marked, labels[i],
                          i == 0 ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 200, 255), 2);
        }
        std::cout << std::endl;

        if (labels.empty()) {
            failures++;
        } else {
            const std::string out = std::filesystem::path(file).stem().string() + "_located.png";
            cv::imwrite(out, marked);
            std::cout << "    marked image: " << out << std::endl;
        }
    }

    return failures == 0 ? 0 : 1;
}

// Runs the OCR pipeline on saved capture images (e.g. debug/region_0.png)
// and prints what the tool would read - for tuning against real screenshots.
int runOcrOnFiles(const std::vector<std::string>& files) {
    SCMiner::OCRConfig ocrConfig;
    ocrConfig.tessdataPath = "./tessdata";
    ocrConfig.pageSegMode = 8;
    ocrConfig.minConfidence = 20.0;

    SCMiner::OCREngine ocr;
    const bool ocrReady = ocr.initialize(ocrConfig);
    if (!ocrReady) std::cerr << "Tesseract not available (no tessdata) - OCR paths stay off." << std::endl;

    SCMiner::ScreenCapture capture;
    SCMiner::SignatureAnalyzer analyzer;
    SCMiner::GlyphReader reader;
    reader.loadTemplates("./glyphs");
    // The very policy the app uses, so measurements reflect real behaviour
    auto policy = SCMiner::makeReadingPolicy(analyzer);
    int failures = 0;

    for (const auto& file : files) {
        cv::Mat image = cv::imread(file, cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cout << file << " -> could not load image" << std::endl;
            failures++;
            continue;
        }

        // Same order as the app: template reader first, Tesseract as fallback
        std::optional<int> value;
        double confidence = 0.0;
        bool viaGlyphs = false;
        SCMiner::GlyphReader::Reading glyphReading;
        auto isSignature = [&analyzer](int v) { return analyzer.isSingleSignature(v); };
        auto isSum = [&analyzer](int v) { return analyzer.isKnownSignature(v); };
    const std::vector<int> known = analyzer.knownSignatures();
        if (auto glyphValue = reader.read(image, &glyphReading, isSignature, &known, isSum)) {
            std::string name;
            int cluster = 0;
            // A reading that needed a hesitant glyph resolved has to be
            // cleaner than one read outright: in sun glare the digits bloat
            // into lumps that fit several templates, and letting those be
            // repaired turns a blur into a confident wrong number.
            if (SCMiner::GlyphReader::goodEnough(glyphReading) &&
                analyzer.isKnownSignature(*glyphValue)) {
                value = *glyphValue;
                confidence = glyphReading.weakest;
                viaGlyphs = true;
            }
        }
        // Same rule as the app: no digit row with a pin, no OCR
        if (!value && glyphReading.markerLikely &&
            glyphReading.pinScore >= SCMiner::GlyphReader::OCR_MIN_PIN) {
            static const bool useOcr = DEV_ENV("SCMINER_OCR") != nullptr && ocrReady;   // see the pipeline
            auto candidates = useOcr ? capture.preprocessCandidates(image) : std::vector<cv::Mat>();
            value = useOcr ? ocr.recognizeBest(candidates, policy) : std::nullopt;
            confidence = ocr.getLastConfidence();
            // The very floor the app applies - see MIN_OCR_CONFIDENCE
            if (confidence < SCMiner::MIN_OCR_CONFIDENCE) value.reset();
            // ...and the same agreement rule: Tesseract confirms the cutter
            // or it says nothing
            if (value && *value != glyphReading.value) value.reset();
        }

        std::cout << file << " -> ";
        if (value) {
            std::cout << *value;
            std::cout << " (confidence " << (int)confidence << "%" << (viaGlyphs ? ", glyphs" : ", ocr") << ")";
            auto top = analyzer.getMostLikely(*value);
            if (top.probability > 0.0) {
                // A combination already names its own counts
                std::cout << "  => " << top.name;
                if (top.clusterSize > 0) std::cout << " x" << top.clusterSize;
                if (top.difference < 1.0) std::cout << " [EXACT]";
            }
        } else {
            std::cout << "no number recognized";
            failures++;
        }
        std::cout << std::endl;
    }

    return failures == 0 ? 0 : 1;
}

// Reads saved captures with the template reader alone and shows every glyph
// with its best match - for judging the reader without Tesseract in the mix.
int runGlyphOnFiles(const std::vector<std::string>& files) {
    SCMiner::GlyphReader reader;
    std::cout << "Loaded " << reader.loadTemplates("./glyphs") << " glyph templates, "
              << SCMiner::GlyphReader::pinTemplateCount() << " pin templates." << std::endl;
    SCMiner::SignatureAnalyzer analyzer;
    int failures = 0;

    for (const auto& file : files) {
        cv::Mat image = cv::imread(file, cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cout << file << " -> could not load image" << std::endl;
            failures++;
            continue;
        }

        // SCMINER_ROW_HINT=x,y,w,h reads the crop the way the pipeline reads
        // a locator candidate: told where the digit row is
        cv::Rect rowHint;
        if (const char* hintText = DEV_ENV("SCMINER_ROW_HINT")) {
            int hx = 0, hy = 0, hw = 0, hh = 0;
            if (sscanf_s(hintText, "%d,%d,%d,%d", &hx, &hy, &hw, &hh) == 4) rowHint = cv::Rect(hx, hy, hw, hh);
        }
        const cv::Rect* hint = rowHint.area() > 0 ? &rowHint : nullptr;
        SCMiner::GlyphReader::Trace trace;
        trace.wantPicture = true;
        const auto glyphs = SCMiner::GlyphReader::extractGlyphs(image, &trace, true,
                                                                SCMiner::GlyphReader::PIN_MIN_NCC, hint);
        std::cout << file << " -> plate " << trace.plate.width << "x" << trace.plate.height
                  << " luma " << static_cast<int>(trace.plateLuma) << " std " << static_cast<int>(trace.plateStd)
                  << " " << (trace.channel.empty() ? "-" : trace.channel) << " thr " << trace.threshold
                  << " pin " << static_cast<int>(trace.pinScore * 100) << "%"
                  << (trace.separator ? " sep+" : " sep-")
                  << ", " << glyphs.size() << " glyph(s)";
        if (!trace.picture.empty()) {
            std::filesystem::create_directories("debug");
            cv::imwrite("debug/glyph_" + std::filesystem::path(file).stem().string() + ".png",
                        trace.picture);
        }
        for (const auto& g : glyphs) {
            double similarity = 0.0;
            double margin = 0.0;
            const int digit = reader.classify(g, &similarity, &margin);
            std::cout << " [" << (digit < 0 ? '?' : static_cast<char>('0' + digit))
                      << " " << static_cast<int>(similarity * 100) << "%+" << static_cast<int>(margin * 100)
                      << " " << g.box.width << "x" << g.box.height << "]";
        }

        SCMiner::GlyphReader::Reading reading;
        auto value = reader.read(image, &reading, {}, nullptr, {}, hint);
        if (value) {
            std::cout << " => " << *value << " (weakest " << static_cast<int>(reading.weakest) << "%, mean "
                      << static_cast<int>(reading.mean) << "%, margin " << static_cast<int>(reading.minMargin)
                      << ", dc/pc " << static_cast<int>(reading.digitChroma) << "/"
                      << static_cast<int>(reading.pinChroma) << ", gap " << static_cast<int>(reading.commaGap * 100) << ")";
            std::string name;
            int cluster = 0;
            if (analyzer.isExactMatch(*value, name, cluster)) {
                std::cout << " " << name << " x" << cluster << " [EXACT]";
            }
        } else {
            std::cout << " => no reading";
            failures++;
        }
        std::cout << std::endl;
    }

    return failures == 0 ? 0 : 1;
}

// Cuts the digits out of captures whose value is known and stores them as
// templates (--harvest <value> <img...>). Only captures that segment into
// exactly the right number of glyphs are used, so a bad split can never
// poison the template set.

// The thousands comma stands after the first L-3 digits of an L-digit
// value, so a row of L glyphs whose widest gap sits exactly there carries
// the digits in the label's order: a pin fragment in front or a lost last
// digit - the two ways a row of the right count ends up one place off -
// both move that gap by one.
static bool commaGapFits(const std::vector<SCMiner::GlyphReader::Glyph>& glyphs, size_t digits) {
    if (glyphs.size() != digits || digits < 4) return false;
    std::vector<int> gaps;
    for (size_t i = 1; i < glyphs.size(); i++) {
        gaps.push_back(glyphs[i].box.x - glyphs[i - 1].box.br().x);
    }
    const size_t at = digits - 4;   // the gap after digit L-3, 0-based
    int widest = gaps.front();
    size_t where = 0;
    for (size_t i = 1; i < gaps.size(); i++) {
        if (gaps[i] > widest) { widest = gaps[i]; where = i; }
    }
    if (where != at) return false;
    // ...and clearly the widest: the comma adds its own width to that gap
    std::vector<int> others;
    for (size_t i = 0; i < gaps.size(); i++) {
        if (i != at) others.push_back(gaps[i]);
    }
    std::sort(others.begin(), others.end());
    const int typical = others[others.size() / 2];
    return widest >= typical + 2 && widest >= typical * 1.3;
}

int runHarvest(const std::string& digits, const std::vector<std::string>& files,
               const std::string& outDir) {
    // extractGlyphs only returns a row once it has matched the pin, so the
    // templates have to be loaded here as well
    SCMiner::GlyphReader reader;
    reader.loadTemplates("./glyphs");
    int saved = 0;
    int skipped = 0;

    SCMiner::ScreenCapture capture;
    for (const auto& file : files) {
        cv::Mat image = cv::imread(file, cv::IMREAD_COLOR);
        if (image.empty()) {
            skipped++;
            continue;
        }
        // A whole search area rather than a crop: the locator offers its
        // candidates, each is read with its row hint the way the live scan
        // reads it, and the one that reads as the value is the crop that is
        // harvested - so the templates come from exactly the pictures the
        // app will see, and a candidate that is not the marker cannot
        // teach it anything.
        cv::Rect frameHint;
        if (image.cols > 400) {
            // The app's own search area and its order - nearest the aim
            // first - when the snapshot's sidecar says what they were (see
            // sidecarGeometry); the whole picture otherwise
            const cv::Rect whole(0, 0, image.cols, image.rows);
            cv::Rect area = whole;
            cv::Point aim(image.cols / 2, image.rows / 2);
            if (const auto geometry = sidecarGeometry(file)) {
                const cv::Rect inside = geometry->area & whole;
                if (inside.width > 10 && inside.height > 6) {
                    area = inside;
                    aim = cv::Point(geometry->aim.x + geometry->aim.width / 2,
                                    geometry->aim.y + geometry->aim.height / 2);
                }
            }
            const cv::Point aimInArea(aim.x - area.x, aim.y - area.y);
            cv::Mat search = image(area);
            auto labels = capture.locateSignatureLabels(search, 12);
            std::stable_sort(labels.begin(), labels.end(), [&](const cv::Rect& a, const cv::Rect& b) {
                const cv::Point ca(a.x + a.width / 2, a.y + a.height / 2);
                const cv::Point cb(b.x + b.width / 2, b.y + b.height / 2);
                return cv::norm(ca - aimInArea) < cv::norm(cb - aimInArea);
            });
            cv::Mat crop;
            static const bool traceHarvest = DEV_ENV("SCMINER_HARVEST_TRACE") != nullptr;
            if (traceHarvest) {
                std::cerr << "[harvest] " << file << ": area " << area.x << "," << area.y << " " << area.width
                          << "x" << area.height << ", " << labels.size() << " candidate(s)" << std::endl;
            }
            // A candidate the set cannot read yet but shaped like the label
            // - as many glyphs, the comma gap where the label puts it - is
            // what a new face looks like before it is learnt. The nearest
            // such to the aim is kept for the case that none reads.
            cv::Mat byShape;
            cv::Rect shapeHint;
            for (const auto& label : labels) {
                cv::Rect rowHint;
                if (auto row = capture.rowOf(label)) {
                    rowHint = cv::Rect(row->x - label.x, row->y - label.y, row->width, row->height);
                }
                const cv::Rect* hintHere = rowHint.area() > 0 ? &rowHint : nullptr;
                SCMiner::GlyphReader::Reading probe;
                const auto value = reader.read(search(label), &probe, {}, nullptr, {}, hintHere);
                if (traceHarvest) {
                    std::cerr << "[harvest]   candidate " << label.x << "," << label.y << " " << label.width << "x"
                              << label.height << " read " << (value ? std::to_string(*value) : std::string("-"))
                              << ", " << probe.glyphs.size() << " glyph(s)" << std::endl;
                }
                if (value && std::to_string(*value) == digits) {
                    crop = search(label).clone();
                    frameHint = rowHint;
                    break;
                }
                if (byShape.empty()) {
                    auto shapes = probe.glyphs;
                    if (shapes.size() != digits.size()) {
                        shapes = SCMiner::GlyphReader::extractGlyphs(search(label), nullptr, true,
                                                                     SCMiner::GlyphReader::PIN_MIN_NCC, hintHere);
                        if (traceHarvest) std::cerr << "[harvest]     extract gave " << shapes.size() << std::endl;
                    }
                    if (traceHarvest && shapes.size() == digits.size()) {
                        std::cerr << "[harvest]     shape fits the label: " << (commaGapFits(shapes, digits.size()) ? "yes" : "no") << std::endl;
                    }
                    if (commaGapFits(shapes, digits.size())) {
                        byShape = search(label).clone();
                        shapeHint = rowHint;
                    }
                }
            }
            if (crop.empty() && !byShape.empty()) {
                std::cout << file << " -> no candidate reads " << digits
                          << ", taking the one shaped like it nearest the aim" << std::endl;
                crop = byShape;
                frameHint = shapeHint;
            }
            if (crop.empty()) {
                std::cout << file << " -> skipped (no candidate reads " << digits << ")" << std::endl;
                skipped++;
                continue;
            }
            image = crop;
        }
        const cv::Rect* hint = frameHint.area() > 0 ? &frameHint : nullptr;

        // The row the reader itself settles on, with the value ignored -
        // the label is the one given on the command line. Only when that
        // row does not fit the label is the first pinned row tried. On the
        // panel this harvests most markers that the pinned row alone skips
        // as "5 glyphs for 4 digits".
        const bool panel = outDir.size() >= 4 && outDir.compare(outDir.size() - 4, 4, "_mfd") == 0;
        static const bool trace = DEV_ENV("SCMINER_HARVEST_TRACE") != nullptr;
        if (trace) std::cerr << "[harvest] " << file << " read..." << std::endl;
        SCMiner::GlyphReader::Reading reading;
        reader.read(image, &reading, {}, nullptr, {}, hint);
        auto glyphs = reading.glyphs;
        std::vector<int> seen = reading.digits;
        if (trace) std::cerr << "[harvest]   read gave " << glyphs.size() << " glyphs, " << seen.size() << " digits" << std::endl;
        if (glyphs.size() != digits.size()) {
            glyphs = SCMiner::GlyphReader::extractGlyphs(image, nullptr, true, SCMiner::GlyphReader::PIN_MIN_NCC, hint);
            if (trace) std::cerr << "[harvest]   extract gave " << glyphs.size() << " glyphs" << std::endl;
            seen.clear();
            for (const auto& g : glyphs) {
                if (trace) std::cerr << "[harvest]   classify glyph " << g.box.x << "," << g.box.y << " bitmap " << g.bitmap.cols << "x" << g.bitmap.rows << std::endl;
                seen.push_back(reader.classify(g, nullptr, nullptr, nullptr, panel));
            }
        }
        if (glyphs.size() != digits.size()) {
            std::cout << file << " -> skipped (" << glyphs.size() << " glyphs for "
                      << digits.size() << " digits)" << std::endl;
            skipped++;
            continue;
        }
        // The label names the digits by position, so the shapes have to BE
        // the digits in that order. A row with a pin fragment in front and
        // its last digit lost still has the right count - and every label
        // is then one place off: a pin filed as 3, a 3 as 9, a 9 as 0, a 2
        // as 0, and a set that learnt it so reads 39,000 as 30,000. So the
        // reader, with the set it has, must agree with the label on most
        // positions; the positions it disagrees on are exactly the variants
        // worth learning. With no set loaded yet nothing can be checked and
        // the row is taken as it is - that is the bootstrap, and it wants
        // looking at.
        //
        // One disagreement, not two: with two allowed, the rings of the
        // radar screen beside a 12,900 pass as 1-2-9-0-0 on three agreeing
        // zeros-and-ones and teach the set two shapes that are not digits.
        std::string seenText;
        for (size_t i = 0; i < glyphs.size(); i++) {
            seenText += seen[i] < 0 ? '?' : static_cast<char>('0' + seen[i]);
        }
        if (reader.templateCount(panel) > 0) {
            int agree = 0;
            for (size_t i = 0; i < glyphs.size(); i++) {
                if (seen[i] == digits[i] - '0') agree++;
            }
            // With the comma gap where the label puts it the shapes ARE the
            // digits in order (commaGapFits), and the set may then disagree
            // on up to three of them - that is what a face it has not learnt
            // looks like (12,000 on the night HUD reading as 28,000).
            // Without that gap, one disagreement.
            const bool shaped = commaGapFits(glyphs, digits.size());
            const int need = shaped ? (std::max)(2, static_cast<int>(glyphs.size()) - 3)
                                    : (std::max)(3, static_cast<int>(glyphs.size()) - 1);
            if (agree < need) {
                std::cout << file << " -> skipped (reader saw " << seenText << " for " << digits
                          << ", " << agree << " of " << glyphs.size() << " agree)" << std::endl;
                skipped++;
                continue;
            }
        }
        std::cout << file << " -> " << digits << " (reader saw " << seenText << ")" << std::endl;

        const std::filesystem::path path(file);
        const std::string tag = path.parent_path().filename().string() + "_" + path.stem().string();
        for (size_t i = 0; i < glyphs.size(); i++) {
            if (trace) std::cerr << "[harvest]   save " << i << " bitmap " << glyphs[i].bitmap.cols << "x" << glyphs[i].bitmap.rows << std::endl;
            if (SCMiner::GlyphReader::saveTemplate(glyphs[i], digits[i] - '0', outDir,
                                                   tag + "_" + std::to_string(i))) {
                saved++;
            }
        }
    }

    std::cout << "Harvested " << saved << " glyph(s), skipped " << skipped << " file(s)." << std::endl;
    return 0;
}

// Replays a sequence of frames as consecutive scans and reports how well the
// lock holds. The live path cannot be judged from single images: the question
// is not whether a frame reads, but whether the search window is still on top
// of the marker one frame later, after the view has turned. This measures
// exactly that - the match score of the last picture of the marker against the
// next frame, and how far it had travelled.
int runFollowOnFiles(const std::vector<std::string>& files) {
    SCMiner::GlyphReader reader;
    reader.loadTemplates("./glyphs");
    SCMiner::ScreenCapture capture;
    SCMiner::SignatureAnalyzer analyzer;
    auto isSignature = [&analyzer](int v) { return analyzer.isKnownSignature(v); };
    const std::vector<int> known = analyzer.knownSignatures();

    cv::Mat patch;                 // the marker as it last read
    cv::Mat previousView;          // the previous frame, for the camera shift
    cv::Rect lock;
    bool haveLock = false;
    int followed = 0, dropped = 0, confirmed = 0, totalShift = 0;
    // SCMINER_READ_LOCK=1 tries a second place to read: the rectangle the
    // follower is holding. The locator has to find a row of glyphs from
    // scratch every frame, and under the ship's light amplification it
    // often does not - while the follower sits on the marker at 0.85 and
    // above, two pixels of movement per frame, for as long as it is on
    // screen. This measures what asking the reader to look THERE as well
    // would be worth, using nothing new: the existing follower, the
    // existing reader and the same goodEnough() bar the app accepts by.
    const bool readLock = DEV_ENV("SCMINER_READ_LOCK") != nullptr;
    int fromLock = 0;

    for (const auto& file : files) {
        cv::Mat image = cv::imread(file, cv::IMREAD_COLOR);
        if (image.empty()) continue;
        const std::string name = std::filesystem::path(file).filename().string();

        // What the camera did, measured from the whole frame the way the live
        // scan does it - printed beside the marker's real movement so the
        // two can be compared. If they agree, the box can be carried by the
        // camera alone and never needs to recognise the marker again.
        double camDx = 0.0, camDy = 0.0, camResponse = 0.0;
        {
            cv::Mat grey;
            cv::cvtColor(image, grey, cv::COLOR_BGR2GRAY);
            grey.convertTo(grey, CV_32F, 1.0 / 255.0);
            if (previousView.size() == grey.size()) {
                const cv::Point2d d = cv::phaseCorrelate(previousView, grey, cv::noArray(), &camResponse);
                camDx = d.x;
                camDy = d.y;
            }
            previousView = grey;
        }

        double score = -1.0;
        int shift = -1;
        if (haveLock && !patch.empty() &&
            image.cols >= patch.cols && image.rows >= patch.rows) {
            cv::Mat corr;
            cv::Point at;
            cv::matchTemplate(image, patch, corr, cv::TM_CCOEFF_NORMED);
            cv::minMaxLoc(corr, nullptr, &score, nullptr, &at);
            if (score >= 0.65) {
                shift = static_cast<int>(std::lround(cv::norm(cv::Point(at) - lock.tl())));
                lock = cv::Rect(at.x, at.y, patch.cols, patch.rows);
                followed++;
                totalShift += shift;
            } else {
                dropped++;
            }
        }

        // A reading is what refreshes the picture the follower matches
        // against - and it has to be a reading the live scan would ACCEPT.
        // Taking every value that merely resolves against the signature table
        // lets junk read off terrain reset the template every few frames, and
        // the follow rate then says nothing about the app.
        int value = -1;
        for (const auto& label : capture.locateSignatureLabels(image, 3)) {
            SCMiner::GlyphReader::Reading reading;
            auto v = reader.read(image(label), &reading, isSignature, &known);
            if (!v || !analyzer.isKnownSignature(*v)) continue;
            if (!SCMiner::GlyphReader::goodEnough(reading)) continue;
            value = *v;
            // Track the plate, not the crop: the margin of terrain around it
            // is the part that changes when the view turns
            cv::Rect plate = reading.plate & cv::Rect(0, 0, label.width, label.height);
            if (plate.width >= 12 && plate.height >= 6) {
                lock = cv::Rect(label.x + plate.x, label.y + plate.y, plate.width, plate.height);
                patch = image(lock).clone();
            } else {
                lock = label;
                patch = image(label).clone();
            }
            haveLock = true;
            confirmed++;
            break;
        }

        // ...and, when the locator came up empty, the place the follower
        // is holding. Padded the way the locator pads its own probes: the
        // lock is the PLATE, and the map pin stands two digit-heights to
        // the left of it, outside that rectangle.
        bool lockRead = false;
        if (readLock && value < 0 && haveLock && score >= 0.65) {
            // SCMINER_LOCK_PAD=0 reads the held rectangle exactly as it is.
            // That is the interesting case: the follower is holding the very
            // crop a reading came out of, so there is nothing to pad - the
            // pin and the margin the reader needs are already inside it, and
            // padding only adds terrain and the compass ribbon below.
            static const double padScale = DEV_ENV("SCMINER_LOCK_PAD")
                ? std::atof(DEV_ENV("SCMINER_LOCK_PAD")) : 1.0;
            cv::Rect probe = lock;
            if (padScale > 0.0) {
                const int padX = (std::max)(8, (int)std::lround(lock.height * 0.5 * padScale));
                const int padY = (std::max)(2, (int)std::lround(lock.height / 12.0 * padScale));
                probe = cv::Rect(lock.x - 2 * padX, lock.y - padY,
                                 lock.width + 3 * padX, lock.height + 2 * padY);
            }
            probe &= cv::Rect(0, 0, image.cols, image.rows);
            if (probe.width > 10 && probe.height > 6) {
                SCMiner::GlyphReader::Reading reading;
                auto v = reader.read(image(probe), &reading, isSignature, &known);
                if (v && analyzer.isKnownSignature(*v) &&
                    SCMiner::GlyphReader::goodEnough(reading)) {
                    value = *v;
                    lockRead = true;
                    fromLock++;
                } else if (DEV_ENV("SCMINER_LOCK_WHY")) {
                    // Why a rectangle the follower is sitting on did not
                    // produce a reading - the interesting half of this
                    // experiment, and unanswerable from the summary alone
                    std::cout << "    [lock] " << probe.x << "," << probe.y << " "
                              << probe.width << "x" << probe.height << " -> ";
                    if (!v) std::cout << "no value";
                    else if (!analyzer.isKnownSignature(*v)) std::cout << *v << " unknown";
                    else std::cout << *v << " not good enough";
                    std::cout << "  pin " << (int)(reading.pinScore * 100)
                              << " weakest " << (int)(reading.weakest * 100)
                              << " mean " << (int)(reading.mean * 100)
                              << " glyphs " << reading.glyphCount
                              << (reading.grid ? " grid" : "")
                              << (reading.separator ? " sep" : " NOsep") << std::endl;
                }
            }
        }

        std::cout << name;
        if (camResponse > 0.0) {
            std::cout << "  camera " << std::fixed << std::setprecision(1) << camDx << ","
                      << camDy << " (r " << std::setprecision(2) << camResponse << ")";
        }
        if (score >= 0.0) {
            std::cout << "  follow " << std::fixed << std::setprecision(2) << score;
            if (shift >= 0) std::cout << " (moved " << shift << " px)";
            else std::cout << " LOST";
        } else if (!haveLock) {
            std::cout << "  no lock yet";
        }
        if (value >= 0) std::cout << "  read " << value << (lockRead ? " (from the lock)" : "");
        if (haveLock) {
            std::cout << "  @ " << lock.x << "," << lock.y
                      << " " << lock.width << "x" << lock.height;
        }
        std::cout << std::endl;
    }

    if (readLock) {
        std::cout << "\n" << fromLock
                  << " reading(s) came from the lock, which the locator had not offered"
                  << std::endl;
    }
    std::cout << "\nfollowed " << followed << ", dropped " << dropped
              << ", readings " << confirmed;
    if (followed > 0) {
        std::cout << ", average travel " << (totalShift / followed) << " px per frame";
    }
    std::cout << std::endl;
    return 0;
}

#endif  // BEDROCK_DEV_TOOLS
// Reports how the screen is actually being read, and what it costs.
//
// This is the first thing to check when nothing is recognised on a machine
// that used to work. An HDR display read through GDI hands the reader a white
// slab where the HUD text should be - measured on one scene, 22.7 % of the
// screen centre was pure white against 0.4 % with HDR switched off - and
// nothing downstream can recover pixels that are all exactly 255.
int runCaptureCheck() {
    SCMiner::ScreenCapture capture;

    cv::Rect area(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                  GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (auto viewport = SCMiner::ScreenCapture::gameViewport()) {
        area = *viewport;
        std::cout << "Game viewport : " << area.width << "x" << area.height
                  << " at " << area.x << "," << area.y << std::endl;
        std::cout << "HUD scale     : "
                  << static_cast<double>(area.height) / SCMiner::GlyphReader::REFERENCE_VIEWPORT_HEIGHT
                  << " (reference " << SCMiner::GlyphReader::REFERENCE_VIEWPORT_HEIGHT << " px tall)" << std::endl;
    } else {
        std::cout << "Game viewport : not found - measuring the whole desktop" << std::endl;
    }

    SCMiner::CaptureRegion region;
    region.x = area.x;
    region.y = area.y;
    region.width = area.width;
    region.height = area.height;

    cv::Mat frame = capture.captureRegion(region);
    if (frame.empty()) {
        std::cout << "Capture       : FAILED - nothing came back" << std::endl;
        return 1;
    }

    const char* how = "?";
    switch (capture.lastSource()) {
        case SCMiner::ScreenCapture::Source::DxgiHdr: how = "Desktop Duplication, HDR (float, tone mapped here)"; break;
        case SCMiner::ScreenCapture::Source::Dxgi:    how = "Desktop Duplication, SDR"; break;
        case SCMiner::ScreenCapture::Source::Gdi:     how = "GDI BitBlt (duplication unavailable)"; break;
        default: break;
    }
    std::cout << "Capture path  : " << how << std::endl;
    if (!capture.captureNote().empty()) {
        std::cout << "  duplication : " << capture.captureNote() << std::endl;
    }
    std::cout << "Frame         : " << frame.cols << "x" << frame.rows << std::endl;
    if (!capture.rawStats().empty()) {
        std::cout << "Light         : " << capture.rawStats() << std::endl;
    }

    // The middle band is where the HUD and the markers live; the edges are
    // cockpit and sky and would only dilute the number.
    cv::Rect band(frame.cols / 4, frame.rows / 4, frame.cols / 2, frame.rows / 2);
    cv::Mat centre = frame(band);
    cv::Mat gray, low, high;
    cv::cvtColor(centre, gray, cv::COLOR_BGR2GRAY);
    const double clipped = 100.0 * cv::countNonZero(gray >= 250) / gray.total();
    const double dark = 100.0 * cv::countNonZero(gray <= 5) / gray.total();
    std::cout << std::fixed << std::setprecision(1)
              << "Pure white    : " << clipped << " %  (a healthy capture is under ~3 %)" << std::endl
              << "Pure black    : " << dark << " %" << std::endl
              << "Mean brightness: " << cv::mean(gray)[0] << std::endl;
    if (clipped > 8.0) {
        std::cout << "\n  WARNING: the highlights are being cut off. If this display runs in\n"
                     "  HDR and the path above says GDI, that is the cause - the reader is\n"
                     "  being handed a white slab. Either let duplication work or turn HDR off."
                  << std::endl;
    }

    std::filesystem::create_directories("debug");
    cv::imwrite("debug/capture_check.png", frame);
    std::cout << "\nSaved debug/capture_check.png" << std::endl;
    return 0;
}

// Watches the live picture and reports how well the view movement can be read
// out of it, band by band.
//
// Phase correlation over the whole search area has to cope with parts that do
// NOT move: the canopy frame, the ship's own instruments. They pull the answer
// towards zero and drag the confidence down. A horizontal band that contains
// only world - the compass ribbon, the horizon, terrain - moves cleanly, so
// splitting the area into bands and asking each one separately says where the
// usable signal actually is.
// --scanstages <frames...>: what the signature scan's stages cost, on a run of
// search-area frames off the app's own recording (debug/recording/scan_N.mkv):
// the camera-motion bands, the lock's template match over the whole area and
// over a window about the lock, and the locator over the whole area
#ifdef BEDROCK_DEV_TOOLS
int runScanStages(const std::vector<std::string>& files) {
    SCMiner::ScreenCapture capture;
    cv::Mat previous;
    cv::Mat patch;
    double tGrey = 0, tBands = 0, tFollowAll = 0, tFollowNear = 0, tLocate = 0;
    int n = 0;
    auto ms = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    for (const std::string& file : files) {
        const cv::Mat view = cv::imread(file, cv::IMREAD_COLOR);
        if (view.empty()) continue;
        auto t0 = std::chrono::steady_clock::now();
        cv::Mat grey;
        cv::cvtColor(view, grey, cv::COLOR_BGR2GRAY);
        grey.convertTo(grey, CV_32F, 1.0 / 255.0);
        tGrey += ms(t0);
        if (!previous.empty() && previous.size() == grey.size()) {
            t0 = std::chrono::steady_clock::now();
            const int bandHeight = grey.rows / 6;
            for (int b = 0; b < 6; b++) {
                const cv::Rect strip(0, b * bandHeight, grey.cols, bandHeight);
                double response = 0.0;
                cv::phaseCorrelate(previous(strip), grey(strip), cv::noArray(), &response);
            }
            tBands += ms(t0);
            if (patch.empty()) patch = view(cv::Rect(view.cols / 2 - 60, view.rows / 2 - 20, 120, 40)).clone();
            cv::Mat corr;
            t0 = std::chrono::steady_clock::now();
            cv::matchTemplate(view, patch, corr, cv::TM_CCOEFF_NORMED);
            tFollowAll += ms(t0);
            t0 = std::chrono::steady_clock::now();
            const cv::Rect window = cv::Rect(view.cols / 2 - 210, view.rows / 2 - 170, 420, 340) & cv::Rect(0, 0, view.cols, view.rows);
            cv::matchTemplate(view(window), patch, corr, cv::TM_CCOEFF_NORMED);
            tFollowNear += ms(t0);
            t0 = std::chrono::steady_clock::now();
            capture.locateSignatureLabels(view, 8);
            tLocate += ms(t0);
            n++;
        }
        previous = grey;
    }
    if (n == 0) return 1;
    std::cout << "over " << n << " frames of " << (previous.cols) << "x" << previous.rows << ":" << std::endl;
    std::cout << "  grey + float      " << std::fixed << std::setprecision(1) << tGrey / (n + 1) << " ms" << std::endl;
    std::cout << "  6 bands motion    " << tBands / n << " ms" << std::endl;
    std::cout << "  follow, whole     " << tFollowAll / n << " ms" << std::endl;
    std::cout << "  follow, 420x340   " << tFollowNear / n << " ms" << std::endl;
    std::cout << "  locate, whole     " << tLocate / n << " ms" << std::endl;
    const SCMiner::GlyphReader::Profile& p = SCMiner::GlyphReader::profile();
    std::cout << "    of which tophats " << p.locTophatMs / n << " ms, rows " << p.locRowsMs / n << " ms, probes "
              << p.locProbeMs / n << " ms" << std::endl;
    return 0;
}

int runMotionCheck(int seconds) {
    SCMiner::ScreenCapture capture;

    SCMiner::CaptureRegion region;
    int margin = 400;
    {
        std::ifstream file("region_config.txt");
        std::string line;
        while (std::getline(file, line)) {
            const size_t pos = line.find('=');
            if (pos == std::string::npos) continue;
            const std::string key = line.substr(0, pos);
            int value = 0;
            try { value = std::stoi(line.substr(pos + 1)); } catch (...) { continue; }
            if (key == "x") region.x = value;
            else if (key == "y") region.y = value;
            else if (key == "width") region.width = value;
            else if (key == "height") region.height = value;
            else if (key == "trackmargin") margin = value;
        }
    }
    const cv::Rect marked(region.x, region.y, region.width, region.height);
    region.x -= margin;
    region.y -= margin;
    region.width += 2 * margin;
    region.height += 2 * margin;
    std::cout << "Marked region : " << marked.x << "," << marked.y << "  "
              << marked.width << "x" << marked.height << std::endl;
    std::cout << "Search area   : " << region.width << "x" << region.height
              << " (margin " << margin << ")" << std::endl;
    std::cout << "\nMove the view while this runs.\n" << std::endl;

    constexpr int BANDS = 6;
    cv::Mat previous;
    std::vector<std::vector<double>> responses(BANDS + 1);
    std::vector<std::vector<double>> shifts(BANDS + 1);

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int frames = 0;
    while (std::chrono::steady_clock::now() < until) {
        cv::Mat shot = capture.captureRegion(region);
        if (shot.empty()) break;
        cv::Mat grey;
        cv::cvtColor(shot, grey, cv::COLOR_BGR2GRAY);
        grey.convertTo(grey, CV_32F, 1.0 / 255.0);

        if (previous.size() == grey.size()) {
            frames++;
            double r = 0.0;
            cv::Point2d d = cv::phaseCorrelate(previous, grey, cv::noArray(), &r);
            responses[BANDS].push_back(r);
            shifts[BANDS].push_back(d.x);

            const int bandHeight = grey.rows / BANDS;
            for (int b = 0; b < BANDS; b++) {
                const cv::Rect strip(0, b * bandHeight, grey.cols, bandHeight);
                double br = 0.0;
                cv::Point2d bd = cv::phaseCorrelate(previous(strip), grey(strip), cv::noArray(), &br);
                responses[b].push_back(br);
                shifts[b].push_back(bd.x);
            }
        }
        previous = grey;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (frames < 2) {
        std::cout << "Not enough frames captured." << std::endl;
        return 1;
    }

    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    auto usable = [](const std::vector<double>& v) {
        int n = 0;
        for (double x : v) if (x >= 0.15) n++;
        return n;
    };

    std::cout << frames << " frame pairs\n" << std::endl;
    std::cout << "band   rows              median r   usable(>=0.15)   median dx" << std::endl;
    const int bandHeight = previous.rows / BANDS;
    for (int b = 0; b < BANDS; b++) {
        std::cout << "  " << b << "    " << std::setw(4) << b * bandHeight << "-"
                  << std::setw(4) << (b + 1) * bandHeight << "          "
                  << std::fixed << std::setprecision(3) << std::setw(6) << median(responses[b])
                  << "        " << std::setw(3) << usable(responses[b]) << "/" << frames
                  << "          " << std::setprecision(1) << std::setw(6) << median(shifts[b])
                  << std::endl;
    }
    std::cout << "  all  whole area          "
              << std::fixed << std::setprecision(3) << std::setw(6) << median(responses[BANDS])
              << "        " << std::setw(3) << usable(responses[BANDS]) << "/" << frames
              << "          " << std::setprecision(1) << std::setw(6) << median(shifts[BANDS])
              << std::endl;
    return 0;
}

#endif  // BEDROCK_DEV_TOOLS
// Replays a Game.log through the state reader and prints every change of
// system, ship or seat with the log's own time stamp: what the app would
// have known, and when.
int runGameLog(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) {
        std::cout << "cannot open " << file << std::endl;
        return 1;
    }
    // The same state machine the app runs, line by line, every change printed
    SCMiner::GameState state;
    std::string line;
    std::string system, ship, place;
    bool inSeat = false;
    int changes = 0;
    int gadgets = 0;
    // ...and the contracts, the way the library reads them: one taken, one
    // over, and what a gathering order among them wants
    SCMiner::ContractScan scan;
    std::vector<SCMiner::ContractScan::Unpaid> unpaid;
    std::vector<SCMiner::ContractScan::Running> running;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string stamp = (line.size() > 20 && line[0] == '<') ? line.substr(1, 19) : std::string(19, ' ');
        const int before = scan.runningChanges();
        scan.line(line, unpaid);
        scan.rankEvents().clear();
        if (scan.runningChanges() != before) {
            const std::vector<SCMiner::ContractScan::Running>& now = scan.running();
            for (const auto& r : now) {
                bool known = false;
                for (const auto& o : running) known = known || o.mission == r.mission;
                if (known) continue;
                const SCMiner::RepContract* c = SCMiner::Reputation::contract(r.contract);
                std::cout << stamp << "  taken   " << r.contract << (r.shared ? "  (shared)" : "");
                if (c && c->oreCount > 0) {
                    std::cout << "  wants";
                    for (int k = 0; k < c->oreCount; k++) {
                        const SCMiner::OrderOre& ore = SCMiner::ORDER_ORES[c->oreFirst + k];
                        std::cout << (k ? (c->oreChoice ? " or " : ", ") : " ") << ore.least << (ore.scu ? " SCU " : " ")
                                  << ore.name;
                    }
                }
                std::cout << (c ? "" : "  (not in this build's tables)") << std::endl;
            }
            for (const auto& o : running) {
                bool still = false;
                for (const auto& r : now) still = still || o.mission == r.mission;
                if (!still) std::cout << stamp << "  over    " << o.contract << std::endl;
            }
            running = now;
        }
        SCMiner::GameState::Event event;
        if (!SCMiner::GameState::parse(line, &event)) continue;
        if (!state.apply(event)) continue;
        if (state.gadgetChanges() != gadgets) {
            gadgets = state.gadgetChanges();
            std::cout << stamp << "  gadget  "
                      << (state.gadgetOnRock().empty() ? std::string("back in the hand")
                                                       : state.gadgetOnRock() + " on " + state.gadgetRock())
                      << std::endl;
        }
        if (state.system() != system) {
            system = state.system();
            changes++;
            std::cout << stamp << "  system  " << system << std::endl;
        }
        if (state.place() != place) {
            place = state.place();
            changes++;
            const SCMiner::GamePlace where = SCMiner::placeByLogName(place, system);
            std::cout << stamp << "  place   "
                      << (place.empty() ? (state.inSpace() ? "(open space)" : "(nowhere named yet)") : place.c_str())
                      << (where.oreLocation.empty() ? "" : "  ores of " + where.oreLocation) << std::endl;
        }
        if (state.ship() != ship || state.inSeat() != inSeat) {
            ship = state.ship();
            inSeat = state.inSeat();
            changes++;
            std::cout << stamp << "  ship    " << SCMiner::GameState::label(ship)
                      << (inSeat ? "" : "  (left the seat)") << std::endl;
        }
    }
    std::cout << changes << " change(s). Now: system " << (system.empty() ? "?" : system)
              << ", place " << (place.empty() ? "?" : place) << ", ship "
              << (ship.empty() ? "?" : SCMiner::GameState::label(ship))
              << (!ship.empty() && !inSeat ? " (left)" : "") << "; " << running.size()
              << " contract(s) still running, " << gadgets << " gadget change(s)" << std::endl;
    return 0;
}

#ifdef BEDROCK_DEV_TOOLS
// The screenshot run lives in DevShots.cpp, so the Linux build can take
// the same pictures - see include/DevShots.h

// Walks the Blueprints window's filters the way a player clicks them, and
// reports any combination the window would offer and then answer with an
// empty list - a class of bug that cannot be found by looking at one
// screen.
int runCraftTest() {
    using namespace SCMiner;
    std::cout << "\n=== Blueprint filter test ===\n" << std::endl;
    std::cout << CRAFT_BLUEPRINT_COUNT << " blueprints, " << CRAFT_TYPE_COUNT << " components, "
              << CRAFT_MATERIAL_COUNT << " materials, " << CRAFT_CLASS_COUNT << " classes" << std::endl;

    // The same rule the window uses, in one place, so the test cannot drift
    auto matches = [](const CraftBlueprint& bp, int type, int material, char grade, int size, int cls) {
        if (type >= 0 && bp.type != type) return false;
        if (grade && bp.grade != grade) return false;
        if (size >= 0 && bp.size != size) return false;
        if (cls >= 0 && bp.itemClass != cls) return false;
        if (material >= 0) {
            bool uses = false;
            for (int s = 0; s < bp.count; s++) {
                if (CRAFT_SLOTS[bp.first + s].material == material) { uses = true; break; }
            }
            if (!uses) return false;
        }
        return true;
    };
    auto count = [&](int type, int material, char grade, int size, int cls) {
        int n = 0;
        for (int b = 0; b < CRAFT_BLUEPRINT_COUNT; b++) {
            if (matches(CRAFT_BLUEPRINTS[b], type, material, grade, size, cls)) n++;
        }
        return n;
    };

    int empties = 0, checked = 0;

    // Every component, and inside it every grade / size / class the bar would
    // offer. An offered chip that empties the list is the bug being hunted.
    for (int t = -1; t < CRAFT_TYPE_COUNT; t++) {
        std::vector<char> grades;
        std::vector<int> sizes, classes;
        for (int b = 0; b < CRAFT_BLUEPRINT_COUNT; b++) {
            const CraftBlueprint& bp = CRAFT_BLUEPRINTS[b];
            if (t >= 0 && bp.type != t) continue;
            if (bp.grade && std::find(grades.begin(), grades.end(), bp.grade) == grades.end()) grades.push_back(bp.grade);
            if (bp.size >= 0 && std::find(sizes.begin(), sizes.end(), (int)bp.size) == sizes.end()) sizes.push_back(bp.size);
            if (bp.itemClass >= 0 && std::find(classes.begin(), classes.end(), (int)bp.itemClass) == classes.end()) {
                classes.push_back(bp.itemClass);
            }
        }
        const int total = count(t, -1, 0, -1, -1);
        if (t >= 0 && total == 0) {
            std::cout << "  EMPTY component: " << CRAFT_TYPES[t] << std::endl;
            empties++;
        }
        for (char gr : grades) {
            checked++;
            if (count(t, -1, gr, -1, -1) == 0) {
                std::cout << "  EMPTY: " << (t < 0 ? "any" : CRAFT_TYPES[t]) << " + grade " << gr << std::endl;
                empties++;
            }
        }
        for (int sz : sizes) {
            checked++;
            if (count(t, -1, 0, sz, -1) == 0) {
                std::cout << "  EMPTY: " << (t < 0 ? "any" : CRAFT_TYPES[t]) << " + size " << sz << std::endl;
                empties++;
            }
        }
        for (int cl : classes) {
            checked++;
            if (count(t, -1, 0, -1, cl) == 0) {
                std::cout << "  EMPTY: " << (t < 0 ? "any" : CRAFT_TYPES[t]) << " + " << CRAFT_CLASSES[cl] << std::endl;
                empties++;
            }
        }
    }

    // Every material against every component it really occurs with
    for (int m = 0; m < CRAFT_MATERIAL_COUNT; m++) {
        if (count(-1, m, 0, -1, -1) == 0) {
            std::cout << "  EMPTY material: " << CRAFT_MATERIALS[m].name << std::endl;
            empties++;
        }
        checked++;
    }

    // What the window prints on each row has to be there to print
    int noSlots = 0, badSlot = 0, badStat = 0, badMaterial = 0, longRow = 0;
    for (int b = 0; b < CRAFT_BLUEPRINT_COUNT; b++) {
        const CraftBlueprint& bp = CRAFT_BLUEPRINTS[b];
        if (bp.count == 0) { noSlots++; continue; }
        if (bp.count > 8) longRow++;   // the detail view keeps eight sliders
        for (int s = 0; s < bp.count; s++) {
            const CraftSlot& slot = CRAFT_SLOTS[bp.first + s];
            if (slot.material >= CRAFT_MATERIAL_COUNT) badMaterial++;
            if (slot.slot >= CRAFT_SLOT_NAME_COUNT) badSlot++;
            // Every value the slot moves, not only the first: 1,263 slots
            // move two or three.
            for (int k = 0; k < slot.effCount; k++) {
              const CraftEffect& eff = CRAFT_EFFECTS[slot.effFirst + k];
              if (eff.stat >= CRAFT_STAT_COUNT) badStat++;
              if (effectMovesStat(eff)) {
                // A moving stat has to sit at stock in the middle and stay
                // inside a sane band at both ends
                const double at500 = effectFactor(eff, CRAFT_QUALITY_BASE);
                const double atMin = effectFactor(eff, CRAFT_QUALITY_MIN);
                const double atMax = effectFactor(eff, CRAFT_QUALITY_MAX);
                // The game does hand out one wide one (a mitigation factor
                // running 9.0 down to 1.1, where lower is better), so the
                // bar is only against numbers that cannot be real
                if (!(atMin > 0.01 && atMin < 100.0) || !(at500 > 0.01 && at500 < 100.0) ||
                    !(atMax > 0.01 && atMax < 100.0)) {
                    std::cout << "  ODD factor " << atMin << "/" << at500 << "/" << atMax << " on "
                              << bp.name << " slot " << s << std::endl;
                    empties++;
                }
              }
            }
        }
    }
    std::cout << "\nrows without a slot: " << noSlots << ", more than 8 slots: " << longRow
              << ", bad material/slot/stat index: " << badMaterial << "/" << badSlot << "/" << badStat
              << std::endl;

    // The ore finder has to know a material before the window offers the jump
    int noPlace = 0;
    for (int m = 0; m < CRAFT_MATERIAL_COUNT; m++) {
        if (locationsFor(CRAFT_MATERIALS[m].locationName, MiningFamily::Ship).empty() &&
            locationsFor(CRAFT_MATERIALS[m].locationName, MiningFamily::ROC).empty() &&
            locationsFor(CRAFT_MATERIALS[m].locationName, MiningFamily::FPS).empty()) {
            std::cout << "  no mining place for " << CRAFT_MATERIALS[m].name << std::endl;
            noPlace++;
        }
    }

    // What the detail view will print, counted so a change that breaks it shows
    int flat = 0, lower = 0;
    for (int i = 0; i < CRAFT_SLOT_COUNT; i++) {
        const CraftSlot& sl = CRAFT_SLOTS[i];
        for (int k = 0; k < sl.effCount; k++) {
            const CraftEffect& e = CRAFT_EFFECTS[sl.effFirst + k];
            if (e.stat >= 0 && !effectMovesStat(e)) flat++;
            if (e.better == 1) lower++;
        }
    }
    std::cout << "slots where quality changes nothing: " << flat << ", where lower is better: " << lower
              << std::endl;

    std::cout << "\n" << checked << " filter combinations checked, " << empties << " empty, "
              << noPlace << " material(s) without a place (they have none in the game data)." << std::endl;
    std::cout << ((empties == 0 && noSlots == 0 && badMaterial == 0 && badSlot == 0 && badStat == 0)
                      ? "=== Filter test PASSED ==="
                      : "=== Filter test FOUND PROBLEMS ===")
              << std::endl;
    // A material with no place is not a fault: two of them are not mined.
    return (empties == 0 && noSlots == 0) ? 0 : 1;
}

#endif  // BEDROCK_DEV_TOOLS

// The self-test lives in SelfTest.cpp, so the Linux build runs the same
// checks - see include/SelfTest.h


} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // First thing of all, before any flag is looked at: bring across
    // whatever an older copy left beside the exe. Copied, never moved -
    // see AppPaths.h. Every path in the app resolves under AppPaths::data()
    // from here on, so this has to happen before anything reads a file.
    SCMiner::AppPaths::migrateFromProgramFolder();

    // Crisp rendering on high-DPI displays (per-monitor aware)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Set the working directory to the exe folder so that tessdata,
    // region_config.txt and debug output resolve consistently
    SetCurrentDirectoryA(getExecutableDir().c_str());

    // Parse the command line
    bool consoleMode = false;
    bool debugConsole = false;
    bool selfTest = false;
    std::vector<std::string> ocrFiles;
    std::vector<std::string> locateFiles;
    std::vector<std::string> analyzeValues;
    std::vector<std::string> glyphFiles;
    std::vector<std::string> pipelineFiles;
    std::vector<std::string> harvestFiles;
    bool harvestMfd = false;
    std::vector<std::string> harvestPinFiles;
    std::string harvestPinValue;
    bool harvestPinMfd = false;
    std::string gameLogFile;   // --gamelog <Game.log>: replay what the app would know
    std::string harvestValue;
    bool glyphPrune = false;
    bool showViewport = false;
    std::string screenshotDir;
    std::vector<std::string> scanPanelFiles;
    bool scanPanelLive = false;
    std::string panelBenchDir, panelBenchOut;
    int panelBenchShard = 0, panelBenchShards = 1;
    std::vector<std::string> panelGlyphArgs;
    std::string updateCheckUrl;
    bool updateCheckAsked = false;
    bool checkCapture = false;
    bool takeSnapshot = false;
    bool craftTest = false;
    int motionSeconds = 0;
    std::vector<std::string> followFiles;
    for (int i = 1; i < __argc; i++) {
        const std::string arg = __argv[i];
        if (arg == "--console" || arg == "-c") consoleMode = true;
        else if (arg == "--debug") debugConsole = true;
        else if (arg == "--selftest" || arg == "--test") selfTest = true;
#ifdef BEDROCK_DEV_TOOLS
        else if (arg == "--ocr") {
            // every following argument is an image file
            for (int j = i + 1; j < __argc; j++) {
                ocrFiles.push_back(__argv[j]);
            }
            break;
        }
        else if (arg == "--locate") {
            for (int j = i + 1; j < __argc; j++) {
                locateFiles.push_back(__argv[j]);
            }
            break;
        }
        else if (arg == "--analyze") {
            for (int j = i + 1; j < __argc; j++) {
                analyzeValues.push_back(__argv[j]);
            }
            break;
        }
        else if (arg == "--panelglyphs") {
            for (int j = i + 1; j < __argc; j++) {
                panelGlyphArgs.push_back(__argv[j]);
            }
            break;
        }
        else if (arg == "--firing" && i + 4 < __argc) {
            setupConsole(true);
            const std::string dump = i + 5 < __argc ? __argv[i + 5] : "";
            const double angle = i + 6 < __argc ? std::atof(__argv[i + 6]) : 27.0;
            return runFiring(__argv[i + 1], __argv[i + 2], __argv[i + 3], __argv[i + 4], dump, angle);
        }
        else if (arg == "--cutlong" && i + 5 < __argc) {
            setupConsole(true);
            return runCutLong(__argv[i + 1], std::atof(__argv[i + 2]), std::atof(__argv[i + 3]),
                              std::atof(__argv[i + 4]), __argv[i + 5]);
        }
        else if (arg == "--shapeprobe" && i + 1 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            for (int k = i + 1; k < __argc; k++) files.push_back(__argv[k]);
            return runShapeProbe(files);
        }
        else if (arg == "--labelboot" && i + 11 < __argc) {
            // --labelboot <seed frame> <x> <y> <scale> <w1> <h1> <ink> <min score> <frames dir> <prefix> <out dir>:
            // a label column's shape grown from one cut at (x, y) of a frame
            // where the column stands at <scale>, <w1> x <h1> at size 1
            setupConsole(true);
            const std::string seedFile = __argv[i + 1];
            const double x = std::atof(__argv[i + 2]), y = std::atof(__argv[i + 3]), scale = std::atof(__argv[i + 4]);
            const int w1 = std::atoi(__argv[i + 5]), h1 = std::atoi(__argv[i + 6]);
            const int ink = std::atoi(__argv[i + 7]);
            const double minScore = std::atof(__argv[i + 8]);
            const std::string dir = __argv[i + 9], prefix = __argv[i + 10], outDir = __argv[i + 11];
            const cv::Mat seedFrame = cv::imread(seedFile, cv::IMREAD_COLOR);
            if (seedFrame.empty()) return 1;
            // the seed: the column cut at its size and brought to size 1, in the ink
            std::vector<cv::Mat> planes;
            const cv::Mat grey = [&] {
                cv::Mat g;
                // channelGrey is private to ScanPanel.cpp; the probe's inks match it
                std::vector<cv::Mat> pl;
                cv::split(seedFrame, pl);
                cv::Mat r, gg, b;
                pl[2].convertTo(r, CV_32F);
                pl[1].convertTo(gg, CV_32F);
                pl[0].convertTo(b, CV_32F);
                if (ink == 6) g = cv::max(2.0 * gg - r - b, 0.0);
                else if (ink == 4) g = cv::max(r - b, 0.0);
                else g = cv::min(cv::min(r, gg), b);
                return g;
            }();
            const cv::Matx23d map(scale, 0, x - 3.0 * scale + 0.5 * scale - 0.5, 0, scale, y - 3.0 * scale + 0.5 * scale - 0.5);
            cv::Mat seed;
            cv::warpAffine(grey, seed, map, cv::Size(w1, h1), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_REPLICATE);
            {
                double lo, hi;
                cv::minMaxLoc(seed, &lo, &hi);
                seed = (seed - lo) * (230.0 / (std::max)(1e-6, hi - lo)) + 12.0;
            }
            std::vector<std::string> files;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                const std::string name = entry.path().filename().string();
                if (name.rfind(prefix, 0) == 0 && entry.path().extension() == ".jpg") files.push_back(entry.path().string());
            }
            std::sort(files.begin(), files.end());
            std::filesystem::create_directories(outDir, ec);
            std::vector<std::tuple<std::string, double, double, int, int>> found;
            const cv::Mat avg = SCMiner::bootstrapLabelShape(seed, files, ink, minScore, found, 2);
            if (avg.empty()) {
                std::cout << "nothing matched the seed" << std::endl;
                return 1;
            }
            std::ofstream list(outDir + "/found.csv", std::ios::trunc);
            list << "file,score,scale,x,y" << std::endl;
            for (const auto& f : found) {
                list << std::filesystem::path(std::get<0>(f)).filename().string() << "," << std::get<1>(f) << ","
                     << std::get<2>(f) << "," << std::get<3>(f) << "," << std::get<4>(f) << std::endl;
            }
            std::ofstream inc(outDir + "/labels.inc", std::ios::trunc);
            inc << "inline const float SHAPE_BOOT_LABELS[] = {";
            for (int k = 0; k < avg.rows * avg.cols; k++) {
                char num[16];
                std::snprintf(num, sizeof(num), "%.1ff", avg.at<float>(k / avg.cols, k % avg.cols));
                inc << (k ? "," : "") << num;
            }
            inc << "};  // " << avg.cols << " x " << avg.rows << ", " << found.size() << " frames" << std::endl;
            cv::Mat view, seedView;
            avg.convertTo(view, CV_8U);
            seed.convertTo(seedView, CV_8U);
            cv::resize(view, view, cv::Size(), 6, 6, cv::INTER_NEAREST);
            cv::resize(seedView, seedView, cv::Size(), 6, 6, cv::INTER_NEAREST);
            cv::imwrite(outDir + "/labels_avg.png", view);
            cv::imwrite(outDir + "/labels_seed.png", seedView);
            std::cout << found.size() << " of " << files.size() << " frames taken in" << std::endl;
            return 0;
        }
        else if (arg == "--turretharvest" && i + 2 < __argc) {
            // --turretharvest <truth.csv> <out dir> [glyph] [ink]
            setupConsole(true);
            const char glyph = i + 3 < __argc ? __argv[i + 3][0] : 'T';
            const int ink = i + 4 < __argc ? std::atoi(__argv[i + 4]) : 4;
            return runTurretHarvest(__argv[i + 1], __argv[i + 2], glyph, ink);
        }
        else if (arg == "--refineryreplay" && i + 4 < __argc) {
            setupConsole(true);
            return runRefineryReplay(__argv[i + 1], __argv[i + 2], std::atoll(__argv[i + 3]), std::atof(__argv[i + 4]),
                                     i + 5 < __argc ? std::atoi(__argv[i + 5]) : 4);
        }
        else if (arg == "--refinery" && i + 1 < __argc) {
            // --refinery <pictures, or a folder> [--draw <dir>]
            setupConsole(true);
            std::vector<std::string> files;
            std::string drawDir;
            for (int k = i + 1; k < __argc; k++) {
                const std::string a = __argv[k];
                if (a == "--draw" && k + 1 < __argc) {
                    drawDir = __argv[++k];
                } else if (std::filesystem::is_directory(a)) {
                    for (const auto& entry : std::filesystem::directory_iterator(a)) {
                        const std::string ext = entry.path().extension().string();
                        if (ext == ".jpg" || ext == ".png") files.push_back(entry.path().string());
                    }
                } else {
                    files.push_back(a);
                }
            }
            std::sort(files.begin(), files.end());
            return runRefinery(files, drawDir);
        }
        else if (arg == "--contents" && i + 1 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            if (std::filesystem::is_directory(__argv[i + 1])) {
                for (const auto& entry : std::filesystem::directory_iterator(__argv[i + 1])) {
                    const std::string ext = entry.path().extension().string();
                    if (ext == ".jpg" || ext == ".png") files.push_back(entry.path().string());
                }
                std::sort(files.begin(), files.end());
                if (i + 3 < __argc) {
                    const int shard = std::atoi(__argv[i + 2]), shards = (std::max)(1, std::atoi(__argv[i + 3]));
                    std::vector<std::string> mine;
                    for (size_t f = 0; f < files.size(); f++) {
                        if ((int)(f % (size_t)shards) == shard) mine.push_back(files[f]);
                    }
                    files = mine;
                }
            } else {
                for (int k = i + 1; k < __argc; k++) files.push_back(__argv[k]);
            }
            return runContents(files);
        }
#ifdef _WIN32
        else if (arg == "--hidecheck") {
            setupConsole(true);
            return runHideCheck();
        }
#endif
        else if (arg == "--loadoutread" && i + 1 < __argc) {
            // --loadoutread <pictures...>: the laser and modules a one-seat
            // ship's mining HUD shows, as the app would read them
            setupConsole(true);
            SCMiner::TextLineReader* lines = panelLines();
            if (!lines) return 1;
            // a folder for its pictures, and a shard and a shard count to
            // split it: --loadoutread <folder> [shard shards]
            std::vector<std::string> files;
            if (std::filesystem::is_directory(__argv[i + 1])) {
                for (const auto& entry : std::filesystem::directory_iterator(__argv[i + 1])) {
                    const std::string ext = entry.path().extension().string();
                    if (ext == ".jpg" || ext == ".png") files.push_back(entry.path().string());
                }
                std::sort(files.begin(), files.end());
                if (i + 3 < __argc) {
                    const int shard = std::atoi(__argv[i + 2]), shards = (std::max)(1, std::atoi(__argv[i + 3]));
                    std::vector<std::string> mine;
                    for (size_t f = 0; f < files.size(); f++) {
                        if ((int)(f % (size_t)shards) == shard) mine.push_back(files[f]);
                    }
                    files = mine;
                }
            } else {
                for (int k = i + 1; k < __argc; k++) files.push_back(__argv[k]);
            }
            for (const std::string& file : files) {
                const cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
                if (frame.empty()) continue;
                const auto started = std::chrono::steady_clock::now();
                SCMiner::SeenLoadout seen;
                SCMiner::readSeenLoadout(frame, *lines, seen);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                std::cout << std::filesystem::path(file).filename().string() << ": table "
                          << (seen.tableFound ? "" : "not ") << "found " << seen.score << " at " << seen.table.x << ","
                          << seen.table.y << " x" << seen.scale << "; laser '" << seen.laserText << "' -> "
                          << (seen.laser ? seen.laser->name : "-") << "; modules" << (seen.modulesRead ? "" : " (unsure)") << ":";
                for (size_t m = 0; m < seen.moduleTexts.size(); m++) {
                    std::cout << " '" << seen.moduleTexts[m] << "'";
                }
                std::cout << " ->";
                for (const SCMiner::MiningModule* mod : seen.modules) std::cout << " " << mod->name;
                std::cout << "; columns " << seen.moduleColumns << (seen.iconPastList ? " past" : "") << "; head";
                for (int r = 0; r < 6; r++) std::cout << (r ? "/" : " ") << seen.head[r];
                std::cout << (seen.headAgrees ? " agrees" : " differs") << "; " << (int)ms << " ms" << std::endl;
            }
            return 0;
        }
        else if (arg == "--inkprobe" && i + 1 < __argc) {
            // --inkprobe <pictures...>: every label column in every ink
            setupConsole(true);
            for (int k = i + 1; k < __argc; k++) {
                const cv::Mat frame = cv::imread(__argv[k], cv::IMREAD_COLOR);
                if (frame.empty()) continue;
                std::cout << __argv[k] << std::endl;
                SCMiner::probePanelInks(frame, std::cout);
            }
            return 0;
        }
        else if (arg == "--paneltiming" && i + 2 < __argc) {
            setupConsole(true);
            return runPanelTiming(__argv[i + 1], std::atoi(__argv[i + 2]));
        }
        else if (arg == "--grabtiming" && i + 3 < __argc) {
            setupConsole(true);
            return runGrabTiming(std::atoi(__argv[i + 1]), std::atoi(__argv[i + 2]), std::atoi(__argv[i + 3]));
        }
        else if (arg == "--compmean" && i + 3 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            for (int k = i + 3; k < __argc; k++) files.push_back(__argv[k]);
            return runCompMean(__argv[i + 1], __argv[i + 2], files);
        }
        else if (arg == "--steadybench" && i + 4 < __argc) {
            setupConsole(true);
            return runSteadyBench(__argv[i + 1], __argv[i + 2], std::atoi(__argv[i + 3]), __argv[i + 4]);
        }
        else if (arg == "--digiteval" && i + 1 < __argc) {
            setupConsole(true);
            return runDigitEval(__argv[i + 1], i + 2 < __argc ? __argv[i + 2] : "");
        }
        else if (arg == "--truthcheck" && i + 2 < __argc) {
            setupConsole(true);
            return runTruthCheck(__argv[i + 1], __argv[i + 2]);
        }
        else if (arg == "--gametexts" && i + 2 < __argc) {
            setupConsole(true);
            return runGameTexts(__argv[i + 1], __argv[i + 2], i + 3 < __argc ? __argv[i + 3] : "");
        }
        else if (arg == "--payouts" && i + 1 < __argc) {
            setupConsole(true);
            return runPayouts(__argv[i + 1]);
        }
        else if (arg == "--ranks" && i + 1 < __argc) {
            setupConsole(true);
            return runRanks(__argv[i + 1]);
        }
        else if (arg == "--uexprobe" && i + 1 < __argc) {
            setupConsole(true);
            return runUexProbe(__argv[i + 1]);
        }
        else if (arg == "--uexreportprobe" && i + 2 < __argc) {
            setupConsole(true);
            return runUexReportProbe(__argv[i + 1], __argv[i + 2]);
        }
        else if (arg == "--uexshotprobe" && i + 2 < __argc) {
            setupConsole(true);
            return runUexShotProbe(__argv[i + 1], __argv[i + 2]);
        }
        else if (arg == "--trades" && i + 1 < __argc) {
            setupConsole(true);
            return runTrades(__argv[i + 1]);
        }
        else if (arg == "--rankcheck" && i + 2 < __argc) {
            setupConsole(true);
            return runRankCheck(__argv[i + 1], __argv[i + 2]);
        }
        else if (arg == "--backupcheck" && i + 2 < __argc) {
            setupConsole(true);
            return runBackupCheck(__argv[i + 1], __argv[i + 2]);
        }
        else if (arg == "--locatecrop" && i + 5 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            for (int k = i + 5; k < __argc; k++) files.push_back(__argv[k]);
            return runLocateCrop(cv::Rect(std::atoi(__argv[i + 1]), std::atoi(__argv[i + 2]), std::atoi(__argv[i + 3]),
                                          std::atoi(__argv[i + 4])),
                                 files);
        }
        else if (arg == "--glyphmean" && i + 6 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            for (int k = i + 6; k < __argc; k++) files.push_back(__argv[k]);
            return runGlyphMean(__argv[i + 1], __argv[i + 2], std::atoi(__argv[i + 3]), std::atoi(__argv[i + 4]),
                                std::atoi(__argv[i + 5]), files);
        }
        else if (arg == "--glyphcompare" && i + 1 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            for (int k = i + 1; k < __argc; k++) files.push_back(__argv[k]);
            return runGlyphCompare(files);
        }
        else if (arg == "--glyphpair" && i + 1 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            for (int k = i + 1; k < __argc; k++) files.push_back(__argv[k]);
            return runGlyphPair(files);
        }
        else if (arg == "--digittrain" && i + 3 < __argc) {
            setupConsole(true);
            return runDigitTrain(__argv[i + 1], __argv[i + 2], __argv[i + 3]);
        }
        else if (arg == "--shapebook" && i + 2 < __argc) {
            setupConsole(true);
            return runShapeBook(__argv[i + 1], __argv[i + 2]);
        }
        else if (arg == "--labelsearch" && i + 2 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            for (int k = i + 2; k < __argc; k++) files.push_back(__argv[k]);
            return runLabelSearch(__argv[i + 1], files);
        }
        else if (arg == "--shapeharvest" && i + 3 < __argc) {
            setupConsole(true);
            return runShapeHarvest(__argv[i + 1], __argv[i + 2], __argv[i + 3]);
        }
        else if (arg == "--facewhy" && i + 3 < __argc) {
            setupConsole(true);
            const int shard = (i + 5 < __argc) ? std::atoi(__argv[i + 4]) : 0;
            const int shards = (i + 5 < __argc) ? std::atoi(__argv[i + 5]) : 1;
            return runFaceWhy(__argv[i + 1], __argv[i + 2], __argv[i + 3], shard, shards);
        }
        else if (arg == "--faceharvest" && i + 3 < __argc) {
            setupConsole(true);
            return runFaceHarvest2(__argv[i + 1], __argv[i + 2], __argv[i + 3]);
        }
        else if (arg == "--facedebug" && i + 2 < __argc) {
            setupConsole(true);
            return runFaceDebug(__argv[i + 1], __argv[i + 2]);
        }
        else if (arg == "--panelharvest" && i + 3 < __argc) {
            setupConsole(true);
            return runPanelHarvest(__argv[i + 1], __argv[i + 2], __argv[i + 3]);
        }
        else if (arg == "--textmap" && i + 6 < __argc) {
            setupConsole(true);
            return runTextMap(__argv[i + 1], __argv[i + 2], std::atoi(__argv[i + 3]),
                              std::atoi(__argv[i + 4]), std::atoi(__argv[i + 5]),
                              std::atoi(__argv[i + 6]));
        }
        else if (arg == "--panelbench" && i + 2 < __argc) {
            // --panelbench <folder> <out.csv> [shard shards]
            panelBenchDir = __argv[i + 1];
            panelBenchOut = __argv[i + 2];
            i += 2;
            if (i + 2 < __argc && std::isdigit((unsigned char)__argv[i + 1][0]) &&
                std::isdigit((unsigned char)__argv[i + 2][0])) {
                panelBenchShard = std::atoi(__argv[i + 1]);
                panelBenchShards = (std::max)(1, std::atoi(__argv[i + 2]));
                i += 2;
            }
        }
        else if (arg == "--scanpanel") {
            // With files: those. Without: whatever is on the screen right now,
            // which is the only way to measure the search against the HUD the
            // player has in front of them.
            scanPanelLive = (i + 1 >= __argc);
            for (int j = i + 1; j < __argc; j++) {
                scanPanelFiles.push_back(__argv[j]);
            }
            break;
        }
        else if (arg == "--glyphprune") glyphPrune = true;
#endif  // BEDROCK_DEV_TOOLS
        else if (arg == "--viewport") showViewport = true;
#ifdef BEDROCK_DEV_TOOLS
        else if (arg == "--screenshot" && i + 1 < __argc) { screenshotDir = __argv[i + 1]; i++; }
#endif  // BEDROCK_DEV_TOOLS
        else if (arg == "--capture") checkCapture = true;
        else if (arg == "--snapshot") takeSnapshot = true;
#ifdef BEDROCK_DEV_TOOLS
        else if (arg == "--crafttest") craftTest = true;
#endif  // BEDROCK_DEV_TOOLS
        else if (arg == "--updatecheck") {
            updateCheckUrl = (i + 1 < __argc && __argv[i + 1][0] != '-') ? __argv[++i] : "";
            updateCheckAsked = true;
        }
#ifdef BEDROCK_DEV_TOOLS
        else if (arg == "--scanstages" && i + 1 < __argc) {
            setupConsole(true);
            std::vector<std::string> files;
            for (int k = i + 1; k < __argc; k++) files.push_back(__argv[k]);
            return runScanStages(files);
        }
        else if (arg == "--motion") {
            motionSeconds = 10;
            if (i + 1 < __argc) {
                try { motionSeconds = std::stoi(__argv[i + 1]); } catch (...) {}
            }
        }
        else if (arg == "--follow") {
            for (int j = i + 1; j < __argc; j++) followFiles.push_back(__argv[j]);
            break;
        }
#endif  // BEDROCK_DEV_TOOLS
        else if (arg == "--gamelog" && i + 1 < __argc) {
            gameLogFile = __argv[i + 1];
            i++;
        }
#ifdef BEDROCK_DEV_TOOLS
        else if (arg == "--pipeline") {
            for (int j = i + 1; j < __argc; j++) {
                pipelineFiles.push_back(__argv[j]);
            }
            break;
        }
        else if (arg == "--glyph") {
            for (int j = i + 1; j < __argc; j++) {
                glyphFiles.push_back(__argv[j]);
            }
            break;
        }
        else if (arg == "--harvestpin" || arg == "--harvestpin-mfd") {
            // --harvestpin [value] <files>: with a value, only the pin beside
            // a row that reads as that value is taken. A file wider than a
            // marker crop is searched with the locator first, and the pin is
            // then cut from the candidate that reads as the value, the way
            // the live scan sees it. -mfd files the pins with the panel set.
            harvestPinMfd = (arg == "--harvestpin-mfd");
            for (int j = i + 1; j < __argc; j++) {
                const std::string a = __argv[j];
                if (j == i + 1 && !a.empty() && std::all_of(a.begin(), a.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    harvestPinValue = a;
                    continue;
                }
                harvestPinFiles.push_back(__argv[j]);
            }
            break;
        }
        else if ((arg == "--harvest" || arg == "--harvest-mfd") && i + 1 < __argc) {
            // --harvest-mfd feeds the instrument-panel set (glyphs_mfd),
            // --harvest the canopy set. Two fonts, two folders: mixed into
            // one pool the wide panel zeros stole a canopy 6 and turned a
            // real 60,000 into 80,000.
            harvestMfd = (arg == "--harvest-mfd");
            harvestValue = __argv[i + 1];
            for (int j = i + 2; j < __argc; j++) {
                harvestFiles.push_back(__argv[j]);
            }
            break;
        }
#endif  // BEDROCK_DEV_TOOLS
    }

    // The diagnostics below spread over every core unless told otherwise,
    // while the live scan is capped at a quarter of them (see
    // Application::run). To measure what a scan really costs, the tools
    // have to run under the same cap: SCMINER_CV_THREADS=6.
    if (const char* threadCap = DEV_ENV("SCMINER_CV_THREADS")) {
        const int want = std::atoi(threadCap);
        if (want > 0) cv::setNumThreads(want);
    }

    if (selfTest) {
        setupConsole(true);
        return runSelfTest();
    }

    if (updateCheckAsked) {
        setupConsole(true);
        SCMiner::Updater updater;
        const std::string url = updateCheckUrl.empty() ? std::string(BEDROCK_UPDATE_URL)
                                                       : updateCheckUrl;
        std::cout << "this build is " << BEDROCK_VERSION << "\n";
        std::cout << "asking " << url << "\n";
        updater.configure(url, true);
        updater.checkInBackground();
        for (int i = 0; i < 400 && (updater.state() == SCMiner::Updater::State::Checking ||
                                    updater.state() == SCMiner::Updater::State::Idle);
             i++) {
            Sleep(100);
        }
        switch (updater.state()) {
            case SCMiner::Updater::State::Available:
                std::cout << "NEWER: " << updater.latest().version << "\n  "
                          << updater.latest().notes << "\n  " << updater.latest().url << "\n";
                break;
            case SCMiner::Updater::State::UpToDate:
                std::cout << "up to date (the file offers " << updater.latest().version << ")\n";
                break;
            case SCMiner::Updater::State::Failed:
                std::cout << "FAILED: " << updater.couldNotSay() << "\n";
                break;
            default:
                std::cout << "no answer in 40 seconds, or checking is switched off\n";
                break;
        }
        return 0;
    }

#ifdef BEDROCK_DEV_TOOLS
    if (!panelGlyphArgs.empty()) {
        setupConsole(true);
        return runPanelGlyphs(panelGlyphArgs);
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (scanPanelLive) {
        setupConsole(true);
        // into debug\: pictures beside the player's data go into every backup
        // (two 9 MB pictures turn a half-megabyte backup into 30 MB)
        std::error_code ec;
        std::filesystem::create_directories(SCMiner::AppPaths::data() / "debug", ec);
        return runScanPanelLive((SCMiner::AppPaths::data() / "debug").string());
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!scanPanelFiles.empty()) {
        setupConsole(true);
        return runScanPanel(scanPanelFiles);
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!panelBenchDir.empty()) {
        setupConsole(true);
        return runPanelBench(panelBenchDir, panelBenchOut, panelBenchShard, panelBenchShards);
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!screenshotDir.empty()) {
        setupConsole(true);
        return runScreenshots(screenshotDir);
    }
#endif  // BEDROCK_DEV_TOOLS

    if (!gameLogFile.empty()) {
        setupConsole(true);
        return runGameLog(gameLogFile);
    }

#ifdef BEDROCK_DEV_TOOLS
    if (!pipelineFiles.empty()) {
        setupConsole(true);
        return runPipelineOnFiles(pipelineFiles);
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!followFiles.empty()) {
        setupConsole(true);
        return runFollowOnFiles(followFiles);
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!glyphFiles.empty()) {
        setupConsole(true);
        return runGlyphOnFiles(glyphFiles);
    }
#endif  // BEDROCK_DEV_TOOLS

    // Reports the rectangle the search is pinned to. Worth having on hand:
    // when the scan finds nothing at all, the first question is whether it is
    // even looking at the game.
    if (checkCapture) {
        setupConsole(true);
        return runCaptureCheck();
    }

#ifdef BEDROCK_DEV_TOOLS
    if (craftTest) {
        setupConsole(true);
        return runCraftTest();
    }
#endif  // BEDROCK_DEV_TOOLS

    if (takeSnapshot) {
        setupConsole(true);
        SCMiner::Application app;
        if (!app.initialize()) return 1;
        app.snapshotFromConsole();
        return 0;
    }

#ifdef BEDROCK_DEV_TOOLS
    if (motionSeconds > 0) {
        setupConsole(true);
        return runMotionCheck(motionSeconds);
    }
#endif  // BEDROCK_DEV_TOOLS

    if (showViewport) {
        setupConsole(true);
        if (auto viewport = SCMiner::ScreenCapture::gameViewport()) {
            std::cout << "Game viewport: " << viewport->width << "x" << viewport->height
                      << " at " << viewport->x << "," << viewport->y << std::endl;
            int margin = 0;
            const auto box = SCMiner::Application::defaultAimBox(*viewport, &margin);
            std::cout << "Automatic aim box: " << box.x << "," << box.y << " " << box.width << "x"
                      << box.height << ", search margin " << margin << " px" << std::endl;
            std::cout << "HUD scale: "
                      << static_cast<double>(viewport->height) / SCMiner::GlyphReader::REFERENCE_VIEWPORT_HEIGHT
                      << " (reference " << SCMiner::GlyphReader::REFERENCE_VIEWPORT_HEIGHT << " px tall)" << std::endl;
        } else {
            std::cout << "Game viewport: not found - Star Citizen is not running, "
                         "so the search falls back to the monitor" << std::endl;
        }
        return 0;
    }

#ifdef BEDROCK_DEV_TOOLS
    if (glyphPrune) {
        setupConsole(true);
        SCMiner::GlyphReader reader;
        const int loaded = reader.loadTemplates("./glyphs");
        const int removed = reader.pruneTemplates(true);
        std::cout << "Pruned " << removed << " of " << loaded << " templates." << std::endl;
        return 0;
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!harvestFiles.empty()) {
        setupConsole(true);
        return runHarvest(harvestValue, harvestFiles, harvestMfd ? "./glyphs_mfd" : "./glyphs");
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!harvestPinFiles.empty()) {
        setupConsole(true);
        int saved = 0;
        SCMiner::GlyphReader pinReader;   // the value check reads with the sets it has
        pinReader.loadTemplates("./glyphs");
        SCMiner::ScreenCapture pinCapture;
        const std::string pinDir = harvestPinMfd ? "./glyphs_mfd" : "./glyphs";
        for (const auto& file : harvestPinFiles) {
            cv::Mat image = cv::imread(file, cv::IMREAD_COLOR);
            const std::filesystem::path path(file);
            const std::string tag = path.parent_path().filename().string() + "_" + path.stem().string();
            if (image.empty()) {
                std::cout << file << " -> could not load" << std::endl;
                continue;
            }
            bool got = false;
            if (image.cols > 400 && !harvestPinValue.empty()) {
                // A whole search area: every candidate the locator offers,
                // read with its row hint, and the pin cut where the value
                // comes out. A snapshot's own area and aim, as in the
                // digit harvest, when the sidecar has them.
                const cv::Rect whole(0, 0, image.cols, image.rows);
                cv::Rect area = whole;
                cv::Point aim(image.cols / 2, image.rows / 2);
                if (const auto geometry = sidecarGeometry(file)) {
                    const cv::Rect inside = geometry->area & whole;
                    if (inside.width > 10 && inside.height > 6) {
                        area = inside;
                        aim = cv::Point(geometry->aim.x + geometry->aim.width / 2,
                                        geometry->aim.y + geometry->aim.height / 2);
                    }
                }
                const cv::Point aimInArea(aim.x - area.x, aim.y - area.y);
                cv::Mat search = image(area);
                auto labels = pinCapture.locateSignatureLabels(search, 12);
                std::stable_sort(labels.begin(), labels.end(), [&](const cv::Rect& a, const cv::Rect& b) {
                    const cv::Point ca(a.x + a.width / 2, a.y + a.height / 2);
                    const cv::Point cb(b.x + b.width / 2, b.y + b.height / 2);
                    return cv::norm(ca - aimInArea) < cv::norm(cb - aimInArea);
                });
                for (const auto& label : labels) {
                    cv::Rect rowHint;
                    if (auto row = pinCapture.rowOf(label)) {
                        rowHint = cv::Rect(row->x - label.x, row->y - label.y, row->width, row->height);
                    }
                    if (pinReader.harvestPin(search(label), pinDir, tag, harvestPinValue,
                                             rowHint.area() > 0 ? &rowHint : nullptr)) {
                        got = true;
                        break;
                    }
                }
            } else {
                got = pinReader.harvestPin(image, pinDir, tag, harvestPinValue);
            }
            if (got) {
                saved++;
            } else {
                std::cout << file << " -> no row reading " << (harvestPinValue.empty() ? "as a number" : harvestPinValue)
                          << " with a usable pin strip, nothing harvested" << std::endl;
            }
        }
        std::cout << "Harvested " << saved << " pin template(s)." << std::endl;
        return 0;
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!ocrFiles.empty()) {
        setupConsole(true);
        return runOcrOnFiles(ocrFiles);
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!locateFiles.empty()) {
        setupConsole(true);
        return runLocateOnFiles(locateFiles);
    }
#endif  // BEDROCK_DEV_TOOLS

#ifdef BEDROCK_DEV_TOOLS
    if (!analyzeValues.empty()) {
        setupConsole(true);
        return runAnalyze(analyzeValues);
    }
#endif  // BEDROCK_DEV_TOOLS

    if (consoleMode || debugConsole) {
        setupConsole(true);
    }

    SCMiner::Application app;

    if (!app.initialize()) {
        if (consoleMode || debugConsole) {
            std::cerr << "\nInitialization failed! Press Enter to exit..." << std::endl;
            std::cin.get();
        }
        return 1;
    }

    if (consoleMode) {
        app.run();
    } else {
        app.runWithGUI(hInstance);
    }

    return 0;
}
