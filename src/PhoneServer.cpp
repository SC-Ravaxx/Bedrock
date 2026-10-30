#include "PhoneServer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "../third_party/qrcodegen/qrcodegen.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
namespace {
using Socket = SOCKET;
constexpr Socket NO_SOCKET = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
}  // namespace
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
namespace {
using Socket = int;
constexpr Socket NO_SOCKET = -1;
void closeSocket(Socket s) { ::close(s); }
}  // namespace
#endif

namespace SCMiner {

namespace {

using Clock = std::chrono::steady_clock;

// A page that has not asked for a view for this long is not looking at it
constexpr auto WANTED_FOR = std::chrono::seconds(3);
// ...and a device that has asked nothing for this long is not watching
constexpr auto WATCHING_FOR = std::chrono::seconds(10);
// The most a request's head may be: the page's are a few hundred bytes
constexpr size_t HEAD_MAX = 8192;
// ...and how long it may take to come. A browser opens a connection or two
// ahead, in case it wants them, and sends nothing on them until it does:
// they are waited on alongside the others, and dropped after this.
constexpr auto HEAD_WAIT = std::chrono::seconds(5);
// Devices' connections open at once - a phone has one or two
constexpr size_t MAX_CLIENTS = 32;

// Windows wants its sockets started, once, before the first is made
bool netReady() {
#ifdef _WIN32
    static const bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
#else
    return true;
#endif
}

void setTimeouts(Socket s, int ms) {
#ifdef _WIN32
    const DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    timeval t{};
    t.tv_sec = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#endif
}

bool sendAll(Socket s, const char* data, size_t size) {
    while (size > 0) {
        const int chunk = (int)(std::min)(size, (size_t)1 << 20);
#ifdef _WIN32
        const int sent = ::send(s, data, chunk, 0);
#else
        // (a device gone mid-answer is an error here, not a SIGPIPE that
        // ends Bedrock)
        const int sent = (int)::send(s, data, (size_t)chunk, MSG_NOSIGNAL);
#endif
        if (sent <= 0) return false;
        data += sent;
        size -= (size_t)sent;
    }
    return true;
}

int recvSome(Socket s, char* into, int room) {
#ifdef _WIN32
    return ::recv(s, into, room, 0);
#else
    return (int)::recv(s, into, (size_t)room, 0);
#endif
}

// The answer is sent, and the socket closed so that all of it arrives: the
// request has been read to its end, so nothing is left unread that would
// make the close a reset, and the system sends what is still on its way
// after the close returns.
void closeGently(Socket s) {
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
    closeSocket(s);
}

// Two tokens the same, compared in a time that does not say how much of
// the one matched the other
bool sameToken(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char differ = 0;
    for (size_t i = 0; i < a.size(); i++) differ |= (unsigned char)(a[i] ^ b[i]);
    return differ == 0;
}

std::string reply(const char* status, const char* type, const std::string& body, const std::string& more = {}) {
    std::string head = std::string("HTTP/1.1 ") + status + "\r\n";
    if (type) head += std::string("Content-Type: ") + type + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\n";
    head += "X-Content-Type-Options: nosniff\r\n";
    head += "Referrer-Policy: no-referrer\r\n";
    head += "Connection: close\r\n";
    head += more;
    head += "\r\n";
    return head + body;
}

}  // namespace

PhoneServer::PhoneServer() = default;

PhoneServer::~PhoneServer() { stop(); }

std::string PhoneServer::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool PhoneServer::start(const std::string& token, int port, bool loopbackOnly) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        for (Shown& shown : shown_) shown = Shown{};
        peers_.clear();
        taps_.clear();
    }
    const auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = why;
        return false;
    };
    if (!tokenOk(token)) return fail("Address has no token");
    if (!netReady()) return fail("Windows network sockets unavailable");
    Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == NO_SOCKET) return fail("No socket available");
