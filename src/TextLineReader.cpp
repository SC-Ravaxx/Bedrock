#include "TextLineReader.h"
#include "DevSwitch.h"
#include "EmbeddedData.h"

#include <mutex>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace SCMiner {

namespace {

// The recogniser's input: PaddleOCR's recognition shape. The model file has
// it fixed (tools/fix_onnx_shape.py); a line narrower than this is padded
// out, a wider one squeezed, as PaddleOCR itself does.
constexpr int LINE_H = 48;
constexpr int LINE_W = 320;

// Room around the glyph reader's row, in row heights. On 230 labelled
// frames 0.3 / 0.6 / 0.3 read 197; the locator's own row box, which often
// takes the pin in, read 173, and 0.2 on the left with 1.0 on the right 191.
constexpr double ROOM_LEFT = 0.3;
constexpr double ROOM_RIGHT = 0.6;
constexpr double ROOM_ABOVE_BELOW = 0.3;

// A cutter row this many figures longer than the number read means the
// recogniser lost a digit: 4,000 read where the row held 44,000 (three of
// the recogniser's six wrong values on the labelled frames). A difference of
// one is normal - the pin's tip or a comma blob counted in.
constexpr int MAX_ROW_SURPLUS = 1;

// When the glyph reader counts as having hesitated over its value: its least
// sure digit led the runner-up by less than this (Reading::minMargin, 0..100).
// Among its accepted readings that is one in twenty-five - lead 0 or 1 - and
// it is where its wrong values were; the neural reader's wrong overrules all
// came against a lead of 2 to 7.
constexpr double HESITANT_LEAD_DEFAULT = 1.5;

// How sure the recogniser has to be of a line to overrule a glyph reader
// that did not hesitate. Measured, see judge().
constexpr double OVERRULE_MEAN = 0.93;
// ...and a bar the workshop can move, to weigh "it overrules only a glyph
// reader that hesitated" against "it always overrules" on the same frames
// (SCMINER_TEXTLINE_LEAD=100 makes every disagreement an overrule)
inline double hesitantLead() {
    static const double bar = DEV_ENV("SCMINER_TEXTLINE_LEAD")
                                  ? std::atof(DEV_ENV("SCMINER_TEXTLINE_LEAD")) : HESITANT_LEAD_DEFAULT;
    return bar;
}

// The least mean certainty a line may come with: none. On the labelled
// frames its wrong values came at 71-92 % and right ones from 63 % up, so a
// floor costs more readings than it saves; a value only this reader saw has
// to come back in later scans before it is shown, and that is the guard.
constexpr double MIN_MEAN = 0.0;

}  // namespace

struct TextLineReader::Impl {
    cv::dnn::Net net;
    std::vector<std::string> keys;   // index 1.. of the model's output; 0 is CTC's blank
    bool ready = false;
    // The net runs one line at a time: the loadout off the HUD is read on a
    // thread of its own (Application::readLoadoutOnHud), the panels and the
    // signatures on the window's
    std::mutex running;
};

TextLineReader::TextLineReader() : impl_(std::make_unique<Impl>()) {}
TextLineReader::~TextLineReader() = default;

bool TextLineReader::ready() const { return impl_->ready; }

bool TextLineReader::load(const void* model, size_t modelBytes, const std::string& keys) {
    impl_->ready = false;
    if (!model || modelBytes == 0 || keys.empty()) return false;
    try {
        impl_->net = cv::dnn::readNetFromONNX(static_cast<const char*>(model), modelBytes);
        if (impl_->net.empty()) return false;
        impl_->net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        impl_->net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    } catch (const cv::Exception& e) {
        std::cerr << "[textline] the model could not be built: " << e.what() << std::endl;
        return false;
    }
    impl_->keys.clear();
    size_t start = 0;
    while (start < keys.size()) {
        size_t end = keys.find('\n', start);
        if (end == std::string::npos) end = keys.size();
        std::string key = keys.substr(start, end - start);
        if (!key.empty() && key.back() == '\r') key.pop_back();
        impl_->keys.push_back(key);
        start = end + 1;
    }
    // PaddleOCR's alphabet ends in a space the file does not spell out
    impl_->keys.push_back(" ");
    impl_->ready = true;
    // The first pass through a net allocates everything it will need; paid
    // here rather than on the first marker the player looks at
    try {
        read(cv::Mat(LINE_H, 120, CV_8UC3, cv::Scalar::all(40)));
    } catch (const cv::Exception& e) {
        std::cerr << "[textline] the model does not run: " << e.what() << std::endl;
        impl_->ready = false;
    }
    return impl_->ready;
}

