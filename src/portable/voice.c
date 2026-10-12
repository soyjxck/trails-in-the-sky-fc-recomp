/*
 * Voices: SoraVoice's, played by the port itself.
 *
 * SoraVoice adds spoken lines to the game: its scripts (voice\scena, taken
 * in by src/mods/vfs.c) carry a tag in the dialogue text where a line is
 * voiced, `#` then one to ten digits then a lowercase `v`, and its voice
 * files are in voice\ogg. On Windows a DLL it injects watches the game read
 * the text. Here the lift itself calls out at the same three places
 * (PATCHES in run_lift.py):
 *
 *   fc_voice_tag      the text reader has come to a `#` in a window's text
 *   fc_voice_playing  it is about to play the typing beep: not over a voice
 *   fc_voice_page     the player has turned the page: the voice stops
 *
 * The game itself draws nothing for the tag and needs no telling.
 *
 * Which file a tag means: six digits or more are the file's own name, ten as
 * ch<digits>.ogg and six as ed<the last four>.ogg. Five or fewer are a number
 * into the list of the ch files in name order (four, three, two and one
 * digits start 100,000, 110,000, 111,000 and 111,100 further on). One voice
 * plays at a time; a new one takes the place of the last.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"

#define VOICE_DIR "voice/ogg"         /* under the game's folder, which is the working directory */

static char (*g_names)[11];            /* the ten digits of each ch file, in order */
static int g_count;
static volatile int g_listed;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_clip;                /* the voice playing, as the mixer knows it */
static volatile unsigned g_turn;       /* goes up with every new voice and every stop: a decode that is late gives way */
static volatile int g_pending;         /* a voice is being decoded */
static uint32_t g_last_at;             /* the tag last answered, so that reading it twice plays it once */
static char g_last_digits[12];
static int g_on, g_log;

static int by_name(const void* a, const void* b) { return memcmp(a, b, 10); }
static void* list_voices(void* arg) {
    DIR* d = opendir(VOICE_DIR);
    struct dirent* e;
    int cap = 0;
    (void)arg;
    while (d && (e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        int ok = (n[0] == 'c' || n[0] == 'C') && (n[1] == 'h' || n[1] == 'H') && strlen(n) == 16;
        for (int i = 2; ok && i < 12; i++) ok = isdigit((unsigned char)n[i]);
        if (!ok) continue;
        if (g_count == cap) g_names = realloc(g_names, (size_t)(cap = cap ? cap * 2 : 40000) * sizeof *g_names);
        memcpy(g_names[g_count], n + 2, 10), g_names[g_count][10] = 0, g_count++;
    }
    if (d) closedir(d);
    qsort(g_names, (size_t)g_count, sizeof *g_names, by_name);
    fprintf(stderr, "[voice] %d voice files in %s\n", g_count, VOICE_DIR);
    g_listed = 1;
    return NULL;
}

/* Is there a voice folder? Then the files are listed, on a thread of their
 * own: there are tens of thousands, and no line is spoken in the first seconds. */
void fc_voice_init(int log) {
    DIR* d = opendir(VOICE_DIR);
    pthread_t t;
    if (!d) return;
    closedir(d);
    g_on = 1, g_log = log;
    pthread_create(&t, NULL, list_voices, NULL);
    pthread_detach(t);
}

typedef struct { unsigned turn; char path[64]; } job;
static void* play(void* arg) {
    job* j = arg;
    uint32_t frames = 0;
    int rate = 0, channels = 0;
    int16_t* pcm = hle_vorbis_decode_file(j->path, &frames, &rate, &channels);
    pthread_mutex_lock(&g_lock);
    if (pcm && j->turn == g_turn) {
        hle_audio_clip_stop(g_clip);
        g_clip = hle_audio_clip_play(pcm, frames, rate, channels, 1.0f);
        pcm = NULL;                                   /* the mixer's now */
    } else if (!pcm && g_log) fprintf(stderr, "[voice] %s could not be read\n", j->path);
    if (j->turn == g_turn) g_pending = 0;
    pthread_mutex_unlock(&g_lock);
    free(pcm);
    free(j);
    return NULL;
}

void fc_voice_tag(uint32_t at) {
    char digits[12];
    int n = 0;
    if (!g_on) return;
    while (n < 10 && isdigit(MEM8(at + 1 + (uint32_t)n))) digits[n] = (char)MEM8(at + 1 + (uint32_t)n), n++;
    digits[n] = 0;
    if (!n || MEM8(at + 1 + (uint32_t)n) != 'v') return;
    if (at == g_last_at && !strcmp(digits, g_last_digits)) return;      /* the same tag, read again */
    g_last_at = at, strcpy(g_last_digits, digits);

    job* j = calloc(1, sizeof *j);
    if (n == 10) snprintf(j->path, sizeof j->path, VOICE_DIR "/ch%s.ogg", digits);
    else if (n >= 6) snprintf(j->path, sizeof j->path, VOICE_DIR "/ed%s.ogg", digits + n - 4);
    else {
        static const int from[6] = { 0, 111100, 111000, 110000, 100000, 0 };
        int index = atoi(digits) + from[n];
        if (!g_listed || index >= g_count) {
            if (g_log) fprintf(stderr, "[voice] #%sv: no file number %d (%d listed)\n", digits, index, g_count);
            free(j);
            return;
        }
        snprintf(j->path, sizeof j->path, VOICE_DIR "/ch%s.ogg", g_names[index]);
    }
    if (g_log) fprintf(stderr, "[voice] #%sv -> %s\n", digits, j->path);
    pthread_mutex_lock(&g_lock);
    j->turn = ++g_turn;
    g_pending = 1;
    pthread_mutex_unlock(&g_lock);
    pthread_t t;
    if (pthread_create(&t, NULL, play, j) == 0) pthread_detach(t);
    else free(j), g_pending = 0;
}

int fc_voice_playing(void) { return g_on && (g_pending || hle_audio_clip_playing(g_clip)); }

void fc_voice_page(void) {
    if (!g_on) return;
    pthread_mutex_lock(&g_lock);
    g_turn++, g_pending = 0;
    hle_audio_clip_stop(g_clip), g_clip = 0;
    g_last_at = 0, g_last_digits[0] = 0;
    pthread_mutex_unlock(&g_lock);
}
