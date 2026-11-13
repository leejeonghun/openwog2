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
    static void stop();
    static void free_chunk(Mix_Chunk* chunk);

private:
    static bool initialized_;
    static Mix_Chunk* current_chunk_;
    static int current_channel_;
};
