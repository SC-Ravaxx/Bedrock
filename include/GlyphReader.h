#pragma once

#include <cstdlib>
#include <opencv2/opencv.hpp>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace SCMiner {

// For the bench: what the reader's inner stages cost since the last report,
// per frame (SCMINER_READER_STAGES=1)
void reportReaderStages(std::ostream& out, int frames);


// Reads the digits of a signature marker without OCR.
//
// The marker layout never varies: a coloured plate (its hue follows the ship
// UI colour), the pin glyph on the left, white digits on the right. On that
// plate the digits are the only bright neutral shapes, so a threshold learnt
// from the plate itself yields every digit as its own connected component -
// the glow that welds them together for a brightness-based OCR is coloured
// and stays in the background. Each component is normalised to a small
// bitmap and matched against templates harvested from real captures of the
// game's HUD font. The pin next to the row is matched the same way: it is
// what tells the marker from every text label drawn in the same font.
class GlyphReader {
public:
    static constexpr int GLYPH_W = 12;
    static constexpr int GLYPH_H = 20;
    static constexpr int PIN_W = 16;
    static constexpr int PIN_H = 24;
    // How far the best digit must beat the runner-up (0..100). Zero: the pin
    // check guards against letters being read as digits, and as a gate this
    // only costs real readings - at 13 px an 8 and a 6 score alike, and it is
    // the repair against the signature table that settles those, not a
    // confidence margin.
    static constexpr double MIN_MARGIN = 0.0;
    // The strip left of the digit row must correlate with a pin template at
    // least this well (0..1) for the row to count as a signature marker.
    //
    // Measured on 924 frames of live footage (12 recordings, two HUD
    // colours): the 51 correct readings carried pins of 63-99 % (median 87);
    // the 13 wrong readings that were not a marker at all - "ATMO", "NOISE",
    // "H-FUEL", "CPLD", chat lines, a compass heading, all read into table
    // values - sat at 51-82 %, nine of them below 60. A marker whose pin
    // matched worse than 65 % was, in both cases seen, a marker that had
    // lost a digit ("16,000" read as 6,000 at 55 and 64 %). The one correct
    // reading this bar costs (a 6,370 at 63 %) is a single frame of a marker
    // that read at 76-85 % in the frames around it; a missed frame is
    // harmless live, a wrong number is not.
    // A bar a developer build lets the environment move (SCMINER_*); a
    // release build takes the figure as written, and the name is not in it
#ifdef BEDROCK_DEV_TOOLS
    static double bar(const char* name, double fallback) {
        const char* v = std::getenv(name);
        return v ? std::atof(v) : fallback;
    }
#define BEDROCK_BAR(name, fallback) bar(name, fallback)
#else
#define BEDROCK_BAR(name, fallback) (fallback)
#endif
    inline static const double PIN_MIN_NCC = BEDROCK_BAR("SCMINER_PIN_MIN", 0.65);

    // What the LOCATOR demands - the same bar. It only has room for a
    // handful of candidates, and a label that sneaks in takes the slot the
    // real marker needed.
    inline static const double PIN_MIN_NCC_STRICT = BEDROCK_BAR("SCMINER_PIN_MIN", 0.65);

    // A row WITHOUT its thousands comma is only believed beside a pin that
    // leaves no doubt. Measured: 50 of the 51 correct live readings showed
    // the comma and 9 of the 13 wrong ones did not; the sun-blown captures
    // whose comma drowns in the glare carry pins of 88 % and up.
    inline static const double PIN_MIN_NCC_LONE = BEDROCK_BAR("SCMINER_PIN_LONE", 0.85);

    // The grid reconstructs a value out of the table and a plate, so it
    // must be surer of the plate than the cutter: on live footage it read
    // HUD words into table values seven times at pins of 51-70 %, against
    // one real marker at 83 %.
    inline static const double GRID_MIN_PIN = BEDROCK_BAR("SCMINER_GRID_PIN", 0.75);

    // Tesseract is only asked about a pinned row, and it may only CONFIRM
    // the value the cutter itself nearly read (see the call sites). Its own
    // guesses over a rejected row are how "CPLD" became 4,210 and a chat
    // line 39,000 - at pins of 78-82 %, the same pin a real 8,480 carried
    // that the cutter had read at weakest 78 and Tesseract then confirmed.
    // Two readers agreeing is evidence; one reader guessing is not.
    inline static const double OCR_MIN_PIN = BEDROCK_BAR("SCMINER_OCR_PIN", 0.65);