bool TextLineReader::loadEmbedded() {
    // The pair the program carries (TEXTLINE_MODEL, TEXTLINE_KEYS): a
    // resource on Windows, assembled in on Linux - see EmbeddedData.cpp
    const unsigned char* model = nullptr;
    const unsigned char* keys = nullptr;
    size_t modelBytes = 0, keyBytes = 0;
    if (!embeddedResource("TEXTLINE_MODEL", &model, &modelBytes) ||
        !embeddedResource("TEXTLINE_KEYS", &keys, &keyBytes)) {
        return false;
    }
    return load(model, modelBytes, std::string(reinterpret_cast<const char*>(keys), keyBytes));
}

TextLineReader::Line TextLineReader::read(const cv::Mat& input) {
    Line line;
    if (!impl_->ready || input.empty()) return line;
    const auto start = std::chrono::steady_clock::now();
    cv::Mat bgr;
    if (input.channels() == 4) {
        cv::cvtColor(input, bgr, cv::COLOR_BGRA2BGR);
    } else if (input.channels() == 1) {
        cv::cvtColor(input, bgr, cv::COLOR_GRAY2BGR);
    } else {
        bgr = input;
    }
    const int width = std::clamp(static_cast<int>(std::lround(bgr.cols * static_cast<double>(LINE_H) / bgr.rows)),
                                 1, LINE_W);
    cv::Mat sized;
    cv::resize(bgr, sized, cv::Size(width, LINE_H), 0, 0, cv::INTER_CUBIC);
    if (width < LINE_W) {
        // Mid-grey: zero once normalised, which is what PaddleOCR pads with
        cv::Mat padded;
        cv::copyMakeBorder(sized, padded, 0, 0, 0, LINE_W - width, cv::BORDER_CONSTANT, cv::Scalar::all(127.5));
        sized = padded;
    }
    // (x/255 - 0.5) / 0.5, the normalisation the model was trained with;
    // the channels stay in OpenCV's BGR order, which is PaddleOCR's too
    const cv::Mat blob =
        cv::dnn::blobFromImage(sized, 1.0 / 127.5, cv::Size(), cv::Scalar::all(127.5), false, false);
    // (held until the output is read: it is the net's own memory)
    std::lock_guard<std::mutex> one(impl_->running);
    impl_->net.setInput(blob);
    const cv::Mat out = impl_->net.forward();   // 1 x steps x classes, already a softmax

    // CTC, greedy: the likeliest class at every step, repeats merged, blanks dropped
    const int steps = out.size[1], classes = out.size[2];
    const float* p = out.ptr<float>();
    int previous = -1;
    double sum = 0.0;
    int kept = 0;
    line.least = 1.0;
    for (int t = 0; t < steps; t++) {
        const float* row = p + static_cast<size_t>(t) * classes;
        const int best = static_cast<int>(std::max_element(row, row + classes) - row);
        if (best != 0 && best != previous && best - 1 < static_cast<int>(impl_->keys.size())) {
            line.text += impl_->keys[best - 1];
            sum += row[best];
            line.least = (std::min)(line.least, static_cast<double>(row[best]));
            kept++;
        }
        previous = best;
    }
    line.mean = kept > 0 ? sum / kept : 0.0;
    if (kept == 0) line.least = 0.0;
    line.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return line;
}

std::optional<TextLineReader::Number> TextLineReader::signatureIn(const std::string& input) {
    std::string t = input;
    // The alphabet has the full-width comma and full stop as well
    for (const auto& [wide, plain] : {std::pair<const char*, char>{"\xEF\xBC\x8C", ','},
                                      std::pair<const char*, char>{"\xEF\xBC\x8E", '.'}}) {
        for (size_t at; (at = t.find(wide)) != std::string::npos;) t.replace(at, std::strlen(wide), 1, plain);
    }
    // The shapes a recogniser trained on words gives the HUD's digits
    for (char& c : t) {
        if (c == 'o' || c == 'O') c = '0';
        else if (c == 'l' || c == 'I' || c == '|') c = '1';
    }
    auto digit = [&](size_t i) { return i < t.size() && t[i] >= '0' && t[i] <= '9'; };
    for (size_t i = 0; i < t.size(); i++) {
        if (!digit(i) || (i > 0 && digit(i - 1))) continue;
        size_t j = i;
        while (digit(j)) j++;
        const size_t lead = j - i;
        if (lead >= 1 && lead <= 3 && j < t.size() && (t[j] == ',' || t[j] == '.') && digit(j + 1) &&
            digit(j + 2) && digit(j + 3) && !digit(j + 4)) {
            Number n;
            n.value = std::stoi(t.substr(i, lead) + t.substr(j + 1, 3));
            n.digits = static_cast<int>(lead) + 3;
            return n;
        }
    }
    return std::nullopt;
}

