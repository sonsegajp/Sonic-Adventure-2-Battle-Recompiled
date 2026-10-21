// DVD: the disc's file system table sits in guest memory the way the IPL leaves it, so the SDK's
// own file-system code (DVDOpen, DVDConvertPathToEntrynum, DVDChangeDir...) runs natively. Only the
// reads are replaced: a worker thread serves them from the user's disc image and completes each one
// through the interrupt path (callbacks run in interrupt context, as on the console).
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace rt {

namespace {
std::FILE* s_disc = nullptr;
std::vector<u8> s_fstCopy;  // the FST as read from the disc, for SA2_DVD_LOG checks
std::mutex s_discLock;

// DVDCommandBlock (2001 SDK)
enum : u32 {
    CB_COMMAND = 0x08,
    CB_STATE = 0x0C,
    CB_OFFSET = 0x10,
    CB_LENGTH = 0x14,
    CB_ADDR = 0x18,
    CB_CURR = 0x1C,
    CB_TRANSFERRED = 0x20,
    CB_ID = 0x24,
    CB_CALLBACK = 0x28,
};
enum : u32 { ST_END = 0, ST_BUSY = 1, ST_CANCELED = 10 };

struct Request {
    u32 block, addr, length, callback;
    u64 offset;
};
std::mutex s_qLock;
std::condition_variable s_qCv;
std::deque<Request> s_queue;
bool s_workerStarted = false;

bool readRaw(u64 offset, void* dst, u32 len) {
    std::lock_guard g(s_discLock);
    if (!s_disc) return false;
    _fseeki64(s_disc, s64(offset), SEEK_SET);
    return std::fread(dst, 1, len, s_disc) == len;
}

void complete(const Request& r, s32 result) {
    sched::postIrq([r, result](Ctx& c) {
        if (rd32(r.block + CB_STATE) == ST_CANCELED) return;
        wr32(r.block + CB_STATE, result >= 0 ? ST_END : u32(-1));
        wr32(r.block + CB_TRANSFERRED, result >= 0 ? u32(result) : 0);
        if (r.callback) {
            c.r[3] = u32(result);
            c.r[4] = r.block;
            c.call(r.callback);
        }
    });
}

void worker() {
    profileRegisterThread();
    std::vector<u8> buf;
    for (;;) {
        Request r;
        {
            std::unique_lock lk(s_qLock);
            s_qCv.wait(lk, [] { return !s_queue.empty(); });
            r = s_queue.front();
            s_queue.pop_front();
        }
        buf.resize(r.length);
        const bool ok = readRaw(r.offset, buf.data(), r.length);
        // A disc read takes time on the console; some code races its own callbacks otherwise.
        std::this_thread::sleep_for(std::chrono::microseconds(500 + r.length / 16));
        if (ok) std::memcpy(hostp(r.addr), buf.data(), r.length);
        complete(r, ok ? s32(r.length) : -1);
    }
}

// File containing a disc offset (from the FST in guest memory), for SA2_DVD_LOG.
std::string fileAt(u64 offset) {
    const u32 fst = rd32(0x80000038);
    if (!fst) return "?";
    const u32 n = rd32(fst + 8);
    const u32 strtab = fst + n * 12;
    // Walk the tree keeping the directory path of each entry.
    std::vector<std::pair<u32, std::string>> dirs{{n, ""}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        const u32 w0 = rd32(fst + i * 12), w1 = rd32(fst + i * 12 + 4), w2 = rd32(fst + i * 12 + 8);
        const std::string name = gstr(strtab + (w0 & 0xFFFFFF));
        if (w0 >> 24) dirs.push_back({w2, dirs.back().second + name + "/"});
        else if (offset >= w1 && offset < u64(w1) + w2) return dirs.back().second + name + "+" + std::to_string(offset - w1);
    }
    return "?";
}

void submit(u32 block, u32 addr, u32 length, u64 offset, u32 callback, u32 command) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    if (s_log) log("[dvd] read %s (%u bytes) -> %08X\n", fileAt(offset).c_str(), length, addr);
    wr32(block + CB_COMMAND, command);
    wr32(block + CB_STATE, ST_BUSY);
    wr32(block + CB_ADDR, addr);
    wr32(block + CB_LENGTH, length);
    wr32(block + CB_OFFSET, u32(offset));
    wr32(block + CB_TRANSFERRED, 0);
    wr32(block + CB_CALLBACK, callback);
    std::lock_guard g(s_qLock);
    if (!s_workerStarted) {
        s_workerStarted = true;
        std::thread(worker).detach();
    }
    s_queue.push_back({block, addr, length, callback, offset});
    s_qCv.notify_one();
}
}  // namespace

