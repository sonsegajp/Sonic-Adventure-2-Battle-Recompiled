#include "recompiler/emit.h"

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <set>

namespace sa2rc {

namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    const int n = std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    if (n < int(sizeof buf)) return std::string(buf, n > 0 ? size_t(n) : 0);
    std::string s(size_t(n) + 1, '\0');
    va_start(ap, f);
    std::vsnprintf(s.data(), s.size(), f, ap);
    va_end(ap);
    s.resize(size_t(n));
    return s;
}

std::string hex(u32 v) { return fmt("0x%Xu", v); }

u32 maskOf(u32 mb, u32 me) {
    const u32 m1 = 0xFFFFFFFFu >> mb;
    const u32 m2 = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (m1 & m2) : (m1 | m2);
}

bool isUncond(u32 bo) { return (bo & 0x14) == 0x14; }

struct Unsupported {
    std::string what;
};

class FuncEmitter {
public:
    FuncEmitter(const Program& p, const ModuleInfo& mi, const Func& f, EmitStats& st)
        : p_(p), mi_(mi), m_(*mi.mod), f_(f), st_(st) {}

    std::string run();

private:
    const Program& p_;
    const ModuleInfo& mi_;
    const Module& m_;
    const Func& f_;
    EmitStats& st_;
    u32 pc_ = 0;
    std::vector<std::string> lines_;
    std::set<u32> labels_;

    void E(const std::string& s) { lines_.push_back("    " + s); }

    // ---- addresses ----
    const ModuleInfo* infoOf(u32 id) const {
        for (const auto& i : p_.info)
            if (i.mod->id == id) return &i;
        return nullptr;
    }
    // Run-time guest address of a code/data address in module `mod`.
    std::string realAddr(u32 mod, u32 a) const {
        if (mod == 0) return hex(a);
        return fmt("(g_relBase[%u][%u] + 0x%Xu)", mod, relSecOf(a), a & 0xFFFFFF);
    }
    std::string callExpr(u32 mod, u32 a) const {
        if (const ModuleInfo* ti = infoOf(mod); ti && ti->funcs.count(a)) return funcSymbol(*ti->mod, a) + "(c)";
        return "c.call(" + realAddr(mod, a) + ")";
    }
    std::string retAddr() const { return realAddr(m_.id, pc_ + 4); }
    bool inFunc(u32 a) const { return a >= f_.start && a < f_.end; }

    // ---- immediates (relocation aware) ----
    const Reloc* halfReloc() const { return m_.isDol() ? nullptr : m_.relocOf(pc_, true); }
    std::string relocTarget(const Reloc& r) const { return realAddr(r.module, r.target); }
    // Sign-extended 16-bit immediate (addi, loads/stores, cmpi...).
    std::string immS(u32 w) const {
        if (const Reloc* r = halfReloc()) {
            if (r->type == R_PPC_ADDR16_LO || r->type == R_PPC_ADDR16) {
                if (r->module == 0) return hex(u32(sext16(r->target & 0xFFFF)));
                return "lo16s(" + relocTarget(*r) + ")";
            }
            throw Unsupported{fmt("reloc type %u on simm", r->type)};
        }
        return hex(u32(sext16(w & 0xFFFF)));
    }
    // Unsigned 16-bit immediate (ori, andi., xori, cmpli).
    std::string immU(u32 w) const {
        if (const Reloc* r = halfReloc()) {
            if (r->type == R_PPC_ADDR16_LO || r->type == R_PPC_ADDR16) {
                if (r->module == 0) return hex(r->target & 0xFFFF);
                return "(" + relocTarget(*r) + " & 0xFFFFu)";
            }
            throw Unsupported{fmt("reloc type %u on uimm", r->type)};
        }
        return hex(w & 0xFFFF);
    }
    // Shifted immediate of addis/lis/oris/xoris/andis.
    std::string immHi(u32 w) const {
        if (const Reloc* r = halfReloc()) {
            if (r->type == R_PPC_ADDR16_HA) {
                if (r->module == 0) return hex((r->target + 0x8000u) & 0xFFFF0000u);
                return "ha16(" + relocTarget(*r) + ")";
            }
            if (r->type == R_PPC_ADDR16_HI) {
                if (r->module == 0) return hex(r->target & 0xFFFF0000u);
                return "(" + relocTarget(*r) + " & 0xFFFF0000u)";
            }
            throw Unsupported{fmt("reloc type %u on hi imm", r->type)};
        }
        return hex((w & 0xFFFF) << 16);
    }

    static std::string R(u32 n) { return fmt("c.r[%u]", n); }
    static std::string F(u32 n) { return fmt("c.f[%u]", n); }
    static std::string P1(u32 n) { return fmt("c.ps1[%u]", n); }
    std::string RA0(u32 a) const { return a ? R(a) : std::string("0u"); }
    std::string eaD(u32 a, u32 w) const { return a ? "(" + R(a) + " + " + immS(w) + ")" : immS(w); }
    std::string eaX(u32 a, u32 b) const { return a ? "(" + R(a) + " + " + R(b) + ")" : R(b); }

    // ---- branches ----
    std::string branchCond(u32 bo, u32 bi) {
        std::vector<std::string> parts;
        if (!(bo & 0x04)) {
            E("c.ctr--;");
            parts.push_back((bo & 0x02) ? "c.ctr == 0" : "c.ctr != 0");
        }
        if (!(bo & 0x10)) {
            const std::string t = fmt("c.crbit(%u)", bi);
            parts.push_back((bo & 0x08) ? t : "!" + t);
        }
        if (parts.empty()) return "true";
        std::string s = parts[0];
        for (size_t i = 1; i < parts.size(); i++) s += " && " + parts[i];
        return s;
    }
    void branchTo(u32 tmod, u32 target, bool link, const std::string& cond) {
        const bool always = cond == "true";
        if (link) {
            E("c.lr = " + retAddr() + ";");
            const std::string call = callExpr(tmod, target);
            E(always ? call + ";" : "if (" + cond + ") { " + call + "; }");
            return;
        }
        if (tmod == m_.id && inFunc(target)) {
            labels_.insert(target);
            std::string jump = fmt("goto L_%08X;", target);
            if (target <= pc_) jump = "{ c.poll(); " + jump + " }";  // back-edge: let interrupts in
            E(always ? jump : "if (" + cond + ") " + jump);
            return;
        }
        const std::string call = callExpr(tmod, target);
        E(always ? call + "; return;" : "if (" + cond + ") { " + call + "; return; }");
    }

