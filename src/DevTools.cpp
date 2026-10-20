// The two measurements the readers are tuned against - see DevTools.h.
// Kept apart from main.cpp so that the Linux build runs the same code
// over the same frames; nothing in it is Windows-only.
#define _CRT_SECURE_NO_WARNINGS
#include "DevTools.h"
#include "DevSwitch.h"
#include "GlyphReader.h"
#include "OCREngine.h"
#include "ScanPanel.h"
#include "ScreenCapture.h"
#include "SignatureAnalyzer.h"
#include "TextLineReader.h"
#include "RefineryTerminal.h"
#include "RefineryOrders.h"
#include "RefineryShare.h"
#include "RefineryData.h"
#include "RefiningData.h"
#include "Updater.h"
#include "resource.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

SCMiner::TextLineReader* panelLines() {
    static SCMiner::TextLineReader lines;
    static const bool loaded = lines.loadEmbedded();
    return loaded ? &lines : nullptr;
}

// What the app knew when it took a snapshot, from the .txt beside the PNG:
//   game viewport: 5120x1440 at 0,0
//   aim box: 2384,385 352x180 (auto), search margin 400
//   search area: 1984,0 1152x965
// The PNG is the viewport, so both rects come back relative to it.
std::optional<SnapshotGeometry> sidecarGeometry(const std::string& pngFile) {
    std::filesystem::path sidecar(pngFile);
    sidecar.replace_extension(".txt");
    std::ifstream in(sidecar);
    if (!in.is_open()) return std::nullopt;
    SnapshotGeometry g;
    cv::Point origin(0, 0);
    bool haveAim = false, haveArea = false;
    std::string line;
    while (std::getline(in, line)) {
        int x = 0, y = 0, w = 0, h = 0;
        if (std::sscanf(line.c_str(), "game viewport: %dx%d at %d,%d", &w, &h, &x, &y) == 4) {
            origin = cv::Point(x, y);
        } else if (std::sscanf(line.c_str(), "aim box: %d,%d %dx%d", &x, &y, &w, &h) == 4) {
            g.aim = cv::Rect(x, y, w, h);
            haveAim = true;
        } else if (std::sscanf(line.c_str(), "search area: %d,%d %dx%d", &x, &y, &w, &h) == 4) {
            g.area = cv::Rect(x, y, w, h);
            haveArea = true;
        }
    }
    if (!haveAim || !haveArea) return std::nullopt;
    g.aim -= origin;
    g.area -= origin;
    return g;
}

// The bench the reader is tuned against: a recording cut into frames and read
// exactly the way the app reads the screen, so that a change is measured over
// every frame of a real session instead of judged on the one frame that
// prompted it. A shard and a shard count split the folder, so several of
// these can run at once.
int runPanelBench(const std::string& dir, const std::string& outCsv, int shard, int shards) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".jpg" || ext == ".png") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());

    SCMiner::GlyphReader glyphs;
    glyphs.loadTemplates("./glyphs");
    SCMiner::OCREngine ocr;
    SCMiner::OCRConfig cfg;
    if (!ocr.initialize(cfg)) {
        // Not a reason to stop: this is what every fresh install is - no
        // tessdata beside the exe - and a bench that only runs with it can miss
        // a rock view that is dead on every such install. The shapes read on
        // their own; what is lost without Tesseract is what this run then shows
        // against one with it.
        std::cout << "no tessdata - reading by the shapes alone, as a fresh install does" << std::endl;
    }

    std::ofstream out(outCsv, std::ios::trunc);
    if (!out.is_open()) {
        std::cout << "cannot write " << outCsv << std::endl;
        return 1;
    }
    out << "file,found,figures,ore,mass,res,res_printed,inst,scu,"
           "box_x,box_y,box_w,box_h,head,body,ms,note" << std::endl;

    // A name or a note can hold a comma, and a quote would end the field
    auto quoted = [](std::string text) {
        for (char& c : text) {
            if (c == '"') c = '\'';
        }
        return "\"" + text + "\"";
    };

    int done = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (shards > 1 && (int)(i % (size_t)shards) != shard) continue;
        const cv::Mat frame = cv::imread(files[i], cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        const auto started = std::chrono::steady_clock::now();
        const SCMiner::ScanPanel p = SCMiner::readScanPanel(frame, ocr, &glyphs, panelLines());
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started).count();
        const SCMiner::RockStats& r = p.stats;
        char figures[160];
        std::snprintf(figures, sizeof(figures), "%.0f,%.0f,%s,%.2f,%.2f", r.mass, r.resistance,
                      r.resistance < 0 ? "" : (r.resistanceAdjusted ? "colour" : "white"),
                      r.instability, r.scu);
        out << std::filesystem::path(files[i]).filename().string() << "," << (p.found ? 1 : 0)
            << "," << p.figuresRead << "," << quoted(r.ore) << "," << figures << ","
            << p.box.x << "," << p.box.y << "," << p.box.width << "," << p.box.height << ","
            << p.headRule << "," << p.bodyRule << "," << (int)ms << "," << quoted(p.note)
            << std::endl;
        if (++done % 25 == 0) std::cout << done << " read" << std::endl;
    }
    std::cout << done << " frames into " << outCsv << std::endl;
    return 0;
}

