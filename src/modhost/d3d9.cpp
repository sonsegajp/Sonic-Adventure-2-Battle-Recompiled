// Direct3D 9 for mod code. The PC game draws with Direct3D 9 and mods reach its device through the
// renderer (PC 0x1A557C0, the device at +0x38) to make shaders, textures and draw. Here the game
// draws with GX, so the device is the mod host's: every method of every interface (with its exact
// stdcall argument count) is accepted, the objects it creates are real (reference counts,
// descriptions, lockable memory), and what mods draw with it directly is not shown yet.
#include "modhost/sysapi.h"

#include "runtime/runtime.h"

#include <map>

namespace mh {

namespace {

struct Method {
    const char* name;
    u8 args;  // stack dwords, `this` included
};
#define IUNKNOWN {"QueryInterface", 3}, {"AddRef", 1}, {"Release", 1}
#define RESOURCE {"GetDevice", 2}, {"SetPrivateData", 5}, {"GetPrivateData", 4}, {"FreePrivateData", 2}, \
                 {"SetPriority", 2}, {"GetPriority", 1}, {"PreLoad", 1}, {"GetType", 1}
const Method kDevice[] = {
    IUNKNOWN, {"TestCooperativeLevel", 1}, {"GetAvailableTextureMem", 1}, {"EvictManagedResources", 1},
    {"GetDirect3D", 2}, {"GetDeviceCaps", 2}, {"GetDisplayMode", 3}, {"GetCreationParameters", 2},
    {"SetCursorProperties", 4}, {"SetCursorPosition", 4}, {"ShowCursor", 2}, {"CreateAdditionalSwapChain", 3},
    {"GetSwapChain", 3}, {"GetNumberOfSwapChains", 1}, {"Reset", 2}, {"Present", 5}, {"GetBackBuffer", 5},
    {"GetRasterStatus", 3}, {"SetDialogBoxMode", 2}, {"SetGammaRamp", 4}, {"GetGammaRamp", 3},
    {"CreateTexture", 9}, {"CreateVolumeTexture", 10}, {"CreateCubeTexture", 8}, {"CreateVertexBuffer", 7},
    {"CreateIndexBuffer", 7}, {"CreateRenderTarget", 9}, {"CreateDepthStencilSurface", 9}, {"UpdateSurface", 5},
    {"UpdateTexture", 3}, {"GetRenderTargetData", 3}, {"GetFrontBufferData", 3}, {"StretchRect", 6},
    {"ColorFill", 4}, {"CreateOffscreenPlainSurface", 7}, {"SetRenderTarget", 3}, {"GetRenderTarget", 3},
    {"SetDepthStencilSurface", 2}, {"GetDepthStencilSurface", 2}, {"BeginScene", 1}, {"EndScene", 1},
    {"Clear", 7}, {"SetTransform", 3}, {"GetTransform", 3}, {"MultiplyTransform", 3}, {"SetViewport", 2},
    {"GetViewport", 2}, {"SetMaterial", 2}, {"GetMaterial", 2}, {"SetLight", 3}, {"GetLight", 3},
    {"LightEnable", 3}, {"GetLightEnable", 3}, {"SetClipPlane", 3}, {"GetClipPlane", 3}, {"SetRenderState", 3},
    {"GetRenderState", 3}, {"CreateStateBlock", 3}, {"BeginStateBlock", 1}, {"EndStateBlock", 2},
    {"SetClipStatus", 2}, {"GetClipStatus", 2}, {"GetTexture", 3}, {"SetTexture", 3},
    {"GetTextureStageState", 4}, {"SetTextureStageState", 4}, {"GetSamplerState", 4}, {"SetSamplerState", 4},
    {"ValidateDevice", 2}, {"SetPaletteEntries", 3}, {"GetPaletteEntries", 3}, {"SetCurrentTexturePalette", 2},
    {"GetCurrentTexturePalette", 2}, {"SetScissorRect", 2}, {"GetScissorRect", 2},
    {"SetSoftwareVertexProcessing", 2}, {"GetSoftwareVertexProcessing", 1}, {"SetNPatchMode", 2},
    {"GetNPatchMode", 1}, {"DrawPrimitive", 4}, {"DrawIndexedPrimitive", 7}, {"DrawPrimitiveUP", 5},
    {"DrawIndexedPrimitiveUP", 9}, {"ProcessVertices", 7}, {"CreateVertexDeclaration", 3},
    {"SetVertexDeclaration", 2}, {"GetVertexDeclaration", 2}, {"SetFVF", 2}, {"GetFVF", 2},
    {"CreateVertexShader", 3}, {"SetVertexShader", 2}, {"GetVertexShader", 2}, {"SetVertexShaderConstantF", 4},
    {"GetVertexShaderConstantF", 4}, {"SetVertexShaderConstantI", 4}, {"GetVertexShaderConstantI", 4},
    {"SetVertexShaderConstantB", 4}, {"GetVertexShaderConstantB", 4}, {"SetStreamSource", 5},
    {"GetStreamSource", 5}, {"SetStreamSourceFreq", 3}, {"GetStreamSourceFreq", 3}, {"SetIndices", 2},
    {"GetIndices", 2}, {"CreatePixelShader", 3}, {"SetPixelShader", 2}, {"GetPixelShader", 2},
    {"SetPixelShaderConstantF", 4}, {"GetPixelShaderConstantF", 4}, {"SetPixelShaderConstantI", 4},
    {"GetPixelShaderConstantI", 4}, {"SetPixelShaderConstantB", 4}, {"GetPixelShaderConstantB", 4},
    {"DrawRectPatch", 4}, {"DrawTriPatch", 4}, {"DeletePatch", 2}, {"CreateQuery", 3},
};
const Method kD3D9[] = {
    IUNKNOWN, {"RegisterSoftwareDevice", 2}, {"GetAdapterCount", 1}, {"GetAdapterIdentifier", 4},
    {"GetAdapterModeCount", 3}, {"EnumAdapterModes", 5}, {"GetAdapterDisplayMode", 3}, {"CheckDeviceType", 6},
    {"CheckDeviceFormat", 7}, {"CheckDeviceMultiSampleType", 7}, {"CheckDepthStencilMatch", 6},
    {"CheckDeviceFormatConversion", 5}, {"GetDeviceCaps", 4}, {"GetAdapterMonitor", 2}, {"CreateDevice", 7},
};
const Method kTexture[] = {
    IUNKNOWN, RESOURCE, {"SetLOD", 2}, {"GetLOD", 1}, {"GetLevelCount", 1}, {"SetAutoGenFilterType", 2},
    {"GetAutoGenFilterType", 1}, {"GenerateMipSubLevels", 1}, {"GetLevelDesc", 3}, {"GetSurfaceLevel", 3},
    {"LockRect", 5}, {"UnlockRect", 2}, {"AddDirtyRect", 2},
};
const Method kCubeTexture[] = {
    IUNKNOWN, RESOURCE, {"SetLOD", 2}, {"GetLOD", 1}, {"GetLevelCount", 1}, {"SetAutoGenFilterType", 2},
    {"GetAutoGenFilterType", 1}, {"GenerateMipSubLevels", 1}, {"GetLevelDesc", 3}, {"GetCubeMapSurface", 4},
    {"LockRect", 6}, {"UnlockRect", 3}, {"AddDirtyRect", 3},
};
const Method kVolumeTexture[] = {
    IUNKNOWN, RESOURCE, {"SetLOD", 2}, {"GetLOD", 1}, {"GetLevelCount", 1}, {"SetAutoGenFilterType", 2},
    {"GetAutoGenFilterType", 1}, {"GenerateMipSubLevels", 1}, {"GetLevelDesc", 3}, {"GetVolumeLevel", 3},
    {"LockBox", 5}, {"UnlockBox", 2}, {"AddDirtyBox", 2},
};
const Method kSurface[] = {
    IUNKNOWN, RESOURCE, {"GetContainer", 3}, {"GetDesc", 2}, {"LockRect", 4}, {"UnlockRect", 1}, {"GetDC", 2},
    {"ReleaseDC", 2},
};
const Method kBuffer[] = {IUNKNOWN, RESOURCE, {"Lock", 5}, {"Unlock", 1}, {"GetDesc", 2}};
const Method kShader[] = {IUNKNOWN, {"GetDevice", 2}, {"GetFunction", 3}};
const Method kDecl[] = {IUNKNOWN, {"GetDevice", 2}, {"GetDeclaration", 3}};
const Method kStateBlock[] = {IUNKNOWN, {"GetDevice", 2}, {"Capture", 1}, {"Apply", 1}};
const Method kQuery[] = {IUNKNOWN, {"GetDevice", 2}, {"GetType", 1}, {"GetDataSize", 1}, {"Issue", 2}, {"GetData", 4}};
const Method kSwapChain[] = {
    IUNKNOWN, {"Present", 6}, {"GetFrontBufferData", 2}, {"GetBackBuffer", 4}, {"GetRasterStatus", 2},
    {"GetDisplayMode", 2}, {"GetDevice", 2}, {"GetPresentParameters", 2},
};

enum Kind : u32 { Device, D3D9, Texture, CubeTexture, VolumeTexture, Surface, Buffer, Shader, Decl, StateBlock, Query, SwapChain, kKinds };
struct Iface {
    const char* name;
    const Method* methods;
    size_t count;
};
const Iface kIfaces[kKinds] = {
    {"Device", kDevice, std::size(kDevice)}, {"D3D9", kD3D9, std::size(kD3D9)},
    {"Texture", kTexture, std::size(kTexture)}, {"CubeTexture", kCubeTexture, std::size(kCubeTexture)},
    {"VolumeTexture", kVolumeTexture, std::size(kVolumeTexture)}, {"Surface", kSurface, std::size(kSurface)},
    {"Buffer", kBuffer, std::size(kBuffer)}, {"Shader", kShader, std::size(kShader)}, {"Decl", kDecl, std::size(kDecl)},
    {"StateBlock", kStateBlock, std::size(kStateBlock)}, {"Query", kQuery, std::size(kQuery)},
    {"SwapChain", kSwapChain, std::size(kSwapChain)},
};

constexpr u32 S_OK = 0, E_NOINTERFACE = 0x80004002;
constexpr u32 kWidth = 640, kHeight = 480, kFormat = 21;  // D3DFMT_A8R8G8B8

struct Object {
    Kind kind;
    u32 refs = 1;
    u32 w = 0, h = 0, fmt = kFormat, levels = 1, usage = 0, pool = 0, size = 0;
    u32 mem = 0;  // lockable contents
};
std::map<u32, Object> s_objects;
u32 s_vtables[kKinds];
u32 s_device = 0;
std::map<u32, u32> s_state;  // render, texture stage and sampler states, by (kind << 24 | index)

void method(Cpu& c, u32 ctx);

u32 vtable(Kind k) {
    if (s_vtables[k]) return s_vtables[k];
    const Iface& f = kIfaces[k];
    const u32 vt = host().alloc(4 * u32(f.count));
    for (size_t i = 0; i < f.count; i++)
        M().w32(vt + 4 * u32(i), host().thunk(std::string("d3d9.") + f.name + "::" + f.methods[i].name, method, u32(k) << 16 | u32(i)));
    return s_vtables[k] = vt;
}
u32 make(Kind k, Object o = {}) {
    const u32 a = host().alloc(16);
    M().w32(a, vtable(k));
    o.kind = k;
    s_objects[a] = o;
    return a;
}
u32 contents(Object& o) {
    if (!o.mem) {
        o.size = std::max<u32>(o.size, std::max<u32>(o.w, 1) * std::max<u32>(o.h, 1) * 4);
        o.mem = host().alloc(o.size);
    }
    return o.mem;
}
void out(u32 p, u32 v) {
    if (p) M().w32(p, v);
}
void surfaceDesc(u32 p, const Object& o) {  // D3DSURFACE_DESC
    const u32 v[8] = {o.fmt, 1, o.usage, o.pool, 0, 0, std::max<u32>(o.w, 1), std::max<u32>(o.h, 1)};
    for (u32 i = 0; i < 8; i++) M().w32(p + 4 * i, v[i]);
}
void identity(u32 p) {
    for (u32 i = 0; i < 16; i++) M().w32(p + 4 * i, i % 5 == 0 ? 0x3F800000 : 0);
}
void caps(u32 p) {  // D3DCAPS9: a shader model 3 card
    for (u32 i = 0; i < 304; i += 4) M().w32(p + i, 0);
    M().w32(p + 0, 1);              // HAL
    M().w32(p + 88, 4096);          // MaxTextureWidth
    M().w32(p + 92, 4096);          // MaxTextureHeight
    M().w32(p + 108, 16);           // MaxAnisotropy
    M().w32(p + 148, 8);            // MaxTextureBlendStages
    M().w32(p + 152, 8);            // MaxSimultaneousTextures
    M().w32(p + 160, 8);            // MaxActiveLights
    M().w32(p + 180, 0xFFFFF);      // MaxPrimitiveCount
    M().w32(p + 184, 0xFFFFF);      // MaxVertexIndex
    M().w32(p + 188, 16);           // MaxStreams
    M().w32(p + 192, 255);          // MaxStreamStride
    M().w32(p + 196, 0xFFFE0300);   // VertexShaderVersion 3.0
    M().w32(p + 200, 256);          // MaxVertexShaderConst
    M().w32(p + 204, 0xFFFF0300);   // PixelShaderVersion 3.0
    M().w32(p + 240, 4);            // NumSimultaneousRTs
}

void method(Cpu& c, u32 ctx) {
    const Kind k = Kind(ctx >> 16);
    const u32 i = ctx & 0xFFFF;
    const Method& m = kIfaces[k].methods[i];
    const u32 self = c.arg(0);
    auto a = [&](int n) { return c.arg(n); };
    auto obj = [&]() -> Object& { return s_objects[self]; };
    u32 r = S_OK;
    const std::string name = m.name;
    if (i == 0) {  // QueryInterface
        out(a(2), 0);
        r = E_NOINTERFACE;
    } else if (i == 1) {
        r = ++obj().refs;
    } else if (i == 2) {
        r = obj().refs ? --obj().refs : 0;
    } else if (name == "GetDevice") {
        out(a(1), s_device);
    } else if (k == Device) {
        switch (i) {
        case 4: r = 512u << 20; break;                                          // GetAvailableTextureMem
        case 6: out(a(1), make(D3D9)); break;                                   // GetDirect3D
        case 7: caps(a(1)); break;                                              // GetDeviceCaps
        case 8: M().w32(a(2), kWidth), M().w32(a(2) + 4, kHeight), M().w32(a(2) + 8, 60), M().w32(a(2) + 12, 22); break;
        case 9: M().w32(a(1), 0), M().w32(a(1) + 4, 1), M().w32(a(1) + 8, 0x10010), M().w32(a(1) + 12, 0x40); break;
        case 13: case 14: out(a(2), make(SwapChain)); break;
        case 15: r = 1; break;
        case 18: out(a(4), make(Surface, {Surface, 1, kWidth, kHeight})); break;  // GetBackBuffer
        case 23: out(a(7), make(Texture, {Texture, 1, a(1), a(2), a(5), a(3) ? a(3) : 1, a(4), a(6)})); break;
        case 24: out(a(8), make(VolumeTexture, {VolumeTexture, 1, a(1), a(2), a(6)})); break;
        case 25: out(a(6), make(CubeTexture, {CubeTexture, 1, a(1), a(1), a(4)})); break;
        case 26: out(a(5), make(Buffer, {Buffer, 1, 0, 0, 0, 1, a(2), a(4), a(1)})); break;
        case 27: out(a(5), make(Buffer, {Buffer, 1, 0, 0, a(3), 1, a(2), a(4), a(1)})); break;
        case 28: case 29: out(a(7), make(Surface, {Surface, 1, a(1), a(2), a(3)})); break;
        case 36: out(a(5), make(Surface, {Surface, 1, a(1), a(2), a(3)})); break;
        case 38: out(a(2), make(Surface, {Surface, 1, kWidth, kHeight})); break;  // GetRenderTarget
        case 40: out(a(1), make(Surface, {Surface, 1, kWidth, kHeight, 75})); break;  // GetDepthStencilSurface
        case 45: identity(a(2)); break;                                         // GetTransform
        case 48: {                                                              // GetViewport
            const u32 p = a(1);
            M().w32(p, 0), M().w32(p + 4, 0), M().w32(p + 8, kWidth), M().w32(p + 12, kHeight);
            M().w32(p + 16, 0), M().w32(p + 20, 0x3F800000);
            break;
        }
        case 57: s_state[a(1)] = a(2); break;                                   // SetRenderState
        case 58: out(a(2), s_state[a(1)]); break;
        case 59: out(a(2), make(StateBlock)); break;
        case 61: out(a(1), make(StateBlock)); break;
        case 64: case 88: case 93: case 105: case 108: out(a(i == 64 ? 2 : 1), 0); break;
        case 66: out(a(3), s_state[1u << 24 | a(1) << 8 | a(2)]); break;
        case 67: s_state[1u << 24 | a(1) << 8 | a(2)] = a(3); break;
        case 68: out(a(3), s_state[2u << 24 | a(1) << 8 | a(2)]); break;
        case 69: s_state[2u << 24 | a(1) << 8 | a(2)] = a(3); break;
        case 70: out(a(1), 1); break;                                           // ValidateDevice
        case 78: r = 0; break;
        case 80: c.fpush(0.0); r = 0; break;                                    // GetNPatchMode (float)
        case 86: out(a(2), make(Decl)); break;
        case 90: out(a(1), 0); break;
        case 91: case 106: out(a(2), make(Shader)); break;
        case 101: out(a(2), 0); break;
        case 118: out(a(2), make(Query)); break;
        default: break;  // drawing and state: accepted
        }
    } else if (k == D3D9) {
        if (name == "GetAdapterCount") r = 1;
        else if (name == "GetDeviceCaps") caps(a(3));
        else if (name == "CreateDevice") out(a(6), s_device);
        else if (name == "GetAdapterDisplayMode") M().w32(a(2), kWidth), M().w32(a(2) + 4, kHeight), M().w32(a(2) + 8, 60), M().w32(a(2) + 12, 22);
    } else if (name == "GetLevelCount") {
        r = obj().levels;
    } else if (name == "GetDesc" && k == Buffer) {  // D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC
        const Object& o = obj();
        const u32 p = a(1);
        M().w32(p, o.fmt), M().w32(p + 4, 6), M().w32(p + 8, o.usage), M().w32(p + 12, o.pool), M().w32(p + 16, o.size);
        M().w32(p + 20, 0);
    } else if (name == "GetLevelDesc" || name == "GetDesc") {
        surfaceDesc(name == "GetLevelDesc" ? a(2) : a(1), obj());
    } else if (name == "GetSurfaceLevel" || name == "GetCubeMapSurface") {
        Object& o = obj();
        out(a(name == "GetSurfaceLevel" ? 2 : 3), make(Surface, {Surface, 1, o.w, o.h, o.fmt}));
    } else if (name == "LockRect") {  // D3DLOCKED_RECT {pitch, bits}
        Object& o = obj();
        const u32 lr = a(k == Surface ? 1 : k == CubeTexture ? 3 : 2);
        M().w32(lr, std::max<u32>(o.w, 1) * 4);
        M().w32(lr + 4, contents(o));
    } else if (name == "Lock") {  // buffers: (offset, size, ppData, flags)
        Object& o = obj();
        out(a(3), contents(o) + a(1));
    } else if (name == "GetFunction") {
        out(a(2), 0);
    } else if (name == "GetDataSize") {
        r = 4;
    } else if (name == "GetType") {
        r = k == Texture ? 3 : k == Surface ? 1 : k == Buffer ? 6 : 0;
    } else if (name == "GetContainer" || name == "GetDC") {
        r = E_NOINTERFACE;
    }
    rets(c, r, m.args);
}

}  // namespace

// The PC game's renderer as mods see it: the variable at PC 0x1A557C0 points to it, and its
// Direct3D device is at +0x38.
void d3d9Init() {
    if (s_device) return;
    s_device = make(Device);
    const u32 renderer = host().alloc(0x400);
    for (u32 i = 0; i < 0x400; i += 4) M().w32(renderer + i, 0);
    M().w32(renderer + 0x38, s_device);
    M().w32(host().virtualVariable(0x01A557C0, 4), renderer);
}

}  // namespace mh
