// The launcher's view of the user's disc image: its files (by FST path), PRS decompression, GVR
// textures (GVM archives) and the message font - everything it draws with comes from here.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace launcher {

using Bytes = std::vector<uint8_t>;

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w * h * 4
};

class Disc {
public:
    // A GameCube disc image (.iso/.gcm); false with a reason when it is not one.
    bool open(const std::filesystem::path& path, std::string& error);
    bool isOpen() const { return m_open; }
    std::string gameId() const;  // e.g. GSNE8P
    std::string title() const;   // the name in the disc header
    bool read(const std::string& name, Bytes& out);  // a file by its path on the disc

private:
    bool readAt(uint64_t offset, void* dst, size_t size);
    std::ifstream m_file;
    Bytes m_header, m_fst;
    bool m_open = false;
};

Bytes prsDecompress(const Bytes& src);
// Every GVR texture in a blob (a GVM archive or a lone GVR), in order; undecodable ones come back
// empty so indices still match the archive's.
std::vector<Image> gvrTextures(const Bytes& blob);

// efmsgfont_ascii24S.bin: 224 glyphs (code points 0x20-0xFF, Windows-1252), 24x24 at 1 bit per
// pixel, rows of three bytes read as a little-endian 24-bit number whose bit x is pixel x.
struct Font {
    static constexpr int kCell = 24, kFirst = 0x20, kCount = 224;
    Bytes bits;
    bool load(Disc& disc);
    bool pixel(int glyph, int x, int y) const;
};

}  // namespace launcher