    void insn(u32 w);
    void op31(u32 w);
    void op63(u32 w);
    void op59(u32 w);
    void op4(u32 w);
    void loadStore(u32 op, u32 w);
    void trapCond(u32 to, const std::string& a, const std::string& b);
};

std::string FuncEmitter::run() {
    std::vector<std::pair<size_t, u32>> marks;
    for (u32 pc = f_.start; pc < f_.end; pc += 4) {
        pc_ = pc;
        marks.push_back({lines_.size(), pc});
        const u32 w = m_.word(pc);
        try {
            insn(w);
        } catch (const Unsupported& u) {
            E(fmt("c.unimplemented(0x%Xu, 0x%08Xu);  // ", pc, w) + u.what);
            st_.unsupported[u.what]++;
        }
        st_.instructions++;
    }
    // Running off the end continues into the next function.
    const u32 last = m_.word(f_.end - 4);
    const u32 lop = last >> 26;
    const bool ends = (lop == 18 && !(last & 1)) ||
                      (lop == 19 && (bits(last, 21, 30) == 16 || bits(last, 21, 30) == 528) && !(last & 1) &&
                       isUncond(bits(last, 6, 10) | (bits(last, 21, 30) == 528 ? 4 : 0)));
    pc_ = f_.end - 4;
    std::string text;
    size_t mi = 0;
    for (size_t i = 0; i <= lines_.size(); i++) {
        while (mi < marks.size() && marks[mi].first == i) {
            if (labels_.count(marks[mi].second)) text += fmt("L_%08X:;\n", marks[mi].second);
            mi++;
        }
        if (i < lines_.size()) {
            text += lines_[i];
            text += '\n';
        }
    }
    if (!ends) text += "    " + callExpr(m_.id, f_.end) + "; return;\n";
    return text;
}

void FuncEmitter::trapCond(u32 to, const std::string& a, const std::string& b) {
    std::vector<std::string> c;
    if (to & 16) c.push_back("s32(" + a + ") < s32(" + b + ")");
    if (to & 8) c.push_back("s32(" + a + ") > s32(" + b + ")");
    if (to & 4) c.push_back(a + " == " + b);
    if (to & 2) c.push_back(a + " < " + b);
    if (to & 1) c.push_back(a + " > " + b);
    if (c.empty()) return;
    std::string s = c[0];
    for (size_t i = 1; i < c.size(); i++) s += " || " + c[i];
    if (to == 31) E(fmt("c.trap(0x%Xu);", pc_));
    else E("if (" + s + fmt(") c.trap(0x%Xu);", pc_));
}

