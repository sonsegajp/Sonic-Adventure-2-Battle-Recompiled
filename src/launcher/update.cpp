#include "launcher/update.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

namespace launcher {

namespace fs = std::filesystem;

namespace {
constexpr const wchar_t* kLatest = L"https://api.github.com/repos/sonsegajp/Sonic-Adventure-2-Battle-Recompiled/releases/latest";
constexpr const char* kAsset = "-windows-x64.zip";  // the release's zip: SA2BRecompiled-<version>-windows-x64.zip

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::wstring wide(const std::string& s) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
    return w;
}

struct Handle {  // (a WinHTTP handle, closed when done)
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

// GET url (redirects followed, as GitHub's downloads need): each piece of the body goes to sink
// (false stops); total gets the length when the server says it.
bool httpGet(const std::wstring& url, const std::function<bool(const char*, size_t)>& sink, uint64_t& total, std::string& error) {
    wchar_t host[256], path[4096], extra[4096];
    URL_COMPONENTS u{};
    u.dwStructSize = sizeof u;
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 4096;
    u.lpszExtraInfo = extra, u.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) return error = "a bad address", false;
    const std::wstring where = std::wstring(path, u.dwUrlPathLength) + std::wstring(extra, u.dwExtraInfoLength);
    Handle session{WinHttpOpen(L"SA2BRecompiled-Launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h)  // (before Windows 8.1)
        session.h = WinHttpOpen(L"SA2BRecompiled-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return error = "no internet access (" + std::to_string(GetLastError()) + ")", false;
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    Handle conn{WinHttpConnect(session.h, std::wstring(host, u.dwHostNameLength).c_str(), u.nPort, 0)};
    if (!conn.h) return error = "couldn't reach the server (" + std::to_string(GetLastError()) + ")", false;
    Handle req{WinHttpOpenRequest(conn.h, L"GET", where.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!req.h) return error = "couldn't ask the server (" + std::to_string(GetLastError()) + ")", false;
    WinHttpAddRequestHeaders(req.h, L"Accept: application/vnd.github+json, application/octet-stream, */*", DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr))
        return error = "no answer from the server (" + std::to_string(GetLastError()) + ")", false;
    DWORD code = 0, size = sizeof code;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
    if (code != 200) return error = "the server answered " + std::to_string(code), false;
    wchar_t len[32];
    DWORD lenSize = sizeof len;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lenSize, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(len, nullptr, 10);
    std::vector<char> buf;
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) return error = "the download broke off (" + std::to_string(GetLastError()) + ")", false;
        if (!avail) break;
        buf.resize(avail);
        if (!WinHttpReadData(req.h, buf.data(), avail, &got)) return error = "the download broke off (" + std::to_string(GetLastError()) + ")", false;
        if (!got) break;
        if (!sink(buf.data(), got)) return error = "stopped", false;
    }
    return true;
}

// The string value of "key" in a JSON text, from pos on (GitHub's release fields: no escapes in them).
std::string jsonString(const std::string& s, const std::string& key, size_t& pos) {
    const size_t k = s.find("\"" + key + "\"", pos);
    if (k == std::string::npos) return pos = std::string::npos, std::string();
    const size_t a = s.find('"', s.find(':', k) + 1);
    const size_t b = a == std::string::npos ? a : s.find('"', a + 1);
    if (b == std::string::npos) return pos = std::string::npos, std::string();
    pos = b + 1;
    return s.substr(a + 1, b - a - 1);
}

// Runs a program and waits for it; its exit code (-1: it did not start).
long runWait(const std::wstring& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    return long(code);
}
}  // namespace

Updater::~Updater() {
    if (m_thread.joinable()) m_thread.detach();  // (quitting mid-download: the process ends anyway)
}

Updater::Status Updater::status() const {
    std::lock_guard g(m_lock);
    return m_status;
}

void Updater::set(State s, const std::string& error) {
    std::lock_guard g(m_lock);
    m_status.state = s, m_status.error = error;
}

bool Updater::newer(const std::string& a, const std::string& b) {
    std::istringstream x(a), y(b);
    for (int i = 0; i < 3; i++) {
        long p = 0, q = 0;
        char dot;
        x >> p, y >> q;
        if (p != q) return p > q;
        x >> dot, y >> dot;
    }
    return false;
}

void Updater::check(const std::string& current) {
    if (m_thread.joinable()) return;
    set(State::Checking);
    m_thread = std::thread([this, current] {
        std::string body, error;
        uint64_t total = 0;
        if (!httpGet(kLatest, [&](const char* p, size_t n) { return body.append(p, n), body.size() < (8u << 20); }, total, error))
            return set(State::Idle, error);  // (offline, or GitHub busy: no update shown)
        size_t pos = 0;
        std::string tag = jsonString(body, "tag_name", pos), url;
        for (size_t at = 0; at != std::string::npos;) {
            const std::string u = jsonString(body, "browser_download_url", at);
            if (u.size() > std::strlen(kAsset) && u.compare(u.size() - std::strlen(kAsset), std::strlen(kAsset), kAsset) == 0) url = u;
        }
        if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
        std::lock_guard g(m_lock);
        m_status.latest = tag, m_url = url;
        m_status.state = !url.empty() && newer(tag, current) ? State::Available : State::UpToDate;
    });
}

void Updater::install(const fs::path& exeDir) {
    if (m_thread.joinable()) m_thread.join();  // (the check: done by now)
    {
        std::lock_guard g(m_lock);
        if (m_url.empty()) return;
        m_status.state = State::Downloading, m_status.bytes = m_status.total = 0, m_status.error.clear();
    }
    m_thread = std::thread([this, exeDir] { runInstall(exeDir); });
}

void Updater::runInstall(fs::path exeDir) {
    std::error_code ec;
    const fs::path dir = exeDir / "update", zip = dir / "release.zip", unpacked = dir / "unpacked";
    fs::remove_all(dir, ec);
    fs::create_directories(unpacked, ec);
    if (ec) return set(State::Failed, "can't write in " + u8(exeDir) + " (choose a folder of your own, not Program Files)");

    // 1. download
    std::string url, error;
    {
        std::lock_guard g(m_lock);
        url = m_url;
    }
    {
        std::ofstream out(zip, std::ios::binary);
        uint64_t total = 0;
        const bool ok = httpGet(wide(url),
                                [&](const char* p, size_t n) {
                                    out.write(p, std::streamsize(n));
                                    std::lock_guard g(m_lock);
                                    m_status.bytes += n, m_status.total = std::max(m_status.total, total);
                                    return bool(out);
                                },
                                total, error);
        if (!ok || !out) return set(State::Failed, "the download failed: " + (error.empty() ? "couldn't save it" : error));
    }

    // 2. unpack (Windows 10's own tar.exe reads zips)
    set(State::Unpacking);
    const char* root = std::getenv("SystemRoot");
    const fs::path tar = fs::path(root ? root : "C:\\Windows") / "System32" / "tar.exe";
    if (runWait(tar.wstring(), L"-xf \"" + zip.wstring() + L"\" -C \"" + unpacked.wstring() + L"\"") != 0)
        return set(State::Failed, "couldn't unpack the download");
    fs::path from = unpacked;  // (the zip holds one folder, SA2BRecompiled)
    for (const auto& e : fs::directory_iterator(unpacked, ec))
        if (e.is_directory()) from = e.path();
    if (!fs::exists(from / "SA2BLauncher.exe", ec) || !fs::exists(from / "sa2b.exe", ec))
        return set(State::Failed, "the download isn't a release of this program");

    // 3. put its files in place of ours: each one there is renamed to .old first (a running program
    // or library can be renamed, not overwritten); anything going wrong puts ours back
    set(State::Installing);
    struct Moved {
        fs::path target, old;
    };
    std::vector<Moved> moved;
    auto undo = [&] {
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code e2;
            fs::remove(it->target, e2);
            if (!it->old.empty()) fs::rename(it->old, it->target, e2);
        }
    };
    for (const auto& e : fs::recursive_directory_iterator(from, ec)) {
        if (!e.is_regular_file()) continue;
        const fs::path target = exeDir / fs::relative(e.path(), from, ec);
        fs::create_directories(target.parent_path(), ec);
        Moved m{target, {}};
        if (fs::exists(target, ec)) {
            m.old = target;
            m.old += ".old";
            fs::remove(m.old, ec);
            if (fs::exists(m.old, ec)) m.old.replace_extension(".old" + std::to_string(GetTickCount64()));  // (an older .old still in use)
            fs::rename(target, m.old, ec);
            if (ec) return undo(), set(State::Failed, "couldn't replace " + u8(target.filename()) + ": " + ec.message());
        }
        moved.push_back(m);
        fs::copy_file(e.path(), target, fs::copy_options::overwrite_existing, ec);
        if (ec) return undo(), set(State::Failed, "couldn't write " + u8(target.filename()) + ": " + ec.message());
    }
    set(State::Done);
}

void Updater::cleanup(const fs::path& exeDir) {
    std::error_code ec;
    for (const fs::path& d : {exeDir, exeDir / "tools", exeDir / "tools" / "llvm"})
        for (const auto& e : fs::directory_iterator(d, ec))
            if (e.is_regular_file() && u8(e.path().extension()).rfind(".old", 0) == 0) fs::remove(e.path(), ec);
    fs::remove_all(exeDir / "update", ec);
}

}  // namespace launcher
