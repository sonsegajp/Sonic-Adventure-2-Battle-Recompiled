#pragma once
// The whole recompile as one call, from the program's files in memory: what the developer CLI
// (tools/recomp/sa2recompile.cpp) does from orig/, and the launcher from the player's disc
// (src/launcher/setup.cpp).
#include "recompiler/emit.h"

#include <string>
#include <utility>
#include <vector>

namespace sa2rc {

struct Input {
    std::vector<u8> dol;                                         // main.dol
    std::vector<std::pair<std::string, std::vector<u8>>> rels;   // code modules: file name, contents
    std::string names;    // config/<game id>/names.txt: the SDK functions replaced by name
    std::string patches;  // config/<game id>/patchpoints.txt (may be empty)
};

// Loads and analyses every module into `prog` (modules in file-name order; a module whose id is
// already taken is skipped, with a line in `notes`). False with a reason on a bad file.
bool buildProgram(const Input& in, Program& prog, std::string& error, std::string* notes = nullptr,
                  const std::function<void(float)>& progress = {});

// Libraries whose named functions the runtime replaces (applyNames' hleLibs).
const std::set<std::string>& hleLibraries();

}  // namespace sa2rc