void FuncEmitter::insn(u32 w) {
    const u32 op = w >> 26;
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20);
    switch (op) {
    case 18: {  // b / bl
        u32 tmod = m_.id, t;
        if (const Reloc* r = m_.isDol() ? nullptr : m_.relocOf(pc_, false); r && r->type == R_PPC_REL24) {
            tmod = r->module;
            t = r->target;
        } else {
            const s32 li = sext(w & 0x03FFFFFC, 26);
            t = (w & 2) ? u32(li) : pc_ + u32(li);
        }
        if ((w & 1) && tmod == m_.id && t == pc_ + 4) {  // bl to the next instruction: just reads the PC
            E("c.lr = " + retAddr() + ";");
            return;
        }
        branchTo(tmod, t, w & 1, "true");
        return;
    }
    case 16: {  // bc
        u32 tmod = m_.id, t;
        if (const Reloc* r = m_.isDol() ? nullptr : m_.relocOf(pc_, false); r && r->type == R_PPC_REL14) {
            tmod = r->module;
            t = r->target;
        } else {
            const s32 bd = sext16(w & 0xFFFC);
            t = (w & 2) ? u32(bd) : pc_ + u32(bd);
        }
        branchTo(tmod, t, w & 1, branchCond(d, a));
        return;
    }
    case 19: {
        const u32 xo = bits(w, 21, 30);
        const bool lk = w & 1;
        if (xo == 16) {  // bclr
            const std::string cond = branchCond(d, a);
            if (lk) E("{ const u32 t = c.lr; c.lr = " + retAddr() + "; if (" + cond + ") c.call(t); }");
            else E(cond == "true" ? "return;" : "if (" + cond + ") return;");
            return;
        }
        if (xo == 528) {  // bcctr
            const std::string cond = branchCond(d | 4, a);
            if (lk) {
                E("c.lr = " + retAddr() + ";");
                E(cond == "true" ? "c.call(c.ctr);" : "if (" + cond + ") c.call(c.ctr);");
                return;
            }
            auto it = mi_.jumpTables.find(pc_);
            if (it != mi_.jumpTables.end()) {
                std::set<u32> targets(it->second.begin(), it->second.end());
                std::string cases;
                for (u32 t : targets) {
                    if (!inFunc(t)) continue;
                    labels_.insert(t);
                    cases += fmt(" case 0x%Xu: goto L_%08X;", m_.isDol() ? t : (t & 0xFFFFFF), t);
                }
                const std::string sel =
                    m_.isDol() ? std::string("c.ctr") : fmt("(c.ctr - g_relBase[%u][%u])", m_.id, relSecOf(f_.start));
                E("if (" + cond + ") { switch (" + sel + ") {" + cases + " default: c.call(c.ctr); return; } }");
                return;
            }
            E(cond == "true" ? "c.call(c.ctr); return;" : "if (" + cond + ") { c.call(c.ctr); return; }");
            return;
        }
        const std::string ea = fmt("c.crbit(%u)", a), eb = fmt("c.crbit(%u)", b);
        std::string v;
        switch (xo) {
        case 257: v = ea + " && " + eb; break;            // crand
        case 449: v = ea + " || " + eb; break;            // cror
        case 193: v = ea + " != " + eb; break;            // crxor
        case 225: v = "!(" + ea + " && " + eb + ")"; break;  // crnand
        case 33: v = "!(" + ea + " || " + eb + ")"; break;   // crnor
        case 289: v = ea + " == " + eb; break;            // creqv
        case 129: v = ea + " && !" + eb; break;           // crandc
        case 417: v = ea + " || !" + eb; break;           // crorc
        default: break;
        }
        if (!v.empty()) {
            E(fmt("c.setCrbit(%u, ", d) + v + ");");
            return;
        }
        if (xo == 0) {  // mcrf
            E(fmt("c.cr[%u] = c.cr[%u];", d >> 2, a >> 2));
            return;
        }
        if (xo == 150) {  // isync
            E("// isync");
            return;
        }
        throw Unsupported{fmt("op19 xo%u", xo)};
    }
    case 3: trapCond(d, R(a), immS(w)); return;  // twi
    case 7: E(R(d) + " = " + R(a) + " * " + immS(w) + ";"); return;  // mulli
    case 8: E(R(d) + " = c.addCarry(~" + R(a) + ", " + immS(w) + ", 1);"); return;  // subfic
    case 10: E(fmt("c.cmpu(%u, ", d >> 2) + R(a) + ", " + immU(w) + ");"); return;  // cmpli
    case 11: E(fmt("c.cmps(%u, s32(", d >> 2) + R(a) + "), s32(" + immS(w) + "));"); return;  // cmpi
    case 12: E(R(d) + " = c.addCarry(" + R(a) + ", " + immS(w) + ", 0);"); return;  // addic
    case 13:  // addic.
        E(R(d) + " = c.addCarry(" + R(a) + ", " + immS(w) + ", 0);");
        E("c.rc0(" + R(d) + ");");
        return;
    case 14: E(R(d) + " = " + (a ? R(a) + " + " + immS(w) : immS(w)) + ";"); return;  // addi / li
    case 15: E(R(d) + " = " + (a ? R(a) + " + " + immHi(w) : immHi(w)) + ";"); return;  // addis / lis
    case 17: E("c.syscall();"); return;  // sc
    case 20: case 21: case 23: {  // rlwimi / rlwinm / rlwnm
        const u32 mb = bits(w, 21, 25), me = bits(w, 26, 30);
        const u32 mask = maskOf(mb, me);
        std::string rot;
        if (op == 23) rot = "c.rotl(" + R(d) + ", " + R(b) + ")";
        else rot = b ? "c.rotl(" + R(d) + ", " + fmt("%u", b) + ")" : R(d);
        if (op == 20) E(R(a) + " = (" + rot + " & " + hex(mask) + ") | (" + R(a) + " & " + hex(~mask) + ");");
        else E(R(a) + " = " + (mask == 0xFFFFFFFFu ? rot : rot + " & " + hex(mask)) + ";");
        if (w & 1) E("c.rc0(" + R(a) + ");");
        return;
    }
    case 24: E(R(a) + " = " + R(d) + " | " + immU(w) + ";"); return;  // ori (nop when all zero)
    case 25: E(R(a) + " = " + R(d) + " | " + immHi(w) + ";"); return;  // oris
    case 26: E(R(a) + " = " + R(d) + " ^ " + immU(w) + ";"); return;  // xori
    case 27: E(R(a) + " = " + R(d) + " ^ " + immHi(w) + ";"); return;  // xoris
    case 28:  // andi.
        E(R(a) + " = " + R(d) + " & " + immU(w) + ";");
        E("c.rc0(" + R(a) + ");");
        return;
    case 29:  // andis.
        E(R(a) + " = " + R(d) + " & " + immHi(w) + ";");
        E("c.rc0(" + R(a) + ");");
        return;
    case 31: op31(w); return;
    case 59: op59(w); return;
    case 63: op63(w); return;
    case 4: op4(w); return;
    case 56: case 57: case 60: case 61: {  // psq_l / psq_lu / psq_st / psq_stu
        const bool wbit = bits(w, 16, 16), upd = op == 57 || op == 61;
        const u32 i = bits(w, 17, 19);
        const s32 off = sext(w & 0xFFF, 12);
        const std::string ea = a ? fmt("(c.r[%u] + 0x%Xu)", a, u32(off)) : hex(u32(off));
        const char* fn = op < 60 ? "psqLoad" : "psqStore";
        if (upd) E(fmt("{ const u32 ea = %s; c.%s(%u, ea, %s, %u); c.r[%u] = ea; }", ea.c_str(), fn, d, wbit ? "true" : "false", i, a));
        else E(fmt("c.%s(%u, %s, %s, %u);", fn, d, ea.c_str(), wbit ? "true" : "false", i));
        return;
    }
    default:
        if (op >= 32 && op <= 55) {
            loadStore(op, w);
            return;
        }
        throw Unsupported{fmt("op%u", op)};
    }
}