#ifndef _WIN32
    // A restart finds its port again rather than a port still waiting out
    // the last connection. (Windows lets it have the port as it is, and its
    // own SO_EXCLUSIVEADDRUSE would be what kept it from it for minutes.)
    const int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    bool bound = false;
    const int tries = port <= 0 ? 1 : PORTS_TRIED;
    for (int i = 0; i < tries && !bound; i++) {
        at.sin_port = htons((uint16_t)(port <= 0 ? 0 : port + i));
        bound = bind(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0;
    }
    if (!bound) {
        closeSocket(s);
        if (port <= 0) return fail("No free port");
        return fail("Ports " + std::to_string(port) + " to " + std::to_string(port + tries - 1) +
                    " all in use");
    }
    if (listen(s, 8) != 0) {
        closeSocket(s);
        return fail("Cannot listen on the port");
    }
    sockaddr_in got{};
    socklen_t length = sizeof(got);
    getsockname(s, reinterpret_cast<sockaddr*>(&got), &length);
    port_ = ntohs(got.sin_port);
    token_ = token;
    listener_ = (intptr_t)s;
    stopping_ = false;
    running_ = true;
    thread_ = std::thread([this] { serve(); });
    return true;
}

void PhoneServer::stop() {
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
    if (listener_ != (intptr_t)NO_SOCKET) {
        closeSocket((Socket)listener_);
        listener_ = (intptr_t)NO_SOCKET;
    }
    running_ = false;
    port_ = 0;
}

// One thread, every device: what each has sent so far is kept until its
// request is whole, and answered then
void PhoneServer::serve() {
    struct Client {
        Socket socket;
        std::string peer, head;
        Clock::time_point since;
    };
    std::vector<Client> clients;
    const Socket listener = (Socket)listener_;
    while (!stopping_) {
        // a fifth of a second at a time, so stop() is not kept waiting
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener, &ready);
        Socket top = listener;
        for (const Client& client : clients) {
            FD_SET(client.socket, &ready);
            if (client.socket > top) top = client.socket;
        }
        timeval wait{};
        wait.tv_usec = 200 * 1000;
        const int n = select((int)top + 1, &ready, nullptr, nullptr, &wait);
        if (n < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const auto now = Clock::now();
        for (size_t i = 0; i < clients.size();) {
            Client& client = clients[i];
            bool done = false;   // answered, gone, or given up on
            if (n > 0 && FD_ISSET(client.socket, &ready)) {
                char buffer[2048];
                const int got = recvSome(client.socket, buffer, (int)sizeof(buffer));
                if (got <= 0) {
                    done = true;
                } else {
                    client.head.append(buffer, (size_t)got);
                    if (client.head.find("\r\n\r\n") != std::string::npos) {
                        answer((intptr_t)client.socket, client.peer, client.head);
                        done = true;
                    } else if (client.head.size() > HEAD_MAX) {
                        const std::string out =
                            reply("431 Request Header Fields Too Large", "text/plain", "Request too long.\n");
                        sendAll(client.socket, out.data(), out.size());
                        done = true;
                    }
                }
            }
            if (!done && now - client.since > HEAD_WAIT) done = true;
            if (done) {
                closeGently(client.socket);
                clients.erase(clients.begin() + (std::ptrdiff_t)i);
            } else {
                i++;
            }
        }
        if (n > 0 && FD_ISSET(listener, &ready)) {
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            const Socket client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
            if (client != NO_SOCKET) {
                if (clients.size() >= MAX_CLIENTS) {
                    closeSocket(client);
                } else {
                    char address[INET_ADDRSTRLEN] = {};
                    inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
                    // (an answer that cannot be sent in this long is not)
                    setTimeouts(client, 3000);
                    clients.push_back({client, address, std::string(), now});
                }
            }
        }
    }
    for (const Client& client : clients) closeSocket(client.socket);
}

