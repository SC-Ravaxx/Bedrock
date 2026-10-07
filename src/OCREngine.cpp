#include "OCREngine.h"
#include "DevSwitch.h"
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <map>

namespace SCMiner {

#ifdef BEDROCK_TESSERACT
OCREngine::OCREngine() : tessApi_(std::make_unique<tesseract::TessBaseAPI>()) {
}
#else
OCREngine::OCREngine() = default;
#endif

OCREngine::~OCREngine() {
#ifdef BEDROCK_TESSERACT
    if (tessApi_) {
        tessApi_->End();
    }
#endif
}

bool OCREngine::initialize(const OCRConfig& config) {
    config_ = config;
#ifndef BEDROCK_TESSERACT
    // Not in this build: every reading goes on without it, as in release
    // packages, which carry no tessdata
    return false;
#else
    // Initialize Tesseract
    int result = tessApi_->Init(config.tessdataPath.c_str(), config.language.c_str());

    if (result != 0) {
        std::cerr << "Error: could not initialize Tesseract!" << std::endl;
        std::cerr << "Make sure the tessdata folder is next to the executable." << std::endl;
        std::cerr << "Expected path: " << config.tessdataPath << std::endl;
        return false;
    }

    // Tune OCR settings for digits. SCMINER_PSM overrides the segmentation
    // mode for experiments (8 = single word, 7 = single line, 13 = raw line).
    int pageSegMode = config.pageSegMode;
    if (const char* override = DEV_ENV("SCMINER_PSM")) {
        pageSegMode = std::atoi(override);
    }
    tessApi_->SetPageSegMode(static_cast<tesseract::PageSegMode>(pageSegMode));

    // Digits only. The comma glyph is already removed from the image by the
    // preprocessing (height filter), and allowing ",." here makes Tesseract
    // segment whole words differently, wrecking otherwise clean reads.
    tessApi_->SetVariable("tessedit_char_whitelist", "0123456789");

    // Disable dictionaries; we only read single numbers
    tessApi_->SetVariable("load_system_dawg", "false");
    tessApi_->SetVariable("load_freq_dawg", "false");

    initialized_ = true;
    std::cout << "Tesseract OCR initialized successfully." << std::endl;

    return true;
#endif
}

std::string OCREngine::recognizeLine(const cv::Mat& image, const char* whitelist) {
    if (!initialized_ || image.empty()) return std::string();

    cv::Mat gray;
    if (image.channels() == 4) {
        cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    } else if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image;
    }

#ifndef BEDROCK_TESSERACT
    (void)gray;
    (void)whitelist;
    return std::string();
#else
    tessApi_->SetVariable("tessedit_char_whitelist", whitelist);
    tessApi_->SetPageSegMode(tesseract::PSM_SINGLE_LINE);
    tessApi_->SetImage(gray.data, gray.cols, gray.rows, 1, static_cast<int>(gray.step[0]));
    char* out = tessApi_->GetUTF8Text();
    std::string text = out ? std::string(out) : std::string();
    delete[] out;
    lastConfidence_ = tessApi_->MeanTextConf();
    lastRawText_ = text;

    // back to what the signature path expects
    tessApi_->SetVariable("tessedit_char_whitelist", "0123456789");
    tessApi_->SetPageSegMode(static_cast<tesseract::PageSegMode>(config_.pageSegMode));

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
#endif
}

std::optional<int> OCREngine::recognizeNumber(const cv::Mat& image) {
    if (!initialized_) {
        std::cerr << "OCREngine is not initialized!" << std::endl;
        return std::nullopt;
    }

    if (image.empty()) {
        std::cerr << "Empty image passed to OCR!" << std::endl;
        return std::nullopt;
    }

    // Prepare the image for Tesseract
    cv::Mat gray;
    if (image.channels() == 4) {
        cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    } else if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image;
    }

#ifndef BEDROCK_TESSERACT
    lastRawText_.clear();
    lastConfidence_ = 0.0;
#else
    // Feed the image into Tesseract
    tessApi_->SetImage(gray.data, gray.cols, gray.rows, 1, static_cast<int>(gray.step[0]));

    // Recognize text
    char* outText = tessApi_->GetUTF8Text();
    lastRawText_ = outText ? std::string(outText) : std::string();
    delete[] outText;

    // Get the confidence
    lastConfidence_ = tessApi_->MeanTextConf();
