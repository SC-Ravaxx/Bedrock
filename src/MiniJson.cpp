#include "MiniJson.h"

#include <cstdlib>
#include <cstring>

namespace SCMiner {

const JsonValue* JsonValue::find(const std::string& key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto& member : members) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

double JsonValue::numberOr(const std::string& key, double fallback) const {
    const JsonValue* value = find(key);
    return value && value->kind == Kind::Number ? value->number : fallback;
}

std::string JsonValue::textOr(const std::string& key, const std::string& fallback) const {
    const JsonValue* value = find(key);
    return value && value->kind == Kind::String ? value->text : fallback;
}

namespace {

// Deeper than any answer Bedrock reads; a bound, so a hostile answer cannot
// run the stack out
constexpr int MAX_DEPTH = 64;

class Reader {
public:
    explicit Reader(const std::string& text) : s_(text) {}

    bool document(JsonValue& out, std::string& why) {
        space();
        if (!value(out, 0)) {
            why = error_.empty() ? "not JSON" : error_;
            why += " at byte " + std::to_string(at_);
            return false;
        }
        space();
        if (at_ != s_.size()) {
            why = "more after the value, at byte " + std::to_string(at_);
            return false;
        }
        return true;
    }

private:
    const std::string& s_;
    size_t at_ = 0;
    std::string error_;

    bool fail(const char* what) {
        if (error_.empty()) error_ = what;
        return false;
    }

    void space() {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) at_++;
    }

    bool word(const char* w) {
        const size_t n = std::strlen(w);
        if (s_.compare(at_, n, w) != 0) return false;
        at_ += n;
        return true;
    }

    bool value(JsonValue& out, int depth) {
        if (depth > MAX_DEPTH) return fail("nested too deep");
        if (at_ >= s_.size()) return fail("ends early");
        const char c = s_[at_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return string(out.text);
        }
        if (c == 't' || c == 'f') {
            out.kind = JsonValue::Kind::Bool;
            out.boolean = c == 't';
            return word(out.boolean ? "true" : "false") || fail("not a word JSON knows");
        }
        if (c == 'n') {
            out.kind = JsonValue::Kind::Null;
            return word("null") || fail("not a word JSON knows");
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("not a value");
    }

    bool object(JsonValue& out, int depth) {
        out.kind = JsonValue::Kind::Object;
        at_++;   // {
        space();
        if (at_ < s_.size() && s_[at_] == '}') {
            at_++;
            return true;
        }
        while (true) {
            space();
            if (at_ >= s_.size() || s_[at_] != '"') return fail("a name in quotes was expected");
            std::string key;
            if (!string(key)) return false;
            space();
            if (at_ >= s_.size() || s_[at_] != ':') return fail("a colon was expected");
            at_++;
            space();
            out.members.emplace_back(std::move(key), JsonValue());
            if (!value(out.members.back().second, depth + 1)) return false;
            space();
            if (at_ >= s_.size()) return fail("ends inside an object");
            if (s_[at_] == ',') {
                at_++;
                continue;
            }
            if (s_[at_] == '}') {
                at_++;
                return true;
            }
            return fail("a comma or a closing brace was expected");
        }
    }

    bool array(JsonValue& out, int depth) {
        out.kind = JsonValue::Kind::Array;
        at_++;   // [
        space();
        if (at_ < s_.size() && s_[at_] == ']') {
            at_++;
            return true;
        }
        while (true) {
            space();
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) return false;
            space();
            if (at_ >= s_.size()) return fail("ends inside an array");
            if (s_[at_] == ',') {
                at_++;
                continue;
            }
            if (s_[at_] == ']') {
                at_++;
                return true;
            }
            return fail("a comma or a closing bracket was expected");
        }
    }

    static void putUtf8(std::string& out, unsigned long cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned long& cp) {
        if (at_ + 4 > s_.size()) return fail("a \\u escape ends early");
        cp = 0;
        for (int i = 0; i < 4; i++) {
            const char h = s_[at_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= (unsigned long)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (unsigned long)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (unsigned long)(h - 'A' + 10);
            else return fail("a \\u escape with no hex digit");
        }
        return true;
    }

    bool string(std::string& out) {
        at_++;   // "
        out.clear();
        while (at_ < s_.size()) {
            const char c = s_[at_++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return fail("a control character inside a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= s_.size()) break;
            const char e = s_[at_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned long cp = 0;
                    if (!hex4(cp)) return false;
                    // a pair of surrogates is one character beyond the first plane
                    if (cp >= 0xD800 && cp <= 0xDBFF && at_ + 6 <= s_.size() && s_[at_] == '\\' &&
                        s_[at_ + 1] == 'u') {
                        at_ += 2;
                        unsigned long low = 0;
                        if (!hex4(low)) return false;
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            putUtf8(out, 0xFFFD);
                            cp = low;
                        }
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        cp = 0xFFFD;   // half a pair: the replacement character
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return fail("an escape JSON does not know");
            }
        }
        return fail("a string that never ends");
    }

    bool number(JsonValue& out) {
        const size_t from = at_;
        if (s_[at_] == '-') at_++;
        while (at_ < s_.size() && ((s_[at_] >= '0' && s_[at_] <= '9') || s_[at_] == '.' || s_[at_] == 'e' ||
                                   s_[at_] == 'E' || s_[at_] == '+' || s_[at_] == '-')) {
            at_++;
        }
        const std::string token = s_.substr(from, at_ - from);
        char* end = nullptr;
        out.kind = JsonValue::Kind::Number;
        out.number = std::strtod(token.c_str(), &end);
        if (token.empty() || end != token.c_str() + token.size()) return fail("not a number");
        return true;
    }
};

}  // namespace

bool parseJson(const std::string& text, JsonValue& out, std::string& why) {
    out = JsonValue();
    Reader reader(text);
    return reader.document(out, why);
}

}  // namespace SCMiner