void PhoneServer::answer(intptr_t client, const std::string& peer, const std::string& head) {
    const Socket s = (Socket)client;
    const auto send = [&](const std::string& out) { sendAll(s, out.data(), out.size()); };
    Request request;
    if (!parseRequest(head, request)) {
        send(reply("400 Bad Request", "text/plain", "Invalid request.\n"));
        return;
    }
    const Route where = route(request, token_);
    // Behind the token, a tap is the one thing sent, and the rest is only
    // asked for. Without it, nothing is there - whatever the method.
    if (where != Route::NotFound) {
        const bool tap = where == Route::Tap;
        if (request.method != (tap ? "POST" : "GET")) {
            send(reply("405 Method Not Allowed", "text/plain", "Not that way.\n",
                       tap ? "Allow: POST\r\n" : "Allow: GET\r\n"));
            return;
        }
    }
    const View view = queryValue(request.query, "view") == "window" ? View::Window : View::Overlay;
    switch (where) {
        case Route::AddSlash:
            send(reply("301 Moved Permanently", "text/plain", "", "Location: /" + token_ + "/\r\n"));
            return;
        case Route::Page:
            send(reply("200 OK", "text/html; charset=utf-8", page(),
                       "Content-Security-Policy: default-src 'none'; img-src blob:; media-src data:; style-src 'unsafe-inline'; "
                       "script-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'; "
                       "frame-ancestors 'none'\r\n"
                       "X-Frame-Options: DENY\r\n"));
            return;
        case Route::State: {
            const int pixels = std::clamp(std::atoi(queryValue(request.query, "w").c_str()), 0, 8000);
            std::string json;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto now = Clock::now();
                Shown& shown = shown_[(int)view];
                shown.askers[peer] = {pixels, now};
                // (another view asked for from the same device is not
                // looked at there any more)
                shown_[1 - (int)view].askers.erase(peer);
                peers_[peer] = now;
                // ...and who has not asked for a while forgotten
                for (auto it = peers_.begin(); it != peers_.end();) {
                    if (now - it->second > WATCHING_FOR) it = peers_.erase(it);
                    else ++it;
                }
                for (Shown& each : shown_) {
                    for (auto it = each.askers.begin(); it != each.askers.end();) {
                        if (now - it->second.second > WANTED_FOR) it = each.askers.erase(it);
                        else ++it;
                    }
                }
                json = "{\"frame\":" + std::to_string(shown.frame);
                if (shown.picture) {
                    json += ",\"width\":" + std::to_string(shown.picture->width) +
                            ",\"height\":" + std::to_string(shown.picture->height);
                }
                json += std::string(",\"control\":") + (control_ ? "true" : "false");
                if (!status_.empty()) json += "," + status_;
                json += "}";
            }
            send(reply("200 OK", "application/json", json));
            return;
        }
        case Route::Frame: {
            std::shared_ptr<const Picture> picture;
            std::shared_ptr<const std::vector<uint8_t>> encoded;
            int frame = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                Shown& shown = shown_[(int)view];
                picture = shown.picture;
                frame = shown.frame;
                if (shown.pngFrame == frame) encoded = shown.png;
            }
            if (!picture) {
                send(reply("404 Not Found", "text/plain", "No image yet.\n"));
                return;
            }
            // made here, on this thread, and kept for the next device that
            // asks for the same picture
            if (!encoded) {
                encoded = std::make_shared<const std::vector<uint8_t>>(png(*picture));
                std::lock_guard<std::mutex> lock(mutex_);
                Shown& shown = shown_[(int)view];
                if (shown.frame == frame) {
                    shown.pngFrame = frame;
                    shown.png = encoded;
                }
            }
            send(reply("200 OK", "image/png", std::string(encoded->begin(), encoded->end())));
            return;
        }
        case Route::Tap: {
            if (!control_) {
                send(reply("403 Forbidden", "text/plain", "View only.\n"));
                return;
            }
            Tap tap;
            tap.view = view;
            tap.x = std::clamp(std::atoi(queryValue(request.query, "x").c_str()), 0, 100000);
            tap.y = std::clamp(std::atoi(queryValue(request.query, "y").c_str()), 0, 100000);
            tap.right = queryValue(request.query, "b") == "r";
            tap.frame = (std::max)(0, std::atoi(queryValue(request.query, "n").c_str()));
            bool taken = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (taps_.size() < MAX_TAPS) {
                    taps_.push_back(tap);
                    taken = true;
                }
                peers_[peer] = Clock::now();
            }
            send(reply("200 OK", "application/json", taken ? "{\"taken\":true}" : "{\"taken\":false}"));
            return;
        }
        case Route::NotFound:
        default:
            send(reply("404 Not Found", "text/plain", "Not found.\n"));
            return;
    }
}

// Several devices on the one view: drawn as wide as the widest of them wants
bool PhoneServer::wanted(View view, int* devicePixels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    bool any = false;
    int widest = 0;
    for (const auto& asker : shown_[(int)view].askers) {
        if (now - asker.second.second > WANTED_FOR) continue;
        any = true;
        widest = (std::max)(widest, asker.second.first);
    }
    if (any && devicePixels) *devicePixels = widest;
    return any;
}

int PhoneServer::watching() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    int count = 0;
    for (const auto& peer : peers_) {
        if (now - peer.second <= WATCHING_FOR) count++;
    }
    return count;
}