    struct Glyph {
        cv::Mat bitmap;   // GLYPH_H x GLYPH_W, CV_32F, 0..1
        cv::Rect box;     // where it sat in the input image
        double aspect;    // original width / height
        double fill;      // share of the box the strokes cover
    };

    // What the segmentation did, for --glyph diagnostics
    // How good a reading has to be before it counts as a signature.
    //
    // Measured across the 50 labelled captures: every CORRECT reading comes
    // back at weakest 99 and mean 99. The wrong ones - sun-glare lumps welded
    // into digit shapes, HUD text, the mining laser's own readout - land at
    // weakest 59 to 77. A floor of 50 lets every one of those through, and a
    // wrong number shown confidently is worse than no number at all.
    //
    // A reading that needed a hesitant glyph resolved against the signature
    // table has to be cleaner still: that repair turns a blur into a table
    // value, which is exactly how glare produces confident nonsense.
    static constexpr double MIN_WEAKEST = 80.0;
    static constexpr double MIN_MEAN = 88.0;
    static constexpr double MIN_WEAKEST_REPAIRED = 88.0;
    static constexpr double MIN_MEAN_REPAIRED = 93.0;

    // The grid reader's bar. Its scores are the same template scores, but its
    // real guard is the LEAD: how far, per digit, the winning table value
    // stands above the runner-up value. Two values one digit apart score the
    // same everywhere but at that window; if the window cannot tell them
    // apart the reading is a coin toss and must not be shown, however good
    // the mean looks.
    static constexpr double GRID_MIN_MEAN = 78.0;
    static constexpr double GRID_MIN_WEAKEST = 62.0;
    static constexpr double GRID_MIN_LEAD = 4.0;
    // ...and the same question asked of the cutter. MIN_MARGIN above is off
    // because on the canopy an 8 and a 6 score alike at 13 px and the
    // signature table settles them. On the Drake instrument panel the table
    // settles them WRONG: 20,800 is as much a table value as 10,800, and a
    // reading whose runner-up digit scores the same as the winner would show
    // as EXACT. Measured over 112 locator candidates from fifteen labelled
    // Golem frames, after the weakest/mean bars above: 35 wrong readings got
    // through, 24 of them with a lead of 0 or 1 - and every one of the 40
    // right readings had a lead of 2 or more. Two is therefore free on that
    // material; four (the grid's bar) would cost six right readings and
    // remove all but three wrong ones. The canopy fixture reads at 6 either
    // way.
    //
    // Two bars, because the two fonts sit on opposite sides of it. The one
    // real canopy reading on file - 60,000 off a Prospector over an ice
    // planet - carries a lead of 1 and is right, so the canopy keeps its bar
    // at 0. The panel gets 2.
    inline static const double CUT_MIN_LEAD = BEDROCK_BAR("SCMINER_CUT_LEAD", 0.0);
    inline static const double CUT_MIN_LEAD_MFD = BEDROCK_BAR("SCMINER_CUT_LEAD_MFD", 2.0);

    // The pair discriminant (see scoreDigits): asked when two digits score
    // within PAIR_TIE of each other, trusted from PAIR_MIN_CONFIDENCE (a
    // class mean sits at 1), and the winner then leads by at least
    // PAIR_LEAD times that confidence. A pair is learnt from a set that
    // holds PAIR_MIN_TEMPLATES of each digit.
    inline static const double PAIR_TIE = BEDROCK_BAR("SCMINER_PAIR_TIE", 0.06);
    inline static const double PAIR_MIN_CONFIDENCE = BEDROCK_BAR("SCMINER_PAIR_CONF", 0.4);
    static constexpr double PAIR_LEAD = 0.04;
    static constexpr int PAIR_MIN_TEMPLATES = 4;
    static constexpr double PAIR_VARIANCE_FLOOR = 0.02;

    struct Trace {
        cv::Rect plate;
        double scale = 1.0;    // the picture was analysed at this magnification
        int pad = 0;           // ...and with this margin added around it first
        std::string channel;   // which digit channel produced the result
        int threshold = 0;
        double pinScore = 0.0; // best pin correlation left of the row
        double plateLuma = 0.0; // the plate's brightness as captured, 0..255
        double plateStd = 0.0;  // texture inside the plate
        bool separator = false; // thousands comma found where the game draws it

