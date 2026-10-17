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
 *
 * SoraVoice keeps its voiced scripts in the game's own folder, as
 * voice\scena\<name>; mods_scan_game takes them from there, before any mod
 * folder, so the mod works as it was installed.
 *
 * The same file serves the Windows host and the portable one: folders are
 * listed through the C library of whichever it is.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SEP "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
#define SEP "/"
#endif

#include "recomp_types.h"
#include "mods.h"

#define FC_DIR_COUNTS   0x0079AF90u   /* int[32]: entries in each archive's table */
#define FC_DIR_TABLES   0x0079B010u   /* DirEntry*[32] */
#define FC_ARCHIVES     32
#define DIR_ENTRY       36            /* +0 name[12], +0x10 size, +0x20 offset */
#define OVERRIDE_TAG    0x80000000u
/* What the game reads an entry into is 6 MB (its staging buffer). */
#define MAX_PAYLOAD     (6u << 20)
#define PATH_ROOM       600

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
    char     path[PATH_ROOM];
} override_t;

static override_t* g_ov;
static int g_nov, g_cap;
static char g_mod_names[64][64];
static int g_nmods;
static int g_applied, g_unknown;

/* ---- folders, on either kind of host --------------------------------------------- */
typedef void (*seen_fn)(void* ctx, const char* dir, const char* name, int is_dir, uint32_t size, int too_big);
static void list_dir(const char* dir, seen_fn seen, void* ctx) {
#ifdef _WIN32
    char pattern[PATH_ROOM];
    WIN32_FIND_DATAA fd;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        seen(ctx, dir, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, fd.nFileSizeLow, fd.nFileSizeHigh != 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    struct dirent* e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char path[PATH_ROOM];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) continue;
        seen(ctx, dir, e->d_name, S_ISDIR(st.st_mode), (uint32_t)st.st_size, st.st_size > 0x7FFFFFFF);
    }
    closedir(d);
#endif
}
static int is_dir(const char* path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

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

typedef struct { int mod, archive, packed; } scan_t;
static void add_file(void* ctx, const char* dir, const char* name, int is_folder, uint32_t size, int too_big) {
    const scan_t* sc = (const scan_t*)ctx;
    override_t o;
    memset(&o, 0, sizeof o);
    if (is_folder || too_big || !entry_name(name, o.name)) return;
    o.archive = sc->archive;
    o.mod = sc->mod;
    o.packed = sc->packed;
    o.raw = !sc->packed && (!memcmp(o.name + 8, "._DS", 4) || !memcmp(o.name + 8, ".WAV", 4));
    o.file_size = size;
    o.payload_size = sc->packed || o.raw ? o.file_size : stored_size(o.file_size);
    snprintf(o.path, sizeof o.path, "%s" SEP "%s", dir, name);
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
    scan_t sc = { mod, archive, packed };
    list_dir(dir, add_file, &sc);
}

/* Every ED6_DTnn folder under `root`. */
static void scan_archives(int mod, const char* root, int packed) {
    for (int a = 0; a < FC_ARCHIVES; a++) {
        char dir[PATH_ROOM];
        snprintf(dir, sizeof dir, "%s" SEP "ED6_DT%02X", root, a);
        if (is_dir(dir)) scan_archive_folder(mod, a, dir, packed);
    }
}

static int by_name(const void* a, const void* b) {
    const unsigned char *x = (const unsigned char*)a, *y = (const unsigned char*)b;
    for (; *x && tolower(*x) == tolower(*y); x++, y++) {}
    return tolower(*x) - tolower(*y);
}
static void add_mod(void* ctx, const char* dir, const char* name, int is_folder, uint32_t size, int too_big) {
    (void)ctx, (void)dir, (void)size, (void)too_big;
    if (is_folder && name[0] != '.' && g_nmods < 64) {
        strncpy(g_mod_names[g_nmods], name, sizeof g_mod_names[0] - 1);
        g_nmods++;
    }
}

/* The game's own folder, where SoraVoice puts its scripts. */
void mods_scan_game(const char* game_dir) {
    char sub[PATH_ROOM];
    int before = g_nov;
    snprintf(sub, sizeof sub, "%s" SEP "voice" SEP "scena", game_dir);
    if (!is_dir(sub) || g_nmods >= 64) return;
    strcpy(g_mod_names[g_nmods], "the game's voice" SEP "scena");
    scan_archive_folder(g_nmods, 1, sub, 0);
    fprintf(stderr, "[mods] %s: %d files\n", g_mod_names[g_nmods], g_nov - before);
    g_nmods++;
}

void mods_scan(const char* mods_dir) {
    int first = g_nmods;
    list_dir(mods_dir, add_mod, NULL);
    qsort(g_mod_names + first, (size_t)(g_nmods - first), sizeof g_mod_names[0], by_name);
    for (int m = first; m < g_nmods; m++) {
        char mod_root[PATH_ROOM], sub[PATH_ROOM];
        int before = g_nov;
        snprintf(mod_root, sizeof mod_root, "%s" SEP "%s", mods_dir, g_mod_names[m]);
        scan_archives(m, mod_root, 0);
        snprintf(sub, sizeof sub, "%s" SEP "data", mod_root);
        scan_archives(m, sub, 0);
        snprintf(sub, sizeof sub, "%s" SEP "packed", mod_root);
        scan_archives(m, sub, 1);
        snprintf(sub, sizeof sub, "%s" SEP "voice" SEP "scena", mod_root);
        if (is_dir(sub)) scan_archive_folder(m, 1, sub, 0);
        fprintf(stderr, "[mods] %s: %d files\n", g_mod_names[m], g_nov - before);
    }
}

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
            if (memcmp((const void*)ADDR(e), o->name, 12)) continue;
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
    FILE* f = fopen(o->path, "rb");
    int ok = 1;
    if (!f) return 0;
    if (o->packed || o->raw) {
        uint32_t want = o->file_size < room ? o->file_size : room;
        ok = fread(dst, 1, want, f) == want;
    } else if (room < o->payload_size) {
        ok = 0;
    } else {                          /* stored runs: see the top of this file */
        uint8_t* p = dst;
        uint32_t left = o->file_size;
        *p++ = 2, *p++ = 0, *p++ = left != 0;
        while (left && ok) {
            uint32_t n = left > 0x1FFF ? 0x1FFF : left;
            *p++ = (uint8_t)(n + 4), *p++ = (uint8_t)((n + 4) >> 8);
            *p++ = (uint8_t)(0x20 | (n >> 8)), *p++ = (uint8_t)n;
            ok = fread(p, 1, n, f) == n;
            p += n;
            left -= n;
            *p++ = left != 0;
        }
    }
    fclose(f);
    return ok;
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
        if (!deliver(o, (uint8_t*)ADDR(dst), size))
            fprintf(stderr, "[mods] cannot read %s (%u bytes wanted)\n", o->path, size);
        g_eax = 1;
        g_esp += 4;                   /* the return address; the caller pops the two arguments */
        return;
    }
    fc_orig_00477180();
}
