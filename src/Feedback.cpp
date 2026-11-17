#include "Feedback.h"

#include "RockScans.h"
#include "TextField.h"

#include <algorithm>
#include <cstdio>

namespace SCMiner {
namespace Feedback {

namespace {

// A UTF-8 text as a JSON string: the quote and the backslash escaped, a
// line break as \n, any other control character as \u00XX; everything else
// as it stands - JSON is UTF-8
std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c < 0x20 || c == 0x7F) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        } else {
            out += (char)c;
        }
    }
    out += "\"";
    return out;
}

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Where the JSON string that begins at `at` (its opening quote) ends, past
// its escapes: the index after its closing quote; npos where it does not end
size_t stringEnd(const std::string& json, size_t at) {
    size_t i = at + 1;
    while (i < json.size() && json[i] != '"') i += json[i] == '\\' ? 2 : 1;
    return i < json.size() ? i + 1 : std::string::npos;
}

// The value of `key` among the members of the answer's outermost object, as
// it stands in the text - "17", "\"refused\"" -; "" where it has no such
// member. Members of inner objects and words inside strings do not count.
std::string member(const std::string& json, const std::string& key) {
    size_t i = 0;
    while (i < json.size() && space(json[i])) i++;
    if (i >= json.size() || json[i] != '{') return "";
    int depth = 0;
    while (i < json.size()) {
        const char c = json[i];
        if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (--depth <= 0) return "";
        } else if (c == '"') {
            const size_t end = stringEnd(json, i);
            if (end == std::string::npos) return "";
            const bool named = depth == 1 && end - i - 2 == key.size() && json.compare(i + 1, key.size(), key) == 0;
            i = end;
            if (!named) continue;
            size_t at = i;
            while (at < json.size() && space(json[at])) at++;
            if (at >= json.size() || json[at] != ':') continue;   // a value, not a name
            at++;
            while (at < json.size() && space(json[at])) at++;
            size_t stop = at;
            if (stop < json.size() && json[stop] == '"') {
                stop = stringEnd(json, stop);
                if (stop == std::string::npos) return "";
            } else {
                while (stop < json.size() && json[stop] != ',' && json[stop] != '}' && !space(json[stop])) stop++;
            }
            return json.substr(at, stop - at);
        }
        i++;
    }
    return "";
}

}  // namespace

std::string message(const std::string& typed) {
    return Chars::toUtf8(Chars::trimmed(Chars::plain(Chars::fromUtf8(typed), true)));
}

std::string name(const std::string& typed) {
    return Chars::toUtf8(Chars::trimmed(Chars::plain(Chars::fromUtf8(typed), false)));
}

size_t characters(const std::string& text) {
    return Chars::fromUtf8(text).size();
}

Fault fault(const std::string& typedText, const std::string& typedName) {
    const size_t length = characters(message(typedText));
    if (length == 0) return Fault::Empty;
    if (length > TEXT_MOST) return Fault::TooLong;
    if (characters(name(typedName)) > NAME_MOST) return Fault::NameTooLong;
    return Fault::None;
}

const char* platform() {
#ifdef _WIN32
    return "windows";
#else
    return "linux";
#endif
}

std::string body(const std::string& typedText, const std::string& typedName, const std::string& bedrockVersion,
                 int gameVersion, const std::string& channel, const std::string& os, std::int64_t debugBytes) {
    if (fault(typedText, typedName) != Fault::None) return "";
    std::string out = "{\"text\":" + quoted(message(typedText));
    const std::string from = name(typedName);
    if (!from.empty()) out += ",\"name\":" + quoted(from);
    out += ",\"version\":" + quoted(bedrockVersion);
    out += ",\"game\":" + std::to_string(gameVersion > 0 && gameVersion <= 999999 ? gameVersion : 0);
    const std::string kind = RockScans::channelKind(channel);
    if (!kind.empty()) out += ",\"channel\":" + quoted(kind);
    out += ",\"os\":" + quoted(os);
    if (debugBytes > 0) out += ",\"debug\":" + std::to_string(debugBytes);
    out += "}";
    return out.size() <= BODY_MOST ? out : std::string();
}

Outcome outcome(int status) {
    if (status >= 200 && status < 300) return Outcome::Sent;
    if (status == 429) return Outcome::Limited;
    if (status <= 0 || status >= 500) return Outcome::NoConnection;
    return Outcome::Refused;
}

