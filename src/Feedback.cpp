#include "Feedback.h"

#include "RockScans.h"
#include "TextField.h"

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
                 int gameVersion, const std::string& channel, const std::string& os) {
    if (fault(typedText, typedName) != Fault::None) return "";
    std::string out = "{\"text\":" + quoted(message(typedText));
    const std::string from = name(typedName);
    if (!from.empty()) out += ",\"name\":" + quoted(from);
    out += ",\"version\":" + quoted(bedrockVersion);
    out += ",\"game\":" + std::to_string(gameVersion > 0 && gameVersion <= 999999 ? gameVersion : 0);
    const std::string kind = RockScans::channelKind(channel);
    if (!kind.empty()) out += ",\"channel\":" + quoted(kind);
    out += ",\"os\":" + quoted(os) + "}";
    return out.size() <= BODY_MOST ? out : std::string();
}

Outcome outcome(int status) {
    if (status >= 200 && status < 300) return Outcome::Sent;
    if (status == 429) return Outcome::Limited;
    if (status <= 0 || status >= 500) return Outcome::NoConnection;
    return Outcome::Refused;
}

std::string gameName(int gameVersion) {
    if (gameVersion <= 0 || gameVersion > 999999) return "";
    char text[32];
    std::snprintf(text, sizeof(text), "%d.%d.%d", gameVersion / 10000, gameVersion / 100 % 100, gameVersion % 100);
    return text;
}

std::string sentWith(const std::string& bedrockVersion, int gameVersion, bool installSent) {
    const std::string dot = "  \xC2\xB7  ";
    std::string out = "Sent with it: Bedrock " + bedrockVersion;
    const std::string game = gameName(gameVersion);
    if (!game.empty()) out += dot + "Star Citizen " + game;
    if (installSent) out += dot + "Installation number";
    return out;
}

}  // namespace Feedback
}  // namespace SCMiner
