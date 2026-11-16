#pragma once
// A text being typed into a field the window paints itself: what it holds,
// where the caret stands, what is marked, the keys that change it, undo -
// and where its lines break in a field of a given width.
//
// No window system under it. The window hands it the keys, the clipboard's
// text and a measure of its letters, and draws what it holds; so the same
// field behaves the same on Windows and on Linux, and the self-test types
// into it.
//
// Held as UTF-32, a character a place: a letter beyond the Basic
// Multilingual Plane is one step of the caret and one of the field's
// `most`, as the service counts it. (A letter made of several - a flag, a
// family - is as many steps as it has parts.)
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace SCMiner {

namespace Chars {

// UTF-8, and the platform's wide string (UTF-16 on Windows, UTF-32 on
// Linux), to UTF-32 and back. What is no character - a byte that is not
// UTF-8, half a surrogate pair - becomes U+FFFD.
std::u32string fromUtf8(const std::string& text);
std::string toUtf8(const std::u32string& text);
std::u32string fromWide(const std::wstring& text);
std::wstring toWide(const std::u32string& text);

// White space as it is trimmed off a text's ends: what Python's str.strip
// takes, which Bedrock's service trims by
bool isSpace(char32_t c);

// A text as a field takes it: a line break of any kind (CR LF, CR, VT, FF,
// NEL, U+2028, U+2029) one "\n" - or a space, in a field of one line -, a
// tab a space, and no other control character
std::u32string plain(const std::u32string& text, bool lines);

// ...with the white space off both its ends
std::u32string trimmed(const std::u32string& text);

}  // namespace Chars

class TextField {
public:
    // `lines`: Enter breaks the line; `most`: the characters it holds at most
    TextField(bool lines, size_t most) : lines_(lines), most_(most) {}

    const std::u32string& text() const { return text_; }
    bool empty() const { return text_.empty(); }
    size_t most() const { return most_; }
    // The caret, and what is marked: from the lower of caret and anchor up
    // to the higher
    size_t caret() const { return caret_; }
    bool marked() const { return caret_ != anchor_; }
    size_t markFrom() const { return caret_ < anchor_ ? caret_ : anchor_; }
    size_t markTo() const { return caret_ < anchor_ ? anchor_ : caret_; }
    std::u32string markedText() const { return text_.substr(markFrom(), markTo() - markFrom()); }
    // Counts every change of the text: what was laid out for one count holds
    // until the next
    unsigned long long changes() const { return changes_; }

    // The whole text anew (as `plain` has it, cut to `most`): the caret at
    // its end, nothing marked, nothing to undo
    void set(const std::u32string& text);

    // A character typed, over what is marked: false where the field took
    // nothing - a control character, a line break in a field of one line, a
    // field that is full
    bool type(char32_t c);
    // A text put in at the caret, over what is marked - the clipboard's: as
    // much of it as the field has room for
    bool put(const std::u32string& text);
    // Backspace and Delete: what is marked, else the character before or
    // after the caret - or, with `word`, back to the word's start or on to
    // the next word's
    bool backspace(bool word = false);
    bool erase(bool word = false);
    // What is marked taken out (Cut, after its text was read)
    bool cut();

    // The caret moved: `marking` keeps the anchor, as Shift does. Left and
    // right without it let go of what is marked at that end of it.
    void moveTo(size_t at, bool marking);
    void left(bool marking, bool word = false);
    void right(bool marking, bool word = false);
    void markAll();
    // The word at a place marked (a double click): letters and digits, or a
    // run of other signs; nothing where it is white space
    void markWord(size_t at);

    // One step back, and forward again. Letters typed one after the other
    // are one step, and so are characters taken away one after the other.
    bool undo();
    bool redo();

private:
    struct Was {
        std::u32string text;
        size_t caret = 0, anchor = 0;
    };
    enum class Step { None, Typing, Erasing };
    void remember(Step step);
    void changed(Step step);
    bool insert(const std::u32string& clean, Step step);
    void takeOut(size_t from, size_t to);
    size_t wordBefore(size_t at) const;
    size_t wordAfter(size_t at) const;

    bool lines_;
    size_t most_;
    std::u32string text_;
    size_t caret_ = 0, anchor_ = 0;
    unsigned long long changes_ = 0;
    std::vector<Was> undo_, redo_;
    Step last_ = Step::None;
    size_t lastAt_ = 0;   // where the last step left the caret
};

// One line of a text as a field shows it: [from, to) of the text. `hard`:
// it ends at a line break (text[to]) or at the text's end; otherwise the
// field's width broke it, and the next line starts at `to`.
struct TextLine {
    size_t from = 0, to = 0;
    bool hard = true;
};

// How wide text[from, to) is, as the window measures it
using TextWidth = std::function<float(size_t from, size_t to)>;

// The lines of a text in a field `room` wide: broken at its line breaks,
// and where a line is too wide after the last space that fits - the spaces
// stay at the end of their line, however many - or, where one word alone
// is too wide, inside the word. Always one line at least.
std::vector<TextLine> wrapLines(const std::u32string& text, float room, const TextWidth& width);

// Which line the caret is on. The end of a line broken inside a word is
// also the next line's start: `atEnd` says the caret was put at the end.
size_t lineOfCaret(const std::vector<TextLine>& lines, size_t caret, bool atEnd);

// The place in a line nearest to `x` from its start
size_t placeAt(const TextLine& line, float x, const TextWidth& width);

// Where End puts the caret on a line: before the spaces a broken line ends
// in (== line.to where it was broken inside a word, or is not broken)
size_t lineEnd(const std::u32string& text, const TextLine& line);

}  // namespace SCMiner