Answer answer(const std::string& json) {
    Answer out;
    const std::string id = member(json, "id");
    // figures only - no sign, point or exponent -, no leading zero, and few
    // enough for a long long
    if (!id.empty() && id.size() <= 18 && id[0] != '0' && id.find_first_not_of("0123456789") == std::string::npos) {
        for (char c : id) out.id = out.id * 10 + (c - '0');
    }
    const std::string upload = member(json, "upload");
    if (upload.size() == 34 && upload.front() == '"' && upload.back() == '"' &&
        upload.find_first_not_of("0123456789abcdef", 1) == 33) {
        out.upload = upload.substr(1, 32);
    }
    const std::string debug = member(json, "debug");
    out.debugRefused = debug == "\"refused\"";
    out.debugStored = debug == "\"stored\"";
    return out;
}

std::string debugPath(long long id) {
    return "/v1/feedback/" + std::to_string(id) + "/debug";
}

std::vector<std::string> debugHeaders(const std::string& upload) {
    return {"X-Bedrock-Upload: " + upload, "Content-Type: application/zip"};
}

DebugOutcome debugOutcome(int status) {
    // (409: the message has its zip - one sent again whose answer was lost
    // on the way back)
    if (status == 201 || status == 409) return DebugOutcome::Sent;
    if (status == 413) return DebugOutcome::TooLarge;
    if (status == 507) return DebugOutcome::Full;
    if (status <= 0 || (status >= 500 && status < 600)) return DebugOutcome::NoConnection;
    return DebugOutcome::TurnedDown;
}

Result send(const std::string& body, const std::string& zip, const Post& post, const Progress& progress) {
    Result out;
    if (body.empty()) {
        out.status = 413;
        return out;
    }
    std::string said;
    out.status = post("/v1/feedback", {"Content-Type: application/json"}, body, nullptr, said);
    // a message that did not go takes no zip; one without a zip is done
    if (zip.empty() || outcome(out.status) != Outcome::Sent) return out;
    const Answer told = answer(said);
    if (told.debugStored) {
        out.debug = DebugOutcome::Sent;
        return out;
    }
    if (told.debugRefused || told.id <= 0 || told.upload.empty()) {
        out.debug = DebugOutcome::Refused;
        return out;
    }
    if (progress) progress(0, (std::int64_t)zip.size());
    std::string ignored;
    out.debugStatus = post(debugPath(told.id), debugHeaders(told.upload), zip, progress, ignored);
    out.debug = debugOutcome(out.debugStatus);
    return out;
}

Shown shown(Outcome message, DebugOutcome debug) {
    switch (message) {
        case Outcome::NoConnection: return Shown::NoConnection;
        case Outcome::Limited: return Shown::Limited;
        case Outcome::Refused: return Shown::Refused;
        case Outcome::Sent: break;
    }
    switch (debug) {
        case DebugOutcome::NoConnection: return Shown::DebugNoConnection;
        case DebugOutcome::TooLarge: return Shown::DebugTooLarge;
        case DebugOutcome::Full: return Shown::DebugFull;
        case DebugOutcome::TurnedDown: return Shown::DebugTurnedDown;
        case DebugOutcome::Refused: return Shown::DebugRefused;
        case DebugOutcome::None:
        case DebugOutcome::Sent: break;
    }
    return Shown::Sent;
}

int megabytes(std::int64_t bytes) {
    return (int)std::clamp<std::int64_t>(bytes / (1024 * 1024), 1, 1 << 30);
}

std::string gameName(int gameVersion) {
    if (gameVersion <= 0 || gameVersion > 999999) return "";
    char text[32];
    std::snprintf(text, sizeof(text), "%d.%d.%d", gameVersion / 10000, gameVersion / 100 % 100, gameVersion % 100);
    return text;
}

std::string sentWith(const std::string& bedrockVersion, int gameVersion, bool installSent, bool debugFolder) {
    const std::string dot = "  \xC2\xB7  ";
    std::string out = "Sent with it: Bedrock " + bedrockVersion;
    const std::string game = gameName(gameVersion);
    if (!game.empty()) out += dot + "Star Citizen " + game;
    if (installSent) out += dot + "Installation number";
    if (debugFolder) out += dot + "Debug folder";
    return out;
}

}  // namespace Feedback
}  // namespace SCMiner
