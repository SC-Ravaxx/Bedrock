#include "Updater.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#else
#include <QByteArray>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

#include "AppPaths.h"
#include "resource.h"

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

namespace SCMiner {
namespace {

#ifdef _WIN32
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}
#endif

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

// A URL's host, in lower case: "the-forsaken.org" of
// "https://the-forsaken.org/bedrock/bedrock-latest.txt"
std::string hostOf(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    const size_t from = scheme + 3, to = url.find_first_of("/:?#", from);
    std::string host = url.substr(from, to == std::string::npos ? std::string::npos : to - from);
    for (char& c : host) c = (char)std::tolower((unsigned char)c);
    return host;
}

#ifdef _WIN32
// One GET, following redirects (a GitHub release download always redirects
// to its storage host). Everything is refused unless it is https: an updater
// that would take its instructions over plain http is not an updater, it is
// a way in. `headers` go along as they stand; `unpack` asks for the answer
// packed (gzip, deflate) and has WinHTTP unpack it - not for the update's
// zip, whose percentage is counted against the Content-Length of the bytes
// as they come.
bool fetch(const std::string& url, std::string& body, std::string& why,
           std::atomic<int>* percent = nullptr, std::int64_t cap = 64ll * 1024 * 1024,
           const std::vector<std::string>& headers = {}, bool unpack = false) {
    body.clear();
    if (url.rfind("https://", 0) != 0) {
        why = "only https is accepted, and this is not: " + url;
        return false;
    }
    const std::wstring wurl = wide(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &parts)) {
        why = "that is not a url the system can read";
        return false;
    }

