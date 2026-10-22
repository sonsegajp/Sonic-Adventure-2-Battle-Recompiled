#include "launcher/setup.h"

#include "launcher/disc.h"
#include "launcher/sha1.h"
#include "recompiler/build.h"
#include "runtime/abi.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifndef SA2B_VERSION
#define SA2B_VERSION "dev"
#endif

namespace launcher {

namespace fs = std::filesystem;

namespace {

struct EmbeddedFile {
    const char* path;
    const unsigned char* data;
    size_t size;
};
#include "setup_files.inc"  // the module's headers and glue, the name lists (tools/launcher/embed.cmake)

std::string embedded(const char* path) {
    for (const EmbeddedFile& f : kEmbedded)
        if (!std::strcmp(f.path, path)) return std::string(reinterpret_cast<const char*>(f.data), f.size);
    return {};
}

constexpr const char* kGameId = "GSNE8P";
// The disc the recompiler is made for: Redump's dump of Sonic Adventure 2: Battle (USA), 1,459,978,240
// bytes, CRC32 024f4f8f.
constexpr const char* kDiscSha1 = "f8cc51128df2a3b0da9d6f3a234c895a4cd7dc22";
// Its program as extracted (programSha1 below: main.dol and the 95 code modules). A disc image that
// is not a 1:1 copy (trimmed, another dump tool) still has the same program: it is accepted on this.
constexpr const char* kProgramSha1 = "e23d2e3b41d2b051b3360fb1b55f9af6a96d6c37";
// Each step's share of the overall bar: about its share of the time (8 threads: disc ~10 s, program
// ~2 s, recompiling ~27 s, compiling ~530 s, linking ~1 s).
constexpr float kWeight[Setup::kSteps] = {0.02f, 0.01f, 0.05f, 0.91f, 0.01f};
constexpr const char* kDll = "sa2b_game.dll";
constexpr const char* kStamp = "sa2b_game.txt";

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// A REL module's header: id, unlinked list, a sane section table, version 1-3.
bool isRel(const Bytes& h) {
    if (h.size() < 0x40) return false;
    const uint32_t id = be32(&h[0]), next = be32(&h[4]), prev = be32(&h[8]), nsec = be32(&h[12]), secOff = be32(&h[16]), ver = be32(&h[28]);
    return id > 0 && id < 0x1000 && next == 0 && prev == 0 && nsec > 1 && nsec < 64 && (secOff == 0x40 || secOff == 0x48 || secOff == 0x4C) &&
           ver >= 1 && ver <= 3;
}

// sa2b_game.txt: what the module next to it was built by and from.
struct Stamp {
    std::string version, disc, sha1;
    uint64_t size = 0;
    int64_t time = 0;
    bool operator==(const Stamp& o) const = default;
};
Stamp readStamp(const fs::path& p) {
    Stamp s;
    std::ifstream f(p);
    for (std::string l; std::getline(f, l);) {
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "version") s.version = v;
        else if (k == "disc") s.disc = v;
        else if (k == "sha1") s.sha1 = v;
        else if (k == "size") s.size = std::strtoull(v.c_str(), nullptr, 10);
        else if (k == "time") s.time = std::strtoll(v.c_str(), nullptr, 10);
    }
    return s;
}
// The disc as the stamp knows it: its path, size and time (re-verified whenever one changes).
Stamp discIdentity(const fs::path& disc) {
    Stamp s;
    std::error_code ec;
    s.version = std::string(SA2B_VERSION) + "/abi" + std::to_string(SA2B_GAME_ABI);
    s.disc = u8(disc);
    s.size = fs::file_size(disc, ec);
    s.time = int64_t(fs::last_write_time(disc, ec).time_since_epoch().count());
    return s;
}
void writeStamp(const fs::path& p, const Stamp& s) {
    std::ofstream f(p);
    f << "version=" << s.version << "\ndisc=" << s.disc << "\nsize=" << s.size << "\ntime=" << s.time << "\nsha1=" << s.sha1 << "\n";
}

std::string fmtBytes(uint64_t n) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f MB", double(n) / 1048576.0);
    return b;
}