void FuncEmitter::loadStore(u32 op, u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15);
    const bool upd = (op & 1) && op != 47 && op != 46;  // odd opcodes are the update forms (except lmw/stmw)
    const std::string ea = upd ? "ea" : eaD(a, w);
    std::string body;
    switch (op) {
    case 32: case 33: body = R(d) + " = rd32(" + ea + ");"; break;
    case 34: case 35: body = R(d) + " = rd8(" + ea + ");"; break;
    case 36: case 37: body = "wr32(" + ea + ", " + R(d) + ");"; break;
    case 38: case 39: body = "wr8(" + ea + ", " + R(d) + ");"; break;
    case 40: case 41: body = R(d) + " = rd16(" + ea + ");"; break;
    case 42: case 43: body = R(d) + " = u32(s32(s16(u16(rd16(" + ea + ")))));"; break;
    case 44: case 45: body = "wr16(" + ea + ", " + R(d) + ");"; break;
    case 46:  // lmw
        E("{ const u32 ea = " + eaD(a, w) + ";" + fmt(" for (u32 i = %u; i < 32; i++) c.r[i] = rd32(ea + (i - %u) * 4); }", d, d));
        return;
    case 47:  // stmw
        E("{ const u32 ea = " + eaD(a, w) + ";" + fmt(" for (u32 i = %u; i < 32; i++) wr32(ea + (i - %u) * 4, c.r[i]); }", d, d));
        return;
    case 48: case 49: body = fmt("c.f[%u] = c.ps1[%u] = rdf32(", d, d) + ea + ");"; break;
    case 50: case 51: body = F(d) + " = rdf64(" + ea + ");"; break;
    case 52: case 53: body = "wrf32(" + ea + ", " + F(d) + ");"; break;
    case 54: case 55: body = "wrf64(" + ea + ", " + F(d) + ");"; break;
    default: throw Unsupported{fmt("op%u", op)};
    }
    if (upd) {
        const bool isLoad = op <= 35 || (op >= 40 && op <= 43) || op == 48 || op == 49 || op == 50 || op == 51;
        // Load with update: the loaded register wins over rA only in invalid forms (rA == rD).
        if (isLoad) E("{ const u32 ea = " + R(a) + " + " + immS(w) + "; " + R(a) + " = ea; " + body + " }");
        else E("{ const u32 ea = " + R(a) + " + " + immS(w) + "; " + body + " " + R(a) + " = ea; }");
    } else {
        E(body);
    }
}

