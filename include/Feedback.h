#pragma once
// A player's message to the developer, from the Feedback page: what goes to
// Bedrock's service, and what its answer means.
//
// One POST to <service>/v1/feedback when Send is clicked, and only then
// (server/refinery/README.md, "Feedback"):
//
//     {"text": "...", "name": "...", "version": "3.0.4",
//      "game": 41003, "channel": "LIVE", "os": "windows"}
//
// text     the message: 1 to 4000 characters once the white space is off
//          its ends, its line breaks "\n" and no other control character
// name     where the player gave one: one line, 40 characters at most
// version  Bedrock's
// game     the game's version as Game.log's header gives it, 41003 for
//          4.10.3; 0 where it gave none
// channel  as the rock scans name it (RockScans::channelKind), left out
//          where they leave it out
// os       "windows" or "linux"
//
// 16 KB at most. The installation number goes in the request's header, as
// with every request to the service, unless stats=0. The service answers
// 201 where it kept the message, 200 where it had the same one from the same
// sender already, 400, 413 or 415 where it does not fit, 429 where it has
// had enough for now.
//
// A bug report brings its debug folder (DebugZip.h): the body says the zip's
// size as "debug": <bytes>, 1 to DEBUG_MOST, and the answer gives the
// message's "id" and a token, "upload", for one POST of the zip to
// /v1/feedback/<id>/debug within 60 minutes - or "debug": "refused".
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SCMiner {
namespace Feedback {

inline constexpr size_t TEXT_MOST = 4000;                // characters of a message
inline constexpr size_t NAME_MOST = 40;                  // ...of a name
inline constexpr size_t BODY_MOST = 16 * 1024;           // bytes of a request
inline constexpr size_t DEBUG_MOST = 32 * 1024 * 1024;   // ...of a debug folder's zip

// The message and the name as they go, of what was typed (UTF-8 in and
// out): line breaks of any kind "\n" - in a name a space -, a tab a space,
// other control characters out, the white space off both ends
std::string message(const std::string& typed);
std::string name(const std::string& typed);
// How many characters a UTF-8 text has
size_t characters(const std::string& text);

// Why what was typed cannot go, if it cannot
enum class Fault { None, Empty, TooLong, NameTooLong };
Fault fault(const std::string& typedText, const std::string& typedName);

// "windows" or "linux": the build's own
const char* platform();

// The request's body; "" where what was typed cannot go (fault). With
// `debugBytes` above 0 it says "debug": the size of the zip that is to follow
std::string body(const std::string& typedText, const std::string& typedName, const std::string& bedrockVersion,
                 int gameVersion, const std::string& channel, const std::string& os, std::int64_t debugBytes = 0);

// What came of it, by the status the service answered with (0: no answer
// came): kept (201, and 200 for the same message again); the service not
// reached, or not up to it (5xx); too many messages (429); turned down
enum class Outcome { Sent, NoConnection, Limited, Refused };
Outcome outcome(int status);

// What the service's answer to a message says of its zip: the message's
// number and the token for the upload where the zip may come, or that it may
// not. Read strictly, the answer's own members only: "id" a whole number
// above 0, "upload" exactly 32 lowercase hex, "debug" exactly "refused" -
// anything else leaves them 0, "" and false.
struct Answer {
    long long id = 0;
    std::string upload;
    bool debugRefused = false;
};
Answer answer(const std::string& json);

// The zip's upload: POST to the service at debugPath(id) - "/v1/feedback/<id>/debug"
// - with debugHeaders(upload) and the zip as the body. The installation
// number's header is added by the caller, as for every request.
std::string debugPath(long long id);
std::vector<std::string> debugHeaders(const std::string& upload);

// What came of the upload, by its status (0: no answer came): stored (201);
// the service not reached, or not up to it (5xx but 507); too large (413);
// the service's room for zips full (507); turned down - not a zip (400), the
// token wrong, used or expired (403), no such message (404), a zip there
// already (409)
enum class DebugOutcome { Sent, NoConnection, TooLarge, Full, Refused };
DebugOutcome debugOutcome(int status);

// "4.10.3" for 41003; "" for 0
std::string gameName(int gameVersion);
// The page's line of what goes with a message, UTF-8: "Sent with it:
// Bedrock 3.0.4  ·  Star Citizen 4.10.3  ·  Installation number" - without
// the game where its version is not known, without the installation number
// where none is sent
std::string sentWith(const std::string& bedrockVersion, int gameVersion, bool installSent);

}  // namespace Feedback
}  // namespace SCMiner
