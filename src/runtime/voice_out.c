/*
 * What the voices (src/mods/voice.c) need of the Windows host: an Ogg Vorbis
 * file read into samples, and samples played.
 *
 * Reading is the game's own libvorbisfile, dll\vorbisfile.dll in its folder,
 * the one its music is decoded with. It is a 32-bit DLL in a 32-bit process:
 * its functions are called as they are. The file is read into memory first
 * and handed over through callbacks, because a FILE of this C runtime means
 * nothing to the DLL's.
 *
 * Playing is DirectSound, an object of the host's own beside the game's: one
 * buffer holds a whole line and is let go when the next one starts. Like the
 * game's sounds it is heard while the game's window is the active one, and
 * --silent, which mutes the process's audio session, mutes it too.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mods.h"

HWND fc_host_window(void);            /* host.c: the game's top-level window */

/* ---- reading ---------------------------------------------------------------- */
typedef struct {
    size_t (__cdecl* read)(void* to, size_t size, size_t count, void* source);
    int    (__cdecl* seek)(void* source, __int64 offset, int whence);
    int    (__cdecl* close)(void* source);
    long   (__cdecl* tell)(void* source);
} ov_callbacks;
typedef struct { int version, channels; long rate; } vorbis_info_head;   /* how a vorbis_info starts */
#define OV_FILE_ROOM 4096             /* an OggVorbis_File is some 720 bytes in a 32-bit build */
#define OV_HOLE (-3)                  /* a gap in the data: the read goes on after it */

static int (__cdecl* ov_open_callbacks)(void* source, void* vf, const char* initial, long ibytes, ov_callbacks callbacks);
static vorbis_info_head* (__cdecl* ov_info)(void* vf, int link);
static __int64 (__cdecl* ov_pcm_total)(void* vf, int link);
static long (__cdecl* ov_read)(void* vf, char* buffer, int length, int bigendian, int word, int sgned, int* bitstream);
static int (__cdecl* ov_clear)(void* vf);

static int vorbis_ready(void) {
    static volatile LONG state;       /* 0 not tried, 1 there, -1 not there */
    if (state) return state > 0;
    char path[MAX_PATH];
    /* by its full path, and its ogg.dll and vorbis.dll from beside it */
    GetFullPathNameA("dll\\vorbisfile.dll", MAX_PATH, path, NULL);
    HMODULE m = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m) {
        ov_open_callbacks = (void*)GetProcAddress(m, "ov_open_callbacks");
        ov_info = (void*)GetProcAddress(m, "ov_info");
        ov_pcm_total = (void*)GetProcAddress(m, "ov_pcm_total");
        ov_read = (void*)GetProcAddress(m, "ov_read");
        ov_clear = (void*)GetProcAddress(m, "ov_clear");
    }
    int ok = m && ov_open_callbacks && ov_info && ov_pcm_total && ov_read && ov_clear;
    if (!ok) fprintf(stderr, "[voice] no %s (error %lu): the voices stay silent\n", path, m ? 0ul : GetLastError());
    state = ok ? 1 : -1;
    return ok;
}

typedef struct { const uint8_t* data; size_t size, at; } mem_file;
static size_t __cdecl mem_read(void* to, size_t size, size_t count, void* source) {
    mem_file* f = source;
    size_t n = size ? (f->size - f->at) / size : 0;
    if (n > count) n = count;
    memcpy(to, f->data + f->at, n * size);
    f->at += n * size;
    return n;
}
static int __cdecl mem_seek(void* source, __int64 offset, int whence) {
    mem_file* f = source;
    __int64 at = (whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (__int64)f->at : (__int64)f->size) + offset;
    if (at < 0 || at > (__int64)f->size) return -1;
    f->at = (size_t)at;
    return 0;
}
static long __cdecl mem_tell(void* source) { return (long)((mem_file*)source)->at; }
/* Nothing to close, but the game's libvorbisfile is an old one and calls this without looking. */
static int __cdecl mem_close(void* source) { (void)source; return 0; }