void FuncEmitter::op31(u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20);
    const u32 xo = bits(w, 21, 30), xo9 = bits(w, 22, 30);
    const bool rc = w & 1, oe = bits(w, 21, 21);
    const std::string D = R(d), A = R(a), B = R(b);
    // XO-form arithmetic (OE in bit 21).
    std::string arith;
    switch (xo9) {
    case 266: arith = A + " + " + B; break;                              // add
    case 10: arith = "c.addCarry(" + A + ", " + B + ", 0)"; break;       // addc
    case 138: arith = "c.addCarry(" + A + ", " + B + ", c.ca)"; break;   // adde
    case 234: arith = "c.addCarry(" + A + ", 0xFFFFFFFFu, c.ca)"; break; // addme
    case 202: arith = "c.addCarry(" + A + ", 0u, c.ca)"; break;          // addze
    case 40: arith = B + " - " + A; break;                               // subf
    case 8: arith = "c.addCarry(~" + A + ", " + B + ", 1)"; break;       // subfc
    case 136: arith = "c.addCarry(~" + A + ", " + B + ", c.ca)"; break;  // subfe
    case 232: arith = "c.addCarry(~" + A + ", 0xFFFFFFFFu, c.ca)"; break;  // subfme
    case 200: arith = "c.addCarry(~" + A + ", 0u, c.ca)"; break;         // subfze
    case 104: arith = "0u - " + A; break;                                // neg
    case 235: arith = A + " * " + B; break;                              // mullw
    case 75: arith = "c.mulhw(" + A + ", " + B + ")"; break;             // mulhw
    case 11: arith = "c.mulhwu(" + A + ", " + B + ")"; break;            // mulhwu
    case 491: arith = "c.divw(" + A + ", " + B + ")"; break;             // divw
    case 459: arith = "c.divwu(" + A + ", " + B + ")"; break;            // divwu
    default: break;
    }
    if (!arith.empty()) {
        if (oe) throw Unsupported{fmt("op31 xo%u with OE", xo9)};
        E(D + " = " + arith + ";");
        if (rc) E("c.rc0(" + D + ");");
        return;
    }
    // X-form logical: rS = d, rA = a (destination).
    std::string logic;
    switch (xo) {
    case 28: logic = R(d) + " & " + B; break;                // and
    case 60: logic = R(d) + " & ~" + B; break;               // andc
    case 444: logic = d == b ? R(d) : R(d) + " | " + B; break;  // or / mr
    case 412: logic = R(d) + " | ~" + B; break;              // orc
    case 316: logic = R(d) + " ^ " + B; break;               // xor
    case 476: logic = "~(" + R(d) + " & " + B + ")"; break;  // nand
    case 124: logic = "~(" + R(d) + " | " + B + ")"; break;  // nor
    case 284: logic = "~(" + R(d) + " ^ " + B + ")"; break;  // eqv
    case 24: logic = "c.slw(" + R(d) + ", " + B + ")"; break;  // slw
    case 536: logic = "c.srw(" + R(d) + ", " + B + ")"; break; // srw
    case 792: logic = "c.sraw(" + R(d) + ", " + B + ")"; break;  // sraw
    case 824: logic = "c.sraw(" + R(d) + ", " + fmt("%uu", b) + ")"; break;  // srawi
    case 26: logic = "c.cntlzw(" + R(d) + ")"; break;          // cntlzw
    case 922: logic = "u32(s32(s16(u16(" + R(d) + "))))"; break;  // extsh
    case 954: logic = "u32(s32(s8(u8(" + R(d) + "))))"; break;    // extsb
    default: break;
    }
    if (!logic.empty()) {
        E(A + " = " + logic + ";");
        if (rc) E("c.rc0(" + A + ");");
        return;
    }
    const std::string ea = eaX(a, b);
    auto upd = [&](const std::string& body) { E("{ const u32 ea = " + A + " + " + B + "; " + body + " }"); };
    switch (xo) {
    case 0: E(fmt("c.cmps(%u, s32(", d >> 2) + A + "), s32(" + B + "));"); return;  // cmp
    case 32: E(fmt("c.cmpu(%u, ", d >> 2) + A + ", " + B + ");"); return;          // cmpl
    case 4: trapCond(d, A, B); return;                                            // tw
    case 23: E(D + " = rd32(" + ea + ");"); return;                                // lwzx
    case 55: upd(A + " = ea; " + D + " = rd32(ea);"); return;                      // lwzux
    case 87: E(D + " = rd8(" + ea + ");"); return;                                 // lbzx
    case 119: upd(A + " = ea; " + D + " = rd8(ea);"); return;                      // lbzux
    case 279: E(D + " = rd16(" + ea + ");"); return;                               // lhzx
    case 311: upd(A + " = ea; " + D + " = rd16(ea);"); return;                     // lhzux
    case 343: E(D + " = u32(s32(s16(u16(rd16(" + ea + ")))));"); return;           // lhax
    case 375: upd(A + " = ea; " + D + " = u32(s32(s16(u16(rd16(ea)))));"); return; // lhaux
    case 151: E("wr32(" + ea + ", " + D + ");"); return;                           // stwx
    case 183: upd("wr32(ea, " + D + "); " + A + " = ea;"); return;                 // stwux
    case 215: E("wr8(" + ea + ", " + D + ");"); return;                            // stbx
    case 247: upd("wr8(ea, " + D + "); " + A + " = ea;"); return;                  // stbux
    case 407: E("wr16(" + ea + ", " + D + ");"); return;                           // sthx
    case 439: upd("wr16(ea, " + D + "); " + A + " = ea;"); return;                 // sthux
    case 534: E(D + " = bswap32(rd32(" + ea + "));"); return;                      // lwbrx
    case 662: E("wr32(" + ea + ", bswap32(" + D + "));"); return;                  // stwbrx
    case 790: E(D + " = bswap16(u16(rd16(" + ea + ")));"); return;                 // lhbrx
    case 918: E("wr16(" + ea + ", bswap16(u16(" + D + ")));"); return;             // sthbrx
    case 20: E(D + " = rd32(" + ea + ");"); return;                                // lwarx
    case 150:                                                                      // stwcx.
        E("wr32(" + ea + ", " + D + ");");
        E("c.cr[0] = u8(2 | c.so);");
        return;
    case 535: E(fmt("c.f[%u] = c.ps1[%u] = rdf32(", d, d) + ea + ");"); return;     // lfsx
    case 567: upd(A + " = ea; " + fmt("c.f[%u] = c.ps1[%u] = rdf32(ea);", d, d)); return;  // lfsux
    case 599: E(F(d) + " = rdf64(" + ea + ");"); return;                           // lfdx
    case 631: upd(A + " = ea; " + F(d) + " = rdf64(ea);"); return;                 // lfdux
    case 663: E("wrf32(" + ea + ", " + F(d) + ");"); return;                       // stfsx
    case 695: upd("wrf32(ea, " + F(d) + "); " + A + " = ea;"); return;             // stfsux
    case 727: E("wrf64(" + ea + ", " + F(d) + ");"); return;                       // stfdx
    case 759: upd("wrf64(ea, " + F(d) + "); " + A + " = ea;"); return;             // stfdux
    case 983: E("wr32(" + ea + ", u32(bits64(" + F(d) + ")));"); return;           // stfiwx
    case 19: E(D + " = c.getCr();"); return;                                       // mfcr
    case 144: E(fmt("c.mtcrf(0x%Xu, ", bits(w, 12, 19)) + D + ");"); return;      // mtcrf
    case 83: E(D + " = c.msr;"); return;                                           // mfmsr
    case 146: E("c.mtmsr(" + D + ");"); return;                                    // mtmsr
    case 339: case 467: case 371: {                                                // mfspr / mtspr / mftb
        const u32 n = a | (b << 5);
        if (xo == 371) {
            E(D + (n == 269 ? " = c.timebaseHi();" : " = c.timebaseLo();"));
            return;
        }
        const bool to = xo == 467;
        std::string reg;
        if (n == 8) reg = "c.lr";
        else if (n == 9) reg = "c.ctr";
        else if (n >= 912 && n <= 919) reg = fmt("c.gqr[%u]", n - 912);
        if (n == 1) E(to ? "c.setXer(" + D + ");" : D + " = c.getXer();");
        else if (!reg.empty()) E(to ? reg + " = " + D + ";" : D + " = " + reg + ";");
        else E(to ? fmt("c.mtspr(%u, ", n) + D + ");" : D + fmt(" = c.mfspr(%u);", n));
        return;
    }
    case 512:  // mcrxr
        E(fmt("c.cr[%u] = u8(c.so << 3 | c.ov << 2 | c.ca << 1); c.so = c.ov = c.ca = 0;", d >> 2));
        return;
    case 86: case 54: case 278: case 246: case 470: case 982: case 598: case 854: case 306: case 566:
        E("// cache/sync");
        return;
    case 1014: E("c.dcbz(" + ea + ");"); return;  // dcbz
    case 597: E(fmt("c.lswi(%u, ", d) + RA0(a) + fmt(", %u);", b ? b : 32)); return;   // lswi
    case 725: E(fmt("c.stswi(%u, ", d) + RA0(a) + fmt(", %u);", b ? b : 32)); return;  // stswi
    default: throw Unsupported{fmt("op31 xo%u", xo)};
    }
}