#ifdef _WIN32
// A child process (the compiler, the linker) with its output going to a log file. Only that file's
// handle is inherited, so the many children never hold each other's logs open.
struct Child {
    HANDLE proc = nullptr;
    size_t index = 0;
    fs::path log;
};
bool spawn(HANDLE job, const fs::path& exe, const std::wstring& args, const fs::path& dir, const fs::path& logPath, Child& c, std::string& err) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return err = "can't write " + u8(logPath), false;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> buf(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = out;
    si.lpAttributeList = attrs;
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr,
                                   dir.c_str(), &si.StartupInfo, &pi);
    const DWORD e = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(out);
    if (!ok) return err = "couldn't start " + u8(exe.filename()) + " (error " + std::to_string(e) + ")", false;
    AssignProcessToJobObject(job, pi.hProcess);  // ends with the launcher, whatever happens to it
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c.proc = pi.hProcess;
    c.log = logPath;
    return true;
}
HANDLE makeJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    return job;
}
#endif

std::string readText(const fs::path& p, size_t maxLines) {
    std::ifstream f(p);
    std::string out;
    size_t n = 0;
    for (std::string l; n < maxLines && std::getline(f, l); n++) out += l + "\n";
    return out;
}

}  // namespace

const char* Setup::stepName(int s) {
    static const char* names[kSteps] = {"Checking your disc", "Extracting the game's program", "Recompiling it to C++",
                                        "Compiling the game", "Linking the game"};
    return s >= 0 && s < kSteps ? names[s] : "";
}

Setup::~Setup() {
    cancel();
    if (m_thread.joinable()) m_thread.join();
}

fs::path Setup::toolchain(const fs::path& exeDir) {
    std::error_code ec;
    const fs::path dir = exeDir.has_filename() ? exeDir : exeDir.parent_path();  // (the base path ends in a separator)
    for (const fs::path& d : {dir / "tools" / "llvm", dir.parent_path() / "llvm"})
        if (fs::exists(d / "clang.exe", ec) && fs::exists(d / "lld-link.exe", ec)) return d;
    return exeDir / "tools" / "llvm";
}

bool Setup::needed(const fs::path& exeDir, const fs::path& disc) {
    std::error_code ec;
    if (!fs::exists(exeDir / kDll, ec)) return true;
    Stamp want = discIdentity(disc), have = readStamp(exeDir / kStamp);
    want.sha1 = have.sha1;
    return !(want == have);
}

void Setup::start(const fs::path& exeDir, const fs::path& disc, const fs::path& logFile) {
    if (m_running) return;
    if (m_thread.joinable()) m_thread.join();
    m_exeDir = exeDir, m_disc = disc, m_logFile = logFile;
    const char* la = std::getenv("LOCALAPPDATA");
    m_work = la ? fs::path(std::u8string(reinterpret_cast<const char8_t*>(la))) / "SA2BRecompiled" / "build" : exeDir / "setup_work";
    {
        std::lock_guard g(m_lock);
        m_status = {};
        m_status.running = true;
    }
    m_cancel = false;
    m_running = true;
    m_t0 = std::chrono::steady_clock::now();
    std::ofstream(m_logFile, std::ios::trunc);
    m_thread = std::thread([this] { run(); });
}

void Setup::cancel() { m_cancel = true; }

Setup::Status Setup::status() const {
    std::lock_guard g(m_lock);
    Status s = m_status;
    s.elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_t0).count();
    // time left from the pace so far, once there is enough of it to go on
    if (s.running && s.overall > 0.08f && s.elapsed > 10) s.eta = s.elapsed * (1 - s.overall) / s.overall;
    return s;
}

void Setup::set(int step, float frac, const std::string& detail) {
    std::lock_guard g(m_lock);
    m_status.step = step;
    m_status.stepFrac = std::clamp(frac, 0.f, 1.f);
    float o = 0;
    for (int i = 0; i < step; i++) o += kWeight[i];
    m_status.overall = std::min(1.f, o + kWeight[step] * m_status.stepFrac);
    m_status.detail = detail;
}

