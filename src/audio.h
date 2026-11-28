#pragma once
#include <string>
#include <SDL_mixer.h>

class Audio {
public:
    static bool init();
    static void shutdown();

    // Play VOC / WAV audio file on a channel
    static Mix_Chunk* play_voc(const std::string& filepath, int volume = MIX_MAX_VOLUME);
    // sub_5CB97: start a sample that was loaded earlier (4F460 slot) without cutting the
    // samples already playing. Chunks are cached by path, so repeated 4F4CD calls are cheap.
    static bool play_cached(const std::string& filepath, int volume = MIX_MAX_VOLUME);
    // sub_5CA62: decode the sample when 4F460 loads it, so the first 4F4CD play has no load
    // latency (the original keeps the PCM in dword_1124B0[slot]).
    static bool preload_cached(const std::string& filepath);
    // sub_5CC20(0Ah, 0, 3E7h): start the sample 4F3BD loaded into slot 0 on channel 0. The
    // 3E7h repeat count is never decremented by the mixer, so it loops until stopped.
    static bool play_cached_loop_channel0(const std::string& filepath, int volume = MIX_MAX_VOLUME);
    static void stop_channel0();       // sub_5CD0D(0): stop the slot-0 sample
    static void stop_sfx_channels();   // sub_5CD0D(2..7): stop every SFX channel
    static void stop();
    static void free_chunk(Mix_Chunk* chunk);

private:
    static bool initialized_;
    static Mix_Chunk* current_chunk_;
    static int current_channel_;
};
