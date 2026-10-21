// Window, render loop and controllers (Aurora). The game runs on its own host threads; this thread
// owns the window: each frame it takes the game's finished GX command stream (src/runtime/gx.cpp),
// feeds it to Aurora's command processor and presents.
// Debug/automation: --headless renders with the window hidden; SA2_DUMP_DIR=<dir> writes every
// SA2_DUMP_EVERY-th frame as raw RGBA (tools/rgba2png.py converts it); --frames N exits after N.
#include "runtime/gx.h"
#include "runtime/runtime.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include "gx/fifo.hpp"
#include "webgpu/gpu.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace rt {

namespace {
std::mutex s_padLock;
u8 s_padGuest[4][12];  // PADStatus as the game sees it (big-endian)

void auroraLog(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    static const char* names[] = {"debug", "info", "warn", "error", "fatal"};
    static const int minLevel = std::getenv("SA2_AURORA_LOG") ? std::atoi(std::getenv("SA2_AURORA_LOG")) : LOG_WARNING;
    if (int(level) < minLevel) return;
    log("[aurora %s] %s: %.*s\n", names[level < 5 ? level : 4], module, int(len), message);
    if (level == LOG_FATAL) std::abort();
}

void pollPads() {
    PADStatus st[4]{};
    PADRead(st);
    std::lock_guard g(s_padLock);
    for (int i = 0; i < 4; i++) {
        u8* p = s_padGuest[i];
        p[0] = u8(st[i].button >> 8);
        p[1] = u8(st[i].button);
        p[2] = u8(st[i].stickX);
        p[3] = u8(st[i].stickY);
        p[4] = u8(st[i].substickX);
        p[5] = u8(st[i].substickY);
        p[6] = st[i].triggerLeft;
        p[7] = st[i].triggerRight;
        p[8] = st[i].analogA;
        p[9] = st[i].analogB;
        p[10] = u8(st[i].err);
        p[11] = 0;
    }
}

// Frame dumps of the real framebuffer: after the frame's commands are drained, the EFB is snapshotted
// (resolve_pass) and an encoder task copies the snapshot into a buffer on Aurora's render worker;
// the file is written there after submit. Nothing touches the GPU queue from this thread.
struct EfbDump {
    wgpu::Texture tex;
    wgpu::Buffer buf;
    u32 w = 0, h = 0, bpr = 0;
    bool bgra = false;
    std::string path;
};
EfbDump s_efbDumps[4];
u32 s_efbDumpNext = 0;
aurora::gfx::EncoderTaskId s_efbTask = aurora::gfx::InvalidEncoderTask;

void efbCopyTask(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t,
                 void*) {
    EfbDump& d = s_efbDumps[*static_cast<const u32*>(payload) & 3];
    const wgpu::BufferDescriptor bd{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead, .size = u64(d.bpr) * d.h};
    d.buf = ctx.device.CreateBuffer(&bd);
    const wgpu::TexelCopyTextureInfo from{.texture = d.tex};
    const wgpu::TexelCopyBufferInfo to{.layout = {.offset = 0, .bytesPerRow = d.bpr, .rowsPerImage = d.h}, .buffer = d.buf};
    const wgpu::Extent3D ext{d.w, d.h, 1};
    cmd.CopyTextureToBuffer(&from, &to, &ext);
}

void efbWriteTask(const aurora::gfx::EncoderTaskCompletionContext&, const void* payload, size_t, void*) {
    EfbDump& d = s_efbDumps[*static_cast<const u32*>(payload) & 3];
    bool ok = false;
    wgpu::Future fut = d.buf.MapAsync(wgpu::MapMode::Read, 0, u64(d.bpr) * d.h, wgpu::CallbackMode::WaitAnyOnly,
                                      [&](wgpu::MapAsyncStatus s, wgpu::StringView) { ok = s == wgpu::MapAsyncStatus::Success; });
    aurora::webgpu::g_instance.WaitAny(fut, UINT64_MAX);
    if (ok) {
        const u8* px = static_cast<const u8*>(d.buf.GetConstMappedRange(0, u64(d.bpr) * d.h));
        if (std::FILE* f = std::fopen(d.path.c_str(), "wb")) {
            const u32 fmt = d.bgra ? 1 : 0;
            std::fwrite("RGBA", 1, 4, f);
            std::fwrite(&d.w, 4, 1, f);
            std::fwrite(&d.h, 4, 1, f);
            std::fwrite(&fmt, 4, 1, f);
            for (u32 y = 0; y < d.h; y++) std::fwrite(px + size_t(y) * d.bpr, 1, size_t(d.w) * 4, f);
            std::fclose(f);
        }
        d.buf.Unmap();
    }
    d.buf = {};
    d.tex = {};
}

void dumpEfb(const std::string& path) {
    if (s_efbTask == aurora::gfx::InvalidEncoderTask)
        s_efbTask = aurora::gfx::register_encoder_task_type({.label = "EFB dump", .callback = efbCopyTask, .afterSubmit = efbWriteTask});
    aurora::gx::fifo::drain();  // the frame's commands are recorded; the EFB pass is still open
    aurora::gfx::ResolvedTargets rt{};
    if (!aurora::gfx::resolve_pass({.color = true}, rt) || !rt.colorTexture) return;
    const u32 idx = s_efbDumpNext++ & 3;
    EfbDump& d = s_efbDumps[idx];
    d.tex = rt.colorTexture;
    d.w = rt.width;
    d.h = rt.height;
    d.bpr = (d.w * 4 + 255) & ~255u;
    d.bgra = rt.colorFormat == wgpu::TextureFormat::BGRA8Unorm || rt.colorFormat == wgpu::TextureFormat::BGRA8UnormSrgb;
    d.path = path;
    aurora::gfx::push_encoder_task(s_efbTask, &idx, sizeof idx);
}

// (Kept for EFB-copy-to-RAM work: readback of a GX texture copy, as the hardware would write it.)
constexpr u32 kEfbW = 640, kEfbH = 480;
AuroraReadbackStream s_rb = 0;
u8 s_rbKey;  // its address identifies our copy
std::vector<u8> s_rbData(size_t(kEfbW) * kEfbH * 4);
u64 s_rbPending = 0;
std::string s_rbDir;

void requestFrameDump(u64 frameNo) {
    std::vector<u8> cmd;
    auto p8 = [&](u32 v) { cmd.push_back(u8(v)); };
    auto p16 = [&](u32 v) { p8(v >> 8), p8(v); };
    auto p32 = [&](u32 v) { p16(v >> 16), p16(v); };
    auto p64 = [&](u64 v) { p32(u32(v >> 32)), p32(u32(v)); };
    auto pf = [&](float f) { p32(__builtin_bit_cast(u32, f)); };
    if (std::getenv("SA2_DUMP_QUAD")) {
        // Renderer self-test: a green quad in the middle of the screen with the simplest state.
        auto bp = [&](u32 v) { p8(GX_LOAD_BP_REG), p32(v); };
        auto cp = [&](u32 r, u32 v) { p8(GX_LOAD_CP_REG), p8(r), p32(v); };
        auto xf = [&](u32 addr, std::initializer_list<u32> vals) {
            p8(GX_LOAD_XF_REG), p16(u32(vals.size() - 1)), p16(addr);
            for (u32 v : vals) p32(v);
        };
        auto fb = [](float f) { return __builtin_bit_cast(u32, f); };
        bp(0x00000010);                                 // GENMODE: 1 color channel, 1 TEV stage, 0 texgens
        bp(0x20156156);                                 // scissor: full 640x480 (+342 bias)
        bp(0x213D5335);
        bp(0x5902ACAB);                                 // scissor offset 0
        xf(0x101A, {fb(320), fb(-240), fb(16777215), fb(662), fb(582), fb(16777215)});  // viewport 640x480
        bp(0x40000000);                                 // ZMODE: off
        bp(0x41000018);                                 // BLENDMODE: no blend, color + alpha update
        bp(0x28049000);                                 // TEV order: stage 0 = no texture, color channel 0
        bp(0xC008FFFA);                                 // TEV color 0: out = rasterized color
        bp(0xC108FFD0);                                 // TEV alpha 0: out = rasterized alpha
        bp(0xF33FFF00);                                 // alpha compare: always
        xf(0x1009, {1});                                // 1 color channel
        xf(0x100E, {1});                                // color0 from vertex color, no lighting
        xf(0x1010, {1});
        xf(0x103F, {0});                                // no texgens
        xf(0x0000, {fb(1), 0, 0, 0, 0, fb(1), 0, 0, 0, 0, fb(1), 0});  // position matrix 0 = identity
        xf(0x1020, {fb(1.0f / 320), fb(-1), fb(-1.0f / 240), fb(1), fb(-1), fb(-1), 1});  // ortho 640x480, z -> -z-1
        if (const char* st = std::getenv("SA2_QUAD_STATE")) {  // add one piece of the game's state at a time
            if (std::strchr(st, 'z')) bp(0x40000007);           // z compare LEQUAL, no update (game's ZMODE)
            if (std::strchr(st, 'b')) bp(0x410004AD);           // alpha blend (game's BLENDMODE)
            if (std::strchr(st, 'p')) bp(0x43000040);           // PE_CTRL RGB8_Z24, z before texture
            if (std::strchr(st, 'c')) cp(0x30, 0x3CF3C780);     // game's matrix indices
        }
        cp(0x30, 0x3CF3CF00);                           // matrix indices: position 0, textures identity
        xf(0x1018, {0x3CF3CF00});
        cp(0x50, 0x00002200);                           // VCD: position + color0 direct
        cp(0x60, 0);
        cp(0x70, 0x00014009);                           // VAT0: position XYZ f32, color0 RGBA8
        cp(0x80, 0x80000000);
        cp(0x90, 0);
        p8(0x80), p16(4);                               // QUADS fmt 0, 4 vertices
        for (auto [x, y] : {std::pair{160.f, 120.f}, {480.f, 120.f}, {480.f, 360.f}, {160.f, 360.f}}) {
            pf(x), pf(y), pf(-0.5f);
            p32(0x00FF00FF);                            // green, opaque
        }
        static bool s_saved = false;
        if (!s_saved) {
            s_saved = true;
            if (std::FILE* f = std::fopen("build/myquad.bin", "wb")) {
                std::fwrite(cmd.data(), 1, cmd.size(), f);
                std::fclose(f);
            }
        }
    }
    p8(GX_AURORA), p16(GX_AURORA_LOAD_COPY_SRC), p32(0), p32(0), p32(kEfbW), p32(kEfbH);
    p8(GX_AURORA), p16(GX_AURORA_LOAD_COPY_DST), p32(kEfbW), p32(kEfbH), p32(GX_TF_RGBA8), p8(0);
    p8(GX_AURORA), p16(GX_AURORA_LOAD_COPY_DEST), p64(u64(reinterpret_cast<uintptr_t>(&s_rbKey)));
    p8(GX_LOAD_BP_REG), p32(0x52000000);  // texture copy, no clear
    aurora::gx::fifo::write_data(cmd.data(), u32(cmd.size()));
    const AuroraReadbackCopySrc src{frameNo, &s_rbKey};
    if (AuroraReadbackCaptureCopies(s_rb, &src, 1)) s_rbPending = frameNo;
}

void collectFrameDump() {
    if (!s_rbPending) return;
    AuroraReadbackDest d{s_rbPending, s_rbData.data(), u32(s_rbData.size()), GX_FALSE};
    if (!AuroraReadbackGet(s_rb, &d, 1) || !d.found) return;
    char path[512];
    std::snprintf(path, sizeof path, "%s/frame_%05llu.rgba", s_rbDir.c_str(), (unsigned long long)s_rbPending);
    s_rbPending = 0;
    // GX RGBA8: 4x4 tiles of 64 bytes, the first 32 holding A/R pairs and the next 32 G/B pairs.
    std::vector<u8> lin(s_rbData.size());
    for (u32 ty = 0; ty < kEfbH / 4; ty++)
        for (u32 tx = 0; tx < kEfbW / 4; tx++) {
            const u8* t = &s_rbData[(size_t(ty) * (kEfbW / 4) + tx) * 64];
            for (u32 i = 0; i < 16; i++) {
                u8* o = &lin[((size_t(ty) * 4 + i / 4) * kEfbW + tx * 4 + i % 4) * 4];
                o[0] = t[i * 2 + 1], o[1] = t[32 + i * 2], o[2] = t[32 + i * 2 + 1], o[3] = t[i * 2];
            }
        }
    if (std::FILE* f = std::fopen(path, "wb")) {
        const u32 w = kEfbW, h = kEfbH, fmt = 0;
        std::fwrite("RGBA", 1, 4, f);
        std::fwrite(&w, 4, 1, f);
        std::fwrite(&h, 4, 1, f);
        std::fwrite(&fmt, 4, 1, f);
        std::fwrite(lin.data(), 1, lin.size(), f);
        std::fclose(f);
    }
}
}  // namespace

