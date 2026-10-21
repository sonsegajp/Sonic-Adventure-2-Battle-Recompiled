// DVD: the disc's file system table sits in guest memory the way the IPL leaves it, so the SDK's
// own file-system code (DVDOpen, DVDConvertPathToEntrynum, DVDChangeDir...) runs natively. Only the
// reads are replaced: a worker thread serves them from the user's disc image and completes each one
// through the interrupt path (callbacks run in interrupt context, as on the console).
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace rt {

namespace {
std::FILE* s_disc = nullptr;
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

void submit(u32 block, u32 addr, u32 length, u64 offset, u32 callback, u32 command) {
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
    // The IPL loads the FST to the top of memory and records it in the boot block.
    u8 h[12];
    readRaw(0x424, h, 12);
    const u32 fstOff = u32(h[0]) << 24 | h[1] << 16 | h[2] << 8 | h[3];
    const u32 fstSize = u32(h[4]) << 24 | h[5] << 16 | h[6] << 8 | h[7];
    const u32 fst = hleAlloc(fstSize, 32);
    readRaw(fstOff, hostp(fst), fstSize);
    wr32(0x80000038, fst);
    wr32(0x8000003C, fstSize);
    log("[dvd] disc %s, FST %u bytes at %08X\n", path.c_str(), fstSize, fst);
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
