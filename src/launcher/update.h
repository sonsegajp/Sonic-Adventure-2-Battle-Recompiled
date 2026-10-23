// Updates: the launcher asks GitHub for the latest release (its public API: no account, no sign-in)
// and, when it is newer than this one, offers it on the main menu. Updating downloads the release's
// zip, unpacks it (Windows' own tar.exe), and puts its files in place of ours - a running program or
// library is renamed to <name>.old first (Windows allows that), and the .old files are removed the
// next time the launcher starts. Only the release's own files are touched: saves, settings, mods and
// the game module built from the disc stay (a new version builds the module again, as always).
// Both run on their own thread; the launcher draws status() each frame.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace launcher {

class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Unpacking, Installing, Done, Failed };
    struct Status {
        State state = State::Idle;
        std::string latest;  // the newest release's version ("1.2.0")
        std::string error;   // why it failed
        uint64_t bytes = 0, total = 0;  // downloaded so far, of
    };

    ~Updater();
    // Asks for the latest release (in the background). current: this program's version.
    void check(const std::string& current);
    // Downloads and installs it into exeDir (in the background; status() Done: start the new launcher).
    void install(const std::filesystem::path& exeDir);
    Status status() const;
    // At start: what the last update left (the renamed .old files, its download folder).
    static void cleanup(const std::filesystem::path& exeDir);
    // a.b.c newer than x.y.z
    static bool newer(const std::string& a, const std::string& b);

private:
    void set(State s, const std::string& error = "");
    void runInstall(std::filesystem::path exeDir);

    mutable std::mutex m_lock;
    Status m_status;
    std::string m_url;  // the release zip's download link
    std::thread m_thread;
};

}  // namespace launcher
