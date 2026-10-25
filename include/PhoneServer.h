#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SCMiner {

// Bedrock on a phone or tablet, in its browser.
//
// A small web server in the home network that hands a page to whoever opens
// its address, and to that page, pictures of what the overlay and the
// window show - drawn by the window's own code, so they look the same and
// take the colours that are set. The page asks every half second whether
// there is a newer picture of the view it shows, and fetches it only then.
//
// To look at, and where control is switched on (setControl), to use: a tap
// on the picture comes as a POST of where it landed, which the window takes
// as a click there (takeTaps). Otherwise the server answers GET alone. Its
// address carries a token of twelve hex digits, so a device on the same
// network that has not been shown the code finds nothing there either.
//
// Off unless it is switched on. Listening on the network is what makes
// Windows ask whether Bedrock may use it - once, when it is first switched
// on. The self-test listens on this machine alone, which asks nothing.
class PhoneServer {
public:
    enum class View { Overlay = 0, Window = 1 };
    static constexpr int VIEW_COUNT = 2;
    // Tried from here, ten ports on, until one is free
    static constexpr int FIRST_PORT = 47820;
    static constexpr int PORTS_TRIED = 10;

    // A picture of one view as the window draws it: rows of blue, green,
    // red and alpha bytes, width * 4 to a row, opaque
    struct Picture {
        int width = 0, height = 0;
        std::vector<uint8_t> bgra;
    };

    PhoneServer();
    ~PhoneServer();
    PhoneServer(const PhoneServer&) = delete;
    PhoneServer& operator=(const PhoneServer&) = delete;

    // Listens at the first free port from `port` on (0: whichever the
    // system gives), on every network or on this machine alone. False when
    // none could be opened; error() says why.
    bool start(const std::string& token, int port = FIRST_PORT, bool loopbackOnly = false);
    void stop();
    bool running() const { return running_; }
    int port() const { return port_; }
    std::string error() const;

    // Whether a page asked for this view in the last few seconds, and how
    // many of its screen's pixels wide it shows it
    bool wanted(View view, int* devicePixels = nullptr) const;
    // ...and how many devices asked for anything in the last ten seconds
    int watching() const;
    // A new picture of a view. Taken only when it differs from the last one,
    // so the page fetches it only then; true when it was taken.
    bool publish(View view, Picture picture);
    // The picture's number as the page sees it: 0 until there is one
    int frame(View view) const;
    // What the page shows above the picture: the members of a JSON object,
    // without the braces - "live":true,"place":"...",...
    void setStatus(const std::string& members);

    // A tap on the picture of a view, where the page showed it: in the
    // picture's own pixels, on picture number `frame`; `right` for a long
    // press, which the window takes as a right click
    struct Tap {
        View view = View::Overlay;
        int x = 0, y = 0;
        bool right = false;
        int frame = 0;
    };
    // Whether taps are taken. Off, the page is only to look at, and a tap
    // gets a 403.
    void setControl(bool on) { control_ = on; }
    bool control() const { return control_; }
    // The taps that came since the last call, oldest first - no more than
    // MAX_TAPS of them wait, the rest are dropped
    std::vector<Tap> takeTaps();
    static constexpr size_t MAX_TAPS = 16;

    // ---- what needs no network, for the self-test ----

    struct Request {
        std::string method, path, query;
    };
    // The request line out of a request's head. False when it is not one.
    static bool parseRequest(const std::string& head, Request& out);
    // One value out of a query string - "view=window&w=900", "w" -> "900"
    static std::string queryValue(const std::string& query, const std::string& key);
    enum class Route { NotFound, AddSlash, Page, State, Frame, Tap };
    static Route route(const Request& request, const std::string& token);
    // A string as JSON has it: quoted, and escaped where it has to be
    static std::string jsonString(const std::string& text);
    // The page itself
    static const char* page();
    // Twelve hex digits, at random
    static std::string newToken();
    // A token as it may stand in an address: hex digits, 8 to 32 of them
    static bool tokenOk(const std::string& token);
    // This machine's address in the home network: the one its default route
    // goes out of; "" without one
    static std::string lanAddress();
    // The code to scan, for `text`: size * size modules, row by row, true
    // where it is dark. Empty when the text is too long for one.
    static std::vector<bool> qrCode(const std::string& text, int& size);
    // A picture as a PNG - what the page is sent
    static std::vector<uint8_t> png(const Picture& picture);
    // What the server answers on this machine at `port` for `path`: the
    // status line, the head and the body as they came. For the self-test.
    static std::string fetchLocal(int port, const std::string& path, const std::string& method = "GET");

private:
    void serve();
    void answer(intptr_t client, const std::string& peer, const std::string& head);

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> control_{false};
    std::atomic<int> port_{0};
    intptr_t listener_ = -1;
    std::thread thread_;
    std::string token_;

    mutable std::mutex mutex_;
    std::string error_;
    std::string status_;
    struct Shown {
        std::shared_ptr<const Picture> picture;
        int frame = 0;
        // the PNG of `frame`, made when it is first asked for
        int pngFrame = 0;
        std::shared_ptr<const std::vector<uint8_t>> png;
        // who asked for it last, and how wide it stands on their screen
        std::map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> askers;
    };
    std::array<Shown, VIEW_COUNT> shown_;
    std::map<std::string, std::chrono::steady_clock::time_point> peers_;
    std::vector<Tap> taps_;
};

}  // namespace SCMiner