// Runs the WHOLE chain on saved frames the way the live scan does it: locate
// the marker, then read the candidates in order. On frames from a recording
// it counts how many frames of an approach actually yield the value, and
// what the others produce instead.
int runPipelineOnFiles(const std::vector<std::string>& files) {
    SCMiner::OCRConfig ocrConfig;
    ocrConfig.tessdataPath = "./tessdata";
    ocrConfig.pageSegMode = 8;
    ocrConfig.minConfidence = 20.0;

    SCMiner::OCREngine ocr;
    const bool ocrReady = ocr.initialize(ocrConfig);
    if (!ocrReady) std::cerr << "Tesseract not available (no tessdata) - OCR paths stay off." << std::endl;

    SCMiner::GlyphReader reader;
    reader.loadTemplates("./glyphs");
    SCMiner::ScreenCapture capture;
    SCMiner::SignatureAnalyzer analyzer;
    auto policy = SCMiner::makeReadingPolicy(analyzer);
    auto isSignature = [&analyzer](int v) { return analyzer.isSingleSignature(v); };
    auto isSum = [&analyzer](int v) { return analyzer.isKnownSignature(v); };
    const std::vector<int> known = analyzer.knownSignatures();
    // The second reader, as the live scan runs it; SCMINER_NO_TEXTLINE=1
    // measures the glyph reader alone
    SCMiner::TextLineReader textLines;
    if (!DEV_ENV("SCMINER_NO_TEXTLINE")) {
        if (!textLines.loadEmbedded()) std::cerr << "The second reader could not be loaded." << std::endl;
    }
    // What counts as a table value for it: what the analyser explains exactly
    // (as live). SCMINER_TEXTLINE_STRICT=1: the scan's own test instead - one
    // ore, one field or a pair - which on the labelled frames lost a real
    // 86,000 and let the glyph reader's 88,000 stand
    static const bool strictKnown = DEV_ENV("SCMINER_TEXTLINE_STRICT") != nullptr;
    // Two rules to weigh against today's, on the same frames:
    //   HOLD        where the readers differ, the frame yields NOTHING - the
    //               next scan is 300 ms away and usually settles it
    //   NOFALLBACK  the second reader never reads a row on its own
    static const bool holdOnDisagree = DEV_ENV("SCMINER_TEXTLINE_HOLD") != nullptr;
    static const bool noFallback = DEV_ENV("SCMINER_TEXTLINE_NOFALLBACK") != nullptr;
    auto lineKnown = [&analyzer](int v) {
        return strictKnown ? analyzer.isKnownSignature(v) : analyzer.explainsExactly(v);
    };

    // Other monitors, without one: SCMINER_SIM_SCALE grows or shrinks every
    // input picture first (1.5 shows 1440p footage the way 4K draws it),
    // SCMINER_HUD_SCALE tells the reader what the live app would know from
    // the viewport. Both 1 when unset.
    const double simScale = DEV_ENV("SCMINER_SIM_SCALE") ? std::atof(DEV_ENV("SCMINER_SIM_SCALE")) : 1.0;
    if (DEV_ENV("SCMINER_HUD_SCALE")) {
        SCMiner::GlyphReader::setHudScale(std::atof(DEV_ENV("SCMINER_HUD_SCALE")));
    }
    if (const char* native = DEV_ENV("SCMINER_READ_NATIVE")) {
        SCMiner::GlyphReader::setReadAtNativeScale(native[0] != '0');
    }
    if (simScale != 1.0 || SCMiner::GlyphReader::hudScale() != 1.0) {
        std::cout << "simulated picture scale " << simScale << ", hud scale "
                  << SCMiner::GlyphReader::hudScale()
                  << (SCMiner::GlyphReader::readAtNativeScale() ? ", reader at native scale" : "") << std::endl;
    }

    int read = 0;
    int fromSidecar = 0;
    std::map<int, int> tally;

    // Where the time goes. A scan that takes longer than the live interval
    // is a scan the player waits for, so every stage is clocked.
    using Clock = std::chrono::steady_clock;
    auto msSince = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    double totalLocate = 0.0, totalRead = 0.0, totalOcr = 0.0, totalFrame = 0.0, totalLines = 0.0;
    int frames = 0;

    for (const auto& file : files) {
        cv::Mat image = cv::imread(file, cv::IMREAD_COLOR);
        if (image.empty()) {
            std::cout << file << " -> could not load image" << std::endl;
            continue;
        }
        if (simScale > 0.0 && std::abs(simScale - 1.0) > 0.001) {
            cv::resize(image, image, cv::Size(), simScale, simScale,
                       simScale > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA);
        }

        // The app never hands the locator the whole screen: it searches the
        // area around the aim box - the box plus the tracking margin - and
        // reads the candidates nearest the aim point first. A snapshot
        // carries that geometry in the .txt beside it; a frame without one
        // (the recorded clips ARE the search area) is taken as it is, with
        // its centre for the aim point. On 68 MOLE snapshots searched whole,
        // the turret's own readouts left and right of the canopy took every
        // probe and the marker in the box was never looked at (3 read); on
        // the app's area the same marker came up third.
        const cv::Rect whole(0, 0, image.cols, image.rows);
        cv::Rect area = whole;
        cv::Point aim(image.cols / 2, image.rows / 2);
        if (const auto geometry = sidecarGeometry(file)) {
            const cv::Rect inside = geometry->area & whole;
            if (inside.width > 10 && inside.height > 6) {
                area = inside;
                aim = cv::Point(geometry->aim.x + geometry->aim.width / 2,
                                geometry->aim.y + geometry->aim.height / 2);
                fromSidecar++;
            }
        }
        const cv::Point aimInArea(aim.x - area.x, aim.y - area.y);
        cv::Mat search = image(area);

        const auto frameStart = Clock::now();
        // SCMINER_MAX_LABELS sweeps what the live scan uses (SCAN_CANDIDATES)
        static const int maxLabels = DEV_ENV("SCMINER_MAX_LABELS")
                                         ? std::atoi(DEV_ENV("SCMINER_MAX_LABELS")) : 12;
        auto labels = capture.locateSignatureLabels(search, maxLabels);
        std::stable_sort(labels.begin(), labels.end(), [&](const cv::Rect& a, const cv::Rect& b) {
            const cv::Point ca(a.x + a.width / 2, a.y + a.height / 2);
            const cv::Point cb(b.x + b.width / 2, b.y + b.height / 2);
            return cv::norm(ca - aimInArea) < cv::norm(cb - aimInArea);
        });
        const double locateMs = msSince(frameStart);
        double readMs = 0.0, ocrMs = 0.0, lineMs = 0.0;
        std::optional<int> value;
        std::string how;
        cv::Rect where;   // which candidate the value came from
        std::string detail;   // ...and the measurements it was judged by
        std::string unknownGood;   // a clean row the table refused, if any
        // The second reader's word on it, for the line: "nn agrees 68000 97/91 55ms"
        auto opinionNote = [](const SCMiner::TextLineReader::Opinion& o) {
            std::ostringstream n;
            n << " nn " << SCMiner::TextLineReader::name(o.verdict);
            if (o.value) n << " " << o.value;
            n << " " << static_cast<int>(std::lround(o.line.mean * 100)) << "/"
              << static_cast<int>(std::lround(o.line.least * 100)) << " " << static_cast<int>(o.line.ms) << "ms";
            return n.str();
        };
        int linesLeft = SCMiner::TextLineReader::FALLBACK_CANDIDATES;
        // What the second reader tries once the glyph reader has tried every
        // candidate (as live - see Application::processScanWithGUI)
        struct LineTry {
            cv::Rect label;
            SCMiner::GlyphReader::Reading reading;
            std::optional<int> cutter;
        };
        std::vector<LineTry> lineTries;

        // Same budget the live scan uses (Application.cpp, SECOND_CHANCE_CANDIDATES)
        int nearestLeft = 3;
        for (const auto& label : labels) {
            cv::Mat crop = search(label);
            // Where the locator saw the row inside this crop
            cv::Rect rowHint;
            if (auto row = capture.rowOf(label)) {
                rowHint = cv::Rect(row->x - label.x, row->y - label.y, row->width, row->height);
            }

            SCMiner::GlyphReader::Reading reading;
            const auto readStart = Clock::now();
            auto glyphValue = reader.read(crop, &reading, isSignature, &known, isSum,
                                          rowHint.area() > 0 ? &rowHint : nullptr,
                                          nearestLeft-- > 0);
            readMs += msSince(readStart);
            // SCMINER_PIPE_CANDS=1: every candidate as the reader saw it, one line each, in
            // the frame's pixels - for laying another recogniser over the very same crops
            static const bool listCandidates = DEV_ENV("SCMINER_PIPE_CANDS") != nullptr;
            if (listCandidates) {
                std::cout << "  cand " << (label.x + area.x) << "," << (label.y + area.y) << " " << label.width << "x"
                          << label.height << " pin " << static_cast<int>(std::lround(reading.pinScore * 100))
                          << " marker " << (reading.markerLikely ? 1 : 0) << " value " << (glyphValue ? *glyphValue : 0);
                if (rowHint.area() > 0) {
                    std::cout << " row " << (label.x + area.x + rowHint.x) << "," << (label.y + area.y + rowHint.y) << " "
                              << rowHint.width << "x" << rowHint.height;
                }
                if (reading.digitBox.area() > 0) {
                    std::cout << " digits " << (label.x + area.x + reading.digitBox.x) << ","
                              << (label.y + area.y + reading.digitBox.y) << " " << reading.digitBox.width << "x"
                              << reading.digitBox.height;
                }
                std::cout << " glyphs " << reading.glyphCount << " raw ";
                for (int d : reading.digits) std::cout << (d < 0 ? '?' : static_cast<char>('0' + d));
                if (reading.digits.empty()) std::cout << "-";
                std::cout << " wm " << static_cast<int>(std::lround(reading.weakest)) << "/"
                          << static_cast<int>(std::lround(reading.mean));
                if (reading.plate.area() > 0) {
                    std::cout << " plate " << (label.x + area.x + reading.plate.x) << ","
                              << (label.y + area.y + reading.plate.y) << " " << reading.plate.width << "x"
                              << reading.plate.height;
                }
                std::cout << std::endl;
            }
            if (glyphValue) {
                // A structurally clean row that spells no table value: what the
                // table filter alone is holding back. Counted to know what
                // opening the table to two-ore sums would let through.
                if (SCMiner::GlyphReader::goodEnough(reading) &&
                    !analyzer.isKnownSignature(*glyphValue) && unknownGood.empty()) {
                    std::ostringstream u;
                    u << " unknown-good " << *glyphValue << " pin "
                      << static_cast<int>(std::lround(reading.pinScore * 100))
                      << (reading.separator ? " sep+" : " sep-") << " w/m "
                      << static_cast<int>(std::lround(reading.weakest)) << "/"
                      << static_cast<int>(std::lround(reading.mean))
                      << " dc/pc " << static_cast<int>(std::lround(reading.digitChroma)) << "/"
                      << static_cast<int>(std::lround(reading.pinChroma))
                      << (reading.repaired ? " rep" : "") << " at " << label.x << "," << label.y
                      << " " << label.width << "x" << label.height;
                    unknownGood = u.str();
                }
                // One ore or one field, base times count, on the reader's own
                // bar; anything the table only reaches by adding two things
                // - or not at all - on the strict one (see strictlyGood).
                // The distance labels of the LAMP clips ("4.1km", "535.2m")
                // landed on ore-plus-field splits at weakest 80-85.
                std::string exactName; int exactCount = 0;
                const bool single = analyzer.isExactMatch(*glyphValue, exactName, exactCount);
                const bool sum = !single && analyzer.isKnownSignature(*glyphValue);
                if (SCMiner::GlyphReader::goodEnough(reading) &&
                    (single || (sum && SCMiner::GlyphReader::wellPinned(reading)) ||
                     (reading.unknown && SCMiner::GlyphReader::strictlyGood(reading))) &&
                    !SCMiner::GlyphReader::ambiguousValue(reading, isSum, SCMiner::GlyphReader::ambiguityLead())) {
                    value = *glyphValue;
                    how = reading.unknown ? "unknown" : (reading.grid ? "grid" : "glyphs");
                    where = label + area.tl();   // in the frame's pixels
                    {
                        std::ostringstream d;
                        d << " pin " << static_cast<int>(std::lround(reading.pinScore * 100))
                          << (reading.separator ? " sep+" : " sep-")
                          << " w/m " << static_cast<int>(std::lround(reading.weakest)) << "/"
                          << static_cast<int>(std::lround(reading.mean))
                          << " lead " << static_cast<int>(std::lround(reading.minMargin))
                          << " dc/pc " << static_cast<int>(std::lround(reading.digitChroma)) << "/"
                          << static_cast<int>(std::lround(reading.pinChroma))
                          << " gap " << static_cast<int>(std::lround(reading.commaGap * 100))
                          << (reading.repaired ? " rep" : "")
                          << (reading.pinSlid ? " slid" : "");
                        detail = d.str();
                    }
                    // The second reader reads the same row, as in the live
                    // scan. Agreeing, it backs the value; reading another
                    // table value, it overrules it - live, that value then
                    // has to come back before it is shown (Application.cpp)
                    if (textLines.ready()) {
                        const auto opinion = textLines.judge(search, label.tl(), reading, *glyphValue, lineKnown);
                        lineMs += opinion.line.ms;
                        detail += opinionNote(opinion);
                        if (opinion.verdict == SCMiner::TextLineReader::Verdict::Agrees) {
                            how = "agreed";
                        } else if (opinion.verdict == SCMiner::TextLineReader::Verdict::Differs ||
                                   opinion.verdict == SCMiner::TextLineReader::Verdict::Doubts) {
                            const bool overrule = opinion.verdict == SCMiner::TextLineReader::Verdict::Differs;
                            if (holdOnDisagree) {
                                std::ostringstream d;
                                d << " held: glyphs " << *glyphValue << " vs line " << opinion.value << detail;
                                detail = d.str();
                                value.reset();
                                how = "held";
                            } else if (overrule) {
                                value = opinion.value;
                                how = "overruled";
                            } else {
                                how = "doubted";   // the glyph reader's value, which stands (only noted live)
                            }
                        }
                    }
                    break;
                }
            }
            if (!reading.markerLikely || reading.pinScore < SCMiner::GlyphReader::OCR_MIN_PIN) continue;
            // A marker the glyph reader could not read, kept for the second
            // reader - the first few such candidates only
            if (textLines.ready() && linesLeft > 0 && reading.digitBox.area() > 0) {
                linesLeft--;
                lineTries.push_back({label, reading, glyphValue});
            }
            // As in the live loop: Tesseract only confirms a cutter value
            // that is a signature, so it is not asked about anything else
            if (!analyzer.isKnownSignature(reading.value)) continue;

            // Tesseract is opt-in (SCMINER_OCR=1). Measured on 924 live
            // frames and the 74-crop benchmark: 0 of 68 live readings and
            // none of the 72 benchmark reads needed it, while an attempt
            // cost up to 1.5 s per frame in 20-40 Tesseract calls.
            static const bool useOcr = DEV_ENV("SCMINER_OCR") != nullptr;
            if (!useOcr || !ocrReady) continue;
            const auto ocrStart = Clock::now();
            auto renderings = capture.preprocessCandidates(crop);
            auto ocrValue = ocr.recognizeBest(renderings, policy);
            ocrMs += msSince(ocrStart);
            if (ocrValue && ocr.getLastConfidence() >= SCMiner::MIN_OCR_CONFIDENCE &&
                analyzer.isKnownSignature(*ocrValue) && *ocrValue == reading.value) {
                value = *ocrValue;
                how = "ocr";
                where = label;
                {
                    std::ostringstream d;
                    std::string raw = ocr.getLastRawText();
                    for (char& c : raw) if (c == '\n' || c == '\r' || c == '"') c = ' ';
                    d << " pin " << static_cast<int>(std::lround(reading.pinScore * 100))
                      << " conf " << static_cast<int>(std::lround(ocr.getLastConfidence()))
                      << " cutter " << reading.value << " w/m " << static_cast<int>(std::lround(reading.weakest))
                      << "/" << static_cast<int>(std::lround(reading.mean))
                      << " raw \"" << raw << "\"";
                    detail = d.str();
                }
                break;
            }
        }

        // Nothing the glyph reader would stand behind anywhere: the second
        // reader reads the marker-like rows it kept
        if (!value && !noFallback) {
            for (const auto& tried : lineTries) {
                const auto opinion = textLines.judge(search, tried.label.tl(), tried.reading, std::nullopt, lineKnown);
                lineMs += opinion.line.ms;
                if (opinion.verdict != SCMiner::TextLineReader::Verdict::Reads) continue;
                value = opinion.value;
                how = "neural";
                where = tried.label + area.tl();
                std::ostringstream d;
                d << " pin " << static_cast<int>(std::lround(tried.reading.pinScore * 100)) << " cutter "
                  << (tried.cutter ? *tried.cutter : 0) << " w/m "
                  << static_cast<int>(std::lround(tried.reading.weakest)) << "/"
                  << static_cast<int>(std::lround(tried.reading.mean)) << " glyphs " << tried.reading.glyphCount
                  << opinionNote(opinion);
                detail = d.str();
                break;
            }
        }

        std::cout << std::filesystem::path(file).filename().string()
                  << " -> " << labels.size() << " candidate(s), ";
        if (value) {
            std::cout << *value << " (" << how << ") at " << where.x << "," << where.y << " "
                      << where.width << "x" << where.height << detail;
            read++;
            tally[*value]++;
        } else {
            std::cout << "nothing";
        }
        std::cout << unknownGood;
        const double frameMs = msSince(frameStart);
        std::cout << " [ms " << static_cast<int>(frameMs) << ": locate " << static_cast<int>(locateMs)
                  << " read " << static_cast<int>(readMs) << " ocr " << static_cast<int>(ocrMs) << " nn "
                  << static_cast<int>(lineMs) << "]";
        std::cout << std::endl;
        totalLocate += locateMs;
        totalRead += readMs;
        totalOcr += ocrMs;
        totalLines += lineMs;
        totalFrame += frameMs;
        frames++;
    }

    if (frames > 0) {
        std::cout << "\naverage ms per frame: " << static_cast<int>(totalFrame / frames)
                  << " (locate " << static_cast<int>(totalLocate / frames)
                  << ", read " << static_cast<int>(totalRead / frames)
                  << ", ocr " << static_cast<int>(totalOcr / frames)
                  << ", second reader " << static_cast<int>(totalLines / frames) << ")" << std::endl;
        const auto& p = SCMiner::GlyphReader::profile();
        auto per = [&](double ms) { return static_cast<int>(ms / frames); };
        std::cout << "  locator: tophat " << per(p.locTophatMs) << " rows " << per(p.locRowsMs)
                  << " probes " << per(p.locProbeMs) << " (" << p.probes / frames << " probes/frame, "
                  << p.extracts / frames << " extracts)" << std::endl;
        std::cout << "  reader: " << p.reads / frames << " reads/frame, analyse " << per(p.analyseMs)
                  << " rows " << per(p.rowsMs) << " (of which doubled " << per(p.doubledMs)
                  << ") whole " << per(p.wholeMs) << " grid " << per(p.gridMs) << " (" << p.grids / frames
                  << " grids/frame)" << std::endl;
        std::cout << "  pin scoring: " << per(p.pinMs) << " ms/frame over " << p.pins / frames
                  << " strips/frame" << std::endl;
        std::cout << "  locator fallback (colour/dark): rows " << per(p.locFallbackRowsMs) << " probes "
                  << per(p.locFallbackProbeMs) << std::endl;
        SCMiner::reportReaderStages(std::cout, frames);
        std::cout << "  probe stages: analyse " << per(p.probeAnalyseMs) << " rows " << per(p.probeRowsMs)
                  << " whole " << per(p.probeWholeMs) << std::endl;
    }
    std::cout << "\n" << read << " of " << files.size() << " frames produced a value";
    if (fromSidecar > 0) std::cout << " (" << fromSidecar << " searched on the app's own area)";
    if (!tally.empty()) {
        std::cout << ":";
        for (const auto& [value, count] : tally) std::cout << " " << value << " x" << count;
    }
    std::cout << std::endl;
    return 0;
}


