/*
 * Mods: loose files that stand in for entries of the game's archives.
 *
 * The game keeps its data in ED6_DTnn.dat archives, each with a table of
 * 36-byte entries (ED6_DTnn.dir). A mod has always had to replace a whole
 * archive, so two mods touching the same archive could not both be installed,
 * or else needed a loader injected into the game (LB-ARK, SoraVoice). Here the
 * loader is part of the port, and a mod is a folder:
 *
 *   mods\<mod>\ED6_DT05\h_kao000._ch          a file, as an editor works on it
 *   mods\<mod>\data\ED6_DT05\h_kao000._ch     the same, laid out as for LB-ARK
 *   mods\<mod>\packed\ED6_DT05\h_kao000._ch   an entry exactly as an archive holds it
 *   mods\<mod>\voice\scena\T0310   ._SN       a script, laid out as for SoraVoice
 *
 * A file name is the entry's 8.3 name in any case, with or without the spaces
 * the table pads it with. Folders are read in name order and a later mod's
 * file wins.
 *
 * How it works. ReadDirFiles (0x00477580) loads every table; after it, each
 * entry a mod replaces gets the size of its replacement and, for an offset,
 * 0x80000000 + a number: no archive is 2 GB long, so ArchiveReadRaw
 * (0x00477180), which every read of an entry goes through with that entry's
 * own offset and size, knows a replacement by its offset and fills the
 * buffer from the mod's file instead of the archive.
 *
 * The game unpacks most entries after reading them (BzDecompress). A file in
 * `packed` is still packed and goes in as it is. Any other is plain data, and
 * is given to the game as a stream of stored runs, which the unpacker copies
 * through: three bytes for an empty first chunk, then for each 0x1FFF bytes
 * `[u16 n + 4][big-endian u16 n | 0x2000][n bytes][u8 more]`. Textures
 * (._DS) and sounds (.WAV) are not packed in the archives either, and are
 * read as they are.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "mods.h"

#define FC_DIR_COUNTS   0x0079AF90u   /* int[32]: entries in each archive's table */
#define FC_DIR_TABLES   0x0079B010u   /* DirEntry*[32] */
#define FC_ARCHIVES     32
#define DIR_ENTRY       36            /* +0 name[12], +0x10 size, +0x20 offset */
#define OVERRIDE_TAG    0x80000000u
/* What the game reads an entry into is 6 MB (its staging buffer). */
#define MAX_PAYLOAD     (6u << 20)

void fc_orig_00477580(void);          /* ReadDirFiles */
void fc_orig_00477180(void);          /* ArchiveReadRaw */
void sub_004B39E0(void);              /* PumpMessages */

typedef struct {
    int      archive;
    char     name[13];                /* as the table stores it: "H_KAO000._CH" */
    int      packed;                  /* the file is an entry as an archive holds it */
    int      raw;                     /* plain data the game does not unpack (._DS, .WAV) */
    uint32_t file_size, payload_size;
    int      mod;
    char     path[MAX_PATH];
} override_t;

static override_t* g_ov;
static int g_nov, g_cap;
static char g_mod_names[64][64];
static int g_nmods;
static int g_applied, g_unknown;

/* "h_kao000._ch" or "T0310   ._SN" -> "H_KAO000._CH" / "T0310   ._SN": the
 * base padded to 8, the dot and extension to 4, upper case. */
static int entry_name(const char* file, char out[13]) {
    const char* dot = strchr(file, '.');
    size_t base = dot ? (size_t)(dot - file) : strlen(file);
    while (base && file[base - 1] == ' ') base--;
    size_t ext = dot ? strlen(dot) : 0;
    if (!base || base > 8 || ext > 4) return 0;
    memset(out, ' ', 12);
    out[12] = 0;
    for (size_t i = 0; i < base; i++) out[i] = (char)toupper((unsigned char)file[i]);
    for (size_t i = 0; i < ext; i++) out[8 + i] = (char)toupper((unsigned char)dot[i]);
    return 1;
}

static uint32_t stored_size(uint32_t n) {
    return 3 + n + 5 * ((n + 0x1FFE) / 0x1FFF);
}

static void add_file(int mod, int archive, const char* dir, const WIN32_FIND_DATAA* fd, int packed) {
    override_t o;
    memset(&o, 0, sizeof o);
    if (fd->nFileSizeHigh || !entry_name(fd->cFileName, o.name)) return;
    o.archive = archive;
    o.mod = mod;
    o.packed = packed;
    o.raw = !packed && (!memcmp(o.name + 8, "._DS", 4) || !memcmp(o.name + 8, ".WAV", 4));
    o.file_size = fd->nFileSizeLow;
    o.payload_size = packed || o.raw ? o.file_size : stored_size(o.file_size);
    _snprintf(o.path, sizeof o.path - 1, "%s\\%s", dir, fd->cFileName);
    if (o.payload_size > MAX_PAYLOAD) {
        fprintf(stderr, "[mods] %s is too large to load (%u bytes; the game reads an entry into 6 MB): left out\n",
                o.path, o.file_size);
        return;
    }
    if (g_nov == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 1024;
        g_ov = (override_t*)realloc(g_ov, (size_t)g_cap * sizeof *g_ov);
    }
    g_ov[g_nov++] = o;
}

static void scan_archive_folder(int mod, int archive, const char* dir, int packed) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    _snprintf(pattern, sizeof pattern - 1, "%s\\*", dir);
    pattern[sizeof pattern - 1] = 0;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) add_file(mod, archive, dir, &fd, packed);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* Every ED6_DTnn folder under `root`. */
