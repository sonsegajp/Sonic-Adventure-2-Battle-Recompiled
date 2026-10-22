// First-run setup: builds the game's code from the player's own disc. Nothing translated from the
// game ships with the program - the launcher makes sa2b_game.dll on this computer:
//
//   1. Verify    the disc image's SHA-1 against the known dump (else: its program files' SHA-1)
//   2. Extract   main.dol and every code module (.rel, and the PRS-compressed Chao World ones)
//   3. Recompile the PowerPC code to C++ (src/recompiler, linked into the launcher)
//   4. Compile   that C++ with the bundled clang (tools/llvm), one process per core
//   5. Link      sa2b_game.dll with the bundled lld-link, against sa2b.lib (the runtime's exports)
//
// It runs on its own thread; the launcher draws status() each frame (docs/SETUP.md has the details).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace launcher {

class Setup {
public:
    enum Step { Verify, Extract, Recompile, Compile, Link, kSteps };
    static const char* stepName(int s);

    struct Status {
        bool running = false, done = false, failed = false, cancelled = false;
        int step = Verify;
        float stepFrac = 0;   // 0-1 within the step
        float overall = 0;    // 0-1 over all steps
        std::string detail;   // what the step is on, e.g. "120 of 386 files"
        std::string error;    // why it failed
        std::string note;     // something worth telling that is not an error
        float elapsed = 0;    // seconds
        float eta = -1;       // seconds left (-1: not known yet)
    };

    ~Setup();
    // Whether sa2b_game.dll has to be built (again): missing, made by another version of the
    // launcher, or from another disc image.
    static bool needed(const std::filesystem::path& exeDir, const std::filesystem::path& disc);
    // Where the bundled compiler is (tools/llvm next to the launcher; build/llvm when developing).
    static std::filesystem::path toolchain(const std::filesystem::path& exeDir);

    void start(const std::filesystem::path& exeDir, const std::filesystem::path& disc, const std::filesystem::path& logFile);
    void cancel();
    Status status() const;
    bool running() const { return m_running.load(); }

private:
    void run();
    bool verify(std::string& sha1);
    bool extract(std::vector<uint8_t>& dol, std::vector<std::pair<std::string, std::vector<uint8_t>>>& rels, std::string& programSha1);
    bool recompile(const std::vector<uint8_t>& dol, std::vector<std::pair<std::string, std::vector<uint8_t>>>& rels);
    bool compile();
    bool link();
    void set(int step, float frac, const std::string& detail);
    bool fail(const std::string& why);
    void log(const std::string& line);

    std::filesystem::path m_exeDir, m_disc, m_work, m_logFile;
    std::thread m_thread;
    std::atomic<bool> m_running{false}, m_cancel{false};
    mutable std::mutex m_lock;
    Status m_status;
    std::chrono::steady_clock::time_point m_t0;
    std::vector<std::filesystem::path> m_objects;
};

}  // namespace launcher