        bool wantPicture = false;   // set by a caller that will look at the picture
        cv::Mat picture;       // input with plate and glyph boxes drawn (when wanted)
    };

    // Loads "<digit>_<tag>_a<aspect*100>.png" digit templates and
    // "pin_<tag>.png" pin templates; returns how many digit templates
    int loadTemplates(const std::string& dir);
    // The Drake instrument panel draws the signature in a wider face on a
    // dark screen. Its templates are a set of their own: mixed into one
    // pool, the wide panel zeros steal a canopy 6 and a real 60,000 comes
    // out as 80,000. Loaded from <dir>_mfd beside the exe when that exists,
    // else from the GLYPHS_MFD resource; empty means "no such font", and a
    // dark crop then falls back to the canopy set.
    int loadMfdTemplates(const std::string& dir);
    bool hasMfdTemplates() const { return !mfdTemplates_.empty(); }
    // Whether the templates came out of the exe rather than a folder
    bool usesEmbeddedTemplates() const { return embeddedTemplates_; }
    int templateCount() const { return static_cast<int>(templates_.size()); }
    static int pinTemplateCount() { return static_cast<int>(pinTemplates_.size()); }

    // The digit shapes on the plate, left to right. Empty when there is no
    // plate, nothing on it segments into a plausible row of digits, or (with
    // requirePin) no pin stands beside the row.
    static std::vector<Glyph> extractGlyphs(const cv::Mat& bgr, Trace* trace = nullptr,
                                            bool requirePin = true,
                                            double minPinScore = PIN_MIN_NCC,
                                            const cv::Rect* rowHint = nullptr);

    // True when the capture shows the pin at the left end of its plate -
    // used to allow the OCR fallback on markers whose digits did not
    // segment, while text and instruments stay silent
    static bool markerLikely(const cv::Mat& bgr);

    // How much the strip left of a digit row - given as its bounding box in
    // a picture whose colourfulness and brightness are handed in - looks
    // like the map pin. The locator asks this of every row BEFORE cutting a
    // crop and running the full reader over it: the answer costs a few
    // microseconds, the probe a few milliseconds, and most rows are terrain.
    static double pinScoreForRow(const cv::Mat& chroma, const cv::Mat& bright, const cv::Rect& row);

    // The vertical resolution every template, every pixel floor and every
    // ceiling in this reader and in the locator were measured at (a 5120x1440
    // viewport). The game scales its HUD with the height of the picture: on a
    // 4K monitor the digits are half again as tall, and a plate welded by
    // glow stands above the locator's 40 px ceiling - measured on 1440p
    // footage shown at 1.5x, the pipeline read NOTHING. Rather than teaching
    // every bar a factor, a capture from any other height is brought to this
    // one before anything looks at it: the application sets the factor from
    // the game viewport, the locator and the reader scale their answers back.
    static constexpr int REFERENCE_VIEWPORT_HEIGHT = 1440;
    // viewport height / REFERENCE_VIEWPORT_HEIGHT: 1.5 on a 2160 px picture,
    // 0.75 on 1080. One value for the process - there is one game window.
    static void setHudScale(double scale);
    static double hudScale();
    // A rectangle found in a picture brought to the reference scale, mapped
    // back into the picture it was cut from - outwards, never inwards
    static cv::Rect fromReferenceScale(const cv::Rect& r, double scale);
    // Whether the reader looks at the crop as captured while only the
    // locator works at the reference scale. Measured on 1440p footage shown
    // at 1.5x (see the setting's definition for the numbers).
    static void setReadAtNativeScale(bool native);
    static bool readAtNativeScale();
    // The bar that gate applies. Judged from the locator's own row boxes it
    // is blind: real markers score 31-49 % where the probe then finds 92 %,
    // and terrain rows score up to 74 % - the locator's boxes are a different
    // height from the digit boxes the reader finds, and the pin template is
    // scaled by that height. At 0.45 a night clip drops from 22 to 6
    // readings. So the gate is OFF (0) and only reports in the trace;
    // SCMINER_PIN_GATE sets a bar to try.
    inline static const double PIN_GATE = BEDROCK_BAR("SCMINER_PIN_GATE", 0.0);
    // How many candidate rows a frame gets probed with the full reader.
    // Rows are probed longest and tallest first, and a marker is usually a
    // long, tall row - but a small, distant "25,440" whose 2 had merged
    // into its pin was a FOUR-glyph row and ranked 28th of 40 behind every
    // six-glyph line of HUD text. Twenty lost it; thirty keeps it, at up to
    // ten more probes of about 2.5 ms on a frame that has that many rows.
    inline static const int MAX_PROBES = static_cast<int>(BEDROCK_BAR("SCMINER_MAX_PROBES", 30));