bool PhoneServer::publish(View view, Picture picture) {
    std::shared_ptr<const Picture> last;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last = shown_[(int)view].picture;
    }
    // compared outside the lock: a big picture takes a few milliseconds, and
    // the server need not wait for them
    if (last && last->width == picture.width && last->height == picture.height && last->bgra == picture.bgra) {
        return false;
    }
    auto fresh = std::make_shared<const Picture>(std::move(picture));
    std::lock_guard<std::mutex> lock(mutex_);
    Shown& shown = shown_[(int)view];
    shown.picture = std::move(fresh);
    shown.frame++;
    return true;
}

int PhoneServer::frame(View view) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shown_[(int)view].frame;
}

void PhoneServer::setStatus(const std::string& members) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = members;
}

std::vector<PhoneServer::Tap> PhoneServer::takeTaps() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Tap> taken;
    taken.swap(taps_);
    return taken;
}

// ---- what needs no network ----------------------------------------------

bool PhoneServer::parseRequest(const std::string& head, Request& out) {
    const std::string line = head.substr(0, head.find("\r\n"));
    const size_t a = line.find(' ');
    if (a == std::string::npos || a == 0) return false;
    const size_t b = line.find(' ', a + 1);
    if (b == std::string::npos || b == a + 1) return false;
    if (line.compare(b + 1, 5, "HTTP/") != 0) return false;
    const std::string target = line.substr(a + 1, b - a - 1);
    // only a path: the page asks for nothing else
    if (target[0] != '/') return false;
    out.method = line.substr(0, a);
    const size_t q = target.find('?');
    out.path = target.substr(0, q);
    out.query = q == std::string::npos ? std::string() : target.substr(q + 1);
    return true;
}

std::string PhoneServer::queryValue(const std::string& query, const std::string& key) {
    size_t at = 0;
    while (at <= query.size()) {
        size_t end = query.find('&', at);
        if (end == std::string::npos) end = query.size();
        const std::string pair = query.substr(at, end - at);
        const size_t eq = pair.find('=');
        if (pair.substr(0, eq) == key) return eq == std::string::npos ? std::string() : pair.substr(eq + 1);
        at = end + 1;
    }
    return {};
}

PhoneServer::Route PhoneServer::route(const Request& request, const std::string& token) {
    const std::string& path = request.path;
    if (path.size() < 2 || path[0] != '/') return Route::NotFound;
    const size_t slash = path.find('/', 1);
    const std::string first = path.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
    if (!tokenOk(token) || !sameToken(first, token)) return Route::NotFound;
    if (slash == std::string::npos) return Route::AddSlash;
    const std::string rest = path.substr(slash + 1);
    if (rest.empty()) return Route::Page;
    if (rest == "state") return Route::State;
    if (rest == "frame") return Route::Frame;
    if (rest == "tap") return Route::Tap;
    return Route::NotFound;
}

std::string PhoneServer::jsonString(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)(unsigned char)c);
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string PhoneServer::newToken() {
    std::random_device random;
    static const char HEX[] = "0123456789abcdef";
    std::string token;
    for (int i = 0; i < 12; i++) token += HEX[random() & 15];
    return token;
}

bool PhoneServer::tokenOk(const std::string& token) {
    if (token.size() < 8 || token.size() > 32) return false;
    return std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string PhoneServer::lanAddress() {
    if (!netReady()) return {};
    // A socket "connected" to an address out in the world: nothing is sent,
    // but the system picks the way there - the default route - and so the
    // address of the network card the home network is on, rather than one
    // of the virtual ones (WSL, Hyper-V, a VPN's that is not in use)
    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == NO_SOCKET) return {};
    sockaddr_in away{};
    away.sin_family = AF_INET;
    away.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &away.sin_addr);   // (TEST-NET-1: nobody's)
    std::string found;
    if (connect(s, reinterpret_cast<const sockaddr*>(&away), sizeof(away)) == 0) {
        sockaddr_in mine{};
        socklen_t length = sizeof(mine);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&mine), &length) == 0) {
            const uint32_t host = ntohl(mine.sin_addr.s_addr);
            // not the machine itself, and not nothing
            if (host != 0 && (host >> 24) != 127) {
                char text[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &mine.sin_addr, text, sizeof(text));
                found = text;
            }
        }
    }
    closeSocket(s);
    return found;
}

