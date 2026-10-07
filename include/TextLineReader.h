#pragma once

#include <opencv2/core.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "GlyphReader.h"

namespace SCMiner {

// The second reader: a small neural text recogniser run on the row of
// digits the glyph reader cut, as a second opinion on its value and as a
// reader of its own where the glyph reader gives up.
//
// The model is the recognition half of PaddleOCR - PP-OCRv4, 10.8 MB,
// Apache-2.0 - run through OpenCV's dnn module, so nothing is added beside
// the OpenCV Bedrock already carries. It reads one line of text, whatever
// the font, the glow or the colour of the plate; it does not find lines.
// Finding is what the glyph reader's locator and pin check do well, so the
// two are put together that way round: the glyph reader says WHERE the
// digits are (Reading::digitBox), this one says what they read.
//
// Measured on 230 labelled frames (MOLE, Prospector, Golem, every
// light): the glyph reader alone 176 right, 5 wrong, 49 not read. Every
// one of its 5 wrong values the recogniser read correctly, and on 23 of
// the frames it could not read the recogniser could. Its own mistakes are
// of two kinds - a lost leading digit (4,000 for 44,000), which the digit
// count below catches, and 6/8 or 3/2 in a hard light - and it is the
// second kind that the scan's repetition rule is there for: a value only
// this reader saw is never shown off a single frame.
class TextLineReader {
public:
    // How many candidates of one scan it reads where the glyph reader
    // accepted nothing. A line costs 50-60 ms on six threads, 75-95 with the
    // game running beside it; on the labelled frames every value it found
    // came from the first or second marker-like candidate.
    static constexpr int FALLBACK_CANDIDATES = 2;

    TextLineReader();
    ~TextLineReader();
    TextLineReader(const TextLineReader&) = delete;
    TextLineReader& operator=(const TextLineReader&) = delete;

    // The model and its alphabet (one character per line, PaddleOCR's
    // ppocr_keys_v1.txt). False when OpenCV could not build the net - the
    // scan then simply goes on with the glyph reader alone.
    bool load(const void* model, size_t modelBytes, const std::string& keys);
    // The pair the exe carries (resources TEXTLINE_MODEL, TEXTLINE_KEYS)
    bool loadEmbedded();
    bool ready() const;

    struct Line {
        std::string text;     // what it read, UTF-8
        double mean = 0.0;    // mean certainty of the characters kept, 0..1
        double least = 0.0;   // the least certain of them
        double ms = 0.0;      // the net's own time
    };
    // The whole picture read as one line of text
    Line read(const cv::Mat& bgr);

    // A signature in a line of text: "68,000" or "68.000". The HUD always
    // sets the thousands separator, so a bare run of digits - a distance,
    // a clock, a pin read as "2" in front of the number - does not count.
    struct Number {
        int value = 0;
        int digits = 0;   // the figures it is written with
    };
    static std::optional<Number> signatureIn(const std::string& text);

    // Where to read inside the candidate crop: the glyph reader's row with a
    // little room around it - less on the left, where the pin is, which the
    // recogniser otherwise reads as a "2" in front of the number.
    static cv::Rect lineAround(const cv::Rect& digitBox, const cv::Size& crop);

    // What it makes of one candidate, in the terms the scan decides by
    enum class Verdict {
        None,      // nothing it would stand behind
        Agrees,    // it read the glyph reader's value
        Differs,   // it read another table value, where the glyph reader hesitated: it overrules
        Doubts,    // it read another table value, where the glyph reader was sure: it does not
        Reads,     // it read a table value where the glyph reader accepted none
    };
    struct Opinion {
        Verdict verdict = Verdict::None;
        int value = 0;
        Line line;
    };
    // `source` is the picture the candidate was cut from and `origin` where
    // the candidate's top-left corner sat in it. The line is cut from the
    // source, so the room around the digits does not depend on how tightly
    // the locator cut the candidate: two to five pixels lost at a crop's
    // edge can turn 16,780 into 18,780.
    // `accepted` is the glyph reader's value when it accepted one; `known`
    // says what the tables know. A number is only taken when it is a table
    // value and has no digit fewer than the row the glyph reader cut, less
    // one for a stray the cutter counted in (the pin's tip, a comma blob).
    //
    // Where the two differ, the glyph reader's own certainty decides who
    // is heard. Of fourteen disagreements on file the neural reader was
    // right in all eight where the glyph reader had hesitated - a digit
    // whose best and second-best template scored within a point
    // (Reading::minMargin), the 6-or-8 and 3-or-9 its own mistakes come
    // from - or had a digit fewer (7,200 for 72,000), and wrong in all four
    // where the glyph reader had been sure: 9s read as 8 or 0 on a plate
    // washed out to white, a 6 as 8. So it overrules the first kind
    // (Differs) and only casts doubt on the second (Doubts).
    Opinion judge(const cv::Mat& source, const cv::Point& origin, const GlyphReader::Reading& reading,
                  std::optional<int> accepted, const std::function<bool(int)>& known);

    // How a disagreement is settled, without a picture in it: one copy of
    // the rule, which judge() uses and the self-test can put figures into.
    // The second reader overrules a glyph reading when it is sure of its own
    // line, when the glyph reader hesitated over a digit, or when the two
    // do not even agree on how many digits there are; otherwise it is noted
    // and the glyph reader keeps its value.
    static Verdict settle(const GlyphReader::Reading& reading, int accepted, int lineValue, double lineMean);

    // For the log: "agrees", "differs", ...
    static const char* name(Verdict v);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace SCMiner
