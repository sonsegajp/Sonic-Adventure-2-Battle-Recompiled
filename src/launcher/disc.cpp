#include "launcher/disc.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace launcher {

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
bool sameName(const char* a, const std::string& b) {
    size_t i = 0;
    for (; a[i] && i < b.size(); i++)
        if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
    return !a[i] && i == b.size();
}
}  // namespace

bool Disc::open(const std::filesystem::path& path, std::string& error) {
    m_open = false;
    m_file.close();
    m_file.clear();
    m_file.open(path, std::ios::binary);
    if (!m_file) return error = "can't open the file", false;
    m_header.assign(0x440, 0);
    if (!readAt(0, m_header.data(), m_header.size())) return error = "too small to be a disc image", false;
    if (be32(&m_header[0x1C]) != 0xC2339F3D) return error = "not a GameCube disc image", false;
    const uint32_t fstOff = be32(&m_header[0x424]), fstSize = be32(&m_header[0x428]);
    if (fstSize < 12 || fstSize > 0x200000) return error = "damaged disc image (file table)", false;
    m_fst.assign(fstSize, 0);
    if (!readAt(fstOff, m_fst.data(), fstSize)) return error = "damaged disc image (file table)", false;
    return m_open = true;
}

std::string Disc::gameId() const { return m_header.empty() ? "" : std::string(reinterpret_cast<const char*>(m_header.data()), 6); }
std::string Disc::title() const {
    if (m_header.empty()) return {};
    const char* t = reinterpret_cast<const char*>(&m_header[0x20]);
    return std::string(t, strnlen(t, 0x3E0));
}

bool Disc::readAt(uint64_t offset, void* dst, size_t size) {
    m_file.clear();
    m_file.seekg(std::streamoff(offset));
    m_file.read(static_cast<char*>(dst), std::streamsize(size));
    return size_t(m_file.gcount()) == size;
}

bool Disc::read(const std::string& name, Bytes& out) {
    if (!m_open) return false;
    const uint32_t count = be32(&m_fst[8]);
    if (count == 0 || size_t(count) * 12 > m_fst.size()) return false;
    const char* strings = reinterpret_cast<const char*>(&m_fst[size_t(count) * 12]);
    const size_t stringsSize = m_fst.size() - size_t(count) * 12;
    // walk the path one component at a time: a directory's entries run from it to its "next"
    std::vector<std::string> parts;
    for (size_t a = 0, b; a <= name.size(); a = b + 1) {
        b = name.find('/', a);
        if (b == std::string::npos) b = name.size();
        if (b > a) parts.push_back(name.substr(a, b - a));
    }
    uint32_t lo = 1, hi = count;
    for (size_t k = 0; k < parts.size(); k++) {
        bool found = false;
        for (uint32_t i = lo; i < hi;) {
            const uint8_t* e = &m_fst[size_t(i) * 12];
            const bool dir = e[0] != 0;
            const uint32_t nameOff = be32(e) & 0xFFFFFF;
            const char* n = nameOff < stringsSize ? strings + nameOff : "";
            if (sameName(n, parts[k])) {
                if (k + 1 == parts.size()) {
                    if (dir) return false;
                    const uint32_t off = be32(e + 4), size = be32(e + 8);
                    out.assign(size, 0);
                    return readAt(off, out.data(), size);
                }
                if (!dir) return false;
                lo = i + 1, hi = be32(e + 8), found = true;
                break;
            }
            i = dir ? std::max(be32(e + 8), i + 1) : i + 1;
        }
        if (!found) return false;
    }
    return false;
}

// Sega PRS: LZ77 with a bit stream of control flags interleaved with the data.
Bytes prsDecompress(const Bytes& src) {
    Bytes out;
    size_t i = 0;
    int bits = 0;
    uint8_t ctrl = 0;
    auto bit = [&]() -> int {
        if (bits == 0) {
            if (i >= src.size()) return 0;
            ctrl = src[i++], bits = 8;
        }
        const int b = ctrl & 1;
        ctrl >>= 1, bits--;
        return b;
    };
    while (i < src.size()) {
        if (bit()) {  // literal
            out.push_back(src[i++]);
            continue;
        }
        int off, size;
        if (bit()) {  // long copy: 13-bit offset, 3-bit size (0: size in the next byte)
            if (i + 2 > src.size()) break;
            const int v = src[i] | src[i + 1] << 8;
            i += 2;
            if (v == 0) break;
            off = (v >> 3) - 0x2000;
            size = v & 7;
            if (size) size += 2;
            else {
                if (i >= src.size()) break;
                size = src[i++] + 1;
            }
        } else {  // short copy: 2-bit size, 8-bit offset
            size = bit() << 1;
            size = (size | bit()) + 2;
            if (i >= src.size()) break;
            off = src[i++] - 0x100;
        }
        const ptrdiff_t start = ptrdiff_t(out.size()) + off;
        if (start < 0) break;
        for (int k = 0; k < size; k++) out.push_back(out[size_t(start + k)]);
    }
    return out;
}