    // "Bedrock/2.0.3 (Windows)": the version and the platform, and that is
    // all the User-Agent says - the Linux build says (Linux)
    HINTERNET session = WinHttpOpen(L"Bedrock/" BEDROCK_VERSION_W L" (Windows)",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        why = "no network session";
        return false;
    }
    // Not forever: a hanging check must never hold the app up
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 30000);
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                                    WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                    WINHTTP_FLAG_SECURE)
                                : nullptr;
    std::wstring extra;
    for (const std::string& header : headers) extra += wide(header) + L"\r\n";
    if (request && unpack) {
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    }
    bool ok = false;
    if (request &&
        WinHttpSendRequest(request, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(),
                           extra.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            why = "the server answered " + std::to_string(status);
        } else {
            std::int64_t total = 0;
            DWORD len = 0;
            size = sizeof(len);
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &len, &size,
                                    WINHTTP_NO_HEADER_INDEX)) {
                total = len;
            }
            std::vector<char> chunk(64 * 1024);
            DWORD read = 0;
            ok = true;
            while (WinHttpReadData(request, chunk.data(), (DWORD)chunk.size(), &read) && read) {
                body.append(chunk.data(), read);
                if ((std::int64_t)body.size() > cap) {
                    why = "the answer is far bigger than it should be";
                    ok = false;
                    break;
                }
                if (percent && total > 0) {
                    percent->store((int)((std::int64_t)body.size() * 100 / total));
                }
            }
            if (ok && body.empty()) {
                why = "the server sent nothing";
                ok = false;
            }
        }
    } else if (why.empty()) {
        why = "could not reach it";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// One POST, and its answer whatever its status. https only, as above.
bool exchange(const std::string& url, const std::vector<std::string>& headers, const std::string& payload,
              HttpAnswer& answer, std::int64_t cap) {
    answer = HttpAnswer{};
    if (url.rfind("https://", 0) != 0) {
        answer.why = "only https is accepted, and this is not: " + url;
        return false;
    }
    const std::wstring wurl = wide(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]));
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &parts)) {
        answer.why = "that is not a url the system can read";
        return false;
    }
    HINTERNET session = WinHttpOpen(L"Bedrock/" BEDROCK_VERSION_W L" (Windows)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        answer.why = "no network session";
        return false;
    }
    // a screenshot goes along with some: half a minute to send
    WinHttpSetTimeouts(session, 8000, 8000, 30000, 30000);
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    std::wstring extra;
    for (const std::string& header : headers) extra += wide(header) + L"\r\n";
    if (request) {
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    }
    bool answered = false;
    if (request &&
        WinHttpSendRequest(request, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(),
                           extra.empty() ? 0 : (DWORD)-1L, (LPVOID)payload.data(), (DWORD)payload.size(),
                           (DWORD)payload.size(), 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        answer.status = (int)status;
        answered = true;
        std::vector<char> chunk(64 * 1024);
        DWORD read = 0;
        while (WinHttpReadData(request, chunk.data(), (DWORD)chunk.size(), &read) && read) {
            answer.body.append(chunk.data(), read);
            if ((std::int64_t)answer.body.size() > cap) {
                answer.why = "the answer is far bigger than it should be";
                break;
            }
        }
    } else {
        answer.why = "could not reach it";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return answered;
}

std::string sha256Of(const std::string& bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    unsigned char digest[32] = {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        if (BCryptHashData(hash, (PUCHAR)bytes.data(), (ULONG)bytes.size(), 0) == 0 &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            char hex[65] = {};
            for (int i = 0; i < 32; i++) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
            out.assign(hex, 64);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

#else  // Linux: the same two, on Qt Network and QCryptographicHash

// One GET, following redirects, https only - the same rule as above. Run
// on the updater's own thread with an event loop of its own; Qt adopts the
// thread the moment it is asked. Qt asks for a packed answer and unpacks it
// by itself, so `unpack` has nothing left to do here.
bool fetch(const std::string& url, std::string& body, std::string& why,
           std::atomic<int>* percent = nullptr, std::int64_t cap = 64ll * 1024 * 1024,
           const std::vector<std::string>& headers = {}, bool unpack = false) {
    (void)unpack;
    body.clear();
    if (url.rfind("https://", 0) != 0) {
        why = "only https is accepted, and this is not: " + url;
        return false;
    }
    QNetworkAccessManager manager;
    manager.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkRequest request{QUrl(QString::fromStdString(url))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Bedrock/" BEDROCK_VERSION " (Linux)"));
    for (const std::string& header : headers) {
        const size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        request.setRawHeader(QByteArray::fromStdString(trim(header.substr(0, colon))),
                             QByteArray::fromStdString(trim(header.substr(colon + 1))));
    }
    request.setTransferTimeout(30000);
    QEventLoop loop;
    QNetworkReply* reply = manager.get(request);
    bool ok = false;
    QObject::connect(reply, &QNetworkReply::downloadProgress, [&](qint64 got, qint64 total) {
        if (percent && total > 0) percent->store((int)(got * 100 / total));
        if (got > cap) {
            why = "the answer is far bigger than it should be";
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, [&] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (why.empty()) why = "could not reach it: " + reply->errorString().toStdString();
        } else if (status != 200) {
            why = "the server answered " + std::to_string(status);
        } else {
            const QByteArray bytes = reply->readAll();
            if (bytes.isEmpty()) {
                why = "the server sent nothing";
            } else {
                body.assign(bytes.constData(), (size_t)bytes.size());
                ok = true;
            }
        }
        loop.quit();
    });
    loop.exec();
    reply->deleteLater();
    return ok;
}

// One POST, and its answer whatever its status - the same rules as above
bool exchange(const std::string& url, const std::vector<std::string>& headers, const std::string& payload,
              HttpAnswer& answer, std::int64_t cap) {
    answer = HttpAnswer{};
    if (url.rfind("https://", 0) != 0) {
        answer.why = "only https is accepted, and this is not: " + url;
        return false;
    }
    QNetworkAccessManager manager;
    manager.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkRequest request{QUrl(QString::fromStdString(url))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Bedrock/" BEDROCK_VERSION " (Linux)"));
    for (const std::string& header : headers) {
        const size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        request.setRawHeader(QByteArray::fromStdString(trim(header.substr(0, colon))),
                             QByteArray::fromStdString(trim(header.substr(colon + 1))));
    }
    request.setTransferTimeout(30000);
    QEventLoop loop;
    QNetworkReply* reply = manager.post(request, QByteArray(payload.data(), (qsizetype)payload.size()));
    bool answered = false;
    QObject::connect(reply, &QNetworkReply::finished, [&] {
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (status.isValid()) {
            answered = true;
            answer.status = status.toInt();
            const QByteArray bytes = reply->readAll();
            answer.body.assign(bytes.constData(), (size_t)(std::min)((std::int64_t)bytes.size(), cap));
        } else {
            answer.why = "could not reach it: " + reply->errorString().toStdString();
        }
        loop.quit();
    });
    loop.exec();
    reply->deleteLater();
    return answered;
}

std::string sha256Of(const std::string& bytes) {
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray::fromRawData(bytes.data(), (qsizetype)bytes.size()), QCryptographicHash::Sha256);
    return digest.toHex().toStdString();
}

#endif  // _WIN32

std::vector<int> partsOf(const std::string& version) {
    std::vector<int> out;
    std::string digits;
    for (char c : version + ".") {
        if (c >= '0' && c <= '9') {
            digits += c;
        } else if (c == '.') {
            out.push_back(digits.empty() ? 0 : std::atoi(digits.c_str()));
            digits.clear();
        } else {
            break;      // "4.9.0-beta" compares as 4.9.0
        }
    }
    return out;
}

}  // namespace

bool httpsGet(const std::string& url, const std::vector<std::string>& headers, std::string& body,
              std::string& why, std::int64_t cap) {
    return fetch(url, body, why, nullptr, cap, headers, true);
}

bool httpsPost(const std::string& url, const std::vector<std::string>& headers, const std::string& payload,
               HttpAnswer& answer, std::int64_t cap) {
    return exchange(url, headers, payload, answer, cap);
}

bool Updater::isNewer(const std::string& candidate, const std::string& current) {
    const std::vector<int> a = partsOf(candidate), b = partsOf(current);
    for (size_t i = 0; i < (std::max)(a.size(), b.size()); i++) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

bool Updater::parseManifest(const std::string& text, Release& out) {
    out = Release{};
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string clean = trim(line);
        if (clean.empty() || clean[0] == '#') continue;
        const size_t eq = clean.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(clean.substr(0, eq));
        const std::string value = trim(clean.substr(eq + 1));
        if (key == "version") out.version = value;
        else if (key == "notes") out.notes = value;
#ifdef _WIN32
        else if (key == "url") out.url = value;
        else if (key == "sha256") out.sha256 = value;
#else
        // The Linux package has keys of its own in the same file; the Windows
        // build never sees them and this build never sees the zip's
        else if (key == "url_linux") out.url = value;
        else if (key == "sha256_linux") out.sha256 = value;
#endif
    }
    if (out.version.empty() || partsOf(out.version).empty()) return false;
    if (out.url.rfind("https://", 0) != 0) return false;
    // No hash, no update: it is the only thing standing between a truncated
    // download and an unpacked one.
    if (out.sha256.size() != 64) return false;
    for (char c : out.sha256) {
        if (!isxdigit((unsigned char)c)) return false;
    }
    return true;
}

Updater::~Updater() {
    join();
}

void Updater::join() {
    if (worker_.joinable()) worker_.join();
}

void Updater::configure(const std::string& manifestUrl, bool enabled) {
    url_ = manifestUrl;
    // A build nobody has pointed anywhere yet must not go looking. The
    // placeholder in resource.h says OWNER/REPO and is not a real address.
    enabled_ = enabled && url_.rfind("https://", 0) == 0 &&
               url_.find("OWNER/REPO") == std::string::npos;
}

bool Updater::validInstallId(const std::string& id) {
    if (id.size() != 32) return false;
    for (char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string Updater::installId(const std::string& file) {
    {
        std::ifstream in(file);
        std::string kept;
        if (in && std::getline(in, kept)) {
            while (!kept.empty() && (kept.back() == '\r' || kept.back() == ' ')) kept.pop_back();
            if (validInstallId(kept)) return kept;
        }
    }
    // random_device is the system's own source on both: rand_s on Windows,
    // /dev/urandom on Linux
    std::random_device random;
    static const char* const HEX = "0123456789abcdef";
    std::string id;
    for (int word = 0; word < 4; word++) {
        const unsigned int value = random();
        for (int nibble = 0; nibble < 8; nibble++) id += HEX[(value >> (nibble * 4)) & 15];
    }
    std::ofstream out(file, std::ios::trunc);
    out << id << "\n";
    return out ? id : std::string();
}

std::vector<std::string> Updater::installHeader() const {
    if (installId_.empty()) return {};
    return {"X-Bedrock-Install: " + installId_};
}

void Updater::checkInBackground() {
    if (!enabled_ || state_ == State::Checking || state_ == State::Downloading) return;
    join();
    state_ = State::Checking;
    worker_ = std::thread([this] {
        std::string body, why;
        if (!fetch(url_, body, why, nullptr, 256 * 1024, installHeader())) {
            error_ = why;
            state_ = State::Failed;
            return;
        }
        Release found;
        if (!parseManifest(body, found)) {
            error_ = "the version file does not read like one";
            state_ = State::Failed;
            return;
        }
        latest_ = found;
        state_ = isNewer(found.version, BEDROCK_VERSION) ? State::Available : State::UpToDate;
    });
}

void Updater::downloadInBackground() {
    if (state_ != State::Available) return;
    join();
    percent_ = 0;
    state_ = State::Downloading;
    worker_ = std::thread([this] {
        std::string body, why;
        // the install's number only where the update lies beside the
        // version file - not to GitHub or wherever else url= points
        const bool beside = hostOf(latest_.url) == hostOf(url_);
        if (!fetch(latest_.url, body, why, &percent_, 64ll * 1024 * 1024,
                   beside ? installHeader() : std::vector<std::string>())) {
            error_ = why;
            state_ = State::Failed;
            return;
        }
        std::string digest = sha256Of(body);
        std::transform(digest.begin(), digest.end(), digest.begin(), ::tolower);
        std::string want = latest_.sha256;
        std::transform(want.begin(), want.end(), want.begin(), ::tolower);
        if (digest.empty() || digest != want) {
            // Not unpacked, not kept: what arrived is not what was promised
            error_ = "the download does not match its checksum - nothing was unpacked";
            state_ = State::Failed;
            return;
        }
        std::error_code ec;
        const std::filesystem::path folder = AppPaths::data() / "update";
        std::filesystem::create_directories(folder, ec);
#ifdef _WIN32
        const std::filesystem::path zip = folder / ("Bedrock_" + latest_.version + ".zip");
#else
        const std::filesystem::path zip = folder / ("Bedrock-" + latest_.version + "-x86_64.AppImage");
#endif
        std::ofstream out(zip, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            error_ = "cannot write to " + folder.string();
            state_ = State::Failed;
            return;
        }
        out.write(body.data(), (std::streamsize)body.size());
        out.close();
        zipPath_ = zip.string();
        state_ = State::Ready;
    });
}

// Windows' own tools by their full path. A tasklist, find, ping, xcopy or
// powershell that comes earlier on the PATH is another program - Git's Unix
// tools put a find there that never finds the pid, and the copying would
// begin while Bedrock is still running. Only the builtins of cmd itself
// (set, if, for, mkdir, rmdir, del, start) go by name.
std::string Updater::applyScript(const std::string& program, const std::string& zip,
                                 const std::string& staging, unsigned long pid) {
    const std::string id = std::to_string(pid);
    std::string s;
    s += "@echo off\r\n";
    s += "setlocal\r\n";
    s += "set \"PROG=" + program + "\"\r\n";
    s += "set \"ZIP=" + zip + "\"\r\n";
    s += "set \"STAGE=" + staging + "\"\r\n";
    s += ":: Windows' own tools, whatever else the PATH holds\r\n";
    s += "set \"SYS=%SystemRoot%\\System32\"\r\n";
    s += ":: wait for Bedrock to close - it cannot overwrite itself\r\n";
    s += "set /a tries=0\r\n";
    s += ":wait\r\n";
    s += "\"%SYS%\\tasklist.exe\" /fi \"PID eq " + id + "\" | \"%SYS%\\find.exe\" \"" + id +
         "\" >nul || goto gone\r\n";
    s += "\"%SYS%\\ping.exe\" -n 2 127.0.0.1 >nul\r\n";
    s += "set /a tries+=1\r\n";
    s += "if %tries% lss 30 goto wait\r\n";
    s += "echo Bedrock is still running - nothing was changed.\r\n";
    s += "exit /b 1\r\n";
    s += ":gone\r\n";
    s += "mkdir \"%STAGE%\" 2>nul\r\n";
    s += ":: NOT tar: the one that ships with Windows takes C:... for a remote\r\n";
    s += ":: host name, and this build of it will not read a zip at all.\r\n";
    s += ":: Expand-Archive is on every Windows 10 and later.\r\n";
    s += "\"%SYS%\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -Command "
         "\"Expand-Archive -LiteralPath '%ZIP%' -DestinationPath '%STAGE%' -Force\" || "
         "(echo could not unpack & exit /b 1)\r\n";
    s += ":: a hand-out zip holds one folder; step into it when it does\r\n";
    s += "for /d %%D in (\"%STAGE%\\*\") do if exist \"%%D\\Bedrock.exe\" set \"STAGE=%%D\"\r\n";
    s += "if not exist \"%STAGE%\\Bedrock.exe\" (echo no Bedrock.exe in the package & exit /b 1)\r\n";
    s += "\"%SYS%\\xcopy.exe\" \"%STAGE%\\*\" \"%PROG%\\\" /e /y /i >nul || (echo could not copy & exit /b 1)\r\n";
    s += "rmdir /s /q \"%STAGE%\" 2>nul\r\n";
    s += "del /q \"%ZIP%\" 2>nul\r\n";
    s += "start \"\" \"%PROG%\\Bedrock.exe\"\r\n";
    return s;
}

#ifndef _WIN32
// The hand-over on Linux: the AppImage is one file, and a running one may
// be written over - the kernel keeps the old inode for us. The new one is
// put beside it, made executable, moved into place, and started; only then
// does this one quit. Run from a plain binary rather than an AppImage
// ($APPIMAGE unset) there is nothing to replace, and the download is kept.
bool Updater::apply() {
    if (state_ != State::Ready || zipPath_.empty()) return false;
    join();
    const char* self = std::getenv("APPIMAGE");
    if (!self || !*self) {
        error_ = "not running as an AppImage - the new package is in " + zipPath_;
        return false;
    }
    std::error_code ec;
    const std::filesystem::path target(self);
    const std::filesystem::path fresh = target.parent_path() / (target.filename().string() + ".new");
    std::filesystem::copy_file(zipPath_, fresh, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        error_ = "cannot write beside " + target.string() + ": " + ec.message();
        return false;
    }
    std::filesystem::permissions(fresh, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                            std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    std::filesystem::rename(fresh, target, ec);
    if (ec) {
        error_ = "cannot replace " + target.string() + ": " + ec.message();
        std::filesystem::remove(fresh, ec);
        return false;
    }
    std::filesystem::remove(zipPath_, ec);
    // The new one, detached, so it outlives this process
    const std::string command = "nohup \"" + target.string() + "\" >/dev/null 2>&1 &";
    if (std::system(command.c_str()) != 0) {
        error_ = "the new Bedrock did not start; it is in place at " + target.string();
        return false;
    }
    return true;
}
#else
bool Updater::apply() {
    if (state_ != State::Ready || zipPath_.empty()) return false;
    join();
    std::error_code ec;
    const std::filesystem::path folder = AppPaths::data() / "update";
    const std::filesystem::path script = folder / "apply.cmd";
    const std::filesystem::path staging = folder / "staging";
    std::filesystem::remove_all(staging, ec);

    // The script, not the app, does the replacing: Windows will not let a
    // running exe be written over, so this waits for us to be gone first.
    // It only ever touches the PROGRAM folder - the player's things are
    // somewhere else entirely, which is the whole reason this is safe.
    std::ofstream out(script, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << applyScript(AppPaths::program().string(), zipPath_, staging.string(), GetCurrentProcessId());
    out.close();

    // cmd.exe out of the system folder as well, with /d (no AutoRun from
    // the registry) and /s, which takes off the outer pair of quotes and
    // leaves the script's own path whole, whatever characters it holds
    wchar_t systemDir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring shell =
        (length > 0 && length < MAX_PATH) ? std::wstring(systemDir) + L"\\cmd.exe" : std::wstring(L"cmd.exe");
    const std::wstring cmd = L"\"" + shell + L"\" /d /s /c \"\"" + wide(script.string()) + L"\"\"";
    std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
    buffer.push_back(L'\0');
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
#endif  // _WIN32

}  // namespace SCMiner
