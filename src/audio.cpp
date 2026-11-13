#include "audio.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <SDL.h>

bool Audio::initialized_ = false;
Mix_Chunk* Audio::current_chunk_ = nullptr;
int Audio::current_channel_ = -1;

// dword_1124B0 slots: 4F460 loads a sample into a slot and 4F4CD starts it later. The port
// keeps the decoded chunks alive for the process lifetime; 5CD0D only disables a slot, and
// re-loading the same path is therefore free.
static std::map<std::string, Mix_Chunk*> g_chunk_cache;

bool Audio::init() {
    if (initialized_) return true;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            fprintf(stderr, "AUDIO: warning: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
            return false;
        }
    }

    // 22050Hz, 16-bit Stereo, 2048 buffer
    if (Mix_OpenAudio(22050, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        fprintf(stderr, "AUDIO: warning: Mix_OpenAudio failed: %s\n", Mix_GetError());
        return false;
    }

    // The original mixes up to 8 sample slots at once (dword_11273C table).
    Mix_AllocateChannels(16);
    initialized_ = true;
    return true;
}

void Audio::shutdown() {
    if (!initialized_) return;
    stop();
    for (auto& kv : g_chunk_cache) {
        if (kv.second) Mix_FreeChunk(kv.second);
    }
    g_chunk_cache.clear();
    Mix_CloseAudio();
    initialized_ = false;
}

// ---- Convert raw 8-bit unsigned PCM (22050 Hz mono) to a Mix_Chunk (TTL.LST method) ----
static Mix_Chunk* load_raw_pcm_as_chunk(const uint8_t* pcm_data, size_t pcm_len,
                                        int src_rate = 22050, int src_channels = 1) {
    int mix_freq = 0;
    Uint16 mix_format = 0;
    int mix_channels = 0;

    if (!Mix_QuerySpec(&mix_freq, &mix_format, &mix_channels)) {
        fprintf(stderr, "AUDIO: warning: Mix_QuerySpec failed: %s\n", Mix_GetError());
        return nullptr;
    }

    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, AUDIO_U8, src_channels, src_rate,
                          mix_format, mix_channels, mix_freq) < 0) {
        fprintf(stderr, "AUDIO: warning: SDL_BuildAudioCVT failed: %s\n", SDL_GetError());
        return nullptr;
    }

    cvt.len = (int)pcm_len;
    cvt.buf = (Uint8*)SDL_malloc(cvt.len * cvt.len_mult);
    if (!cvt.buf) return nullptr;

    memcpy(cvt.buf, pcm_data, pcm_len);

    if (SDL_ConvertAudio(&cvt) < 0) {
        fprintf(stderr, "AUDIO: warning: SDL_ConvertAudio failed: %s\n", SDL_GetError());
        SDL_free(cvt.buf);
        return nullptr;
    }

    Mix_Chunk* chunk = (Mix_Chunk*)SDL_malloc(sizeof(Mix_Chunk));
    if (!chunk) {
        SDL_free(cvt.buf);
        return nullptr;
    }

    chunk->allocated = 1;
    chunk->abuf = cvt.buf;
    chunk->alen = cvt.len_cvt;
    chunk->volume = MIX_MAX_VOLUME;

    return chunk;
}

// Decode a MAGICSPR/town sample. Three containers occur: RIFF, Creative VOC and headerless
// raw PCM behind a 0x40-byte header.
static Mix_Chunk* load_chunk_from_file(const std::string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "AUDIO: warning: Cannot open file '%s'\n", filepath.c_str());
        return nullptr;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::vector<uint8_t> buffer((size_t)(file_size > 0 ? file_size : 0));
    if (file_size <= 0 || fread(buffer.data(), 1, (size_t)file_size, fp) != (size_t)file_size) {
        fclose(fp);
        return nullptr;
    }
    fclose(fp);

    Mix_Chunk* chunk = nullptr;
    if (file_size >= 4 && memcmp(buffer.data(), "RIFF", 4) == 0) {
        SDL_RWops* rw = SDL_RWFromMem(buffer.data(), (int)file_size);
        chunk = Mix_LoadWAV_RW(rw, 1);
    } else if (file_size >= 26 && memcmp(buffer.data(), "Creative Voice File", 19) == 0) {
        uint16_t header_size = (uint16_t)buffer[0x14] | ((uint16_t)buffer[0x15] << 8);
        const uint8_t* pcm_data = buffer.data() + header_size;
        size_t pcm_len = (size_t)file_size - header_size;
        if (pcm_len > 6 && pcm_data[0] == 1) {
            uint8_t tc = pcm_data[4];
            int rate = (256 - tc > 0) ? (1000000 / (256 - tc)) : 22050;
            chunk = load_raw_pcm_as_chunk(pcm_data + 6, pcm_len - 6, rate, 1);
        } else {
            chunk = load_raw_pcm_as_chunk(pcm_data, pcm_len, 22050, 1);
        }
    } else {
        fprintf(stderr, "AUDIO: Loading '%s' as Raw 8-Bit PCM (22050Hz Mono, %ld bytes)\n",
                filepath.c_str(), file_size);
        chunk = load_raw_pcm_as_chunk(buffer.data(), (size_t)file_size, 22050, 1);
    }

    if (!chunk) fprintf(stderr, "AUDIO: warning: Failed to process sound '%s'\n", filepath.c_str());
    return chunk;
}

Mix_Chunk* Audio::play_voc(const std::string& filepath, int volume) {
    if (!initialized_) {
        if (!init()) return nullptr;
    }

    stop();

    Mix_Chunk* chunk = load_chunk_from_file(filepath);
    if (!chunk) return nullptr;

    Mix_VolumeChunk(chunk, volume);
    current_channel_ = Mix_PlayChannel(-1, chunk, 0);
    current_chunk_ = chunk;

    return chunk;
}

bool Audio::play_cached(const std::string& filepath, int volume) {
    if (!initialized_) {
        if (!init()) return false;
    }

    Mix_Chunk* chunk = nullptr;
    auto it = g_chunk_cache.find(filepath);
    if (it != g_chunk_cache.end()) {
        chunk = it->second;
    } else {
        chunk = load_chunk_from_file(filepath);
        if (!chunk) return false;
        g_chunk_cache[filepath] = chunk;
    }

    Mix_VolumeChunk(chunk, volume);
    return Mix_PlayChannel(-1, chunk, 0) >= 0;
}

void Audio::stop() {
    if (current_channel_ != -1) {
        Mix_HaltChannel(current_channel_);
        current_channel_ = -1;
    }
    if (current_chunk_) {
        Mix_FreeChunk(current_chunk_);
        current_chunk_ = nullptr;
    }
}

void Audio::free_chunk(Mix_Chunk* chunk) {
    if (chunk) {
        Mix_FreeChunk(chunk);
    }
}