static void scan_archives(int mod, const char* root, int packed) {
    for (int a = 0; a < FC_ARCHIVES; a++) {
        char dir[MAX_PATH];
        _snprintf(dir, sizeof dir - 1, "%s\\ED6_DT%02X", root, a);
        dir[sizeof dir - 1] = 0;
        if (GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES) scan_archive_folder(mod, a, dir, packed);
    }
}

static int by_name(const void* a, const void* b) { return _stricmp((const char*)a, (const char*)b); }

void mods_scan(const char* mods_dir) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    _snprintf(pattern, sizeof pattern - 1, "%s\\*", mods_dir);
    pattern[sizeof pattern - 1] = 0;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && g_nmods < 64) {
            strncpy(g_mod_names[g_nmods], fd.cFileName, sizeof g_mod_names[0] - 1);
            g_nmods++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_mod_names, (size_t)g_nmods, sizeof g_mod_names[0], by_name);
    for (int m = 0; m < g_nmods; m++) {
        char root[MAX_PATH], sub[MAX_PATH];
        int before = g_nov;
        _snprintf(root, sizeof root - 1, "%s\\%s", mods_dir, g_mod_names[m]);
        root[sizeof root - 1] = 0;
        scan_archives(m, root, 0);
        _snprintf(sub, sizeof sub - 1, "%s\\data", root);
        scan_archives(m, sub, 0);
        _snprintf(sub, sizeof sub - 1, "%s\\packed", root);
        scan_archives(m, sub, 1);
        _snprintf(sub, sizeof sub - 1, "%s\\voice\\scena", root);
        if (GetFileAttributesA(sub) != INVALID_FILE_ATTRIBUTES) scan_archive_folder(m, 1, sub, 0);
        fprintf(stderr, "[mods] %s: %d files\n", g_mod_names[m], g_nov - before);
    }
}

int mods_count(void) { return g_nmods; }

/* ReadDirFiles, wrapped: with the tables loaded, point the replaced entries
 * at their replacements. A later mod's file for the same entry takes the
 * earlier one's place, since the entry can only point at one. */
void fc_wrap_00477580(void) {
    fc_orig_00477580();
    for (int i = 0; i < g_nov; i++) {
        const override_t* o = &g_ov[i];
        uint32_t table = MEM32(FC_DIR_TABLES + 4 * (uint32_t)o->archive);
        int count = (int)MEM32(FC_DIR_COUNTS + 4 * (uint32_t)o->archive), found = 0;
        for (int k = 0; table && k < count; k++) {
            uint32_t e = table + (uint32_t)k * DIR_ENTRY;
            if (memcmp((const void*)(uintptr_t)e, o->name, 12)) continue;
            MEM32(e + 0x10) = o->payload_size;
            MEM32(e + 0x20) = OVERRIDE_TAG | (uint32_t)i;
            found = 1;
            break;
        }
        if (found) g_applied++;
        else if (g_unknown++ < 8)
            fprintf(stderr, "[mods] %s: no entry named %s in ED6_DT%02X: left out\n", g_mod_names[o->mod], o->name, o->archive);
    }
    if (g_nov)
        fprintf(stderr, "[mods] %d files from %d mods stand in for archive entries%s\n", g_applied, g_nmods,
                g_unknown ? " (some named no entry)" : "");
}

/* A file into the buffer, as the game expects that entry to arrive. */
static int deliver(const override_t* o, uint8_t* dst, uint32_t room) {
    HANDLE h = CreateFileA(o->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (o->packed || o->raw) {
        BOOL ok = ReadFile(h, dst, o->file_size < room ? o->file_size : room, &got, NULL);
        CloseHandle(h);
        return ok && got == (o->file_size < room ? o->file_size : room);
    }
    if (room < o->payload_size) { CloseHandle(h); return 0; }
    {   /* stored runs: see the top of this file */
        uint8_t* p = dst;
        uint32_t left = o->file_size;
        *p++ = 2, *p++ = 0, *p++ = left != 0;
        while (left) {
            uint32_t n = left > 0x1FFF ? 0x1FFF : left;
            *p++ = (uint8_t)(n + 4), *p++ = (uint8_t)((n + 4) >> 8);
            *p++ = (uint8_t)(0x20 | (n >> 8)), *p++ = (uint8_t)n;
            if (!ReadFile(h, p, n, &got, NULL) || got != n) { CloseHandle(h); return 0; }
            p += n;
            left -= n;
            *p++ = left != 0;
        }
    }
    CloseHandle(h);
    return 1;
}

/* ArchiveReadRaw, wrapped. ecx = buffer, edx = archive, on the stack the
 * entry's offset and size; the caller removes those. Returns al = 1. */
void fc_wrap_00477180(void) {
    uint32_t offset = MEM32(g_esp + 4), size = MEM32(g_esp + 8);
    if ((offset & OVERRIDE_TAG) && (offset & ~OVERRIDE_TAG) < (uint32_t)g_nov) {
        const override_t* o = &g_ov[offset & ~OVERRIDE_TAG];
        uint32_t dst = g_ecx;
        {   /* the original pumps messages before a read, so a long load does not hang the window */
            PUSH32(g_esp, RECOMP_RETADDR);
            sub_004B39E0();
        }
        if (!deliver(o, (uint8_t*)(uintptr_t)dst, size))
            fprintf(stderr, "[mods] cannot read %s (%u bytes wanted)\n", o->path, size);
        g_eax = 1;
        g_esp += 4;                   /* the return address; the caller pops the two arguments */
        return;
    }
    fc_orig_00477180();
}