bool Setup::fail(const std::string& why) {
    log("FAILED: " + why);
    std::lock_guard g(m_lock);
    if (m_cancel) m_status.cancelled = true;
    else m_status.failed = true, m_status.error = why;
    return false;
}

void Setup::log(const std::string& line) {
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_t0).count();
    char b[32];
    std::snprintf(b, sizeof b, "%7.1f s  ", t);
    std::ofstream(m_logFile, std::ios::app) << b << line << "\n";
}

void Setup::run() {
    log("setup " SA2B_VERSION ": building " + std::string(kDll) + " from " + u8(m_disc));
    std::error_code ec;
    bool ok = false;
    do {
        // the program's folder must take the module (not so in Program Files, say)
        {
            const fs::path probe = m_exeDir / "sa2b_write_test.tmp";
            std::ofstream(probe) << "1";
            if (!fs::exists(probe, ec)) {
                fail("The game's folder can't be written to (" + u8(m_exeDir) + "). Move the game to a folder of your own, such as Documents, and try again.");
                break;
            }
            fs::remove(probe, ec);
        }
        const fs::path tools = toolchain(m_exeDir);
        if (!fs::exists(tools / "clang.exe", ec) || !fs::exists(tools / "lld-link.exe", ec) || !fs::exists(m_exeDir / "sa2b.lib", ec)) {
            fail("Files the setup needs are missing (tools/llvm, sa2b.lib). Extract the whole download again.");
            break;
        }
        std::string discSha1;
        if (!verify(discSha1)) break;
        Stamp stamp = discIdentity(m_disc);
        stamp.sha1 = discSha1;
        // the same disc under another name or date, and a module already built from it: nothing to do
        if (const Stamp old = readStamp(m_exeDir / kStamp); fs::exists(m_exeDir / kDll, ec) && old.version == stamp.version && old.sha1 == discSha1) {
            log("module already built from this disc");
            writeStamp(m_exeDir / kStamp, stamp);
            ok = true;
            break;
        }
        std::vector<uint8_t> dol;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> rels;
        std::string programSha1;
        if (!extract(dol, rels, programSha1)) break;
        if (discSha1 != kDiscSha1) {
            if (programSha1 != kProgramSha1) {
                fail(std::string("This disc image is not the Sonic Adventure 2: Battle (USA) disc the recompiler was made for: its program differs. "
                                 "Expected the Redump dump, SHA-1 ") + kDiscSha1 + ".");
                break;
            }
            std::lock_guard g(m_lock);
            m_status.note = "Your disc image is not a 1:1 copy of the Redump dump, but the game's program on it is identical.";
        }
        fs::remove_all(m_work, ec);
        fs::create_directories(m_work / "gen", ec);
        if (!recompile(dol, rels)) break;
        dol.clear(), rels.clear();
        if (!compile()) break;
        if (!link()) break;
        // in place: the new module, then the stamp that says what it is
        fs::remove(m_exeDir / kStamp, ec);
        fs::copy_file(m_work / kDll, m_exeDir / kDll, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fail("Couldn't put " + std::string(kDll) + " in the game's folder: " + ec.message() + ". Is the game still running?");
            break;
        }
        writeStamp(m_exeDir / kStamp, stamp);
        ok = true;
    } while (false);
    if (!ok && m_cancel) log("cancelled");
    // the work files (about 1 GB) are never kept: a failed build is started over
    fs::remove_all(m_work, ec);
    {
        std::lock_guard g(m_lock);
        m_status.running = false;
        m_status.done = ok;
        if (!ok && m_cancel) m_status.cancelled = true, m_status.failed = false;
        if (ok) m_status.overall = 1, m_status.stepFrac = 1;
    }
    log(ok ? "done" : "stopped");
    m_running = false;
}