std::vector<bool> PhoneServer::qrCode(const std::string& text, int& size) {
    size = 0;
    try {
        const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = code.getSize();
        std::vector<bool> modules((size_t)n * (size_t)n);
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) modules[(size_t)y * (size_t)n + (size_t)x] = code.getModule(x, y);
        }
        size = n;
        return modules;
    } catch (const std::exception&) {
        return {};
    }
}

std::vector<uint8_t> PhoneServer::png(const Picture& picture) {
    std::vector<uint8_t> out;
    if (picture.width <= 0 || picture.height <= 0 ||
        picture.bgra.size() < (size_t)picture.width * (size_t)picture.height * 4) {
        return out;
    }
    const cv::Mat bgra(picture.height, picture.width, CV_8UC4, const_cast<uint8_t*>(picture.bgra.data()));
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    // little squeezing: the pictures are flat colour and come out small
    // anyway, and quick matters more on a phone that waits for them
    cv::imencode(".png", bgr, out, {cv::IMWRITE_PNG_COMPRESSION, 2});
    return out;
}

std::string PhoneServer::fetchLocal(int port, const std::string& path, const std::string& method) {
    if (!netReady()) return {};
    const Socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == NO_SOCKET) return {};
    setTimeouts(s, 3000);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_port = htons((uint16_t)port);
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string got;
    if (connect(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0) {
        const std::string ask = method + " " + path +
                                " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        if (sendAll(s, ask.data(), ask.size())) {
            char buffer[4096];
            int n;
            while ((n = recvSome(s, buffer, (int)sizeof(buffer))) > 0) got.append(buffer, (size_t)n);
        }
    }
    closeSocket(s);
    return got;
}

// ---- the page ------------------------------------------------------------
// One file, nothing from anywhere else: the colours come with every answer
// to "state", so the page wears the ones Bedrock is set to.
const char* PhoneServer::page() {
    return R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0f1013">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black">
<meta name="apple-mobile-web-app-title" content="Bedrock">
<title>Bedrock</title>
<style>
:root{--bg:#0f1013;--card:#181a1e;--text:#eeeff1;--dim:#a0a6b0;--accent:#e0ba5c}
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-tap-highlight-color:transparent;-webkit-user-select:none;user-select:none}
body{display:flex;flex-direction:column;padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)}
header{display:flex;align-items:center;gap:10px;padding:8px 10px}
.name{display:flex;align-items:center;gap:8px;font-weight:600;letter-spacing:.02em}
.name i{display:block;width:8px;height:8px;background:var(--accent)}
.views{display:flex;gap:3px;margin-left:auto;padding:3px;border-radius:9px;background:var(--card)}
button{font:inherit;color:var(--dim);background:none;border:0;border-radius:7px;padding:6px 13px;cursor:pointer}
.views button.on{color:var(--accent);background:var(--bg);box-shadow:inset 0 0 0 1px var(--accent)}
#full{padding:7px 8px;line-height:0}
#stage{flex:1;min-height:0;position:relative;margin:0 6px 6px;overflow:hidden}
#shot{position:absolute;left:0;right:0;top:0;margin:0 auto;max-width:100%;max-height:100%;transition:opacity .3s;-webkit-touch-callout:none;touch-action:manipulation}
body.control #shot{cursor:pointer}
#note{position:absolute;left:0;right:0;top:40%;margin:auto;max-width:24em;padding:0 20px;text-align:center;color:var(--dim)}
.ring{position:absolute;width:40px;height:40px;margin:-20px 0 0 -20px;border-radius:50%;border:2px solid var(--accent);pointer-events:none;transition:transform .4s ease-out,opacity .4s ease-out}
.ring.long{border-style:dashed}
#toast{position:fixed;left:50%;bottom:calc(18px + env(safe-area-inset-bottom));transform:translateX(-50%);max-width:90%;padding:9px 15px;border-radius:9px;background:var(--card);box-shadow:0 0 0 1px rgba(255,255,255,.1);text-align:center;opacity:0;transition:opacity .25s;pointer-events:none}
#toast.on{opacity:1}
#toast.hint{pointer-events:auto;color:var(--accent);box-shadow:inset 0 0 0 1px var(--accent)}
</style>
</head>
<body>
<header>
<div class="name"><i></i>Bedrock</div>
<div class="views"><button data-view="overlay">Overlay</button><button data-view="window">Window</button></div>
<button id="full" title="Full screen" aria-label="Full screen"><svg width="18" height="18" viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="2"><path d="M1 6V1h5M12 1h5v5M17 12v5h-5M6 17H1v-5"/></svg></button>
</header>
<div id="stage"><img id="shot" alt="" hidden draggable="false"><div id="note">Connecting to Bedrock&hellip;</div></div>
<div id="toast" role="status"></div>
<script>
'use strict';
const $=id=>document.getElementById(id);
const img=$('shot'),note=$('note'),stage=$('stage'),toastBox=$('toast'),root=document.documentElement;
// the view in the address (#window, #overlay), else the one last looked at
const asked=location.hash.slice(1);
let view=asked==='window'||asked==='overlay'?asked:localStorage.getItem('bedrock-view')==='window'?'window':'overlay';
let shown=0,busy=false,loading=false,misses=0,gone=false,url=null,colours='',control=false;
function say(t){note.textContent=t;note.hidden=!t;img.style.opacity=t&&!img.hidden?'.3':'1'}
function choose(v){view=v;localStorage.setItem('bedrock-view',v);history.replaceState(null,'','#'+v);shown=0;
 for(const b of document.querySelectorAll('.views button'))b.classList.toggle('on',b.dataset.view===v);poll()}