cv::Rect TextLineReader::lineAround(const cv::Rect& digitBox, const cv::Size& crop) {
    const double h = digitBox.height;
    const int left = static_cast<int>(std::lround(h * ROOM_LEFT));
    const int right = static_cast<int>(std::lround(h * ROOM_RIGHT));
    const int vertical = static_cast<int>(std::lround(h * ROOM_ABOVE_BELOW));
    const cv::Rect around(digitBox.x - left, digitBox.y - vertical, digitBox.width + left + right,
                          digitBox.height + 2 * vertical);
    return around & cv::Rect(0, 0, crop.width, crop.height);
}

TextLineReader::Opinion TextLineReader::judge(const cv::Mat& source, const cv::Point& origin,
                                              const GlyphReader::Reading& reading, std::optional<int> accepted,
                                              const std::function<bool(int)>& known) {
    Opinion opinion;
    if (!impl_->ready || source.empty() || reading.digitBox.area() <= 0) return opinion;
    const cv::Rect where = lineAround(reading.digitBox + origin, source.size());
    if (where.width < 8 || where.height < 6) return opinion;
    opinion.line = read(source(where));
    static const double minMean = DEV_ENV("SCMINER_TEXTLINE_MEAN") ? std::atof(DEV_ENV("SCMINER_TEXTLINE_MEAN"))
                                                                    : MIN_MEAN;
    if (opinion.line.mean < minMean) return opinion;
    const auto number = signatureIn(opinion.line.text);
    if (!number || !known(number->value)) return opinion;
    if (reading.glyphCount - number->digits > MAX_ROW_SURPLUS) return opinion;
    opinion.value = number->value;
    if (!accepted) {
        // A row the glyph reader read too, and this one reads as the same number
        // with one more figure in front of it, is the pin: the marker's own icon
        // sits left of the digits and a recogniser trained on words makes a 1 or a
        // 2 of it (26,800 for 6,800). None of the 41 lone readings on moving
        // footage has this shape, so the guard costs nothing.
        const std::string mine = std::to_string(number->value);
        const std::string theirs = reading.value > 0 ? std::to_string(reading.value) : std::string();
        if (!theirs.empty() && mine.size() == theirs.size() + 1 &&
            mine.compare(mine.size() - theirs.size(), theirs.size(), theirs) == 0) {
            opinion.value = 0;
            return opinion;
        }
        opinion.verdict = Verdict::Reads;
    } else {
        opinion.verdict = settle(reading, *accepted, number->value, opinion.line.mean);
    }
    return opinion;
}

TextLineReader::Verdict TextLineReader::settle(const GlyphReader::Reading& reading, int accepted, int lineValue,
                                               double lineMean) {
    if (lineValue == accepted) return Verdict::Agrees;
    const bool hesitated = reading.minMargin < hesitantLead();
    const bool digitLost = std::to_string(accepted).size() != std::to_string(lineValue).size();
    // ...or this reader is sure of what it read. On moving footage the glyph
    // reader is wrong with a clear lead often enough - a 6 read as an 8 looks
    // the same on every frame of that marker, so the scan repeating the reading
    // only confirms the mistake. On thirteen disagreements in moving MOLE
    // footage the recogniser's own certainty separates the two: right at
    // 92-98 %, wrong at 87-92 %. With this, all sixteen disagreements on that
    // footage come out right, and the three on still pictures where the glyph
    // reader was right and this one was not are still left alone.
    static const double sureMean = DEV_ENV("SCMINER_TEXTLINE_OVERRULE_MEAN")
                                       ? std::atof(DEV_ENV("SCMINER_TEXTLINE_OVERRULE_MEAN"))
                                       : OVERRULE_MEAN;
    const bool sure = lineMean >= sureMean;
    return hesitated || digitLost || sure ? Verdict::Differs : Verdict::Doubts;
}

const char* TextLineReader::name(Verdict v) {
    switch (v) {
        case Verdict::Agrees: return "agrees";
        case Verdict::Differs: return "differs";
        case Verdict::Doubts: return "doubts";
        case Verdict::Reads: return "reads";
        default: return "none";
    }
}

}  // namespace SCMiner
