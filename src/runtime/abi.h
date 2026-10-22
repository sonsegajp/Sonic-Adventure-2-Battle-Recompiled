#pragma once
// The game module's interface version (src/runtime/modules.h: GameModule). It changes whenever
// GameModule or anything in ctx.h the module relies on changes: the runtime refuses a module built
// for another, and the launcher's setup stamp includes it, so the module is built again.
#define SA2B_GAME_ABI 1
