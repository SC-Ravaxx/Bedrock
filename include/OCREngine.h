#pragma once

#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <opencv2/opencv.hpp>
// Only a developer build links Tesseract (BEDROCK_TESSERACT, CMakeLists.txt);
// in a release, which carries no tessdata, the engine is there but never
// starts
#ifdef BEDROCK_TESSERACT
#include <tesseract/baseapi.h>
#endif
#include "MiningData.h"

namespace SCMiner {

class OCREngine {
public:
    OCREngine();
    ~OCREngine();

    // Initializes Tesseract with the given configuration
    bool initialize(const OCRConfig& config);

    // Recognizes numbers in the image and returns the detected value
    std::optional<int> recognizeNumber(const cv::Mat& image);

    // One line of text, as Tesseract read it, with nothing interpreted. The
    // signature reader wants a number and only a number; the scan panel
    // wants "14.71 SCU" and "BEXALITE (RAW)", where the decimal point and
    // the letters are the content rather than noise. `whitelist` is the set
    // of characters Tesseract may answer with, and it is put back afterwards
    // so the signature path keeps its digits-only rule.
    std::string recognizeLine(const cv::Mat& image, const char* whitelist);

    // Domain knowledge the consensus vote uses to judge raw readings.
    struct ReadingPolicy {
        // Repairs a misread to its unique table value (identity if unknown).
        // Applied BEFORE voting so renderings that fail on different digits
        // still converge on the same candidate.
        std::function<int(int)> canonicalize;
        // Rates how well a value fits the signature table.
        std::function<double(int)> bonus;
    };

    // Runs recognizeNumber on several preprocessed candidates (one per
    // contrast channel/variant) and returns the value they agree on.
    // Confidence and raw text afterwards refer to the winning candidate.
    std::optional<int> recognizeBest(const std::vector<cv::Mat>& candidates,
                                     const ReadingPolicy& policy = {});

    // Returns the confidence of the last recognition (0-100)
    double getLastConfidence() const;

    // How many renderings agreed on the winning value in the last
    // recognizeBest call. High agreement means a single frame can be trusted,
    // which matters for signatures that only flash up briefly.
    int getLastAgreement() const;

    // Checks whether the engine is initialized
    bool isInitialized() const;

    // Returns the raw text of the last recognition
    std::string getLastRawText() const;

    // Lets the caller pump its window between renderings: a full pass over
    // twenty of them holds the thread for a second otherwise
    void setYield(std::function<void()> f) { yield_ = std::move(f); }

private:
#ifdef BEDROCK_TESSERACT
    std::unique_ptr<tesseract::TessBaseAPI> tessApi_;
#endif
    bool initialized_ = false;
    double lastConfidence_ = 0.0;
    int lastAgreement_ = 0;
    std::string lastRawText_;
    OCRConfig config_;
    std::function<void()> yield_;

    // Strips non-numeric characters from the recognized text
    std::string extractNumbers(const std::string& text);
};

} // namespace SCMiner