void FuncEmitter::op59(u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20), cc = bits(w, 21, 25);
    const u32 xo = bits(w, 26, 30);
    const std::string A = F(a), B = F(b), C = F(cc);
    std::string e;
    switch (xo) {
    case 21: e = A + " + " + B; break;
    case 20: e = A + " - " + B; break;
    case 25: e = A + " * " + C; break;
    case 18: e = A + " / " + B; break;
    case 29: e = "__builtin_fma(" + A + ", " + C + ", " + B + ")"; break;
    case 28: e = "__builtin_fma(" + A + ", " + C + ", -" + B + ")"; break;
    case 31: e = "-__builtin_fma(" + A + ", " + C + ", " + B + ")"; break;
    case 30: e = "-__builtin_fma(" + A + ", " + C + ", -" + B + ")"; break;
    case 24: e = "c.fres(" + B + ")"; break;
    case 22: e = "__builtin_sqrt(" + B + ")"; break;
    default: throw Unsupported{fmt("op59 xo%u", xo)};
    }
    // Single-precision results fill both paired-single slots (Gekko).
    E(fmt("{ const double t = c.rs(%s); c.f[%u] = t; c.ps1[%u] = t; }", e.c_str(), d, d));
    if (w & 1) E("c.rc1();");
}

void FuncEmitter::op63(u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20), cc = bits(w, 21, 25);
    const u32 xo5 = bits(w, 26, 30), xo = bits(w, 21, 30);
    const std::string A = F(a), B = F(b), C = F(cc), D = F(d);
    std::string e;
    switch (xo5) {
    case 18: e = A + " / " + B; break;
    case 20: e = A + " - " + B; break;
    case 21: e = A + " + " + B; break;
    case 22: e = "__builtin_sqrt(" + B + ")"; break;
    case 23: e = "c.fsel(" + A + ", " + C + ", " + B + ")"; break;
    case 25: e = A + " * " + C; break;
    case 26: e = "c.frsqrte(" + B + ")"; break;
    case 28: e = "__builtin_fma(" + A + ", " + C + ", -" + B + ")"; break;
    case 29: e = "__builtin_fma(" + A + ", " + C + ", " + B + ")"; break;
    case 30: e = "-__builtin_fma(" + A + ", " + C + ", -" + B + ")"; break;
    case 31: e = "-__builtin_fma(" + A + ", " + C + ", " + B + ")"; break;
    default: break;
    }
    if (!e.empty()) {
        E(D + " = " + e + ";");
        if (w & 1) E("c.rc1();");
        return;
    }
    switch (xo) {
    case 0: case 32: E(fmt("c.fcmp(%u, ", d >> 2) + A + ", " + B + ");"); return;  // fcmpu / fcmpo
    case 12: E(fmt("{ const double t = c.rs(%s); c.f[%u] = t; c.ps1[%u] = t; }", B.c_str(), d, d)); break;  // frsp
    case 14: E(D + " = dbl(0xFFF8000000000000ull | c.fctiw(" + B + ", false));"); break;  // fctiw
    case 15: E(D + " = dbl(0xFFF8000000000000ull | c.fctiw(" + B + ", true));"); break;   // fctiwz
    case 40: E(D + " = dbl(bits64(" + B + ") ^ 0x8000000000000000ull);"); break;         // fneg
    case 72: E(D + " = " + B + ";"); break;                                               // fmr
    case 136: E(D + " = dbl(bits64(" + B + ") | 0x8000000000000000ull);"); break;         // fnabs
    case 264: E(D + " = dbl(bits64(" + B + ") & 0x7FFFFFFFFFFFFFFFull);"); break;         // fabs
    case 38: E(fmt("c.fpscr |= 0x%Xu;", 0x80000000u >> d)); break;                        // mtfsb1
    case 70: E(fmt("c.fpscr &= ~0x%Xu;", 0x80000000u >> d)); break;                       // mtfsb0
    case 134: {                                                                           // mtfsfi
        const u32 sh = 28 - 4 * (d >> 2);
        E(fmt("c.fpscr = (c.fpscr & ~0x%Xu) | 0x%Xu;", 0xFu << sh, bits(w, 16, 19) << sh));
        break;
    }
    case 583: E(D + " = dbl(0xFFF8000000000000ull | c.fpscr);"); break;  // mffs
    case 711: {                                                          // mtfsf
        const u32 fm = bits(w, 7, 14);
        u32 m = 0;
        for (int i = 0; i < 8; i++)
            if (fm & (0x80 >> i)) m |= 0xFu << (28 - 4 * i);
        E(fmt("c.fpscr = (c.fpscr & ~0x%Xu) | (u32(bits64(", m) + B + fmt(")) & 0x%Xu);", m));
        break;
    }
    case 64:  // mcrfs
        E(fmt("c.cr[%u] = u8((c.fpscr >> %u) & 0xF);", d >> 2, 28 - 4 * (a >> 2)));
        return;
    default: throw Unsupported{fmt("op63 xo%u", xo)};
    }
    if (w & 1) E("c.rc1();");
}