namespace {
void rgb565(uint16_t v, uint8_t* o) {
    const int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    o[0] = uint8_t(r << 3 | r >> 2), o[1] = uint8_t(g << 2 | g >> 4), o[2] = uint8_t(b << 3 | b >> 2), o[3] = 255;
}

// The GameCube's tiled texture layouts (CMPR 8x8 tiles of 4x4 blocks, the others 4x4 or 8x4 tiles).
bool decode(int fmt, const uint8_t* d, size_t size, int w, int h, Image& img) {
    img.w = w, img.h = h;
    img.rgba.assign(size_t(w) * h * 4, 0);
    auto put = [&](int x, int y, const uint8_t* px) {
        if (x < w && y < h) std::memcpy(&img.rgba[(size_t(y) * w + x) * 4], px, 4);
    };
    size_t pos = 0;
    auto need = [&](size_t n) { return pos + n <= size; };
    if (fmt == 14) {  // CMPR
        for (int ty = 0; ty < h; ty += 8)
            for (int tx = 0; tx < w; tx += 8)
                for (int sub = 0; sub < 4; sub++) {
                    if (!need(8)) return false;
                    const uint16_t c0 = be16(d + pos), c1 = be16(d + pos + 2);
                    const uint32_t idx = be32(d + pos + 4);
                    pos += 8;
                    uint8_t pal[4][4];
                    rgb565(c0, pal[0]), rgb565(c1, pal[1]);
                    for (int c = 0; c < 3; c++) {
                        if (c0 > c1) {
                            pal[2][c] = uint8_t((2 * pal[0][c] + pal[1][c]) / 3);
                            pal[3][c] = uint8_t((pal[0][c] + 2 * pal[1][c]) / 3);
                        } else {
                            pal[2][c] = uint8_t((pal[0][c] + pal[1][c]) / 2);
                            pal[3][c] = 0;
                        }
                    }
                    pal[2][3] = 255, pal[3][3] = c0 > c1 ? 255 : 0;
                    const int bx = tx + (sub & 1) * 4, by = ty + (sub >> 1) * 4;
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) put(bx + x, by + y, pal[idx >> (30 - 2 * (y * 4 + x)) & 3]);
                }
        return true;
    }
    if (fmt == 5) {  // RGB5A3
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4)
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        if (!need(2)) return false;
                        const uint16_t v = be16(d + pos);
                        pos += 2;
                        uint8_t px[4];
                        if (v & 0x8000) {
                            const int r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                            px[0] = uint8_t(r << 3 | r >> 2), px[1] = uint8_t(g << 3 | g >> 2), px[2] = uint8_t(b << 3 | b >> 2), px[3] = 255;
                        } else {
                            const int a = v >> 12 & 7;
                            px[0] = uint8_t((v >> 8 & 15) * 17), px[1] = uint8_t((v >> 4 & 15) * 17), px[2] = uint8_t((v & 15) * 17);
                            px[3] = uint8_t(a << 5 | a << 2 | a >> 1);
                        }
                        put(tx + x, ty + y, px);
                    }
        return true;
    }
    if (fmt == 6) {  // RGBA8: AR and GB halves per 4x4 tile
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += 4) {
                if (!need(64)) return false;
                for (int i = 0; i < 16; i++) {
                    const uint8_t px[4] = {d[pos + 2 * i + 1], d[pos + 32 + 2 * i], d[pos + 32 + 2 * i + 1], d[pos + 2 * i]};
                    put(tx + i % 4, ty + i / 4, px);
                }
                pos += 64;
            }
        return true;
    }
    if (fmt == 1 || fmt == 3 || fmt == 4) {  // I8 (8x4 tiles), IA8, RGB565
        const int bw = fmt == 1 ? 8 : 4;
        for (int ty = 0; ty < h; ty += 4)
            for (int tx = 0; tx < w; tx += bw)
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < bw; x++) {
                        uint8_t px[4];
                        if (fmt == 1) {
                            if (!need(1)) return false;
                            px[0] = px[1] = px[2] = d[pos++], px[3] = 255;
                        } else if (fmt == 3) {
                            if (!need(2)) return false;
                            px[0] = px[1] = px[2] = d[pos + 1], px[3] = d[pos];
                            pos += 2;
                        } else {
                            if (!need(2)) return false;
                            rgb565(be16(d + pos), px);
                            pos += 2;
                        }
                        put(tx + x, ty + y, px);
                    }
        return true;
    }
    return false;
}
}  // namespace

std::vector<Image> gvrTextures(const Bytes& blob) {
    std::vector<Image> out;
    for (size_t i = 0; i + 16 <= blob.size(); i++) {
        if (std::memcmp(&blob[i], "GVRT", 4) != 0) continue;
        const int fmt = blob[i + 11], w = be16(&blob[i + 12]), h = be16(&blob[i + 14]);
        Image img;
        if (w > 0 && h > 0 && w <= 1024 && h <= 1024 && !decode(fmt, &blob[i + 16], blob.size() - i - 16, w, h, img)) img = {};
        out.push_back(std::move(img));
        i += 3;
    }
    return out;
}

bool Font::load(Disc& disc) {
    return disc.read("efmsgfont_ascii24S.bin", bits) && bits.size() >= size_t(kCount) * 72;
}
bool Font::pixel(int glyph, int x, int y) const {
    const uint8_t* r = &bits[size_t(glyph) * 72 + size_t(y) * 3];
    return (uint32_t(r[0]) | uint32_t(r[1]) << 8 | uint32_t(r[2]) << 16) >> x & 1;
}

}  // namespace launcher
