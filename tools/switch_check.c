/*
 * Trails in the Sky FC, Switch: a check of the console and the SD card.
 *
 * A small program to start from the Homebrew Menu the same way as the game.
 * It says on the screen, and writes to check.txt beside itself, what the game
 * needs and whether it is there: how the Homebrew Menu was opened (which
 * decides how much memory a program gets), the memory, the game's files, and
 * whether the graphics driver gives an OpenGL ES 3 context.
 *
 * Built by tools/build_portable.sh switch, as trails-check.nro.
 */
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

static char g_text[6000];
static int g_problems;
static void say(const char* fmt, ...) {
    va_list ap;
    size_t n = strlen(g_text);
    va_start(ap, fmt);
    vsnprintf(g_text + n, sizeof g_text - n, fmt, ap);
    va_end(ap);
}
static void save(const char* home) {
    char path[400];
    FILE* f;
    snprintf(path, sizeof path, "sdmc:%s/check.txt", home);
    if ((f = fopen(path, "w")) == NULL) f = fopen("sdmc:/trails-check.txt", "w");
    if (f) { fputs(g_text, f); fclose(f); }
}

/* An OpenGL ES 3 context on the console's own window, as the game makes one. */
static void graphics(void) {
    static const EGLint want[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                   EGL_DEPTH_SIZE, 24, EGL_NONE };
    static const EGLint version[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig cfg;
    EGLint n = 0;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) { say("  graphics: EGL would not start (0x%X)\n", eglGetError()); g_problems++; return; }
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(dpy, want, &cfg, 1, &n) || n < 1) { say("  graphics: no OpenGL ES 3 configuration\n"); g_problems++; eglTerminate(dpy); return; }
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, nwindowGetDefault(), NULL);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, version);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
        say("  graphics: no OpenGL ES 3 context (0x%X)\n", eglGetError());
        g_problems++;
    } else {
        GLint max = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
        say("  graphics: %s, %s, textures up to %d\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER), max);
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
}

int main(int argc, char** argv) {
    char home[300] = "/switch/trails", path[500];
    const char* self = argc > 0 && argv && argv[0] ? argv[0] : "";
    const char* at = strchr(self, ':');
    const char* cut;
    struct stat st;
    u64 total = 0, used = 0;
    PadState pad;

    at = at ? at + 1 : self;
    cut = strrchr(at, '/');
    if (at[0] == '/' && cut && cut > at && (size_t)(cut - at) < sizeof home) { memcpy(home, at, (size_t)(cut - at)); home[cut - at] = 0; }

    say("  Trails in the Sky FC: a check of this console and card\n\n");
    say("  started as: %s\n  folder:     sdmc:%s\n", self[0] ? self : "(not said)", home);
    u32 hos = hosversionGet();
    say("  system:     %u.%u.%u%s\n", HOSVER_MAJOR(hos), HOSVER_MINOR(hos), HOSVER_MICRO(hos), hosversionIsAtmosphere() ? ", Atmosphere" : "");

    AppletType type = appletGetAppletType();
    int app = type == AppletType_Application || type == AppletType_SystemApplication;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    say("\n  Homebrew Menu opened from: %s\n", app ? "a game (full memory): good" : "the Album (little memory): NOT ENOUGH for the game");
    if (!app) g_problems++;
    say("  memory:     %u MB for this program\n", (unsigned)(total >> 20));
    {
        void* big = memalign(0x10000, 0x20000000u);
        say("  512 MB in one piece: %s\n", big ? "yes" : "NO");
        if (!big) g_problems++;
        free(big);
    }

    snprintf(path, sizeof path, "sdmc:%s/game/ed6_win_DX9.exe", home);
    if (stat(path, &st) == 0) say("\n  game/ed6_win_DX9.exe: there (%ld bytes)\n", (long)st.st_size);
    else { say("\n  game/ed6_win_DX9.exe: MISSING\n    expected at %s\n", path); g_problems++; }
    {
        int dat = 0, dir = 0, nested = 0;
        struct dirent* e;
        DIR* d;
        snprintf(path, sizeof path, "sdmc:%s/game", home);
        d = opendir(path);
        while (d && (e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            if (n > 4 && !strncasecmp(e->d_name, "ED6_DT", 6)) { if (!strcasecmp(e->d_name + n - 4, ".dat")) dat++; else if (!strcasecmp(e->d_name + n - 4, ".dir")) dir++; }
            if (!strcasecmp(e->d_name, "Trails in the Sky FC")) nested = 1;
        }
        if (d) closedir(d);
        say("  game archives: %d .dat and %d .dir files%s\n", dat, dir, dat >= 20 && dir >= 20 ? "" : "  (TOO FEW: the game has more than twenty of each)");
        if (dat < 20 || dir < 20) g_problems++;
        if (nested) say("  the game's folder is INSIDE game: move what is in it up one level\n");
    }
    snprintf(path, sizeof path, "sdmc:%s/trails.nro", home);
    if (stat(path, &st) == 0) say("  trails.nro: %ld bytes\n", (long)st.st_size);
    else say("  trails.nro: not beside this program\n");
    save(home);                               /* before the graphics driver is tried, in case that stops the program */

    say("\n");
    graphics();
    say("\n  %s\n", g_problems ? "Something above has to be put right before the game can start." : "Everything the game needs is here.");
    save(home);

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("\n%s\n  This is also in check.txt in that folder.  Press + to leave.\n", g_text);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