for(const b of document.querySelectorAll('.views button'))b.onclick=()=>choose(b.dataset.view);
// how wide the picture stands on this screen, in its own pixels: Bedrock
// draws it that wide, so it is sharp here
function wide(){const w=stage.clientWidth,h=stage.clientHeight;let s=w;
 if(img.naturalWidth&&img.naturalHeight)s=Math.min(w,h*img.naturalWidth/img.naturalHeight);
 return Math.round(Math.max(1,s)*(window.devicePixelRatio||1))}
function wear(s){const c=[s.bg,s.card,s.text,s.dim,s.accent].join();if(!s.bg||c===colours)return;colours=c;
 for(const k of['bg','card','text','dim','accent'])root.style.setProperty('--'+k,s[k]);
 document.querySelector('meta[name=theme-color]').content=s.bg}
async function poll(){if(busy||gone)return;busy=true;const v=view;
 try{const r=await fetch('state?view='+v+'&w='+wide(),{cache:'no-store'});
  if(r.status===404){gone=true;say('This address no longer works. Scan the new code in Bedrock\u2019s settings.');return}
  const s=await r.json();misses=0;wear(s);
  control=!!s.control;document.body.classList.toggle('control',control);
  if(v!==view)return;
  if(!s.frame){if(!shown)say('Connecting to Bedrock\u2026');return}
  if(s.frame!==shown&&!loading)load(v,s.frame);else if(!loading&&shown)say('')}
 catch(e){if(++misses>=4)say('Bedrock is not responding. Check that it is running and that this device is on the same network.')}
 finally{busy=false}}
async function load(v,n){loading=true;
 try{const r=await fetch('frame?view='+v+'&n='+n,{cache:'no-store'});if(!r.ok||v!==view)return;
  const u=URL.createObjectURL(await r.blob());
  await new Promise((ok,no)=>{img.onload=ok;img.onerror=no;img.src=u});
  if(url)URL.revokeObjectURL(url);url=u;img.hidden=false;shown=n;say('');
  started.then(hint)}
 catch(e){}finally{loading=false}}
// A tap on the picture is a click there in Bedrock, a long press a right
// click: sent as the place in the picture's own pixels, and the picture
// asked for again soon after, for what the click did
function ring(cx,cy,long){const s=stage.getBoundingClientRect(),d=document.createElement('div');
 d.className=long?'ring long':'ring';d.style.left=(cx-s.left)+'px';d.style.top=(cy-s.top)+'px';stage.appendChild(d);
 requestAnimationFrame(()=>requestAnimationFrame(()=>{d.style.transform='scale(1.8)';d.style.opacity='0'}));
 setTimeout(()=>d.remove(),450)}