#endif

    // Extract digits only
    std::string numbers = extractNumbers(lastRawText_);

    if (numbers.empty()) {
        std::cerr << "No digits recognized. Raw text: '" << lastRawText_ << "'" << std::endl;
        return std::nullopt;
    }

    // Signatures are at most 6 digits. Longer strings mean the rendering was
    // noise that Tesseract shredded into digits - reject before converting,
    // which also keeps the value inside int range.
    if (numbers.size() > 6) {
        std::cerr << "Implausible digit count (" << numbers.size()
                  << "): '" << numbers << "'" << std::endl;
        return std::nullopt;
    }

    // Check the confidence
    if (lastConfidence_ < config_.minConfidence) {
        std::cerr << "Low confidence: " << lastConfidence_ << "%" << std::endl;
        std::cerr << "Recognized: " << numbers << " (uncertain)" << std::endl;
    }

    try {
        int value = std::stoi(numbers);
        return value;
    } catch (const std::exception& e) {
        std::cerr << "Could not convert '" << numbers << "' to int: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::optional<int> OCREngine::recognizeBest(const std::vector<cv::Mat>& candidates,
                                            const ReadingPolicy& policy) {
    // Consensus vote instead of "highest single score": the correct value
    // tends to be read by SEVERAL channel/variant renderings, while misreads
    // scatter. A lone high-confidence error therefore cannot outvote the
    // value that multiple independent renderings agree on.
    struct Tally {
        double score = 0.0;      // accumulated weight across renderings
        int votes = 0;
        double bestConfidence = 0.0;
        std::string rawText;
    };
    std::map<int, Tally> tally;

    // One pass over every rendering with the currently selected segmentation
    // mode. Returns true once a rendering hit an exact table value often
    // enough to settle the question.
    auto runPass = [&](const char* passName) {
        bool settled = false;
        for (const auto& candidate : candidates) {
            if (yield_) yield_();
        auto raw = recognizeNumber(candidate);
        if (!raw) continue;

        // Repair before voting: the HUD glow closes the counters of 6/9/0, so
        // different renderings fail on different digits. Mapping each reading
        // onto its unique table value lets them agree.
        const int value = policy.canonicalize ? policy.canonicalize(*raw) : *raw;

        // Weight one reading: OCR confidence, a bonus for a plausible digit
        // count (signatures are 4-6 digits; 1-2 digit reads are noise), and
        // a bonus for matching the in-game signature table.
        const int digits = static_cast<int>(std::to_string(value).length());
        double weight = lastConfidence_;
        if (digits >= 4 && digits <= 6) weight += 20.0;
        else if (digits <= 2) weight -= 30.0;
        if (policy.bonus) weight += policy.bonus(value);
        if (weight < 1.0) weight = 1.0;

        // Set SCMINER_OCR_TRACE=1 to see what every rendering read - the way
        // to tell a bad rendering from a bad vote
        static const bool trace = DEV_ENV("SCMINER_OCR_TRACE") != nullptr;
        if (trace) {
            std::cerr << "  [trace] rendering " << (&candidate - candidates.data())
                      << ": raw=" << *raw << " value=" << value
                      << " conf=" << static_cast<int>(lastConfidence_)
                      << " weight=" << static_cast<int>(weight) << std::endl;
        }

        Tally& entry = tally[value];
        entry.score += weight;
        entry.votes++;
        if (lastConfidence_ > entry.bestConfidence) {
            entry.bestConfidence = lastConfidence_;
            entry.rawText = lastRawText_;
        }

        // Three independent renderings agreeing on an exact table value is
        // as good as it gets - stop early to keep the live loop responsive
        if (entry.votes >= 3 && policy.bonus && policy.bonus(value) >= 40.0) {
            settled = true;
            break;
        }
        }
        (void)passName;
        return settled;
    };

    runPass("primary");

    // Second opinion. The tuned mode treats the readout as a single word,
    // which is right when the digits are cleanly separated. In harsh daylight
    // the glow welds them into a block that only the block-of-text mode can
    // segment - that mode is worse overall (measured 84% vs 90%), so it only
    // gets asked when the primary pass found nothing matching a real
    // signature. Costs nothing on frames that already worked.
    bool haveTableHit = false;
    double bestSoFar = 0.0;
    for (const auto& entry : tally) {
        if (policy.bonus && policy.bonus(entry.first) >= 40.0) haveTableHit = true;
        bestSoFar = (std::max)(bestSoFar, entry.second.bestConfidence);
    }
    // Only ask when the first pass saw text at all. An empty sky produces
    // nothing but single-digit noise at 1-10% confidence, and letting the
    // permissive mode pile more votes onto that turns silence into a
    // confident wrong answer.
#ifdef BEDROCK_TESSERACT
    if (!haveTableHit && bestSoFar >= 35.0 && initialized_) {
        tessApi_->SetPageSegMode(tesseract::PSM_SINGLE_BLOCK);
        runPass("block");
        tessApi_->SetPageSegMode(static_cast<tesseract::PageSegMode>(config_.pageSegMode));
    }
#endif

    if (tally.empty()) {
        lastAgreement_ = 0;
        return std::nullopt;
    }

    auto winner = std::max_element(tally.begin(), tally.end(),
        [](const auto& a, const auto& b) { return a.second.score < b.second.score; });

    // A lone, low-confidence reading that fits no known signature is what an
    // empty sky or a glare-filled capture produces - report nothing rather
    // than a number the caller might believe
    const bool implausible = policy.bonus && policy.bonus(winner->first) < 0.0;
    if (implausible || (winner->second.votes <= 1 && winner->second.bestConfidence < 25.0)) {
        lastAgreement_ = 0;
        return std::nullopt;
    }

    lastConfidence_ = winner->second.bestConfidence;
    lastRawText_ = winner->second.rawText;
    lastAgreement_ = winner->second.votes;
    return winner->first;
}

int OCREngine::getLastAgreement() const {
    return lastAgreement_;
}

double OCREngine::getLastConfidence() const {
    return lastConfidence_;
}

bool OCREngine::isInitialized() const {
    return initialized_;
}

std::string OCREngine::getLastRawText() const {
    return lastRawText_;
}

std::string OCREngine::extractNumbers(const std::string& text) {
    // Concatenate all digits (dropping separators). Tesseract sometimes
    // hallucinates spaces inside one number, so splitting here would tear
    // valid reads apart - the preprocessing already guarantees that only ONE
    // spatial digit group reaches the OCR (unrelated readouts like compass
    // marks are removed before recognition).
    std::string result;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            result += c;
        }
    }
    return result;
}

} // namespace SCMiner