void dvdOpenDisc(const std::string& path) {
    s_disc = std::fopen(path.c_str(), "rb");
    if (!s_disc) fatal("can't open disc image %s", path.c_str());
    // The apploader loads the FST to the top of MEM1 (room for the header's maximum FST size),
    // records it in the boot block and puts arena hi just below it. The game relies on this: it
    // hardcodes buffers (movie work area, module load address) up to a few hundred KB below.
    u8 h[12];
    readRaw(0x424, h, 12);
    auto be = [&](int o) { return u32(h[o]) << 24 | h[o + 1] << 16 | h[o + 2] << 8 | h[o + 3]; };
    const u32 fstOff = be(0), fstSize = be(4), fstMax = std::max(be(8), fstSize);
    const u32 fst = (0x80000000 + kMem1Size - fstMax) & ~31u;
    readRaw(fstOff, hostp(fst), fstSize);
    s_fstCopy.assign(static_cast<const u8*>(hostp(fst)), static_cast<const u8*>(hostp(fst)) + fstSize);
    wr32(0x80000038, fst);
    wr32(0x8000003C, fstSize);
    wr32(0x80000034, fst);  // arena hi
    log("[dvd] disc %s, FST %u bytes at %08X\n", path.c_str(), fstSize, fst);
}

// Path lookups and directory changes stay native; with SA2_DVD_LOG they are logged.
HLE(DVDConvertPathToEntrynum) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    const std::string path = gstr(c.r[3]);
    callOriginal(c, "DVDConvertPathToEntrynum");
    if (s_log) log("[dvd] path '%s' -> entry %d\n", path.c_str(), s32(c.r[3]));
    if (s_log && s32(c.r[3]) < 0) {
        // Is the FST in guest memory still the one read from the disc?
        const u32 fst = rd32(0x80000038);
        const u8* g = static_cast<const u8*>(hostp(fst));
        u32 diffs = 0, first = 0;
        for (u32 i = 0; i < s_fstCopy.size(); i++)
            if (g[i] != s_fstCopy[i] && diffs++ == 0) first = i;
        log("[dvd] FST at %08X: %u bytes differ from the disc's", fst, diffs);
        if (diffs) log(" (first at +%X = %08X)", first, fst + first);
        log("; current dir %d\n", s32(rd32(c.r[13] - 0x7568)));
    }
}
HLE(DVDChangeDir) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    const std::string path = gstr(c.r[3]);
    callOriginal(c, "DVDChangeDir");
    if (s_log) log("[dvd] chdir '%s' -> %d\n", path.c_str(), s32(c.r[3]));
}

HLE(__DVDInitWA) {}
HLE(DVDReset) {}
HLE(DVDCheckDisk) { c.r[3] = 1; }
HLE(DVDGetDriveStatus) { c.r[3] = 0; }  // idle
HLE(DVDReadAbsAsyncPrio) {  // (block, addr, length, offset, callback, prio)
    submit(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], 1);
    c.r[3] = 1;
}
HLE(DVDReadAbsAsyncForBS) {  // (block, addr, length, offset, callback)
    submit(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], 4);
    c.r[3] = 1;
}
HLE(DVDReadDiskID) {  // (block, diskID, callback)
    submit(c.r[3], c.r[4], 0x20, 0, c.r[5], 5);
    c.r[3] = 1;
}
HLE(DVDCancelAsync) {  // (block, callback)
    const u32 block = c.r[3], cb = c.r[4];
    wr32(block + CB_STATE, ST_CANCELED);
    if (cb) sched::postIrq([block, cb](Ctx& ic) {
        ic.r[3] = 0;
        ic.r[4] = block;
        ic.call(cb);
    });
    c.r[3] = 1;
}
HLE(DVDCancel) {
    wr32(c.r[3] + CB_STATE, ST_CANCELED);
    c.r[3] = 0;
}

}  // namespace rt