    // The pin templates, PIN_H x PIN_W, CV_32F. Public because the row
    // collector is a free function that is handed the set to judge with.
    static std::vector<cv::Mat> pinTemplates_;
    // The pin as the instrument panel draws it: amber on a dark screen.
    // Its own set for the same reason the digits have one - 17 panel
    // pins in the shared pool lifted the panel markers to 80-98 % and let
    // enough HUD rows past the locator's probe that the canopy's one real
    // reading was crowded out of the candidate list (36 frames: 1 -> 0).
    static std::vector<cv::Mat> mfdPinTemplates_;

    struct Reading {
        int value = 0;
        int glyphCount = 0;        // digit shapes found, even when unread
        bool markerLikely = false; // a pin was seen (with or without a row)
        bool mfd = false;          // scored against the instrument-panel set
        double weakest = 0.0;      // lowest glyph similarity, 0..100
        double mean = 0.0;         // average glyph similarity, 0..100
        double minMargin = 0.0;    // smallest best-vs-runner-up margin, 0..100
        // The glyphs of the row the value was read from, in reading order.
        // For the harvest: extractGlyphs() hands back the FIRST pinned row
        // of the first channel, and on the instrument panel that is often
        // a row with the comma or a pin fragment counted in - five shapes
        // for four digits, skipped. read() tries every channel and keeps
        // the best, so its row is the one worth learning from.
        std::vector<Glyph> glyphs;
        // What the classifier made of each of those glyphs, before any
        // repair by the table. The harvest compares this with the label it
        // was given: shapes that are the digits in that order agree on most
        // positions, a row one place off agrees on none.
        std::vector<int> digits;
        std::vector<int> runnerUp;    // per digit, the classifier's second choice
        std::vector<double> leads;    // per digit, best minus second, 0..1
        bool repaired = false;     // a hesitant glyph was resolved via `accept`
        bool unknown = false;      // read cleanly, but no table value: the table lacks it
        bool separator = false;    // the thousands comma was seen in the row
        double pinScore = 0.0;     // best pin match beside the row, 0..1
        // The pin was only recognised once the match was allowed to slide
        // a cell or two: at the alignment the crop produced it was under
        // the bar. That is a weaker piece of evidence than a pin that sat
        // where it was looked for, so a reading carrying it has to repeat
        // itself before it is shown - see Application, which treats it
        // exactly as it treats a repaired reading.
        bool pinSlid = false;
        cv::Rect plate;            // the plate inside the crop, for tracking
        cv::Rect digitBox;         // the row the value was read from, strays included, in the crop's pixels
        bool grid = false;         // read by the grid, judged by its own bar
        // The strip beside the row the value was read from, as the pin
        // check saw it: chroma, PIN_H x PIN_W. For the pin harvest - cut in
        // the very scene the row was read in, which may have been lifted
        // or doubled; cutting it again from a fresh scene of the same crop
        // lands most strips on black plate (30 of 49).
        cv::Mat pinStrip;
        // How coloured the digit strokes are against the pin beside them,
        // 0..255 each: a marker's digits are white next to an orange pin,
        // a cockpit readout's digits are as orange as the letters beside
        // them - measured to tell "785.5" on the instrument panel from a
        // signature
        double digitChroma = 0.0;
        double pinChroma = 0.0;
        // The widest gap between neighbouring glyphs of the row, in row
        // heights - the thousands comma's gap, which the Drake face sets
        // about twice as wide as the Prospector's
        double commaGap = 0.0;
        std::string family;        // the scene family the row was scored in ("" = legacy only)
    };

