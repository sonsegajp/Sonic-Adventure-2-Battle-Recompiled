// Developer CLI for the recompiler: sa2recompile --dol main.dol --rels <dir> [--names f] [--funcs out.tsv]
// [--out dir]. The shipped program runs the same code in-process at first-run setup.
#include "recompiler/build.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace sa2rc;

static bool readFile(const std::filesystem::path& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

int main(int argc, char** argv) {
    std::string dolPath, relDir, funcsOut, outDir, namesPath, patchesPath;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k == "--dol") dolPath = argv[i + 1];
        else if (k == "--rels") relDir = argv[i + 1];
        else if (k == "--funcs") funcsOut = argv[i + 1];
        else if (k == "--out") outDir = argv[i + 1];
        else if (k == "--names") namesPath = argv[i + 1];
        else if (k == "--patches") patchesPath = argv[i + 1];
    }
    if (dolPath.empty()) {
        std::fprintf(stderr, "usage: sa2recompile --dol main.dol [--rels dir] [--funcs out.tsv]\n");
        return 2;
    }
    Input in;
    if (!readFile(dolPath, in.dol)) return std::fprintf(stderr, "can't read %s\n", dolPath.c_str()), 1;
    if (!relDir.empty())
        for (auto& e : std::filesystem::directory_iterator(relDir)) {
            if (e.path().extension() != ".rel") continue;
            in.rels.emplace_back(e.path().filename().string(), std::vector<u8>());
            if (!readFile(e.path(), in.rels.back().second)) return std::fprintf(stderr, "can't read %s\n", e.path().string().c_str()), 1;
        }
    auto readText = [](const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    };
    if (!namesPath.empty()) in.names = readText(namesPath);
    if (!patchesPath.empty()) in.patches = readText(patchesPath);
    Program prog;
    std::string err, notes;
    if (!buildProgram(in, prog, err, &notes)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    std::fputs(notes.c_str(), stderr);
    size_t totalF = 0, totalA = 0;
    u64 totalWords = 0, covered = 0;
    for (auto& mi : prog.info) {
        const Module& m = *mi.mod;
        u64 words = 0, cov = 0;
        for (auto& s : m.sections) {
            if (!s.exec || !s.fileOff) continue;
            for (u32 a = s.addr; a < s.addr + s.size; a += 4) words += m.word(a) != 0;
        }
        size_t nf = 0;
        for (auto& [a, f] : mi.funcs) {
            if (f.alias) continue;
            nf++;
            for (u32 x = f.start; x < f.end; x += 4) cov += m.word(x) != 0;
        }
        totalF += nf;
        totalA += mi.aliases;
        totalWords += words;
        covered += cov;
        if (m.isDol() || m.name == "stg13D.rel" || m.name == "titleD.rel")
            std::printf("%-20s funcs %6zu aliases %4zu tail-seeds %4zu gap-seeds %4zu ptr-seeds %4zu jumptables %4zu "
                        "unresolved-bctr %3zu coverage %.3f%%\n",
                        m.name.c_str(), nf, mi.aliases, mi.tailSeeds, mi.gapSeeds, mi.pointerSeeds,
                        mi.jumpTables.size(), mi.unresolvedBctr, words ? 100.0 * cov / words : 0.0);
    }
    std::printf("ALL %zu modules: %zu functions + %zu aliases, %llu code words, coverage %.3f%%\n", prog.modules.size(),
                totalF, totalA, (unsigned long long)totalWords, totalWords ? 100.0 * covered / totalWords : 0.0);
    if (!funcsOut.empty()) {
        std::ofstream o(funcsOut);
        for (auto& mi : prog.info)
            for (auto& [a, f] : mi.funcs)
                o << mi.mod->id << '\t' << std::hex << f.start << '\t' << f.end << std::dec << '\t' << (f.alias ? 1 : 0)
                  << '\n';
    }
    if (!outDir.empty()) {
        EmitStats st;
        auto files = emitProgram(prog, st);
        std::filesystem::create_directories(outDir);
        u64 bytes = 0;
        for (auto& f : files) {
            const auto path = std::filesystem::path(outDir) / f.name;
            // Only rewrite changed files so incremental builds stay incremental.
            std::ifstream old(path, std::ios::binary);
            std::string prev((std::istreambuf_iterator<char>(old)), {});
            if (prev != f.text) std::ofstream(path, std::ios::binary) << f.text;
            bytes += f.text.size();
        }
        std::printf("emitted %zu files, %.1f MB: %zu functions (%zu HLE), %zu instructions\n", files.size(),
                    bytes / 1048576.0, st.functions, st.hle, st.instructions);
        for (auto& [k, v] : st.unsupported) std::printf("  unsupported %s: %zu\n", k.c_str(), v);
        std::printf("direct branches to addresses no function starts at: %zu\n", st.unresolvedTargets.size());
        for (auto& [k, v] : st.unresolvedTargets) std::printf("  %s (%zu)\n", k.c_str(), v);
    }
    return 0;
}
