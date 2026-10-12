/*
 * What the voices (src/mods/voice.c) need of this host: win32hle's reader of
 * Ogg Vorbis files and a clip in its mixer, beside the game's own sounds.
 */
#include "win32hle.h"
#include "mods.h"

int16_t* fc_voice_decode(const char* path, uint32_t* frames, int* rate, int* channels) {
    return hle_vorbis_decode_file(path, frames, rate, channels);
}
unsigned fc_voice_out_play(int16_t* pcm, uint32_t frames, int rate, int channels) {
    return hle_audio_clip_play(pcm, frames, rate, channels, 1.0f);
}
void fc_voice_out_stop(unsigned clip) { hle_audio_clip_stop(clip); }
int fc_voice_out_playing(unsigned clip) { return hle_audio_clip_playing(clip); }