// 1. The whole image's SHA-1.
bool Setup::verify(std::string& sha1) {
    set(Verify, 0, "");
    Disc disc;
    std::string err;
    if (!disc.open(m_disc, err)) return fail("Can't read the disc image: " + err + ".");
    if (disc.gameId() != kGameId) return fail("This disc image is " + disc.gameId() + ", not Sonic Adventure 2: Battle (USA, GSNE8P).");
    std::ifstream f(m_disc, std::ios::binary);
    std::error_code ec;
    const uint64_t total = fs::file_size(m_disc, ec);
    Sha1 h;
    std::vector<char> buf(8 << 20);
    uint64_t done = 0;
    while (f) {
        if (m_cancel) return fail("cancelled");
        f.read(buf.data(), std::streamsize(buf.size()));
        const size_t n = size_t(f.gcount());
        if (!n) break;
        h.add(buf.data(), n);
        done += n;
        set(Verify, total ? float(double(done) / double(total)) : 0, fmtBytes(done) + " of " + fmtBytes(total));
    }
    if (done != total) return fail("Couldn't read the whole disc image.");
    sha1 = h.hex();
    log("disc SHA-1 " + sha1 + (sha1 == kDiscSha1 ? " (the Redump dump)" : " (not the Redump dump's " + std::string(kDiscSha1) + ")"));
    return true;
}

// 2. main.dol and every code module: the .rel files, and the PRS-compressed ones (the Chao World's,
//    ChaoMain.prs and others: a .prs that unpacks to a REL header), named as tools/extract.py does.
bool Setup::extract(std::vector<uint8_t>& dol, std::vector<std::pair<std::string, std::vector<uint8_t>>>& rels, std::string& programSha1) {
    set(Extract, 0, "");
    Disc disc;
    std::string err;
    if (!disc.open(m_disc, err) || !disc.readDol(dol)) return fail("Can't read the game's program from the disc image.");
    std::vector<Disc::File> candidates;
    for (const Disc::File& f : disc.files()) {
        std::string low = f.path;
        for (char& c : low) c = char(std::tolower(uint8_t(c)));
        if (low.size() > 4 && (low.ends_with(".rel") || low.ends_with(".prs"))) candidates.push_back(f);
    }
    size_t packed = 0;
    for (size_t i = 0; i < candidates.size(); i++) {
        if (m_cancel) return fail("cancelled");
        const Disc::File& f = candidates[i];
        std::string name = f.path;
        std::replace(name.begin(), name.end(), '/', '_');
        const bool prs = name.size() > 4 && (name.ends_with(".prs") || name.ends_with(".PRS"));
        if (prs) {
            Bytes head(std::min<uint32_t>(f.size, 512));
            if (!disc.readAt(f.offset, head.data(), head.size())) return fail("Can't read " + f.path + " from the disc image.");
            if (isRel(prsDecompress(head, 0x40))) {
                Bytes raw(f.size);
                if (!disc.readAt(f.offset, raw.data(), raw.size())) return fail("Can't read " + f.path + " from the disc image.");
                rels.emplace_back(name.substr(0, name.size() - 4) + "_prs.rel", prsDecompress(raw));
                packed++;
            }
        } else {
            Bytes raw(f.size);
            if (!disc.readAt(f.offset, raw.data(), raw.size())) return fail("Can't read " + f.path + " from the disc image.");
            rels.emplace_back(name, std::move(raw));
        }
        set(Extract, float(i + 1) / float(candidates.size()), std::to_string(rels.size()) + " code modules found");
    }
    std::sort(rels.begin(), rels.end(), [](auto& a, auto& b) { return a.first < b.first; });
    // the program's SHA-1: each file's name, a NUL, its size (8 bytes, little-endian) and contents
    Sha1 h;
    auto add = [&](const std::string& name, const std::vector<uint8_t>& data) {
        uint8_t len[8];
        for (int k = 0; k < 8; k++) len[k] = uint8_t(uint64_t(data.size()) >> (8 * k));
        h.add(name.c_str(), name.size() + 1);
        h.add(len, 8);
        h.add(data.data(), data.size());
    };
    add("main.dol", dol);
    for (auto& [n, d] : rels) add(n, d);
    programSha1 = h.hex();
    log("main.dol " + std::to_string(dol.size()) + " bytes, " + std::to_string(rels.size()) + " code modules (" + std::to_string(packed) +
        " PRS-compressed); program SHA-1 " + programSha1);
    return true;
}