int16_t* fc_voice_decode(const char* path, uint32_t* frames, int* rate, int* channels) {
    static const ov_callbacks callbacks = { mem_read, mem_seek, mem_close, mem_tell };
    mem_file file = { 0 };
    uint8_t* pcm = NULL;
    void* vf = NULL;
    if (!vorbis_ready()) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = size > 0 ? malloc((size_t)size) : NULL;
    if (data && fread(data, 1, (size_t)size, f) == (size_t)size) file.data = data, file.size = (size_t)size;
    fclose(f);
    if (file.data && (vf = calloc(1, OV_FILE_ROOM)) != NULL && ov_open_callbacks(&file, vf, NULL, 0, callbacks) == 0) {
        const vorbis_info_head* info = ov_info(vf, -1);
        __int64 total = ov_pcm_total(vf, -1);
        if (info && info->channels >= 1 && info->channels <= 2 && info->rate > 0) {
            size_t unit = (size_t)info->channels * 2, got = 0;
            size_t room = (total > 0 ? (size_t)total : (size_t)info->rate * 8) * unit + 8192;
            pcm = malloc(room);
            while (pcm) {
                int section = 0;
                if (room - got < 4096) {              /* longer than the file said: more room */
                    uint8_t* more = realloc(pcm, room += room / 2);
                    if (!more) { free(pcm), pcm = NULL; break; }
                    pcm = more;
                }
                long n = ov_read(vf, (char*)pcm + got, (int)(room - got > 65536 ? 65536 : room - got), 0, 2, 1, &section);
                if (n == OV_HOLE) continue;
                if (n <= 0) break;
                got += (size_t)n;
            }
            if (pcm && got >= unit) *frames = (uint32_t)(got / unit), *rate = (int)info->rate, *channels = info->channels;
            else free(pcm), pcm = NULL;
        }
        ov_clear(vf);
    }
    free(vf);
    free(data);
    return (int16_t*)pcm;
}

/* ---- playing ---------------------------------------------------------------- */
static IDirectSound8* g_sound;
static IDirectSoundBuffer* g_buffer;  /* the line being spoken, or the last one */
static unsigned g_number;             /* its number; they only go up */

static void let_go(void) {
    if (!g_buffer) return;
    IDirectSoundBuffer_Stop(g_buffer);
    IDirectSoundBuffer_Release(g_buffer);
    g_buffer = NULL;
}

unsigned fc_voice_out_play(int16_t* pcm, uint32_t frames, int rate, int channels) {
    static int failed;
    WAVEFORMATEX format = { 0 };
    DSBUFFERDESC desc = { 0 };
    DWORD bytes = frames * (DWORD)channels * 2, n1 = 0, n2 = 0;
    void *p1 = NULL, *p2 = NULL;
    let_go();
    if (!g_sound && !failed) {
        HWND window = fc_host_window();
        HRESULT hr = DirectSoundCreate8(NULL, &g_sound, NULL);
        if (SUCCEEDED(hr)) hr = IDirectSound8_SetCooperativeLevel(g_sound, window ? window : GetDesktopWindow(), DSSCL_PRIORITY);
        if (FAILED(hr)) {
            fprintf(stderr, "[voice] no DirectSound (0x%08lX): the voices stay silent\n", (unsigned long)hr);
            if (g_sound) IDirectSound8_Release(g_sound), g_sound = NULL;
            failed = 1;
        }
    }
    if (g_sound && bytes) {
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = (WORD)channels;
        format.nSamplesPerSec = (DWORD)rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = (WORD)(channels * 2);
        format.nAvgBytesPerSec = (DWORD)rate * format.nBlockAlign;
        desc.dwSize = sizeof desc;
        desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME;
        desc.dwBufferBytes = bytes;
        desc.lpwfxFormat = &format;
        if (SUCCEEDED(IDirectSound8_CreateSoundBuffer(g_sound, &desc, &g_buffer, NULL))) {
            if (SUCCEEDED(IDirectSoundBuffer_Lock(g_buffer, 0, bytes, &p1, &n1, &p2, &n2, 0))) {
                memcpy(p1, pcm, n1);
                if (p2) memcpy(p2, (const uint8_t*)pcm + n1, n2);
                IDirectSoundBuffer_Unlock(g_buffer, p1, n1, p2, n2);
                if (FAILED(IDirectSoundBuffer_Play(g_buffer, 0, 0, 0))) let_go();
            } else let_go();
        }
    }
    free(pcm);
    return g_buffer ? ++g_number : 0;
}

void fc_voice_out_stop(unsigned clip) {
    if (clip && clip == g_number) let_go();
}

int fc_voice_out_playing(unsigned clip) {
    DWORD status = 0;
    if (!clip || clip != g_number || !g_buffer) return 0;
    return SUCCEEDED(IDirectSoundBuffer_GetStatus(g_buffer, &status)) && (status & DSBSTATUS_PLAYING) != 0;
}