    // Whether a reading clears the bar above. One place, so the live scan,
    // the offline pipeline and the benchmark can never drift apart.
    // Where the milliseconds go, accumulated over a run and printed by the
    // pipeline. The live loop is exactly as fast as these stages, so they
    // are measured rather than guessed at.
    struct Profile {
        double analyseMs = 0, rowsMs = 0, doubledMs = 0, wholeMs = 0, gridMs = 0, pinMs = 0;
        double extractMs = 0, locTophatMs = 0, locRowsMs = 0, locProbeMs = 0;
        double locFallbackRowsMs = 0, locFallbackProbeMs = 0;
        double probeAnalyseMs = 0, probeRowsMs = 0, probeWholeMs = 0;
        int reads = 0, extracts = 0, grids = 0, pins = 0, probes = 0;
    };
    static Profile& profile();

    static bool goodEnough(const Reading& reading) {
        // Structure first: a signature is a map pin beside a row of digits,
        // whichever way the digits were read
        if (reading.pinScore < PIN_MIN_NCC) return false;
        if (reading.grid) {
            // The grid finds no comma (it lays windows over the row's extent
            // and never looks for one), so a grid value stands on its pin
            // alone - and then the pin has to carry it, like a cutter row
            // without a comma. The yellow arc of the mining gauge scores 77 %
            // as a pin, and "0.00 SCU/S" beside it reads 111,000 by grid.
            if (!reading.separator && reading.pinScore < PIN_MIN_NCC_LONE) return false;
            return reading.pinScore >= GRID_MIN_PIN &&
                   reading.mean >= GRID_MIN_MEAN && reading.weakest >= GRID_MIN_WEAKEST &&
                   reading.minMargin >= GRID_MIN_LEAD;
        }
        if (!reading.separator && reading.pinScore < PIN_MIN_NCC_LONE) return false;
        // The lead bar, with one exception measured on 60 panel readings:
        // a reading whose weakest glyph is 95 and whose mean is 97 is right
        // at lead 1 as surely as at lead 5 - 15,420 and 2,000 both sat at
        // 99/99 with a lead of 1. Bar 2 alone took 43 right and 5 wrong; with
        // the exception 45 and 6.
        const bool surePanel = reading.weakest >= 95.0 && reading.mean >= 97.0;
        if (reading.minMargin < (reading.mfd ? CUT_MIN_LEAD_MFD : CUT_MIN_LEAD) && !surePanel) return false;
        return reading.weakest >= (reading.repaired ? MIN_WEAKEST_REPAIRED : MIN_WEAKEST) &&
               reading.mean >= (reading.repaired ? MIN_MEAN_REPAIRED : MIN_MEAN);
    }

    // Two table values one hesitant glyph apart - 60,000 and 80,000 on a
    // night HUD, where a 6 and an 8 are seven pixels wide - cannot be told
    // apart by the picture. True when the runner-up of any digit of the
    // reading is within `lead` of the best AND spells another value `known`
    // accepts. Judged on the finished reading by the caller: rejecting it
    // inside the reader only lets a worse row through instead (16,060 for
    // 16,000). SCMINER_AMBIGUITY sets the lead, 0 off.
    static bool ambiguousValue(const Reading& reading, const std::function<bool(int)>& known, double lead);
    static double ambiguityLead();
    // Read better than a table value needs to be: the bar for a value that
    // nothing but the picture vouches for - one the table knows only as a
    // sum of two, or not at all. Under the lamp, the distance labels under
    // ore names ("535.2m", "248.6m") read 53,520 and 24,860 - sums the
    // table happens to hold - at weakest 80-89, lead 1-9, pins of 65-76 %.
    // A real sum reads like a real value does.
    static bool strictlyGood(const Reading& reading) {
        return goodEnough(reading) && reading.weakest >= 90.0 && reading.mean >= 94.0 &&
               reading.minMargin >= 5.0 && reading.pinScore >= 0.75 && !reading.pinSlid;
    }
    // The bar for a value the table reaches only by adding two things - an
    // ore beside a field, two ores. Those are real and common: 69,080 on a
    // MOLE (Hephaestanite x6 beside ROC x11) reads at pins 91-94 and means
    // 92-96 with leads of 1-4, so the lead cannot be the bar - strictlyGood
    // rejects 17 of 20 such readings. Distance labels under the lamp read
    // as sums at pins 83-84 and means 89-90: the pin and the mean tell the
    // two apart.
    static bool wellPinned(const Reading& reading) {
        return goodEnough(reading) && reading.pinScore >= 0.88 && reading.mean >= 92.0 &&
               reading.weakest >= 84.0 && !reading.pinSlid;
    }