// 3. PowerPC -> C++, in this process (src/recompiler).
bool Setup::recompile(const std::vector<uint8_t>& dol, std::vector<std::pair<std::string, std::vector<uint8_t>>>& rels) {
    set(Recompile, 0, "Finding every function");
    sa2rc::Input in;
    in.dol = dol;
    in.rels = std::move(rels);
    in.names = embedded("names.txt");
    in.patches = embedded("patchpoints.txt");
    sa2rc::Program prog;
    std::string err, notes;
    // (about 80% of this step's time is the analysis)
    if (!sa2rc::buildProgram(in, prog, err, &notes, [this](float f) { set(Recompile, 0.8f * f, "Finding every function"); }))
        return fail("The recompiler couldn't read the game's program: " + err);
    if (!notes.empty()) log(notes.substr(0, notes.size() - 1));
    if (m_cancel) return fail("cancelled");
    set(Recompile, 0.8f, "Translating to C++");
    sa2rc::EmitStats st;
    std::vector<sa2rc::OutputFile> files = sa2rc::emitProgram(prog, st);
    if (m_cancel) return fail("cancelled");
    log("recompiled " + std::to_string(st.functions) + " functions, " + std::to_string(st.instructions) + " instructions, into " +
        std::to_string(files.size()) + " files");
    uint64_t bytes = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (m_cancel) return fail("cancelled");
        std::ofstream o(m_work / "gen" / files[i].name, std::ios::binary);
        o.write(files[i].text.data(), std::streamsize(files[i].text.size()));
        if (!o) return fail("Couldn't write the recompiled code to " + u8(m_work) + " (is the disc full? It needs about 1 GB).");
        bytes += files[i].text.size();
        set(Recompile, 0.95f + 0.05f * float(i + 1) / float(files.size()), std::to_string(i + 1) + " of " + std::to_string(files.size()) + " files written");
    }
    log("wrote " + fmtBytes(bytes) + " of C++");
    // what the module is compiled with besides: the runtime's headers and the glue
    for (const EmbeddedFile& f : kEmbedded) {
        const fs::path p = m_work / fs::path(f.path);
        if (p.extension() == ".txt") continue;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(f.data), std::streamsize(f.size));
    }
    return true;
}