void padSnapshot(u8 out[4][12]) {
    std::lock_guard g(s_padLock);
    std::memcpy(out, s_padGuest, sizeof s_padGuest);
}

int runFrontend(int argc, char** argv, bool headless, u64 maxFrames, void (*startGuest)()) {
    if (headless) _putenv("AURORA_HEADLESS=1");
    const AuroraConfig cfg{
        .appName = "SA2B Recompiled",
        .logCallback = &auroraLog,
        .vsync = true,
        .windowWidth = 1280,
        .windowHeight = 960,
    };
    const AuroraInfo info = aurora_initialize(argc, argv, &cfg);
    log("[frontend] Aurora up: backend %d, window %ux%u, framebuffer %ux%u\n", int(info.backend), info.windowSize.width,
        info.windowSize.height, info.windowSize.fb_width, info.windowSize.fb_height);
    // Aurora's own GX/VI state starts from its GXInit defaults and a 640x480 NTSC logical
    // framebuffer; everything after that comes from the game's command stream.
    static u8 s_auroraFifo[0x10000];
    if (aurora_begin_frame()) {
        GXInit(s_auroraFifo, sizeof s_auroraFifo);
        VIInit();
        VIConfigure(&GXNtsc480IntDf);
        aurora_end_frame();
    }
    PADInit();
    pollPads();
    log("[frontend] controllers ready; starting the game\n");
    std::thread(startGuest).detach();

    const char* dumpDir = std::getenv("SA2_DUMP_DIR");
    const u64 dumpEvery = std::getenv("SA2_DUMP_EVERY") ? std::strtoull(std::getenv("SA2_DUMP_EVERY"), nullptr, 10) : 60;
    std::vector<u8> frame;
    u64 presented = 0;
    for (;;) {
        bool exiting = false;
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev)
            if (ev->type == AURORA_EXIT) exiting = true;
        if (exiting) break;
        pollPads();
        if (!gxTakeFrame(frame, 16)) continue;  // keep the window responsive while the game loads
        static u64 s_taken = 0, s_refused = 0;
        if (++s_taken <= 3 || s_taken % 600 == 0)
            log("[frontend] game frame %llu: %zu bytes of GX commands\n", (unsigned long long)s_taken, frame.size());
        if (!aurora_begin_frame()) {
            if (++s_refused <= 3 || s_refused % 600 == 0)
                log("[frontend] aurora_begin_frame refused (%llu)\n", (unsigned long long)s_refused);
            continue;
        }
        static const char* s_isolate = std::getenv("SA2_ISOLATE");  // "api": Aurora API only, "quad": test quad only
        if (s_isolate && !std::strcmp(s_isolate, "api")) {
            GXSetCopyClear(GXColor{0, 200, 0, 255}, 0xFFFFFF);
            GXColor c{0, 200, 0, 255};
            (void)c;
            frame.clear();
        } else if (s_isolate && !std::strcmp(s_isolate, "quad")) {
            frame.clear();
        } else if (s_isolate && !std::strcmp(s_isolate, "apiraw")) {
            // Replays the captured bytes of Aurora's API quad through the raw command path.
            static std::vector<u8> s_raw = [] {
                std::vector<u8> v;
                const char* path = std::getenv("SA2_RAW_FILE") ? std::getenv("SA2_RAW_FILE") : "build/apiquad.bin";
                if (std::FILE* f = std::fopen(path, "rb")) {
                    u8 b[4096];
                    size_t n;
                    while ((n = std::fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
                    std::fclose(f);
                }
                return v;
            }();
            frame = s_raw;
        } else if (s_isolate && !std::strcmp(s_isolate, "apiquad")) {
            // Aurora's own API draws a green quad; its FIFO bytes are saved for comparison.
            frame.clear();
            const u32 before = aurora::gx::fifo::get_buffer_size();
            Mtx44 ortho;
            C_MTXOrtho(ortho, 0, 480, 0, 640, 0, 1);
            GXSetProjection(ortho, GX_ORTHOGRAPHIC);
            Mtx ident;
            PSMTXIdentity(ident);
            GXLoadPosMtxImm(ident, GX_PNMTX0);
            GXSetCurrentMtx(GX_PNMTX0);
            GXSetViewport(0, 0, 640, 480, 0, 1);
            GXSetScissor(0, 0, 640, 480);
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
            GXSetNumChans(1);
            GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
            GXSetNumTexGens(0);
            GXSetNumTevStages(1);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
            GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
            GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
            GXBegin(GX_QUADS, GX_VTXFMT0, 4);
            for (auto [x, y] : {std::pair{160.f, 120.f}, {480.f, 120.f}, {480.f, 360.f}, {160.f, 360.f}}) {
                GXPosition3f32(x, y, -0.5f);
                GXColor4u8(0, 255, 0, 255);
            }
            GXEnd();
            static bool s_saved = false;
            if (!s_saved) {
                s_saved = true;
                const u32 after = aurora::gx::fifo::get_buffer_size();
                if (std::FILE* f = std::fopen("build/apiquad.bin", "wb")) {
                    std::fwrite(aurora::gx::fifo::get_buffer_data() + before, 1, after - before, f);
                    std::fclose(f);
                }
            }
        }
        if (std::getenv("SA2_DUMP_TEST")) {
            // Capture self-test: clear the EFB to red through a cleared copy before the game's commands.
            static const u8 red[] = {GX_LOAD_BP_REG, 0x4F, 0x00, 0xFF, 0xFF,  // clear A=FF R=FF
                                     GX_LOAD_BP_REG, 0x50, 0x00, 0x00, 0x00,  // clear G=0 B=0
                                     GX_LOAD_BP_REG, 0x52, 0x00, 0x08, 0x00}; // texture copy + clear
            aurora::gx::fifo::write_data(red, sizeof red);
        }
        aurora::gx::fifo::write_data(frame.data(), u32(frame.size()));
        presented++;
        if (const char* gxDump = std::getenv("SA2_GXDUMP_FRAME"); gxDump && presented == std::strtoull(gxDump, nullptr, 10)) {
            if (std::FILE* f = std::fopen("build/gxframe.bin", "wb")) {  // decode with tools/gxdump.py
                std::fwrite(frame.data(), 1, frame.size(), f);
                std::fclose(f);
            }
        }
        if (dumpDir && dumpEvery && presented % dumpEvery == 0) {
            if (std::getenv("SA2_DUMP_QUAD") || (s_isolate && !std::strcmp(s_isolate, "quad"))) requestFrameDump(presented);
            char path[512];
            std::snprintf(path, sizeof path, "%s/frame_%05llu.rgba", dumpDir, (unsigned long long)presented);
            dumpEfb(path);
        }
        aurora_end_frame();
        if (presented % 300 == 0 || presented == 30) {
            const AuroraStats* st = aurora_get_stats();
            log("[frontend] %llu frames presented (%zu bytes last); Aurora: %u draws (%u merged), pipelines %u queued %u created, "
                "verts %u B, textures %u B\n",
                (unsigned long long)presented, frame.size(), st->drawCallCount, st->mergedDrawCallCount, st->queuedPipelines,
                st->createdPipelines, st->lastVertSize, st->lastTextureUploadSize);
        }
        if (maxFrames && presented >= maxFrames) break;
    }
    log("[frontend] exiting after %llu frames\n", (unsigned long long)presented);
    std::fflush(stdout);
    std::_Exit(0);
}

}  // namespace rt
