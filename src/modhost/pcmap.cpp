#include "modhost/pcmap.h"

#include "runtime/runtime.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rt {
extern std::atomic<bool> g_relLinked[256];
}

namespace mh {

namespace {

const char* const kX86Regs[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

int x86Reg(const std::string& s) {
    for (int i = 0; i < 8; i++)
        if (s == kX86Regs[i]) return i;
    return -1;
}
int gcReg(const std::string& s) {  // r0..r31, f0..f31 (32+)
    if (s.size() < 2 || (s[0] != 'r' && s[0] != 'f')) return -1;
    const int n = std::atoi(s.c_str() + 1);
    return n < 0 || n > 31 ? -1 : (s[0] == 'f' ? 32 + n : n);
}
u32 hex(const std::string& s) { return u32(std::strtoul(s.c_str(), nullptr, 16)); }
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) out.push_back(cur), cur.clear();
        else cur += ch;
    }
    out.push_back(cur);
    return out;
}

}  // namespace

bool GcAddr::parse(const std::string& s) {
    if (!s.empty() && s[0] == 'm') {
        const auto p = split(s.substr(1), ':');
        if (p.size() != 3) return false;
        module = u32(std::atoi(p[0].c_str())), section = u32(std::atoi(p[1].c_str())), addr = hex(p[2]);
        return module < 256 && section < 32;
    }
    module = 0, addr = hex(s);
    return addr != 0;
}

u32 GcAddr::resolve() const {
    if (!module) return addr;
    if (!rt::g_relLinked[module]) return 0;
    const u32 base = rt::g_relBase[module][section];
    return base ? base + addr : 0;
}

u32 FuncMap::gc() const {
    for (const GcAddr& g : gcs)
        if (const u32 a = g.resolve()) return a;
    return 0;
}