// 4. Every file with the bundled clang, as many at once as there are processor threads (and memory
//    for them), largest first so the last ones finish together.
bool Setup::compile() {
#ifdef _WIN32
    set(Compile, 0, "");
    struct Src {
        fs::path rel;  // from the work folder
        uint64_t size;
    };
    std::vector<Src> srcs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(m_work / "gen", ec))
        if (e.path().extension() == ".cpp") srcs.push_back({fs::path("gen") / e.path().filename(), e.file_size(ec)});
    srcs.push_back({"glue.cpp", fs::file_size(m_work / "glue.cpp", ec)});
    std::sort(srcs.begin(), srcs.end(), [](const Src& a, const Src& b) { return a.size > b.size; });
    uint64_t total = 0, done = 0;
    for (const Src& s : srcs) total += s.size;
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    // A compiler takes up to ~300 MB here. Memory other programs hold is not counted as lost: Windows
    // pages it out when the compilers need it (one per 2 GB installed); what is free right now can
    // allow more.
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const uint64_t byFree = ms.ullAvailPhys / (350ull << 20), byInstalled = ms.ullTotalPhys / (2048ull << 20);
    const unsigned byMemory = unsigned(std::max<uint64_t>({1, byFree, byInstalled}));
    const unsigned jobs = std::min({threads, byMemory, 32u});
    log("compiling " + std::to_string(srcs.size()) + " files (" + fmtBytes(total) + ") with " + std::to_string(jobs) + " at a time");
    fs::create_directories(m_work / "obj", ec);
    const fs::path clang = toolchain(m_exeDir) / "clang.exe";
    HANDLE job = makeJob();
    std::vector<Child> active;
    size_t next = 0, finished = 0;
    m_objects.clear();
    auto stopAll = [&] {
        for (Child& c : active) TerminateProcess(c.proc, 1), WaitForSingleObject(c.proc, 5000), CloseHandle(c.proc);
        active.clear();
        CloseHandle(job);
    };
    while (next < srcs.size() || !active.empty()) {
        if (m_cancel) return stopAll(), fail("cancelled");
        while (active.size() < jobs && next < srcs.size()) {
            const Src& s = srcs[next];
            const fs::path obj = fs::path("obj") / s.rel.filename().replace_extension(".obj");
            const std::wstring args = L"--target=x86_64-pc-windows-msvc -std=c++20 -O2 -g0 -ffreestanding -nostdinc -fno-exceptions -fno-rtti "
                                      L"-fno-threadsafe-statics -DSA2B_GAME_DLL -Iinclude -Igen -w -c \"" +
                                      s.rel.wstring() + L"\" -o \"" + obj.wstring() + L"\"";
            Child c;
            std::string err;
            if (!spawn(job, clang, args, m_work, m_work / "obj" / s.rel.filename().replace_extension(".log"), c, err)) return stopAll(), fail(err);
            c.index = next++;
            active.push_back(c);
            m_objects.push_back(obj);
        }
        std::vector<HANDLE> hs;
        for (const Child& c : active) hs.push_back(c.proc);
        WaitForMultipleObjects(DWORD(hs.size()), hs.data(), FALSE, 250);
        for (size_t i = 0; i < active.size();) {
            if (WaitForSingleObject(active[i].proc, 0) != WAIT_OBJECT_0) {
                i++;
                continue;
            }
            DWORD code = 1;
            GetExitCodeProcess(active[i].proc, &code);
            CloseHandle(active[i].proc);
            const Child c = active[i];
            active.erase(active.begin() + ptrdiff_t(i));
            if (code != 0) {
                const std::string out = readText(c.log, 12);
                log("compiler error in " + u8(srcs[c.index].rel) + ":\n" + out);
                stopAll();
                return fail("The compiler stopped on " + u8(srcs[c.index].rel.filename()) + ". The details are in setup.log.");
            }
            done += srcs[c.index].size;
            finished++;
        }
        set(Compile, float(double(done) / double(total)), std::to_string(finished) + " of " + std::to_string(srcs.size()) + " files");
    }
    CloseHandle(job);
    log("compiled");
    return true;
#else
    return fail("Setting up is only written for Windows so far.");
#endif
}

// 5. sa2b_game.dll: no C library, no entry point; the runtime's functions from sa2b.lib.
bool Setup::link() {
#ifdef _WIN32
    set(Link, 0, "");
    {
        std::ofstream rsp(m_work / "objs.rsp");
        for (const fs::path& o : m_objects) rsp << "\"" << o.generic_string() << "\"\n";
    }
    const std::wstring args = L"/nologo /dll /noentry /nodefaultlib /machine:x64 /opt:ref /opt:noicf /out:" + std::wstring(L"sa2b_game.dll") +
                              L" @objs.rsp \"" + (m_exeDir / "sa2b.lib").wstring() + L"\"";
    HANDLE job = makeJob();
    Child c;
    std::string err;
    if (!spawn(job, toolchain(m_exeDir) / "lld-link.exe", args, m_work, m_work / "link.log", c, err)) return CloseHandle(job), fail(err);
    // the linker doesn't say how far it is: the bar creeps towards the end over about the time it takes
    const auto t0 = std::chrono::steady_clock::now();
    while (WaitForSingleObject(c.proc, 100) == WAIT_TIMEOUT) {
        if (m_cancel) {
            TerminateProcess(c.proc, 1);
            CloseHandle(c.proc), CloseHandle(job);
            return fail("cancelled");
        }
        const float s = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        set(Link, 1 - std::exp(-s / 1.5f), "");
    }
    DWORD code = 1;
    GetExitCodeProcess(c.proc, &code);
    CloseHandle(c.proc), CloseHandle(job);
    if (code != 0) {
        log("linker error:\n" + readText(m_work / "link.log", 20));
        return fail("The linker stopped. The details are in setup.log.");
    }
    std::error_code ec;
    log("linked " + fmtBytes(fs::file_size(m_work / kDll, ec)));
    set(Link, 1, "");
    return true;
#else
    return fail("Setting up is only written for Windows so far.");
#endif
}

}  // namespace launcher
