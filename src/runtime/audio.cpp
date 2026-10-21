// Host audio: every block the AI DMA "plays" (32 kHz, 16-bit big-endian right/left pairs, mixed
// by src/runtime/ax.cpp) goes to an SDL audio stream. Silent with --headless, --mute or SA2_MUTE.
#include "runtime/runtime.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace rt {

namespace {
SDL_AudioStream* s_stream = nullptr;
}

void audioInit(bool enabled) {
    if (!enabled) {
        log("[audio] muted\n");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        log("[audio] no audio: %s\n", SDL_GetError());
        return;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s_stream) {
        log("[audio] no audio device: %s\n", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(s_stream);
    log("[audio] 32 kHz stereo output\n");
}

void audioPushAi(u32 addr, u32 bytes) {
    // SA2_AUDIO_DUMP=<file>: also write everything played as raw 32 kHz 16-bit stereo (even muted).
    static std::FILE* s_dump = std::getenv("SA2_AUDIO_DUMP") ? std::fopen(std::getenv("SA2_AUDIO_DUMP"), "wb") : nullptr;
    if (!s_stream && !s_dump) return;
    static std::vector<s16> buf;
    const u32 frames = bytes / 4;
    buf.resize(frames * 2);
    for (u32 i = 0; i < frames; i++) {
        buf[i * 2] = s16(rd16(addr + i * 4 + 2));  // left
        buf[i * 2 + 1] = s16(rd16(addr + i * 4));  // right
    }
    if (s_dump) std::fwrite(buf.data(), 4, frames, s_dump);
    // The AI is paced by the host clock and the device by its own: keep at most ~100 ms queued.
    if (s_stream && SDL_GetAudioStreamQueued(s_stream) <= 32000 * 4 / 10)
        SDL_PutAudioStreamData(s_stream, buf.data(), int(frames * 4));
}

}  // namespace rt