void FuncEmitter::op4(u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20), cc = bits(w, 21, 25);
    const u32 xo10 = bits(w, 21, 30), xo6 = bits(w, 25, 30), xo5 = bits(w, 26, 30);
    auto pair = [&](const std::string& e0, const std::string& e1, bool round = true) {
        if (round) E(fmt("{ const double t0 = c.rs(%s), t1 = c.rs(%s); c.f[%u] = t0; c.ps1[%u] = t1; }", e0.c_str(), e1.c_str(), d, d));
        else E(fmt("{ const double t0 = %s, t1 = %s; c.f[%u] = t0; c.ps1[%u] = t1; }", e0.c_str(), e1.c_str(), d, d));
        if (w & 1) E("c.rc1();");
    };
    const std::string A0 = F(a), A1 = P1(a), B0 = F(b), B1 = P1(b), C0 = F(cc), C1 = P1(cc);
    switch (xo10) {
    case 0: case 32: E(fmt("c.fcmp(%u, ", d >> 2) + A0 + ", " + B0 + ");"); return;   // ps_cmpu0 / ps_cmpo0
    case 64: case 96: E(fmt("c.fcmp(%u, ", d >> 2) + A1 + ", " + B1 + ");"); return;  // ps_cmpu1 / ps_cmpo1
    case 40: pair("dbl(bits64(" + B0 + ") ^ 0x8000000000000000ull)", "dbl(bits64(" + B1 + ") ^ 0x8000000000000000ull)", false); return;
    case 72: pair(B0, B1, false); return;  // ps_mr
    case 136: pair("dbl(bits64(" + B0 + ") | 0x8000000000000000ull)", "dbl(bits64(" + B1 + ") | 0x8000000000000000ull)", false); return;
    case 264: pair("dbl(bits64(" + B0 + ") & 0x7FFFFFFFFFFFFFFFull)", "dbl(bits64(" + B1 + ") & 0x7FFFFFFFFFFFFFFFull)", false); return;
    case 528: pair(A0, B0, false); return;  // ps_merge00
    case 560: pair(A0, B1, false); return;  // ps_merge01
    case 592: pair(A1, B0, false); return;  // ps_merge10
    case 624: pair(A1, B1, false); return;  // ps_merge11
    case 1014: E("c.dcbz(" + eaX(a, b) + ");"); return;  // dcbz_l
    default: break;
    }
    if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {  // psq_lx / psq_stx / psq_lux / psq_stux
        const bool wbit = bits(w, 21, 21);
        const u32 i = bits(w, 22, 24);
        const char* fn = (xo6 & 1) ? "psqStore" : "psqLoad";
        if (xo6 >= 38) E(fmt("{ const u32 ea = c.r[%u] + c.r[%u]; c.%s(%u, ea, %s, %u); c.r[%u] = ea; }", a, b, fn, d, wbit ? "true" : "false", i, a));
        else E(fmt("c.%s(%u, %s, %s, %u);", fn, d, eaX(a, b).c_str(), wbit ? "true" : "false", i));
        return;
    }
    auto fma = [](const std::string& x, const std::string& y, const std::string& z) { return "__builtin_fma(" + x + ", " + y + ", " + z + ")"; };
    switch (xo5) {
    case 10: pair(A0 + " + " + B1, C1); return;  // ps_sum0
    case 11: pair(C0, A0 + " + " + B1); return;  // ps_sum1
    case 12: pair(A0 + " * " + C0, A1 + " * " + C0); return;  // ps_muls0
    case 13: pair(A0 + " * " + C1, A1 + " * " + C1); return;  // ps_muls1
    case 14: pair(fma(A0, C0, B0), fma(A1, C0, B1)); return;  // ps_madds0
    case 15: pair(fma(A0, C1, B0), fma(A1, C1, B1)); return;  // ps_madds1
    case 18: pair(A0 + " / " + B0, A1 + " / " + B1); return;  // ps_div
    case 20: pair(A0 + " - " + B0, A1 + " - " + B1); return;  // ps_sub
    case 21: pair(A0 + " + " + B0, A1 + " + " + B1); return;  // ps_add
    case 23: pair("c.fsel(" + A0 + ", " + C0 + ", " + B0 + ")", "c.fsel(" + A1 + ", " + C1 + ", " + B1 + ")", false); return;  // ps_sel
    case 24: pair("c.fres(" + B0 + ")", "c.fres(" + B1 + ")"); return;  // ps_res
    case 25: pair(A0 + " * " + C0, A1 + " * " + C1); return;  // ps_mul
    case 26: pair("c.frsqrte(" + B0 + ")", "c.frsqrte(" + B1 + ")"); return;  // ps_rsqrte
    case 28: pair(fma(A0, C0, "-" + B0), fma(A1, C1, "-" + B1)); return;  // ps_msub
    case 29: pair(fma(A0, C0, B0), fma(A1, C1, B1)); return;  // ps_madd
    case 30: pair("-" + fma(A0, C0, "-" + B0), "-" + fma(A1, C1, "-" + B1)); return;  // ps_nmsub
    case 31: pair("-" + fma(A0, C0, B0), "-" + fma(A1, C1, B1)); return;  // ps_nmadd
    default: throw Unsupported{fmt("op4 xo%u", xo10)};
    }
}

std::string funcDecl(const Module& m, u32 a) { return "void " + funcSymbol(m, a) + "(Ctx& c)"; }

std::string emitFunction(const Program& p, const ModuleInfo& mi, const Func& f, EmitStats& st, int hookSlot) {
    std::string out;
    out += "// " + (f.name.empty() ? std::string() : f.name + " ") +
           fmt("%s 0x%08X-0x%08X%s\n", mi.mod->name.c_str(), f.start, f.end, f.alias ? " (alias)" : "");
    st.functions++;
    const std::string sym = funcSymbol(*mi.mod, f.start);
    out += funcDecl(*mi.mod, f.start) + " {\n";
    if (mi.mod->isDol()) out += fmt("    c.enter(0x%Xu);\n", f.start);
    if (hookSlot >= 0) {
        // Named functions check their hook slot first: the runtime fills it with a host replacement
        // (SDK functions replaced by name, enhancement hooks), else the translated body runs.
        st.hle++;
        out += fmt("    if (HleFn h = g_hooks[%d]) [[unlikely]] {\n        h(c);\n        return;\n    }\n", hookSlot);
        out += "    " + sym + "_orig(c);\n}\n";
        out += "void " + sym + "_orig(Ctx& c) {\n";
    }
    out += FuncEmitter(p, mi, f, st).run();
    out += "}\n";
    return out;
}

const char* kPrelude =
    "// Generated from the user's own disc by the SA2B recompiler. Never commit.\n"
    "#include \"runtime/ctx.h\"\n"
    "namespace rt {\nextern HleFn g_hooks[];\n}\n";

}  // namespace