// A recorded picture made over the way another screen or other light would
// show it (SCMINER_REFINERY_AUGMENT), for the refinery bench: the terminals
// were recorded at 5120x1440 in three lights, and the reader has to stand
// everything else. Steps run in turn, ';' between them - scale=0.75,
// zoom=1.3 (nearer), shrink=0.6 (further off), crop=16:9 (the middle of
// it), tint=1.15,1,0.8 (red, green, blue), gain=0.7, gamma=1.3, blur=1.0,
// jpeg=70, rotate=0.5 (degrees), noise=6.
cv::Mat augmented(const cv::Mat& frame, const std::string& spec) {
    cv::Mat out = frame.clone();
    std::stringstream steps(spec);
    std::string step;
    while (std::getline(steps, step, ';')) {
        const size_t eq = step.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = step.substr(0, eq), value = step.substr(eq + 1);
        const double v = std::atof(value.c_str());
        if (name == "scale" && v > 0) {
            cv::resize(out, out, cv::Size(), v, v, v < 1 ? cv::INTER_AREA : cv::INTER_CUBIC);
        } else if (name == "zoom" && v > 1) {
            // nearer: the middle of it, made as large as the whole
            const cv::Size inner(static_cast<int>(std::lround(out.cols / v)), static_cast<int>(std::lround(out.rows / v)));
            const cv::Rect middle((out.cols - inner.width) / 2, (out.rows - inner.height) / 2, inner.width, inner.height);
            const cv::Mat part = out(middle).clone();   // (never resized into itself)
            cv::resize(part, out, out.size(), 0, 0, cv::INTER_CUBIC);
        } else if (name == "shrink" && v > 0 && v < 1) {
            // further off: the whole of it smaller, in the middle of the same picture
            cv::Mat smaller, canvas = cv::Mat::zeros(out.size(), out.type());
            cv::resize(out, smaller, cv::Size(), v, v, cv::INTER_AREA);
            smaller.copyTo(canvas(cv::Rect((out.cols - smaller.cols) / 2, (out.rows - smaller.rows) / 2, smaller.cols, smaller.rows)));
            out = canvas;
        } else if (name == "crop") {
            const size_t colon = value.find(':');
            if (colon == std::string::npos) continue;
            const double aspect = std::atof(value.substr(0, colon).c_str()) / std::atof(value.substr(colon + 1).c_str());
            const int w = (std::min)(out.cols, static_cast<int>(std::lround(out.rows * aspect)));
            out = out(cv::Rect((out.cols - w) / 2, 0, w, out.rows)).clone();
        } else if (name == "tint") {
            double r = 1, g = 1, b = 1;
            if (std::sscanf(value.c_str(), "%lf,%lf,%lf", &r, &g, &b) != 3) continue;
            std::vector<cv::Mat> ch;
            cv::split(out, ch);
            ch[0].convertTo(ch[0], -1, b);
            ch[1].convertTo(ch[1], -1, g);
            ch[2].convertTo(ch[2], -1, r);
            cv::merge(ch, out);
        } else if (name == "gain") {
            out.convertTo(out, -1, v);
        } else if (name == "gamma" && v > 0) {
            cv::Mat lut(1, 256, CV_8U);
            for (int i = 0; i < 256; i++) lut.at<uchar>(i) = cv::saturate_cast<uchar>(255.0 * std::pow(i / 255.0, v));
            cv::LUT(out, lut, out);
        } else if (name == "blur" && v > 0) {
            cv::GaussianBlur(out, out, cv::Size(), v);
        } else if (name == "jpeg") {
            std::vector<uchar> buf;
            cv::imencode(".jpg", out, buf, {cv::IMWRITE_JPEG_QUALITY, static_cast<int>(v)});
            out = cv::imdecode(buf, cv::IMREAD_COLOR);
        } else if (name == "rotate") {
            const cv::Mat m = cv::getRotationMatrix2D(cv::Point2f(out.cols / 2.0f, out.rows / 2.0f), v, 1.0);
            const cv::Mat before = out.clone();
            cv::warpAffine(before, out, m, out.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        } else if (name == "noise" && v > 0) {
            cv::Mat wide, grain(out.size(), CV_16SC3);
            cv::RNG rng(7);
            rng.fill(grain, cv::RNG::NORMAL, 0, v);
            out.convertTo(wide, CV_16SC3);
            wide += grain;
            wide.convertTo(out, CV_8UC3);
        }
    }
    return out;
}

int runRefineryShare(const std::string& picture, const std::string& service) {
    SCMiner::TextLineReader* lines = panelLines();
    if (!lines) return 1;
    const cv::Mat frame = cv::imread(picture, cv::IMREAD_COLOR);
    if (frame.empty()) {
        std::cout << "could not read " << picture << std::endl;
        return 1;
    }
    const std::vector<SCMiner::RefineryBar> bars = SCMiner::findRefineryBars(frame);
    const SCMiner::RefineryScreen screen = SCMiner::readRefineryScreen(frame, bars, *lines);
    std::string station = SCMiner::refineryStationNamed(screen.stationText, "");
    for (const char* system : {"Pyro", "Nyx", "Stanton"}) {
        if (station.empty()) station = SCMiner::refineryStationNamed(screen.stationText, system);
    }
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(picture, ec);
    SCMiner::RefineryShare::Reading r;
    r.station = station;
#ifdef _WIN32
    const auto writtenAt = std::chrono::clock_cast<std::chrono::system_clock>(written);
#else
    // clock_cast is GCC 13's; the Linux package is built with GCC 11 (see
    // Application::setPlaceOverride)
    const auto writtenAt = std::chrono::file_clock::to_sys(written);
#endif
    r.observed = std::chrono::duration_cast<std::chrono::seconds>(writtenAt.time_since_epoch()).count();
    r.capacity = screen.capacity;
    r.workload = screen.workload;
    for (const SCMiner::RefineryPanel& panel : screen.panels) {
        if (panel.kind != SCMiner::RefineryBar::Band::Setup || !panel.whole()) continue;
        for (const SCMiner::RefiningMethod& m : SCMiner::REFINING_METHODS) {
            if (panel.method == m.name) r.method = m.key;
        }
        r.cost = panel.cost;
        r.seconds = panel.seconds;
        for (const SCMiner::RefineryRow& row : panel.rows) {
            if (row.inert || row.material.empty() || row.quantity <= 0 || row.yield < 0) continue;
            r.items.push_back({row.material, row.quantity, row.quality, row.yield});
        }
        break;
    }
    if (station.empty() || (r.capacity < 0 && r.workload < 0 && r.items.empty())) {
        std::cout << "nothing to send: station '" << screen.stationText << "', capacity " << r.capacity
                  << ", workload " << r.workload << std::endl;
        return 1;
    }
    const std::string body = SCMiner::RefineryShare::body(r, BEDROCK_VERSION, 41002, "LIVE");
    std::cout << body << std::endl;
    SCMiner::HttpAnswer answer;
    SCMiner::httpsPost(service + "/v1/refinery/observations", {"Content-Type: application/json"}, body, answer);
    std::cout << "-> " << answer.status << " " << answer.body << (answer.why.empty() ? "" : " (" + answer.why + ")")
              << std::endl;
    // ...and the stations as the service hands them back, read as the app reads them
    std::string stations, why;
    std::map<std::string, SCMiner::RefineryShare::StationState> states;
    if (!SCMiner::httpsGet(service + "/v1/refinery/stations", {}, stations, why) ||
        !SCMiner::RefineryShare::readStations(stations, states, why)) {
        std::cout << "stations: " << why << std::endl;
        return 1;
    }
    for (const auto& [name, s] : states) {
        std::cout << "  " << name << ": capacity " << s.capacity << " %, workload " << s.workload << " ("
                  << SCMiner::refineryWorkloadName(s.workload) << "), observed " << s.observed << ", "
                  << s.sources << " source(s)" << (s.confirmed ? ", confirmed" : "") << std::endl;
    }
    return answer.status >= 200 && answer.status < 300 ? 0 : 1;
}

int runRefinery(const std::vector<std::string>& files, const std::string& drawDir) {
    SCMiner::TextLineReader* lines = panelLines();
    if (!lines) return 1;
    const char* augment = std::getenv("SCMINER_REFINERY_AUGMENT");
    // every n-th picture only (SCMINER_BENCH_EVERY), for the long runs
    const char* everyEnv = std::getenv("SCMINER_BENCH_EVERY");
    const int every = everyEnv ? (std::max)(1, std::atoi(everyEnv)) : 1;
    int index = -1;
    if (!drawDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(drawDir, ec);
    }
    int pictures = 0, withBars = 0, wholePanels = 0, panels = 0;
    double findMs = 0, readMs = 0;
    // the notice is looked for on every picture, panels or none
    for (const std::string& file : files) {
        if (++index % every != 0) continue;
        cv::Mat frame = cv::imread(file, cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        if (augment) frame = augmented(frame, augment);
        pictures++;
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<SCMiner::RefineryBar> bars = SCMiner::findRefineryBars(frame, std::getenv("SCMINER_EVERY_BAR") != nullptr);
        const double f = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        findMs += f;
        std::cout << std::filesystem::path(file).filename().string() << ": " << bars.size() << " bars, find "
                  << (int)std::lround(f) << " ms";
        if (!bars.empty()) withBars++;
        const SCMiner::RefineryScreen screen = SCMiner::readRefineryScreen(frame, bars, *lines, true);
        if (bars.empty()) {
            std::cout << (screen.delivered ? "  WORK ORDER DELIVERED" : "") << std::endl;
            continue;
        }
        readMs += screen.ms;
        std::cout << ", read " << (int)std::lround(screen.ms) << " ms" << (screen.delivered ? "  WORK ORDER DELIVERED" : "")
                  << std::endl;
        std::cout << "  station '" << screen.stationText << "' capacity " << screen.capacity << " % workload "
                  << screen.workload << " (" << SCMiner::refineryWorkloadName(screen.workload) << ")" << std::endl;
        for (size_t i = 0; i < screen.panels.size(); i++) {
            const SCMiner::RefineryPanel& p = screen.panels[i];
            panels++;
            if (p.whole()) wholePanels++;
            const SCMiner::RefineryBar* bp = nullptr;
            for (const SCMiner::RefineryBar& b : bars) {
                if (b.bar == p.bar) bp = &b;
            }
            if (!bp) continue;
            const SCMiner::RefineryBar& b = *bp;
            const char* kind = p.kind == SCMiner::RefineryBar::Band::Setup        ? "SETUP"
                               : p.kind == SCMiner::RefineryBar::Band::Processing ? "PROCESSING"
                               : p.kind == SCMiner::RefineryBar::Band::Completed  ? "COMPLETED"
                                                                                  : "none";
            std::cout << "  " << kind << " bar "
                      << b.bar.x << "," << b.bar.y << " " << b.bar.width << "x" << b.bar.height << " band bgr "
                      << (int)b.bandColour[0] << "," << (int)b.bandColour[1] << "," << (int)b.bandColour[2]
                      << (b.theme == SCMiner::RefineryBar::Theme::Pyro ? " pyro" : "") << " | order " << p.order << " method '" << p.method << "' (" << p.methodText << ") manifest "
                      << p.manifest << " to refine " << p.toRefine << " cost " << p.cost << " seconds " << p.seconds
                      << " yield so far " << p.yieldSoFar << (p.whole() ? " WHOLE" : "") << (p.note.empty() ? "" : " | " + p.note)
                      << std::endl;
            for (const SCMiner::RefineryRow& r : p.rows) {
                std::cout << "    [" << (r.inert ? std::string("inert") : r.material.empty() ? "?" : r.material) << " <"
                          << r.text << "> q " << r.quality << " qty " << r.quantity << " yield " << r.yield << " todo "
                          << r.toDo << " done " << r.done << "]" << std::endl;
            }
        }
        if (!drawDir.empty()) {
            cv::Mat shown = frame.clone();
            for (const SCMiner::RefineryBar& b : bars) {
                cv::rectangle(shown, b.bar, b.band == SCMiner::RefineryBar::Band::Setup ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 200, 255), 2);
            }
            cv::imwrite((std::filesystem::path(drawDir) / std::filesystem::path(file).filename()).string(), shown);
        }
    }
    std::cout << pictures << " pictures, " << withBars << " with bars, " << panels << " panels, " << wholePanels
              << " whole; find " << (pictures ? findMs / pictures : 0) << " ms, read "
              << (withBars ? readMs / withBars : 0) << " ms per picture" << std::endl;
    return 0;
}

int runRefineryReplay(const std::string& dir, const std::string& station, long long start, double frameSeconds,
                      int every) {
    SCMiner::TextLineReader* lines = panelLines();
    if (!lines) return 1;
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string ext = entry.path().extension().string();
        if (ext == ".jpg" || ext == ".png") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    SCMiner::RefineryOrderBook book;
    // SCMINER_REFINERY_BOOK: a book to start from - orders placed before the
    // pictures, to be seen done and collected in them
    if (const char* from = std::getenv("SCMINER_REFINERY_BOOK")) {
        std::cout << "starting from " << from << ": " << (book.load(from) ? book.orders().size() : 0) << " orders"
                  << std::endl;
    }
    // ...and the notice looked for as the app does (Application::lookForRefinery)
    long long lastPanels = -1000;
    for (size_t i = 0; i < files.size(); i += (size_t)(std::max)(1, every)) {
        const cv::Mat frame = cv::imread(files[i], cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        const long long taken = start + (long long)std::llround(i * frameSeconds);
        const std::vector<SCMiner::RefineryBar> bars = SCMiner::findRefineryBars(frame);
        const bool notice = book.anyDone(taken);
        const SCMiner::RefineryScreen read = bars.empty() && !(notice && taken - lastPanels < 30)
                                                 ? SCMiner::RefineryScreen()
                                                 : SCMiner::readRefineryScreen(frame, bars, *lines, notice);
        if (!read.panels.empty()) lastPanels = taken;
        const size_t had = book.orders().size();
        const bool changed = book.take(station, read, taken);
        std::cout << std::filesystem::path(files[i]).filename().string() << ": " << read.panels.size() << " panels"
                  << (read.delivered ? ", WORK ORDER DELIVERED" : "")
                  << (changed ? (book.orders().size() > had   ? "  -> new order"
                                 : book.orders().size() < had ? "  -> collected"
                                                              : "  -> updated")
                              : "")
                  << std::endl;
    }
    std::cout << "\nbook:" << std::endl;
    for (const SCMiner::RefineryOrder& o : book.sorted()) {
        std::cout << "  " << o.station << " #" << o.number << " placed " << o.placed << " due " << o.due << " (in "
                  << (o.due - start) << " s from the first frame) method '" << o.method << "' cost " << o.cost
                  << " seconds " << o.seconds << (o.completed > 0 ? " COMPLETED" : "") << (o.complete() ? " WHOLE" : "")
                  << " |";
        for (const SCMiner::RefineryOrder::Part& p : o.parts) {
            std::cout << " " << p.material << " " << p.quality << " " << p.quantity << " -> " << p.yield;
        }
        std::cout << std::endl;
    }
    // ...and SCMINER_REFINERY_SAVE: where to keep the book for the next set
    if (const char* to = std::getenv("SCMINER_REFINERY_SAVE")) book.save(to);
    return 0;
}
