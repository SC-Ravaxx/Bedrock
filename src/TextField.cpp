#include "TextField.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>

namespace SCMiner {

namespace Chars {

namespace {

constexpr char32_t NO_CHARACTER = 0xFFFD;

bool isSurrogate(char32_t c) { return c >= 0xD800 && c <= 0xDFFF; }
bool isCharacter(char32_t c) { return c <= 0x10FFFF && !isSurrogate(c); }

}  // namespace

std::u32string fromUtf8(const std::string& text) {
    std::u32string out;
    out.reserve(text.size());
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        const unsigned char lead = (unsigned char)text[i];
        if (lead < 0x80) {
            out.push_back(lead);
            i++;
            continue;
        }
        // how many bytes follow, and the least the sequence may say (a
        // longer way of writing a smaller character is no UTF-8)
        int more = 0;
        char32_t c = 0, least = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            more = 1;
            c = lead & 0x1F;
            least = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            more = 2;
            c = lead & 0x0F;
            least = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            more = 3;
            c = lead & 0x07;
            least = 0x10000;
        }
        bool whole = more > 0 && i + (size_t)more < n;
        for (int k = 1; whole && k <= more; k++) {
            const unsigned char next = (unsigned char)text[i + (size_t)k];
            if ((next & 0xC0) != 0x80) whole = false;
            else c = (c << 6) | (next & 0x3F);
        }
        if (whole && c >= least && isCharacter(c)) {
            out.push_back(c);
            i += (size_t)more + 1;
        } else {
            out.push_back(NO_CHARACTER);
            i++;
        }
    }
    return out;
}

std::string toUtf8(const std::u32string& text) {
    std::string out;
    out.reserve(text.size());
    for (char32_t c : text) {
        if (!isCharacter(c)) c = NO_CHARACTER;
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u32string fromWide(const std::wstring& text) {
    std::u32string out;
    out.reserve(text.size());
    const size_t n = text.size();
    for (size_t i = 0; i < n; i++) {
        char32_t c = (char32_t)(std::uint32_t)text[i];
#if WCHAR_MAX <= 0xFFFF
        c &= 0xFFFF;
        // two halves make a character; one alone is none
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            const char32_t low = (char32_t)(std::uint32_t)text[i + 1] & 0xFFFF;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                out.push_back(0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00));
                i++;
                continue;
            }
        }
#endif
        out.push_back(isCharacter(c) ? c : NO_CHARACTER);
    }
    return out;
}

std::wstring toWide(const std::u32string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (char32_t c : text) {
        if (!isCharacter(c)) c = NO_CHARACTER;
#if WCHAR_MAX <= 0xFFFF
        if (c >= 0x10000) {
            const char32_t v = c - 0x10000;
            out.push_back((wchar_t)(0xD800 + (v >> 10)));
            out.push_back((wchar_t)(0xDC00 + (v & 0x3FF)));
            continue;
        }
#endif
        out.push_back((wchar_t)c);
    }
    return out;
}

bool isSpace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::u32string plain(const std::u32string& text, bool lines) {
    std::u32string out;
    out.reserve(text.size());
    const char32_t breakAs = lines ? U'\n' : U' ';
    for (size_t i = 0; i < text.size(); i++) {
        const char32_t c = text[i];
        if (c == U'\r') {
            if (i + 1 < text.size() && text[i + 1] == U'\n') i++;
            out.push_back(breakAs);
        } else if (c == U'\n' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029) {
            out.push_back(breakAs);
        } else if (c == U'\t') {
            out.push_back(U' ');
        } else if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) {
            // a control character: left out
        } else {
            out.push_back(isCharacter(c) ? c : NO_CHARACTER);
        }
    }
    return out;
}

std::u32string trimmed(const std::u32string& text) {
    size_t from = 0, to = text.size();
    while (from < to && isSpace(text[from])) from++;
    while (to > from && isSpace(text[to - 1])) to--;
    return text.substr(from, to - from);
}

}  // namespace Chars

