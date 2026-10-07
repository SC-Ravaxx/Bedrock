#pragma once
// Keeping Bedrock up to date without anyone losing anything.
//
// The player's things live in AppPaths::data(), never in the program folder,
// so replacing the program folder is safe by construction. That is what
// makes an updater reasonable at all: the worst it can do is break the
// program, and the program is a 10 MB download.
//
// What it does, in order:
//   1. Fetches a small manifest over HTTPS - key=value lines:
//        version=4.9.0
//        url=https://.../Bedrock_4.9.0.zip
//        sha256=<64 hex characters of that zip>
//        notes=one line about what changed
//        url_linux=https://.../Bedrock-4.9.0-x86_64.AppImage
//        sha256_linux=<64 hex characters of that AppImage>
//      The last two are the Linux build's; each build reads its own pair
//      and passes over the other's. A file without url_linux is no update
//      for the Linux build.
//   2. Compares the version with this build's. Nothing else happens by
//      itself: the download only starts when the player asks for it.
//   3. Downloads the zip, and checks its SHA-256 against the manifest. A
//      mismatch is thrown away without being unpacked.
//   4. Writes a small script, starts it, and quits. The script waits for
//      this process to be gone, unpacks over the program folder, and starts
//      the new exe. It never touches AppPaths::data().
//
// What the check sends: the version and the platform in the User-Agent
// ("Bedrock/<version> (Windows)", "(Linux)"), and nothing about the player -
// no name, no id, no counter; it is a plain GET for a text file. What it
// deliberately does NOT do: run anything out of the zip except the new
// Bedrock.exe. Set update=0 in region_config.txt and it never looks at all.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace SCMiner {

// One GET over https, made the way the update check makes its own: plain
// http refused, redirects followed, the User-Agent saying the version and
// the platform and nothing else. `headers` go along as they stand
// ("Authorization: Bearer ..."); a packed answer comes out unpacked. False,
// and `why`, for anything but a 200 with a body, and for an answer bigger
// than `cap`. It waits for the answer: never on the UI thread.
bool httpsGet(const std::string& url, const std::vector<std::string>& headers, std::string& body,
              std::string& why, std::int64_t cap = 16ll * 1024 * 1024);

// What a POST came back with: the status and the body whatever the status -
// an API says in its body why it refused, and that is what the player is told
struct HttpAnswer {
    int status = 0;      // 0 where nothing came back
    std::string body;
    std::string why;     // what went wrong on the way, where something did
};
// One POST over https, by the same rules as httpsGet; `payload` goes as it
// stands (headers say what it is). True when an answer came, whatever its
// status. It waits for the answer: never on the UI thread.
bool httpsPost(const std::string& url, const std::vector<std::string>& headers, const std::string& payload,
               HttpAnswer& answer, std::int64_t cap = 1024 * 1024);

class Updater {
public:
    enum class State {
        Idle,        // nothing asked for yet, or checking is switched off
        Checking,
        UpToDate,
        Available,   // newer version found; latest() describes it
        Downloading,
        Ready,       // downloaded and verified, waiting to be applied
        Failed,      // couldNotSay() says why
    };

    struct Release {
        std::string version;
        std::string url;
        std::string sha256;
        std::string notes;
    };

    ~Updater();

    // The manifest URL, and whether to look at all. Both come from
    // region_config.txt, falling back to what the build was made with.
    void configure(const std::string& manifestUrl, bool enabled);

    // Asks, in the background. Safe to call when switched off - it does
    // nothing then, and says so.
    void checkInBackground();
    // Fetches the zip named by the manifest and verifies it, in the
    // background. Only meaningful in state Available.
    void downloadInBackground();
    // Hands over to the script and asks the app to quit. Only in state
    // Ready. Returns false when the script could not be written or started,
    // in which case nothing has happened and the app should stay running.
    bool apply();

    State state() const { return state_.load(); }
    const Release& latest() const { return latest_; }
    const std::string& couldNotSay() const { return error_; }
    int percent() const { return percent_.load(); }
    // False when update=0, or when the build was never pointed at an
    // address. The window leaves the button out entirely then.
    bool enabled() const { return enabled_; }

    // "4.9.0" > "4.10.0" is false: compared as numbers, part by part, so 10
    // sorts after 9. Pure, and the one piece of this worth a test.
    static bool isNewer(const std::string& candidate, const std::string& current);
    // Reads the key=value manifest. False when it carries no usable version
    // or no https url.
    static bool parseManifest(const std::string& text, Release& out);
    // The script apply() hands over to on Windows: it waits for `pid` to be
    // gone, unpacks `zip` into `staging`, copies that over `program` and
    // starts the new exe. Pure, so the self-test can read what it says.
    static std::string applyScript(const std::string& program, const std::string& zip,
                                   const std::string& staging, unsigned long pid);

private:
    void join();

    std::atomic<State> state_{State::Idle};
    std::atomic<int> percent_{0};
    std::string url_;
    bool enabled_ = false;
    Release latest_;
    std::string error_;
    std::string zipPath_;
    std::thread worker_;
};

}  // namespace SCMiner