    // Reads the number. Returns nullopt when no row of digits was found or a
    // glyph could not be classified; glyphCount tells the two apart.
    //
    // `accept` lets the caller say which values exist in the game. At 13 px an
    // 8 and a 6 score alike, and the classifier knows exactly WHICH glyph it
    // is unsure about - so instead of guessing or giving up, the runner-up
    // digit is tried in those positions and the reading that lands on a real
    // signature wins. Trying only the runner-up where the classifier hesitated
    // is far narrower than repairing an arbitrary digit afterwards.
    // accept: what a reading may resolve to by REPAIR - one hesitant digit
    // swapped, a stray end glyph dropped. acceptPlain: what it may ALSO be
    // when taken exactly as read, the full row untouched - the two-ore sums,
    // real but rare, which a misread lands on too easily to be repaired
    // towards (see SignatureAnalyzer::isSingleSignature).
    // `rowHint`, when given, is where the caller already knows the digits
    // to be inside `bgr` - the locator's row. The thresholds are then learnt
    // over that row and the plate around it rather than over whatever else
    // the crop holds (an asteroid, the instrument panel's own text).
    // One pass of the read, with the row hint as given. read() puts a retry
    // around it - see the definition.
    std::optional<int> readOnce(const cv::Mat& input, Reading* reading,
                                const std::function<bool(int)>& accept,
                                const std::vector<int>* candidates,
                                const std::function<bool(int)>& acceptPlain,
                                const cv::Rect* rowHint) const;
    std::optional<int> read(const cv::Mat& bgr, Reading* reading = nullptr,
                            const std::function<bool(int)>& accept = {},
                            const std::vector<int>* candidates = nullptr,
                            const std::function<bool(int)>& acceptPlain = {},
                            const cv::Rect* rowHint = nullptr,
                            bool secondChance = false) const;

    // Best digit for one glyph, how similar it is (0..1) and how far ahead of
    // the runner-up digit it is (margin, 0..1)
    // `panel` scores against the instrument-panel set; read() decides that
    // for itself from the scene, a caller outside it has to say
    int classify(const Glyph& glyph, double* similarity = nullptr,
                 double* margin = nullptr, int* runnerUp = nullptr,
                 bool panel = false) const;

    // How many digit templates a set holds (0: nothing to judge by)
    int templateCount(bool panel) const;

    // How well this glyph matches each digit (0..1), written into scores[10]
    void scoreDigits(const Glyph& glyph, double scores[10]) const;

    // Reads without cutting: lays a fixed-pitch grid of windows over the digit
    // row and asks, for every value in `candidates`, how well ITS digits fit
    // THOSE windows. The fallback for rows the cutter cannot separate.
    // `panelFont`: the font the row is set in, which picks the pins and the
    // digit set - the grid has no comma of its own to tell, so the caller says
    std::optional<int> readByGrid(const cv::Mat& bgr, const std::vector<int>& candidates,
                                  const std::function<bool(int)>& accept, Reading* reading,
                                  bool panelFont) const;

    // Below this margin the classifier is treated as undecided, and at most
    // this many such positions are retried against the signature table
    // A row whose every digit scores at least this, with at least this much
    // lead over the runner-up, is read CLEANLY. Such a row that the table
    // does not know is not repaired towards the table - it is handed back
    // as read and marked unknown. "70,000" on an orange HUD, digits at
    // 95-98 % with leads of 5-24, is in no table (no field of 17.5 or 23.3,
    // salvage stops at 30), and swapping one confident digit "repairs" it
    // to 78,000, 76,000, 10,000 or 90,000. The reading is right, the table
    // is short.
    static constexpr double CLEAN_SCORE = 0.92;
    static constexpr double CLEAN_MARGIN = 0.04;
    static constexpr double AMBIGUOUS_MARGIN = 0.12;
    static constexpr size_t MAX_AMBIGUOUS = 3;

    // Drops templates that resemble another digit more than their own (a
    // wrong split that happened to give the right count). Returns how many
    // were removed; with deleteFiles their files go too.
    int pruneTemplates(bool deleteFiles);

