/* Mods: loose files that stand in for entries of the game's archives (vfs.c),
 * and SoraVoice's voices (voice.c). */
#ifndef FC_MODS_H
#define FC_MODS_H

#include <stdint.h>

/* The game's own folder: SoraVoice's scripts (voice\scena), which come before
 * any mod folder's. Before the game runs, and before mods_scan. */
void mods_scan_game(const char* game_dir);

/* Read every mod folder under mods_dir, in name order. Before the game runs. */
void mods_scan(const char* mods_dir);

/* How many mod folders were found. */
int mods_count(void);

/* Voices. If the game's folder, which is the working directory by now, has
 * SoraVoice's voice\ogg, its files are listed and the lines the scripts tag
 * are spoken. `log` says each tag and its file as they come. */
void fc_voice_init(int log);

/* What voice.c needs of its host.
 *
 * fc_voice_decode: an Ogg Vorbis file as 16-bit samples, malloc'd, or NULL.
 * It may be running on two threads at once.
 *
 * fc_voice_out_play takes the samples (they are the host's to free) and
 * starts them; it returns a number for the clip, 0 if it could not.
 * fc_voice_out_stop and fc_voice_out_playing take that number, and 0 is no
 * clip. These three are called one at a time. */
int16_t* fc_voice_decode(const char* path, uint32_t* frames, int* rate, int* channels);
unsigned fc_voice_out_play(int16_t* pcm, uint32_t frames, int rate, int channels);
void fc_voice_out_stop(unsigned clip);
int fc_voice_out_playing(unsigned clip);

#endif