function tap(cx,cy,long){const r=img.getBoundingClientRect();if(!r.width||!r.height||!img.naturalWidth)return;
 const x=Math.floor((cx-r.left)*img.naturalWidth/r.width),y=Math.floor((cy-r.top)*img.naturalHeight/r.height);
 if(x<0||y<0||x>=img.naturalWidth||y>=img.naturalHeight)return;
 ring(cx,cy,long);if(long&&navigator.vibrate)navigator.vibrate(15);
 fetch('tap?view='+view+'&x='+x+'&y='+y+'&n='+shown+(long?'&b=r':''),{method:'POST',cache:'no-store'})
  .then(r=>{if(r.status===403){control=false;document.body.classList.remove('control')}}).catch(()=>{});
 for(const d of[120,350,700,1300])setTimeout(poll,d)}
let press=null;
img.addEventListener('contextmenu',e=>e.preventDefault());
img.addEventListener('pointerdown',e=>{if(!control||img.hidden||!e.isPrimary)return;
 const p={x:e.clientX,y:e.clientY,long:false};
 p.timer=setTimeout(()=>{if(press!==p)return;p.long=true;tap(p.x,p.y,true)},550);press=p});
img.addEventListener('pointermove',e=>{if(press&&Math.hypot(e.clientX-press.x,e.clientY-press.y)>12){clearTimeout(press.timer);press=null}});
function lift(e){if(!press)return;const p=press;press=null;clearTimeout(p.timer);if(!p.long&&e.type==='pointerup')tap(p.x,p.y,false)}
img.addEventListener('pointerup',lift);img.addEventListener('pointercancel',lift);
// The screen kept on while the page is up. A browser keeps it on for a
// page that asks (the Wake Lock API), on https only - and this page is
// http - and for a video playing. Chrome and its kin do so for one that
// takes up most of the screen, whether it is seen or not: this one is
// see-through and muted, and plays from the start. Safari does so only
// for one with sound, which an iPhone or iPad plays no sooner than a
// touch: this one is silent, and played by the first. Not looped, as
// Safari lets the screen sleep under a looping video: sent back to its
// start instead.
const apple=/iP(hone|ad|od)/.test(navigator.userAgent)||/Macintosh/.test(navigator.userAgent)&&navigator.maxTouchPoints>1;
let lock=null,woke=false,touched=false,hinted=false;
const clip=document.createElement('video');
clip.muted=!apple;clip.playsInline=true;clip.setAttribute('playsinline','');clip.setAttribute('webkit-playsinline','');
clip.preload='auto';clip.setAttribute('aria-hidden','true');
clip.style.cssText='position:fixed;left:0;top:0;width:100%;height:100%;opacity:0;pointer-events:none';
clip.src='data:video/mp4;base64,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';
document.body.appendChild(clip);
clip.addEventListener('timeupdate',()=>{if(clip.currentTime>1)clip.currentTime=Math.random()*.5});
// (its silence not stopping music on the iPhone, where Safari lets it say so)
if(apple&&navigator.audioSession)try{navigator.audioSession.type='ambient'}catch(e){}
async function keepOn(){
 if('wakeLock' in navigator){try{if(!lock||lock.released)lock=await navigator.wakeLock.request('screen');woke=true;return}catch(e){}}
 if(apple&&!touched)return;
 try{if(clip.paused)await clip.play();woke=true}catch(e){woke=false}}
const started=keepOn();
// the first touch, wherever it lands - on the hint best, as there it
// clicks nothing in Bedrock
function touch(){if(touched&&woke)return;touched=true;
 keepOn().then(()=>{if(woke&&toastBox.classList.contains('hint'))toastBox.classList.remove('on','hint')})}
for(const t of['pointerup','touchend','click','keydown'])addEventListener(t,touch,{capture:true,passive:true});
// ...and kept on again on coming back to the page, where the browser had let go
document.addEventListener('visibilitychange',()=>{if(!document.hidden)keepOn().then(()=>{if(!woke)hinted=false})});
function hint(){if(hinted||woke||!navigator.maxTouchPoints)return;hinted=true;
 toastBox.textContent='Tap here to keep the screen on.';toastBox.classList.add('on','hint')}
const full=$('full'),ask=root.requestFullscreen||root.webkitRequestFullscreen;
if(!ask)full.hidden=true;
full.onclick=()=>{if(document.fullscreenElement||document.webkitFullscreenElement)(document.exitFullscreen||document.webkitExitFullscreen).call(document);else ask.call(root)};
document.addEventListener('visibilitychange',()=>{if(!document.hidden)poll()});
addEventListener('resize',()=>poll());
choose(view);setInterval(poll,400);
</script>
</body>
</html>
)PAGE";
}

}  // namespace SCMiner