std::vector<OutputFile> emitProgram(const Program& p, EmitStats& st, int dolChunks, size_t relChunkInsns) {
    std::vector<OutputFile> files;
    const ModuleInfo& dol = p.info[0];
    std::map<u32, int> hookSlot;
    for (auto& [a, f] : dol.funcs)
        if (!f.name.empty()) hookSlot.emplace(a, int(hookSlot.size()));
    auto slotOf = [&](const ModuleInfo& mi, u32 a) {
        if (!mi.mod->isDol()) return -1;
        auto it = hookSlot.find(a);
        return it == hookSlot.end() ? -1 : it->second;
    };
    // Declarations of every main.dol function (REL code calls them directly).
    {
        std::string h = std::string("#pragma once\n") + kPrelude + "namespace rt {\n";
        for (auto& [a, f] : dol.funcs) {
            h += funcDecl(*dol.mod, a) + ";\n";
            if (hookSlot.count(a)) h += "void " + funcSymbol(*dol.mod, a) + "_orig(Ctx& c);\n";
        }
        h += "}\n";
        files.push_back({"dol_funcs.h", std::move(h)});
    }
    // main.dol in chunks of roughly equal size.
    {
        u64 total = 0;
        for (auto& [a, f] : dol.funcs) total += (f.end - f.start) / 4 + 8;
        auto it = dol.funcs.begin();
        u64 done = 0;
        for (int ch = 0; ch < dolChunks; ch++) {
            const u64 goal = total * u64(ch + 1) / u64(dolChunks);
            std::string t = std::string(kPrelude) + "#include \"dol_funcs.h\"\nnamespace rt {\n";
            while (it != dol.funcs.end() && (done < goal || ch == dolChunks - 1)) {
                done += (it->second.end - it->second.start) / 4 + 8;
                t += emitFunction(p, dol, it->second, st, slotOf(dol, it->first));
                ++it;
            }
            t += "}\n";
            files.push_back({fmt("dol_%02d.cpp", ch), std::move(t)});
        }
        std::string t = std::string(kPrelude) + "#include \"dol_funcs.h\"\n#include \"runtime/modules.h\"\nnamespace rt {\n"
                        "extern const FuncEntry kDolFuncs[] = {\n";
        for (auto& [a, f] : dol.funcs) t += fmt("    {0x%08Xu, &", a) + funcSymbol(*dol.mod, a) + "},\n";
        t += "};\n";
        t += fmt("extern const u32 kDolFuncCount = %zuu;\n", dol.funcs.size());
        t += "extern const NameEntry kDolNames[] = {\n";
        size_t named = 0;
        for (auto& [a, f] : dol.funcs)
            if (!f.name.empty()) t += fmt("    {0x%08Xu, \"", a) + f.name + "\"},\n", named++;
        if (!named) t += "    {0, nullptr},\n";
        t += "};\n";
        t += fmt("extern const u32 kDolNameCount = %zuu;\n", named);
        // Hook slots in slot order: name, address and the translated body (for calling the original).
        std::vector<std::pair<int, u32>> bySlot;
        for (auto& [a, sl] : hookSlot) bySlot.push_back({sl, a});
        std::sort(bySlot.begin(), bySlot.end());
        t += "extern const HookEntry kHookTable[] = {\n";
        for (auto& [sl, a] : bySlot)
            t += fmt("    {\"%s\", 0x%08Xu, &%s_orig},\n", dol.funcs.at(a).name.c_str(), a, funcSymbol(*dol.mod, a).c_str());
        if (bySlot.empty()) t += "    {nullptr, 0, nullptr},\n";
        t += "};\n";
        t += fmt("extern const u32 kHookCount = %zuu;\nHleFn g_hooks[%zu];\n}\n", bySlot.size(),
                 bySlot.empty() ? size_t(1) : bySlot.size());
        files.push_back({"dol_table.cpp", std::move(t)});
    }
    // Each REL: its declarations, its code in chunks, and its function table.
    std::string index = std::string(kPrelude) + "#include \"runtime/modules.h\"\nnamespace rt {\n";
    std::string indexList = "extern const RelModuleCode kRelModules[] = {\n";
    size_t nrel = 0;
    for (size_t i = 1; i < p.info.size(); i++) {
        const ModuleInfo& mi = p.info[i];
        const Module& m = *mi.mod;
        const std::string base = fmt("rel_%s", m.name.substr(0, m.name.find('.')).c_str());
        std::string h = std::string("#pragma once\n") + kPrelude + "#include \"dol_funcs.h\"\nnamespace rt {\n";
        for (auto& [a, f] : mi.funcs) h += funcDecl(m, a) + ";\n";
        h += "}\n";
        files.push_back({base + ".h", std::move(h)});
        auto it = mi.funcs.begin();
        int part = 0;
        while (it != mi.funcs.end()) {
            std::string t = std::string(kPrelude) + "#include \"" + base + ".h\"\nnamespace rt {\n";
            size_t n = 0;
            while (it != mi.funcs.end() && (n < relChunkInsns || n == 0)) {
                n += (it->second.end - it->second.start) / 4;
                t += emitFunction(p, mi, it->second, st, -1);
                ++it;
            }
            t += "}\n";
            files.push_back({fmt("%s_%02d.cpp", base.c_str(), part++), std::move(t)});
        }
        std::string t = std::string(kPrelude) + "#include \"" + base + ".h\"\n#include \"runtime/modules.h\"\nnamespace rt {\n";
        t += fmt("extern const FuncEntry k%sFuncs[] = {\n", base.c_str());
        for (auto& [a, f] : mi.funcs) t += fmt("    {0x%08Xu, &", a) + funcSymbol(m, a) + "},\n";
        t += "};\n}\n";
        files.push_back({base + "_table.cpp", std::move(t)});
        index += fmt("extern const FuncEntry k%sFuncs[];\n", base.c_str());
        indexList += fmt("    {%uu, \"%s\", k%sFuncs, %zuu},\n", m.id, m.name.c_str(), base.c_str(), mi.funcs.size());
        nrel++;
    }
    indexList += "};\n";
    index += indexList + fmt("extern const u32 kRelModuleCount = %zuu;\n}\n", nrel);
    files.push_back({"rel_index.cpp", std::move(index)});
    return files;
}

}  // namespace sa2rc