    // Stores a glyph as a template file
    static bool saveTemplate(const Glyph& glyph, int digit,
                             const std::string& dir, const std::string& tag);

    // Cuts the pin out of a capture (left of its digit row) and stores it as
    // a pin template. Returns false when no digit row was found.
    // `expected`, when given, is the marker's value: the pin is then cut
    // beside the row the reader reads as exactly that, and nowhere else.
    // Without it the strip beside the first pinned row of the first channel
    // is taken - which files digits, text and blank plate as pins too (20 of
    // 55 panel pins), and a cockpit warning sign then scores 95 % as a pin.
    bool harvestPin(const cv::Mat& input, const std::string& dir, const std::string& tag,
                    const std::string& expected = "", const cv::Rect* rowHint = nullptr) const;

private:
    struct Template {
        int digit;
        cv::Mat bitmap;
        double aspect;
        // The bitmap with its mean taken out, and that vector's length: what
        // a normalised correlation against it needs, computed once at load
        // instead of once per glyph per template - see scoreDigits
        std::vector<float> centred;
        double norm = 0.0;
        std::string file;
    };
    std::vector<Template> templates_;      // the canopy marker, legacy set
    std::vector<Template> mfdTemplates_;   // the instrument panel

    // One linear discriminant per pair of digits, learnt from a pool's own
    // templates: every pixel weighted by how consistently the two digits
    // differ there, so the few pixels of the 8's bar count and the outline
    // both share does not. Asked only when the correlation leaves two
    // digits within PAIR_TIE of each other.
    struct PairModel {
        std::vector<float> weight;   // per pixel
        std::vector<float> middle;   // per pixel, halfway between the class means
        float halfGap = 0.0f;        // a class mean projects to +-1 once divided by this
        bool valid = false;
    };
    std::vector<PairModel> pairModels_;      // canopy set, indexed a * 10 + b with a < b
    std::vector<PairModel> mfdPairModels_;   // panel set
    // Template families. A file named "0_MB_..." belongs to family MB - a
    // MOLE's blue plate without a helmet - and "pin_MB_..." is one of its
    // pins; a name without such a token is the legacy set. A scene is
    // scored against the legacy set plus the families of its plate's colour
    // and only those: with every family in one pool the reader is confident
    // about blurred markers (a blurred 60,000 reads 42,000, 45,000, 48,000,
    // 72,000 at 91-98 % - some shape among 800 fits anything). Families by
    // the scene: B blue plate (MB, GB, PB), O orange or gold plate (MO, GO,
    // PG), N a dark plate at night (PN), W a plate washed white under the
    // lamp (ML, PW, PL, GL).
    std::map<std::string, std::vector<Template>> rawFamilies_;   // as loaded, by token
    std::map<std::string, std::vector<Template>> familyPools_;   // per scene family: legacy + its own
    std::map<std::string, std::vector<PairModel>> familyPairModels_;
    void buildFamilyPools();
    // The pool a row of a scene family is scored against; nullptr = legacy
    const std::vector<Template>* familyPool(const std::string& family) const;
    static std::string sceneFamilyOf(const std::string& token);   // "MB" -> "B", "" for legacy
    static void buildPairModels(const std::vector<Template>& pool, std::vector<PairModel>& models);
    static double projectPair(const PairModel& model, const cv::Mat& probe);   // > 0 says b
    // Which of the two a read is scoring against. Chosen once per read()
    // from the scene - a plate that had to be opened up is a panel - and
    // read by scoreDigits, which is called from too many places to hand
    // the pool through. Mutable because read() is const; null means the
    // canopy set.
    mutable const std::vector<Template>* pool_ = nullptr;
    // Fills Template::centred and Template::norm from its bitmap
    static void prepareTemplate(Template& t);
    // One template picture, from a file or from the exe; the name carries the
    // digit and the aspect ("5_..._a46.png"), "pin_*" is a pin
    bool addTemplate(const std::string& name, const cv::Mat& gray, const std::string& file);
    bool embeddedTemplates_ = false;
    bool loadingMfd_ = false;   // addTemplate routes pins to the panel set while set
    // pinTemplates_ and mfdPinTemplates_ are public - declared after MAX_PROBES
};

} // namespace SCMiner
