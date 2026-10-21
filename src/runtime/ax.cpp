// AX microcode stand-in: processes the command lists the AX library sends each audio frame.
// Mixing is not implemented yet: the output buffers stay silent.
#include "runtime/runtime.h"

namespace rt {

void axCommandList(u32 addr, u32 size) {
    static u32 s_lists = 0;
    if (++s_lists == 1) log("[ax] first command list at %08X (%u words)\n", addr, size);
}

}  // namespace rt