bool PcMap::load(const std::filesystem::path& file) {
    std::ifstream f(file);
    if (!f) return false;
    std::string line;
    int lineNo = 0, bad = 0;
    size_t nInsn = 0, nFunc = 0, nData = 0;
    while (std::getline(f, line)) {
        lineNo++;
        std::string note;
        if (const size_t hash = line.find('#'); hash != std::string::npos) {
            note = line.substr(hash + 1);
            line.resize(hash);
        }
        std::istringstream in(line);
        std::vector<std::string> tok;
        for (std::string t; in >> t;) tok.push_back(t);
        if (tok.empty()) continue;
        auto fail = [&](const char* why) {
            if (bad++ < 20) rt::log("[modhost] %s:%d: %s\n", file.string().c_str(), lineNo, why);
        };
        if (tok[0] == "insn" && tok.size() >= 4) {
            InsnMap m;
            m.pc = hex(tok[1]);
            for (size_t i = 0; i + 1 < tok[2].size(); i += 2) m.bytes.push_back(u8(hex(tok[2].substr(i, 2))));
            auto sites = [&](const std::string& list, std::vector<GcSite>& out) {
                for (std::string s : split(list, ',')) {
                    GcSite site;
                    if (!s.empty() && s.back() == '+') site.kind = GcSite::After, s.pop_back();
                    else if (!s.empty() && s.back() == '>') site.kind = GcSite::Taken, s.pop_back();
                    if (site.at.parse(s)) out.push_back(site);
                    else fail("bad GameCube place");
                }
            };
            sites(tok[3], m.sites);
            for (size_t i = 4; i < tok.size(); i++) {
                const std::string& o = tok[i];
                if (o == "inv") m.inverted = true;
                else if (o.rfind("imm=", 0) == 0) sites(o.substr(4), m.immSites);
                else if (o.rfind("fpu=", 0) == 0) m.fpuMask = int(hex(o.substr(4)));
                else if (o.rfind("regs=", 0) == 0) {
                    for (const auto& pair : split(o.substr(5), ',')) {
                        const auto kv = split(pair, ':');
                        if (kv.size() == 2 && x86Reg(kv[0]) >= 0 && gcReg(kv[1]) >= 0) m.regs[x86Reg(kv[0])] = gcReg(kv[1]);
                        else fail("bad register pair");
                    }
                } else fail("unknown insn option");
            }
            if (m.bytes.empty() || m.sites.empty()) fail("insn needs bytes and sites");
            m.note = note;
            insns[m.pc] = std::move(m);
            nInsn++;
        } else if (tok[0] == "func" && tok.size() >= 4) {
            FuncMap m;
            m.pc = hex(tok[1]);
            for (const std::string& s : split(tok[2], ',')) {
                GcAddr g;
                if (g.parse(s)) m.gcs.push_back(g);
                else fail("bad GameCube address");
            }
            if (tok[3] == "?") m.known = false;
            else if (tok[3] != "-")
                for (const auto& a : split(tok[3], ',')) {
                    const auto kv = split(a, '=');
                    if (kv.size() != 2) {
                        fail("bad argument");
                        continue;
                    }
                    const int dst = gcReg(kv[0]);
                    int src = x86Reg(kv[1]);
                    if (src < 0 && kv[1].size() > 1 && kv[1][0] == 's') src = 100 + std::atoi(kv[1].c_str() + 1);
                    if (dst < 0 || src < 0) fail("bad argument");
                    else m.args.push_back({dst, src});
                }
            for (size_t i = 4; i < tok.size(); i++) {
                if (tok[i] == "ret=eax") m.ret = 1;
                else if (tok[i] == "ret=st0") m.ret = 2;
                else if (tok[i].rfind("pop=", 0) == 0) m.pop = u32(std::atoi(tok[i].c_str() + 4));
                else if (tok[i].rfind("len=", 0) == 0) m.len = u32(std::atoi(tok[i].c_str() + 4));
                else fail("unknown func option");
            }
            if (m.gcs.empty()) fail("func needs a GameCube address");
            m.note = note;
            funcs[m.pc] = std::move(m);
            nFunc++;
        } else if (tok[0] == "data" && tok.size() >= 4) {
            DataMap m;
            m.pc = hex(tok[1]);
            if (!m.gc.parse(tok[2])) {
                fail("bad GameCube address");
                continue;
            }
            m.size = u32(std::strtoul(tok[3].c_str(), nullptr, 0));
            for (size_t i = 4; i < tok.size(); i++) {
                if (tok[i].rfind("elem=", 0) == 0) m.elem = u32(std::atoi(tok[i].c_str() + 5));
                else if (tok[i] == "ptr") m.ptr = true;
                else fail("unknown data option");
            }
            if (!m.elem || m.size % m.elem) {
                fail("data size is not a whole number of elements");
                continue;
            }
            m.note = note;
            // A hand entry replaces the generated ones it overlaps.
            for (auto it = data.lower_bound(m.pc > 64 ? m.pc - 64 : 0); it != data.end() && it->first < m.pc + m.size;)
                it = it->first + it->second.size > m.pc ? data.erase(it) : std::next(it);
            data[m.pc] = std::move(m);
            nData++;
        } else if (tok[0] == "pad" && tok.size() == 3) {
            padding[hex(tok[1])] = u32(std::strtoul(tok[2].c_str(), nullptr, 0));
        } else {
            fail("unknown entry");
        }
    }
    rt::log("[modhost] %s: %zu instructions, %zu functions, %zu variables%s\n", file.filename().string().c_str(), nInsn,
            nFunc, nData, bad ? " (with errors)" : "");
    return true;
}

const InsnMap* PcMap::insnCovering(u32 pc) const {
    auto it = insns.upper_bound(pc);
    if (it == insns.begin()) return nullptr;
    --it;
    return pc < it->first + it->second.bytes.size() ? &it->second : nullptr;
}

const DataMap* PcMap::dataAt(u32 pc) const {
    auto it = data.upper_bound(pc);
    if (it == data.begin()) return nullptr;
    --it;
    return pc < it->first + it->second.size ? &it->second : nullptr;
}

const FuncMap* PcMap::funcAt(u32 pc) const {
    const auto it = funcs.find(pc);
    return it == funcs.end() ? nullptr : &it->second;
}

const FuncMap* PcMap::funcCovering(u32 pc) const {
    auto it = funcs.upper_bound(pc);
    if (it == funcs.begin()) return nullptr;
    --it;
    return pc < it->first + it->second.len ? &it->second : nullptr;
}

bool PcMap::isPadding(u32 pc) const {
    auto it = padding.upper_bound(pc);
    if (it == padding.begin()) return false;
    --it;
    return pc < it->first + it->second;
}

}  // namespace mh
