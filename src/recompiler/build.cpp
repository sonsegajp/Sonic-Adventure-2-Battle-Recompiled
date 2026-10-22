#include "recompiler/build.h"

#include <algorithm>
#include <sstream>

namespace sa2rc {

const std::set<std::string>& hleLibraries() {
    static const std::set<std::string> libs = {"os", "gx", "vi", "pad", "si", "dvd", "ai", "ar", "arq", "dsp", "card", "exi",
                                               "ax", "db", "OdemuExi2", "amcstubs", "odenotstub", "TRK"};
    return libs;
}

bool buildProgram(const Input& in, Program& prog, std::string& error, std::string* notes, const std::function<void(float)>& progress) {
    prog.modules.clear();
    prog.modules.reserve(in.rels.size() + 1);  // (analysis keeps pointers to the modules)
    prog.modules.emplace_back();
    if (!loadDol(in.dol, prog.modules.back(), error)) return false;
    std::vector<const std::pair<std::string, std::vector<u8>>*> rels;
    for (const auto& r : in.rels) rels.push_back(&r);
    std::sort(rels.begin(), rels.end(), [](auto* a, auto* b) { return a->first < b->first; });
    for (const auto* r : rels) {
        Module m;
        if (!loadRel(r->second, r->first, m, error)) return false;
        // ChaoStgLobby.rel reuses ChaoMotionsD's id and nothing loads it: keep the first module per id.
        if (prog.byId(m.id)) {
            if (notes) *notes += "skipping " + m.name + " (module id " + std::to_string(m.id) + " already used)\n";
            continue;
        }
        prog.modules.push_back(std::move(m));
    }
    analyze(prog, progress);
    // Patch points: main.dol addresses (hex, one per line, '#' comments) where native mod code can run.
    std::istringstream pf(in.patches);
    for (std::string line; std::getline(pf, line);) {
        line = line.substr(0, line.find('#'));
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const u32 a = u32(std::stoul(line, nullptr, 16));
        if (!prog.patchPoints.count(a)) prog.patchPoints.emplace(a, int(prog.patchPoints.size()));
    }
    if (!in.names.empty()) applyNames(prog, in.names, hleLibraries());
    return true;
}

}  // namespace sa2rc
