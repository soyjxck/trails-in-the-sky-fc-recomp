/*
 * Voices: SoraVoice's, played by the port itself.
 *
 * SoraVoice adds spoken lines to the game: its scripts (voice\scena, taken
 * in by vfs.c) carry a tag in the dialogue text where a line is voiced, `#`
 * then one to ten digits then a lowercase `v`, and its voice files are in
 * voice\ogg. With the original game a DLL it injects watches the game read
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
 *
 * The same file serves the Windows host and the portable one. What differs
 * between them, reading an Ogg Vorbis file and playing its samples, each
 * gives in a file of its own (mods.h says what).
 */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#endif

#include "recomp_types.h"
#include "mods.h"

#define VOICE_DIR "voice/ogg"         /* under the game's folder, which is the working directory */

/* A lock and a thread, of whichever system this is. */
#ifdef _WIN32
static CRITICAL_SECTION g_lock;
#define LOCK()   EnterCriticalSection(&g_lock)
#define UNLOCK() LeaveCriticalSection(&g_lock)
#define THREAD(name) static DWORD WINAPI name(LPVOID arg)
#define THREAD_DONE  return 0
static int start(LPTHREAD_START_ROUTINE f, void* arg) {
    HANDLE t = CreateThread(NULL, 0, f, arg, 0, NULL);
    if (!t) return 0;
    CloseHandle(t);
    return 1;
}
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#define THREAD(name) static void* name(void* arg)
#define THREAD_DONE  return NULL
void hle_thread_aside(void);           /* win32hle.h */
typedef struct { void* (*f)(void*); void* arg; } started;
static void* beside(void* p) {         /* not in turns with the game's own thread, where that is a choice */
    started s = *(started*)p;
    free(p);
    hle_thread_aside();
    return s.f(s.arg);
}
static int start(void* (*f)(void*), void* arg) {
    pthread_t t;
    started* s = malloc(sizeof *s);
    if (!s) return 0;
    s->f = f, s->arg = arg;
    if (pthread_create(&t, NULL, beside, s)) { free(s); return 0; }
    pthread_detach(t);
    return 1;
}
#endif

static char (*g_names)[11];            /* the ten digits of each ch file, in order */
static int g_count, g_room;
static volatile int g_listed;
static unsigned g_clip;                /* the voice playing, as the host knows it */
static volatile unsigned g_turn;       /* goes up with every new voice and every stop: a decode that is late gives way */
static volatile int g_pending;         /* a voice is being decoded */
static uint32_t g_last_at;             /* the tag last answered, so that reading it twice plays it once */
static char g_last_digits[12];
static int g_on, g_log;

static int by_name(const void* a, const void* b) { return memcmp(a, b, 10); }
static void add_name(const char* n) {  /* ch<ten digits>.ogg, and nothing else */
    int ok = (n[0] == 'c' || n[0] == 'C') && (n[1] == 'h' || n[1] == 'H') && strlen(n) == 16;
    for (int i = 2; ok && i < 12; i++) ok = isdigit((unsigned char)n[i]);
    if (!ok) return;
    if (g_count == g_room) g_names = realloc(g_names, (size_t)(g_room = g_room ? g_room * 2 : 40000) * sizeof *g_names);
    memcpy(g_names[g_count], n + 2, 10), g_names[g_count][10] = 0, g_count++;
}
static int voice_folder(void) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(VOICE_DIR);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    DIR* d = opendir(VOICE_DIR);
    if (d) closedir(d);
    return d != NULL;
#endif
}
THREAD(list_voices) {
    (void)arg;
#ifdef _WIN32
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(VOICE_DIR "/ch*.ogg", &found);
    if (h != INVALID_HANDLE_VALUE) {
        do add_name(found.cFileName); while (FindNextFileA(h, &found));
        FindClose(h);
    }
#else
    DIR* d = opendir(VOICE_DIR);
    struct dirent* e;
    while (d && (e = readdir(d)) != NULL) add_name(e->d_name);
    if (d) closedir(d);
#endif
    qsort(g_names, (size_t)g_count, sizeof *g_names, by_name);
    fprintf(stderr, "[voice] %d voice files in %s\n", g_count, VOICE_DIR);
    g_listed = 1;
    THREAD_DONE;
}

/* Is there a voice folder? Then the files are listed, on a thread of their
 * own: there are tens of thousands, and no line is spoken in the first seconds. */
void fc_voice_init(int log) {
    if (g_on || !voice_folder()) return;
#ifdef _WIN32
    InitializeCriticalSection(&g_lock);
#endif
    g_on = 1, g_log = log;
    start(list_voices, NULL);
}

typedef struct { unsigned turn; char path[64]; } job;
THREAD(play) {
    job* j = arg;
    uint32_t frames = 0;
    int rate = 0, channels = 0;
    int16_t* pcm = fc_voice_decode(j->path, &frames, &rate, &channels);
    LOCK();
    if (pcm && j->turn == g_turn) {
        fc_voice_out_stop(g_clip);
        g_clip = fc_voice_out_play(pcm, frames, rate, channels);
        if (g_log) fprintf(stderr, "[voice] %s: %.2f s, %d Hz, %d channel%s%s\n", j->path, rate ? (double)frames / rate : 0.0,
                           rate, channels, channels == 1 ? "" : "s", g_clip ? "" : ", and nothing to play it on");
        pcm = NULL;                                   /* the host's now */
    } else if (!pcm && g_log) fprintf(stderr, "[voice] %s could not be read\n", j->path);
    if (j->turn == g_turn) g_pending = 0;
    UNLOCK();
    free(pcm);
    free(j);
    THREAD_DONE;
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
    LOCK();
    j->turn = ++g_turn;
    g_pending = 1;
    UNLOCK();
    if (!start(play, j)) free(j), g_pending = 0;
}

int fc_voice_playing(void) {
    if (!g_on) return 0;
    LOCK();
    int playing = g_pending || fc_voice_out_playing(g_clip);
    UNLOCK();
    return playing;
}

void fc_voice_page(void) {
    if (!g_on) return;
    LOCK();
    g_turn++, g_pending = 0;
    fc_voice_out_stop(g_clip), g_clip = 0;
    g_last_at = 0, g_last_digits[0] = 0;
    UNLOCK();
}