namespace {

constexpr size_t UNDO_STEPS = 100;

// What a character is to the word keys: white space, a letter or digit (or
// anything beyond ASCII), another sign
int kindOf(char32_t c) {
    if (Chars::isSpace(c)) return 0;
    if (c >= 0x80 || c == U'_' || (c >= U'0' && c <= U'9') || (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z')) {
        return 1;
    }
    return 2;
}

}  // namespace

void TextField::set(const std::u32string& text) {
    text_ = Chars::plain(text, lines_);
    if (text_.size() > most_) text_.resize(most_);
    caret_ = anchor_ = text_.size();
    undo_.clear();
    redo_.clear();
    last_ = Step::None;
    changes_++;
}

void TextField::remember(Step step) {
    // letters typed one after the other are one step back, and so are
    // characters taken away one after the other
    const bool goesOn = step != Step::None && step == last_ && caret_ == lastAt_ && !marked() && !undo_.empty();
    if (!goesOn) {
        undo_.push_back({text_, caret_, anchor_});
        if (undo_.size() > UNDO_STEPS) undo_.erase(undo_.begin());
    }
    redo_.clear();
}

void TextField::changed(Step step) {
    anchor_ = caret_;
    last_ = step;
    lastAt_ = caret_;
    changes_++;
}

void TextField::takeOut(size_t from, size_t to) {
    text_.erase(from, to - from);
    caret_ = from;
}

bool TextField::insert(const std::u32string& clean, Step step) {
    const size_t held = text_.size() - (markTo() - markFrom());
    const size_t room = most_ > held ? most_ - held : 0;
    if (clean.empty() || room == 0) return false;
    const std::u32string taken = clean.substr(0, room);
    remember(step);
    if (marked()) takeOut(markFrom(), markTo());
    text_.insert(caret_, taken);
    caret_ += taken.size();
    changed(step);
    return true;
}

bool TextField::type(char32_t c) {
    const std::u32string clean = Chars::plain(std::u32string(1, c), lines_);
    // (a line break in a field of one line is no space typed)
    if (!lines_ && (c == U'\r' || c == U'\n')) return false;
    // a line break ends the run of letters that one step back takes
    return insert(clean, clean == U"\n" ? Step::None : Step::Typing);
}

bool TextField::put(const std::u32string& text) {
    return insert(Chars::plain(text, lines_), Step::None);
}

bool TextField::backspace(bool word) {
    if (marked()) return cut();
    if (caret_ == 0) return false;
    const size_t from = word ? wordBefore(caret_) : caret_ - 1;
    remember(word ? Step::None : Step::Erasing);
    takeOut(from, caret_);
    changed(word ? Step::None : Step::Erasing);
    return true;
}

bool TextField::erase(bool word) {
    if (marked()) return cut();
    if (caret_ >= text_.size()) return false;
    const size_t to = word ? wordAfter(caret_) : caret_ + 1;
    remember(word ? Step::None : Step::Erasing);
    takeOut(caret_, to);
    changed(word ? Step::None : Step::Erasing);
    return true;
}

bool TextField::cut() {
    if (!marked()) return false;
    remember(Step::None);
    takeOut(markFrom(), markTo());
    changed(Step::None);
    return true;
}

void TextField::moveTo(size_t at, bool marking) {
    caret_ = (std::min)(at, text_.size());
    if (!marking) anchor_ = caret_;
}

void TextField::left(bool marking, bool word) {
    if (!marking && marked()) {
        moveTo(markFrom(), false);
        return;
    }
    if (caret_ > 0) moveTo(word ? wordBefore(caret_) : caret_ - 1, marking);
}

void TextField::right(bool marking, bool word) {
    if (!marking && marked()) {
        moveTo(markTo(), false);
        return;
    }
    if (caret_ < text_.size()) moveTo(word ? wordAfter(caret_) : caret_ + 1, marking);
}

void TextField::markAll() {
    anchor_ = 0;
    caret_ = text_.size();
}

void TextField::markWord(size_t at) {
    at = (std::min)(at, text_.size());
    // at a word's end, or at the text's: the word before
    if ((at == text_.size() || kindOf(text_[at]) == 0) && at > 0 && kindOf(text_[at - 1]) != 0) at--;
    if (at >= text_.size() || kindOf(text_[at]) == 0) {
        moveTo(at, false);
        return;
    }
    const int kind = kindOf(text_[at]);
    size_t from = at, to = at + 1;
    while (from > 0 && kindOf(text_[from - 1]) == kind) from--;
    while (to < text_.size() && kindOf(text_[to]) == kind) to++;
    anchor_ = from;
    caret_ = to;
}

// The start of the word before a place: back over white space, then over
// the run of letters - or of signs - before it
size_t TextField::wordBefore(size_t at) const {
    while (at > 0 && kindOf(text_[at - 1]) == 0) at--;
    if (at == 0) return 0;
    const int kind = kindOf(text_[at - 1]);
    while (at > 0 && kindOf(text_[at - 1]) == kind) at--;
    return at;
}

// The start of the word after a place: on over the run the place is in,
// then over the white space behind it
size_t TextField::wordAfter(size_t at) const {
    const size_t n = text_.size();
    if (at < n && kindOf(text_[at]) != 0) {
        const int kind = kindOf(text_[at]);
        while (at < n && kindOf(text_[at]) == kind) at++;
    }
    while (at < n && kindOf(text_[at]) == 0) at++;
    return at;
}

bool TextField::undo() {
    if (undo_.empty()) return false;
    redo_.push_back({text_, caret_, anchor_});
    text_ = undo_.back().text;
    caret_ = undo_.back().caret;
    anchor_ = undo_.back().anchor;
    undo_.pop_back();
    last_ = Step::None;
    changes_++;
    return true;
}

bool TextField::redo() {
    if (redo_.empty()) return false;
    undo_.push_back({text_, caret_, anchor_});
    text_ = redo_.back().text;
    caret_ = redo_.back().caret;
    anchor_ = redo_.back().anchor;
    redo_.pop_back();
    last_ = Step::None;
    changes_++;
    return true;
}

namespace {

// How many places of text [from, limit) take no more than `room`: found in
// steps that double and then halve, so that nothing much longer than what
// fits is ever measured - a word of four thousand letters is measured a
// line at a time
size_t fitting(size_t from, size_t limit, float room, const TextWidth& width) {
    size_t fits = from, over = limit + 1;   // (limit + 1: nothing found too wide yet)
    for (size_t step = 8; fits < limit; step *= 2) {
        const size_t probe = limit - fits < step ? limit : fits + step;
        if (width(from, probe) <= room) {
            fits = probe;
        } else {
            over = probe;
            break;
        }
    }
    while (over <= limit && fits + 1 < over) {
        const size_t middle = fits + (over - fits) / 2;
        if (width(from, middle) <= room) fits = middle;
        else over = middle;
    }
    return fits;
}

}  // namespace

std::vector<TextLine> wrapLines(const std::u32string& text, float room, const TextWidth& width) {
    std::vector<TextLine> lines;
    const size_t n = text.size();
    for (size_t start = 0;;) {
        size_t stop = text.find(U'\n', start);
        if (stop == std::u32string::npos) stop = n;
        // the lines of one paragraph, [start, stop)
        size_t from = start;
        while (from < stop) {
            const size_t most = fitting(from, stop, room, width);
            if (most >= stop) break;   // the rest fits
            size_t to = most;
            if (text[most] == U' ') {
                // it ends behind a word: the spaces after it stay on its
                // line, however many
                while (to < stop && text[to] == U' ') to++;
                if (to == stop) break;
            } else {
                // it ends inside a word: back to the word's start - or, where
                // the word alone is wider than the field, as many of its
                // letters as fit, one at least
                while (to > from && text[to - 1] != U' ') to--;
                if (to == from) to = most > from ? most : from + 1;
            }
            lines.push_back({from, to, false});
            from = to;
        }
        lines.push_back({from, stop, true});
        if (stop == n) break;
        start = stop + 1;
    }
    return lines;
}

size_t lineOfCaret(const std::vector<TextLine>& lines, size_t caret, bool atEnd) {
    for (size_t k = 0; k < lines.size(); k++) {
        const TextLine& line = lines[k];
        if (caret >= line.from && caret < line.to) return k;
        if (caret == line.to && (line.hard || atEnd)) return k;
    }
    return lines.empty() ? 0 : lines.size() - 1;
}

size_t placeAt(const TextLine& line, float x, const TextWidth& width) {
    if (x <= 0 || line.to <= line.from) return line.from;
    // the last place not right of x, then the nearer of it and the next
    size_t at = fitting(line.from, line.to, x, width);
    if (at < line.to && x - width(line.from, at) > width(line.from, at + 1) - x) at++;
    return at;
}

size_t lineEnd(const std::u32string& text, const TextLine& line) {
    if (line.hard) return line.to;
    size_t end = line.to;
    while (end > line.from && end <= text.size() && text[end - 1] == U' ') end--;
    return end == line.from ? line.to : end;
}

}  // namespace SCMiner
