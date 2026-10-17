/*
 * Trails in the Sky FC - the portable host (Linux, and from there the Switch).
 *
 * The same lifted C as the Windows build (src/recomp/gen), on pcrecomp's
 * runtime/win32hle instead of runtime/native32: every import the game makes
 * is answered by the host's own code, so there is no Windows underneath. On a
 * 64-bit host the guest's 4 GB is one reservation and every guest address an
 * offset into it (win32hle's guest_mem.c), which is what an arm64 machine
 * needs.
 *
 * It maps the exe, binds what win32hle and this host answer, and gives any
 * other import a stub that names itself when called: --imports lists them.
 * The game's folder is the working directory; with no exe named, the game is
 * looked for there and then beside the program.
 *
 *   trails-portable [options] [path to ed6_win_DX9.exe]
 *
 * usage() has the options.
 *
 * SoraVoice, if it is installed in the game's folder (voice\scena and
 * voice\ogg), works as it is: its scripts are loaded and its voices played
 * (src/mods/voice.c).
 *
 * On the Switch there is no command line: the same options are read from
 * args.txt beside the program, if it is there, and what cannot work (started
 * with too little memory, the game's files missing) is said on the screen.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"
#include "mods.h"
#include <strings.h>

#define FC_GAME_EXE "ed6_win_DX9.exe"

/* On the Switch the game's folder is on the SD card and there is no console
 * to write to: what a run says goes to a file beside it, ending with a line
 * that starts "done" (how a test run under an emulator knows it has ended). */
#ifdef __SWITCH__
#include <switch.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
/* Everything is in the folder the program was started from (as the Homebrew
 * Menu names it; /switch/trails if it does not say): the game's own folder as
 * "game", its Saved Games folder as "save", trails.log, and args.txt for the
 * options a command line would carry. */
static char g_home[300] = "/switch/trails";
static char g_exe[400], g_saves[400], g_args[400];
static volatile int g_log_open;
static void profile_write(void);
static void log_done(int code) {
    static int said;
    if (said++) return;
    profile_write();
    fprintf(stderr, "done: exit code %d\n", code);
    fflush(stderr);
    g_log_open = 0;
    fclose(stderr);                     /* the card's file is only whole once closed */
}
static void log_done_at_exit(void) { log_done(0); }
/* What the card has of the log is what has been flushed to it. Once a second,
 * so that a run that stops dead still leaves its last words. */
static void* log_keeper(void* arg) {
    (void)arg;
    hle_thread_aside();
    for (;;) {
        svcSleepThread(1000000000ull);
        if (g_log_open) fsync(fileno(stderr));
    }
    return NULL;
}

/* Something the player has to put right: said on the screen, and in the log. */
static void stop_with(const char* fmt, ...) {
    char msg[1000];
    PadState pad;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "cannot start:\n%s\n", msg);
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("\n\n  Trails in the Sky FC\n\n%s\n\n  Press + to leave.\n", msg);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    log_done(1);
    exit(1);
}

/* The settings a first run starts with: a 1280 by 720 picture at 60 frames
 * a second, sound and the movies on. Written only when there is no settings
 * file. */
static const char k_first_settings[] =
    "[Video]\r\n"
    "AdapterDX9=0\r\n"
    "DeviceDX9=0\r\n"
    "ModeDX9=25\r\n"
    "WidthDX9=1280\r\n"
    "HeightDX9=720\r\n"
    "RefreshRateDX9=0\r\n"
    "FormatDX9=22\r\n"
    "VendorIDDX9=4318\r\n"
    "DeviceIDDX9=9860\r\n"
    "KeepAspect=1\r\n"
    "HardwareVertexShaderDX9=1\r\n"
    "WindowMode=1\r\n"
    "UseMipmap=1\r\n"
    "WaitVSync=0\r\n"
    "LowTexture=0\r\n"
    "TextureFilter=3\r\n"
    "FpsMode=2\r\n"
    "FullAntiAlias=0\r\n"
    "LowEffect=0\r\n"
    "ShadowQuality=2\r\n"
    "HighResoText=1\r\n"
    "ForceTextReso=0\r\n"
    "HighResoAssets=1\r\n"
    "BoostFont=0\r\n"
    "MinimapMode=2\r\n"
    "StatusMode=1\r\n"
    "Menu=0\r\n"
    "LogoPlayOff=0\r\n"
    "OpeningPlayOff=0\r\n"
    "UseNewOrbmentLine=1\r\n"
    "UseNewOrbmentLineStyle=1\r\n"
    "[Sound]\r\n"
    "BgmOff=0\r\n"
    "SeOff=0\r\n"
    "BgmVolume=6\r\n"
    "SeVolume=6\r\n"
    "NotUseDirectSound=0\r\n"
    "NotUseDirectSound3d=0\r\n"
    "BtVoiceOff=0\r\n"
    "BtVoiceLang=1\r\n"
    "SoftwareSoundBuffer=0\r\n"
    "NoBeeping=0\r\n"
    "BgmPack=1\r\n"
    "[Control]\r\n"
    "Camera45Deg=0\r\n"
    "UseGamaPad=1\r\n"
    "KeyboardUseDI=1\r\n"
    "MouseUseDI=1\r\n"
    "DefaultRun=0\r\n"
    "ChrTrans=1\r\n"
    "Analog=1\r\n"
    "RightCamera=1\r\n"
    "InvertCamera=0\r\n"
    "ButtonMode=1\r\n"
    "Keyboard=20111b41585a2628252742434d10564e12\r\n"
    "GamePad=000108050403020907060a0b\r\n"
    "PadDeviceNo=0\r\n"
    "PadSensitivity=60\r\n"
    "Mouse=0102\r\n"
    "UseWheel=1\r\n"
    "WheelReverse=0\r\n"
    "RetryOffset=0\r\n"
    "TurboLvField=2\r\n"
    "TurboLvBattle=6\r\n";
static void first_settings(void) {
    char path[480];
    FILE* f;
    snprintf(path, sizeof path, "%s/FALCOM", g_saves);
    mkdir(g_saves, 0777), mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/FALCOM/ED6", g_saves);
    mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/FALCOM/ED6/config.ini", g_saves);
    if ((f = fopen(path, "rb")) != NULL) { fclose(f); return; }
    if ((f = fopen(path, "wb")) == NULL) { fprintf(stderr, "could not write %s\n", path); return; }
    fwrite(k_first_settings, 1, sizeof k_first_settings - 1, f);
    fclose(f);
    fprintf(stderr, "no settings yet: wrote %s\n", path);
}

static void switch_start(int argc, char** argv) {
    char path[480];
    struct stat st;
    pthread_t keeper;
    u64 total = 0, used = 0;
    const char* self = argc > 0 && argv && argv[0] ? argv[0] : "";
    const char* at = strchr(self, ':');
    const char* cut;
    at = at ? at + 1 : self;                                  /* past "sdmc:" */
    cut = strrchr(at, '/');
    if (at[0] == '/' && cut && cut > at && (size_t)(cut - at) < sizeof g_home) {
        char home[sizeof g_home];
        memcpy(home, at, (size_t)(cut - at));
        home[cut - at] = 0;
        snprintf(path, sizeof path, "sdmc:%s", home);
        if (stat(path, &st) == 0) strcpy(g_home, home);
    } else {
        /* Not told where it was started from (an emulator given the file does
         * not say): /switch/trails, or the folder named by the first line of
         * /switch/trails/home.txt, which is how a second copy of the game (a
         * modded one, say) is chosen there. */
        FILE* f = fopen("sdmc:/switch/trails/home.txt", "r");
        char home[sizeof g_home] = "";
        if (f && fgets(home, sizeof home, f)) {
            home[strcspn(home, "\r\n")] = 0;
            snprintf(path, sizeof path, "sdmc:%s", home);
            if (home[0] == '/' && stat(path, &st) == 0) strcpy(g_home, home);
        }
        if (f) fclose(f);
    }
    /* The SD card is the default device from here on, so a path that starts
     * with a slash is on it. */
    snprintf(path, sizeof path, "sdmc:%s", g_home);
    mkdir(path, 0777);
    if (chdir(path) != 0) chdir("sdmc:/");
    if (freopen("trails.log", "w", stderr) || freopen("sdmc:/trails.log", "w", stderr)) {
        g_log_open = 1;
        setvbuf(stderr, NULL, _IONBF, 0);
        pthread_create(&keeper, NULL, log_keeper, NULL);
    }
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(stderr, "started as \"%s\", home %s, applet type %d, memory %u MB of which %u MB in use\n", self, g_home,
            (int)appletGetAppletType(), (unsigned)(total >> 20), (unsigned)(used >> 20));
    snprintf(g_exe, sizeof g_exe, "%s/game/" FC_GAME_EXE, g_home);
    snprintf(g_saves, sizeof g_saves, "%s/save", g_home);
    snprintf(g_args, sizeof g_args, "%s/args.txt", g_home);

    if (appletGetAppletType() != AppletType_Application && appletGetAppletType() != AppletType_SystemApplication)
        stop_with("  The Homebrew Menu was opened from the Album, and a program started\n"
                  "  from there is given too little memory for this game.\n\n"
                  "  Open the Homebrew Menu from a game instead: hold R while you start\n"
                  "  any game, and keep it held until the menu appears.");
    {
        void* guest = memalign(0x10000, 0x20000000u);         /* what the guest's memory will ask for */
        if (!guest)
            stop_with("  There is not enough memory: the game needs 512 MB in one piece and\n"
                      "  about 1 GB in all, and this program was given %u MB.\n\n"
                      "  Open the Homebrew Menu by holding R while you start a game, not\n"
                      "  from the Album.", (unsigned)(total >> 20));
        free(guest);
    }
    if (stat(g_exe, &st) != 0)
        stop_with("  The game's files are not on the SD card.\n\n"
                  "  Copy what is inside the game's folder on your PC (the folder that\n"
                  "  holds ed6_win_DX9.exe and the ED6_DT files) into\n\n"
                  "      sdmc:%s/game\n\n"
                  "  so that this file is there:\n\n"
                  "      sdmc:%s", g_home, g_exe);
    first_settings();
    /* The game's folder is the working directory, as the game expects. */
    snprintf(path, sizeof path, "sdmc:%s/game", g_home);
    if (chdir(path) != 0) stop_with("  Could not enter sdmc:%s/game.", g_home);
}
#endif

extern const uint32_t fc_entry_va;    /* recomp_dispatch.c */
extern const win32hle_shim fc_win32_wide[];   /* win32_wide.c */
void fc_set_saves_folder(const char* host_dir);

/* ---- what the lift calls into (WRAPS in run_lift.py): the buttons and the clock */
void fc_orig_004802C0(void);
void fc_orig_004C11A0(void);

/* ---- a play script (--script FILE) ---------------------------------------------------
 * The Windows host's, line for line (src/runtime/host.c has the format):
 * wait MS, hold BUTTON MS, tap BUTTON [times [interval]], shot NAME, log TEXT,
 * quit; and two of this host's own: touch X Y [MS], a finger on the picture
 * at that point of it for that long (200 ms), and profile, from which a
 * profile (--profile) counts again, leaving out what came before. While a
 * script has steps left it answers Input_Poll instead of the controller. */
static const struct { const char* name; uint32_t bit; } k_buttons[] = {
    { "UP", 0x1 }, { "DOWN", 0x2 }, { "RIGHT", 0x4 }, { "LEFT", 0x8 }, { "ESC", 0x10 }, { "SPACE", 0x20 },
    { "ENTER", 0x40 }, { "CTRL", 0x80 }, { "NUM0", 0x100 }, { "SHIFT", 0x200 }, { "Z", 0x400 }, { "X", 0x800 },
    { "PGUP", 0x1000 }, { "PGDN", 0x2000 }, { "B", 0x8000 }, { "C", 0x10000 }, { "M", 0x20000 },
    { "A", 0x400000 }, { "V", 0x800000 },
};
enum { STEP_WAIT, STEP_HOLD, STEP_SHOT, STEP_LOG, STEP_QUIT, STEP_TOUCH, STEP_PROFILE };
typedef struct { int kind; uint32_t ms, buttons; char text[128]; } step_t;
#define LONGEST_TAP_MS 250u
static step_t* g_steps;
static int g_nsteps, g_step, g_step_entered;
static uint32_t g_step_started, g_step_started_wall;
static char g_shot_pending[64];       /* a script's `shot`, taken at the next Present */
static void wav_close(void);
static void profile_write(void);
static void profile_restart(void);

static uint32_t button_mask(const char* word) {
    for (size_t i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++)
        if (!strcasecmp(word, k_buttons[i].name)) return k_buttons[i].bit;
    return (uint32_t)strtoul(word, NULL, 16);
}
static void add_step(int kind, uint32_t ms, uint32_t buttons, const char* text) {
    g_steps = (step_t*)realloc(g_steps, (size_t)(g_nsteps + 1) * sizeof *g_steps);
    step_t* s = &g_steps[g_nsteps++];
    s->kind = kind, s->ms = ms, s->buttons = buttons;
    snprintf(s->text, sizeof s->text, "%s", text);
}
static void load_script(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256];
    if (!f) { fprintf(stderr, "[script] cannot open %s\n", path); return; }
    while (fgets(line, sizeof line, f)) {
        char verb[16] = "", a[64] = "", rest[128] = "";
        unsigned n1 = 0, n2 = 0, n3 = 0;
        if (sscanf(line, "%15s", verb) != 1 || verb[0] == '#') continue;
        if (!strcasecmp(verb, "wait") && sscanf(line, "%*s %u", &n1) == 1) add_step(STEP_WAIT, n1, 0, "");
        else if (!strcasecmp(verb, "hold") && sscanf(line, "%*s %63s %u", a, &n1) == 2) add_step(STEP_HOLD, n1, button_mask(a), "");
        else if (!strcasecmp(verb, "tap") && sscanf(line, "%*s %63s %u %u", a, &n1, &n2) >= 1) {
            unsigned count = n1 ? n1 : 1, interval = n2 > 100 ? n2 : 500;
            for (unsigned i = 0; i < count; i++) {
                add_step(STEP_HOLD, 100, button_mask(a), "");
                add_step(STEP_WAIT, interval - 100, 0, "");
            }
        } else if (!strcasecmp(verb, "touch") && sscanf(line, "%*s %u %u %u", &n1, &n2, &n3) >= 2) {
            add_step(STEP_TOUCH, n3 ? n3 : 200, (n1 & 0xFFFFu) | (n2 & 0xFFFFu) << 16, "");
        } else if (!strcasecmp(verb, "shot") && sscanf(line, "%*s %47s", a) == 1) add_step(STEP_SHOT, 0, 0, a);
        else if (!strcasecmp(verb, "log") && sscanf(line, "%*s %127[^\r\n]", rest) == 1) add_step(STEP_LOG, 0, 0, rest);
        else if (!strcasecmp(verb, "quit")) add_step(STEP_QUIT, 0, 0, "");
        else if (!strcasecmp(verb, "profile")) add_step(STEP_PROFILE, 0, 0, "");
        else fprintf(stderr, "[script] cannot read line: %s", line);
    }
    fclose(f);
    fprintf(stderr, "[script] %d steps from %s\n", g_nsteps, path);
}
/* The buttons the script holds right now. Its clock stops while the game
 * does (a long load), so a wait is a wait of the game's time. */
static uint32_t scripted_buttons(void) {
    static uint32_t last_poll, script_ms;
    uint32_t wall = hle_ticks_ms();
    if (last_poll) script_ms += wall - last_poll < 34 ? wall - last_poll : 34;
    last_poll = wall;
    while (g_step < g_nsteps) {
        const step_t* s = &g_steps[g_step];
        if (!g_step_entered) {
            g_step_entered = 1;
            g_step_started = script_ms;
            g_step_started_wall = wall;
            if (s->kind == STEP_SHOT) snprintf(g_shot_pending, sizeof g_shot_pending, "%s", s->text);
            if (s->kind == STEP_TOUCH) hle_d3d9_touch(1, (int)(s->buttons & 0xFFFFu), (int)(s->buttons >> 16));
            if (s->kind == STEP_LOG) fprintf(stderr, "[script] %s\n", s->text);
            if (s->kind == STEP_PROFILE) profile_restart();
            if (s->kind == STEP_QUIT) {
                fprintf(stderr, "[script] quit\n");
                wav_close();
                profile_write();
                hle_exit(0);
            }
        }
        int tap = s->kind == STEP_HOLD && s->ms <= LONGEST_TAP_MS;
        uint32_t elapsed = tap ? wall - g_step_started_wall : script_ms - g_step_started;
        if (s->kind == STEP_SHOT && g_shot_pending[0]) return 0;     /* a picture is of a finished frame */
        if (elapsed < s->ms) return s->kind == STEP_HOLD ? s->buttons : 0;
        if (s->kind == STEP_TOUCH) hle_d3d9_touch(0, (int)(s->buttons & 0xFFFFu), (int)(s->buttons >> 16));
        g_step++;
        g_step_entered = 0;
    }
    return 0;
}

/* ---- a controller -----------------------------------------------------------------
 * Input_Poll (0x004802C0) gathers keyboard, mouse and pad into one mask of
 * buttons and returns it. It is wrapped, and what SDL's first controller
 * holds down is added to the mask as the game's own pad bindings would give
 * it: the direction pad and left stick as the four directions, and
 *
 *     A  confirm          B  cancel            (as the console labels them:
 *     X, Y                the game's third and tenth pad buttons   on a PC pad the
 *     L, R                its fourth and fifth                     lower button
 *     ZL                  its ninth                                confirms)
 *     ZR                  turbo: the game runs faster while it is held
 *     +, -                its seventh and eighth
 *     the right stick pressed in: its sixth; the left: the guide
 *
 * DirectInput, which is how the game itself finds a pad, is not answered yet;
 * this needs none of it. */
#include <SDL.h>
#define FC_MOUSE_IN_USE 0x00557E5Eu   /* byte: set while the mouse drives the menus */
#define FC_MOUSE_MODE   0x006B0018u   /* byte: mouse mode */
#define B_UP 0x1ull
#define B_DOWN 0x2ull
#define B_RIGHT 0x4ull
#define B_LEFT 0x8ull
#define B_CONFIRM 0x60ull
#define B_CANCEL 0x90ull
#define B_PAD3 0x400000ull
#define B_PAD4 0x400ull
#define B_PAD5 0x800ull
#define B_PAD6 0x8000ull
#define B_PAD7 0x10000ull
#define B_PAD8 0x20000ull
#define B_PAD9 0x200ull
#define B_PAD10 0x800000ull
#define B_GUIDE (1ull << 32)          /* the two buttons of the mask's upper half */
#define B_TURBO (1ull << 33)
static uint64_t pad_buttons(void) {
    static SDL_Joystick* joy;
    static SDL_GameController* pad;
    static int tried;
    uint64_t b = 0;
    int x = 0, y = 0;
    if (!tried) {
        tried = 1;
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0 || SDL_NumJoysticks() < 1) return 0;
#ifndef __SWITCH__
        if (SDL_IsGameController(0)) pad = SDL_GameControllerOpen(0);
#endif
        if (!pad) joy = SDL_JoystickOpen(0);
        fprintf(stderr, "[pad] %s\n", pad ? SDL_GameControllerName(pad) : joy ? SDL_JoystickName(joy) : "none could be opened");
    }
    if (pad) {
        static const struct { SDL_GameControllerButton from; uint64_t to; } map[] = {
            { SDL_CONTROLLER_BUTTON_RIGHTSTICK, B_PAD6 }, { SDL_CONTROLLER_BUTTON_LEFTSTICK, B_GUIDE },
            { SDL_CONTROLLER_BUTTON_A, B_CONFIRM }, { SDL_CONTROLLER_BUTTON_B, B_CANCEL }, { SDL_CONTROLLER_BUTTON_X, B_PAD3 },
            { SDL_CONTROLLER_BUTTON_Y, B_PAD10 }, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_PAD4 },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_PAD5 }, { SDL_CONTROLLER_BUTTON_START, B_PAD7 },
            { SDL_CONTROLLER_BUTTON_BACK, B_PAD8 }, { SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP },
            { SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT },
            { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT },
        };
        SDL_GameControllerUpdate();
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GameControllerGetButton(pad, map[i].from)) b |= map[i].to;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) b |= B_PAD9;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) b |= B_TURBO;
        x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    } else if (joy) {
        /* The console's own pad, in the order its buttons are numbered: A, B, X,
         * Y, the two sticks, L, R, ZL, ZR, plus, minus, then left, up, right, down. */
        static const uint64_t map[16] = { B_CONFIRM, B_CANCEL, B_PAD3, B_PAD10, B_GUIDE, B_PAD6, B_PAD4, B_PAD5, B_PAD9, B_TURBO,
                                          B_PAD7, B_PAD8, B_LEFT, B_UP, B_RIGHT, B_DOWN };
        SDL_JoystickUpdate();
        for (int i = 0; i < 16 && i < SDL_JoystickNumButtons(joy); i++) if (SDL_JoystickGetButton(joy, i)) b |= map[i];
        if (SDL_JoystickNumAxes(joy) >= 2) x = SDL_JoystickGetAxis(joy, 0), y = SDL_JoystickGetAxis(joy, 1);
    }
    if (x < -16000) b |= B_LEFT;
    if (x > 16000) b |= B_RIGHT;
    if (y < -16000) b |= B_UP;
    if (y > 16000) b |= B_DOWN;
    return b;
}
/* The pointer. The game is driven by buttons or by a pointer, which it
 * draws itself and whose place picks the menu item. It goes over to the
 * pointer whenever the pointer has moved (0x00480FB0) and back at a direction
 * button. But it also glides the pointer to the selected item itself, and
 * here, where that was the only thing that ever moved it, the game took its
 * own gliding for a hand on a mouse: a pointer on the screen of a console
 * that has no mouse. So the game is with the pointer only while a real one
 * is in use, from a mouse's or a finger's report (win32hle counts them; a
 * touch screen is a mouse there) until the next button. */
static int g_pointer_live;
/* The buttons that were down when the host's movie ended, kept from the game
 * until they are let go (fc_wrap_004886D0 says why). */
static uint64_t g_after_movie;

void fc_wrap_004802C0(void) {
    static unsigned seen;
    fc_orig_004802C0();
    unsigned reports = hle_input_pointer_events();
    if (reports != seen) seen = reports, g_pointer_live = 1;
    uint64_t held = g_step < g_nsteps ? scripted_buttons() : pad_buttons();
    g_after_movie &= held;
    held &= ~g_after_movie;
    if (held) g_pointer_live = 0;
    if (!g_pointer_live) MEM8(FC_MOUSE_IN_USE) = 0, MEM8(FC_MOUSE_MODE) = 0;
    g_eax |= (uint32_t)held;      /* beside what the poll found itself, which here is only the pointer's buttons */
    g_edx |= (uint32_t)(held >> 32);
}

/* ---- the frame limiter (--fps N) -------------------------------------------------
 * Clock_Update (0x004C11A0; thiscall, two byte arguments: wait, notify) is
 * where the game holds a frame back: with `wait` set it reads the counter,
 * pumps messages and yields, over and over, until 8 ms have passed since the
 * frame before (src/runtime/host.c has the whole of it). The host waits
 * instead, asleep until just before the frame is due, and tells the game's
 * loop not to: N frames a second, 0 for no wait at all, -1 for the game's own
 * loop. Frames are let go on a schedule, each a period after the last was
 * due, so that waking late costs one frame a little and not every frame
 * after it; a frame that overruns by more than a period starts the schedule
 * again from now.
 *
 * On the Switch Present waits for the display, and that is the limit: a rate
 * the display's sixty divides by (60, 30, 20) is kept by presenting every
 * refresh, or every second or third, which no sleeping here could do as
 * evenly. The rate is the one asked for and stays it: a rate that moved
 * between sixty and thirty by itself was tried and taken out again
 * (docs/bringup.md, 23).
 *
 * The rate is also the Frame Rate row of the game's own OPTION screen, whose
 * own wait the host takes over (src/common/frame_option.c): 60 or 30 on the
 * Switch, Uncapped, 120, 60 or 30 elsewhere. A choice there is in force at
 * once and the game keeps it in its settings; at start it is the rate
 * unless --fps gives one. --original-limiter leaves the game's waits, and
 * its row, as they were. */
#define FC_CLOCK_COUNTER 0x007C8B4Du  /* byte: a performance counter is in use */
#ifdef __SWITCH__
/* The display paces a console's frames; the game's own 8 ms loop is left in
 * (it never has to wait at sixty). */
static int g_fps_limit = -1;
#else
static int g_fps_limit = 120;
#endif
static int g_game_limiter;            /* --original-limiter */
static int g_fps_given;               /* --fps */
/* The processor time the calling thread has had, in nanoseconds. Between two
 * Presents it is what the frame cost, whatever it waited for and wherever
 * (the driver waits for the display in the frame's first clear, not in
 * Present). */
static uint64_t thread_cpu_ns(void) {
#ifdef __SWITCH__
    u64 ticks = 0;
    svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, UINT64_MAX);
    return armTicksToNs(ticks);
#else
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}
/* A frame's work, for whoever asks after a Present: the processor time since
 * the Present before, if the same thread made both (a loading screen is
 * another thread's), and otherwise `took_ns`, the clock's. */
static uint64_t g_worked_ns;          /* summed, for --perf */
static uint64_t frame_work_ns(uint64_t took_ns) {
    static uint64_t last;
    static pthread_t by;
    static int known;
    uint64_t now = thread_cpu_ns(), work = took_ns;
    pthread_t me = pthread_self();
    if (known && pthread_equal(me, by) && now >= last) work = now - last;
    last = now, by = me, known = 1;
    g_worked_ns += work;
    return work;
}
static uint64_t g_waited_ns;          /* in the limiter, for --perf */
static void frame_wait(uint64_t due) {
    for (;;) {
        uint64_t now = hle_now_ns();
        if (now >= due) return;
        uint64_t left = due - now;
        if (left <= 200000u) continue;                  /* the last stretch: no sleep is this exact */
        left -= 200000u;
        struct timespec t = { 0, (long)(left > 4000000u ? 4000000u : left) };
        hle_block_begin();                              /* asleep: the music's thread can have the machine */
        nanosleep(&t, NULL);
        hle_block_end();
    }
}
/* The frame rate as the game's OPTION screen sees it (src/common/frame_option.c).
 * On the Switch it is the display's sixty, or every second refresh's thirty
 * (or a lower rate --fps asked for). */
int fc_rate(void) {
    if (g_game_limiter) return -1;
#ifdef __SWITCH__
    int fps = 60 / (hle_d3d9_vsync > 0 ? hle_d3d9_vsync : 1);
    return g_fps_limit > 0 && g_fps_limit < fps ? g_fps_limit : fps;
#else
    return g_fps_limit;
#endif
}
static void rate_set(int fps) {
#ifdef __SWITCH__
    hle_d3d9_vsync = fps > 0 && fps < 60 ? 60 / fps : 1;
    if (g_fps_limit > 0) g_fps_limit = 0;           /* the display paces it now */
#else
    g_fps_limit = fps;
#endif
}
void fc_rate_choose(int fps) {
    rate_set(fps);
    fprintf(stderr, "[host] frame rate, from the game's OPTION screen: %d\n", fps);
}
void fc_rate_start(int fps) {
    if (g_game_limiter || g_fps_given || fps < 0) return;
#ifdef __SWITCH__
    if (fps != 30) fps = 60;                        /* Uncapped and 120 are a PC's */
#endif
    rate_set(fps);
}

void fc_wrap_004C11A0(void) {
    static uint64_t due;
    if (MEM8(g_esp + 4) && MEM8(FC_CLOCK_COUNTER) && g_fps_limit >= 0) {
        if (g_fps_limit > 0) {
            uint64_t period = 1000000000ull / (uint64_t)g_fps_limit, now = hle_now_ns();
            due += period;
            if (due + period < now || due > now + period) due = now;
            frame_wait(due);
            g_waited_ns += hle_now_ns() - now;
        }
        MEM8(g_esp + 4) = 0;          /* the wait is done: the game's own loop has nothing to add */
    }
    fc_orig_004C11A0();
}

/* MoviePlay (0x004886D0; the file's name, as the game's folder holds it, in
 * ecx): the host plays the whole movie here (movie.c), and a button ends it.
 * It returns with the game's movie state saying "over", which is what the
 * game's own MoviePlay leaves for a movie that is switched off in the
 * settings: 0 is none, 1 playing (drawn and advanced each frame, on
 * DirectShow, which is not here), 2 over. Everything that waits for a movie
 * waits for that 2: the logo's and the opening's own loop (0x00488050), and
 * a scene's script, which asks for it and then stops the movie itself.
 *
 * The button that ended the movie must not reach the game. Its loop reads
 * the buttons once more before it looks at the state, and a confirm or
 * cancel newly down there is its own way of ending a movie: it starts a fade
 * to black and stops the movie when the fade is through. With the movie
 * already over nothing waits for that fade, and it ran on over the title
 * screen, which opened and then went black until a save was loaded. */
#define FC_MOVIE_STATE 0x0079AA3Eu    /* byte */
int fc_movie_play(const char* path, int (*skip)(void));
/* --script-in-movies: a play script's buttons reach a movie too (its clock
 * then runs while one plays), so that a test can end a movie as a player
 * does. Without it a script waits a movie out. */
static int g_script_in_movies;
static uint64_t buttons_now(void) { return g_script_in_movies && g_step < g_nsteps ? scripted_buttons() : pad_buttons(); }
static int movie_skip(void) {
    static uint64_t before = ~0ull;
    uint64_t now = buttons_now() & (B_CONFIRM | B_CANCEL | B_PAD7), pressed = now & ~before;
    before = now;                                    /* a button still down from before the movie does not count */
    return pressed != 0;
}
void fc_wrap_004886D0(void) {
    char name[300];
    snprintf(name, sizeof name, "%s", g_ecx ? (const char*)ADDR(g_ecx) : "");
    for (char* c = name; *c; c++) if (*c == '\\') *c = '/';
    movie_skip();
    fc_movie_play(name, movie_skip);
    g_after_movie = buttons_now();
    MEM8(FC_MOVIE_STATE) = 2;
    g_esp += 4;                                      /* the return address: MoviePlay takes nothing from the stack */
}
/* ReadDirFiles (0x00477580) and ArchiveReadRaw (0x00477180) are wrapped by the
 * mod loader, src/mods/vfs.c, the same file as on Windows. */


/* ---- the sound as mixed, kept as a WAV file (--audio-out) ---------------------- */
static FILE* g_wav;
static uint32_t g_wav_bytes, g_wav_rate;
static void on_audio(const int16_t* stereo, int frames, int rate) {
    if (!g_wav) return;
    g_wav_rate = (uint32_t)rate;
    g_wav_bytes += 4u * (uint32_t)fwrite(stereo, 4, (size_t)frames, g_wav);
}
static void wav_close(void) {
    FILE* f = g_wav;
    uint32_t head[11] = { 0x46464952u, 36 + g_wav_bytes, 0x45564157u, 0x20746D66u, 16, 0x00020001u, g_wav_rate, g_wav_rate * 4,
                          0x00100004u, 0x61746164u, g_wav_bytes };      /* RIFF WAVE fmt: PCM, two channels of 16 bits */
    if (!f) return;
    g_wav = NULL;
    fseek(f, 0, SEEK_SET);
    fwrite(head, 4, 11, f);
    fclose(f);
}
static void wav_open(const char* path) {
    char blank[44] = { 0 };
    if ((g_wav = fopen(path, "wb")) == NULL) { fprintf(stderr, "cannot write %s\n", path); return; }
    fwrite(blank, 1, sizeof blank, g_wav);
    hle_dsound_tap = on_audio;
}

/* ---- test runs: frames counted, pictures saved ------------------------------ */
static const char* g_shots;
/* ---- where the time goes (--perf S; on the Switch every ten seconds unless told otherwise) ----
 * A few lines in the log every S seconds: how many frames, how long the
 * longest, and a frame's time: how much of it its thread was at work (the
 * rest it waited: for the display, the graphics chip, another thread or a
 * file), and its split into drawing (with the programs made and textures
 * filled in it), clearing, presenting and everything else, which is the
 * game's own code; and what was read from files. It is what tells a slow
 * processor from a slow graphics chip from a slow card: the first is a
 * thread at work for nearly all of a long frame, the second a long frame
 * with little work in it and no reads, the third the reads. The wait for the
 * display is in the clearing or the presenting, whichever the driver waits
 * in. */
#ifdef __SWITCH__
static unsigned g_perf_s = 10;
#else
static unsigned g_perf_s;
#endif
static void perf_frame(void) {
    static uint64_t last, since, longest;
    static unsigned frames, over33, over50;
    uint64_t now = hle_now_ns();
    if (!last) { last = since = now; return; }
    uint64_t took = now - last;
    last = now;
    frames++;
    if (took > longest) longest = took;
    if (took > 33400000ull) over33++;
    if (took > 50000000ull) over50++;
    if (now - since < (uint64_t)g_perf_s * 1000000000ull) return;
    hle_perf_t p;
    hle_perf_take(&p);
    double secs = (double)(now - since) / 1e9, per = 1e-6 / (double)frames;      /* ns in all -> ms a frame */
    double frame = secs * 1000.0 / (double)frames, draw = (double)p.draw_ns * per, show = (double)p.present_ns * per;
    double wait = (double)g_waited_ns * per, clear = (double)p.clear_ns * per, worked = (double)g_worked_ns * per;
    g_waited_ns = g_worked_ns = 0;
    const char* how = "";
#ifdef __SWITCH__
    how = appletGetOperationMode() == AppletOperationMode_Console ? (hle_d3d9_vsync > 1 ? " docked, every second refresh;" : " docked;")
                                                                  : (hle_d3d9_vsync > 1 ? " handheld, every second refresh;" : " handheld;");
#endif
    fprintf(stderr, "[perf]%s %.1f frames a second (%u in %.0f s; the longest %.0f ms; %u over 33 ms, %u over 50 ms)\n", how,
            (double)frames / secs, frames, secs, (double)longest / 1e6, over33, over50);
    fprintf(stderr, "[perf]   a frame %.1f ms, its thread at work for %.1f of them: the game %.1f, drawing %.1f (%u draws; %u programs made in %.0f ms; "
            "%u textures filled in %.0f ms, %u of them with a picture GL had), clearing %.1f, presenting %.1f (%.1f of it the swap), "
            "waiting for its time %.1f\n", frame, worked, frame - draw - show - wait - clear, draw, p.draws / frames, p.programs,
            (double)p.program_ns / 1e6, p.textures, (double)p.texture_ns / 1e6, p.textures_seen, clear, show, (double)p.swap_ns * per, wait);
    if (hle_perf_phases)
        fprintf(stderr, "[perf]   of the drawing: finding the program and texture %.1f, state %.1f, uniforms %.1f, vertices %.1f, the call %.1f\n",
                (double)p.phase_ns[0] * per, (double)p.phase_ns[1] * per, (double)p.phase_ns[2] * per, (double)p.phase_ns[3] * per, (double)p.phase_ns[4] * per);
    fprintf(stderr, "[perf]   files: %u reads, %.1f MB, %.0f ms\n", p.reads, (double)p.read_bytes / 1048576.0, (double)p.read_ns / 1e6);
    if (p.skipped) fprintf(stderr, "[perf]   %u draws, clears or presents were not done: another thread kept the graphics context\n", p.skipped);
    since = now, longest = 0, frames = over33 = over50 = 0;
}

static unsigned g_shot_every, g_frames, g_frame_now;
static void on_frame(unsigned frame) {
    g_frame_now = frame;
    if (g_perf_s) {
        static uint64_t last;
        uint64_t now = hle_now_ns();
        frame_work_ns(last ? now - last : 0);
        last = now;
        perf_frame();
    }
    if (g_shot_pending[0]) {
        if (g_shots) {
            char path[700];
            snprintf(path, sizeof path, "%s/%s.png", g_shots, g_shot_pending);
            fprintf(stderr, "[shots] frame %u -> %s%s\n", frame, path, hle_d3d9_save_png(path) == 0 ? "" : " (not written)");
        }
        g_shot_pending[0] = 0;
    }
    if (g_shots && (frame == 1 || (g_shot_every && frame % g_shot_every == 0))) {
        char path[700];
        snprintf(path, sizeof path, "%s/frame_%06u.png", g_shots, frame);
        fprintf(stderr, "[shots] frame %u -> %s%s\n", frame, path, hle_d3d9_save_png(path) == 0 ? "" : " (not written)");
    }
    if (g_frames && frame >= g_frames) {
        fprintf(stderr, "[frames] %u presented with %lu draws: leaving\n", frame, hle_d3d9_draws());
        wav_close();
        profile_write();
        hle_exit(0);
    }
}

/* ---- a fault: say where, in the guest's terms and the host's ------------------ */
#if defined(__linux__)
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
static void on_fault(int sig, siginfo_t* info, void* context) {
    void* frames[24];
    (void)context;
    uintptr_t base = (uintptr_t)hle_g2h(0x10000) - 0x10000, at = (uintptr_t)info->si_addr;
    fprintf(stderr, "\n[fault] signal %d at address %p", sig, info->si_addr);
    if (at >= base && at - base <= 0xFFFFFFFFu) fprintf(stderr, " (the guest's 0x%08X)", (uint32_t)(at - base));
    fprintf(stderr, ", in or under sub_%08X\n", g_cur_func);
    int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, 2);
    _exit(3);
}
static void catch_faults(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL), sigaction(SIGBUS, &sa, NULL), sigaction(SIGFPE, &sa, NULL), sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}
#else
static void catch_faults(void) {}
#endif

/* ---- a profile (--profile FILE, Linux) ---------------------------------------------
 * A timer's signal for every millisecond the process runs; when it lands on
 * the game's thread, the address it interrupted is kept. As the run ends the
 * addresses are written out with where the program was loaded, and
 * tools/profile_portable.py puts names to them from the program's symbols:
 * the lifted functions and the host's own, side by side. */
#if defined(__linux__)
#include <pthread.h>
#include <sys/time.h>
#include <ucontext.h>
/* With each address go the ones it was called from, as far as the unwinder
 * finds them (the lifted code and the host keep no frame pointers): time in
 * a library is of no use without knowing who asked for it. */
#define PROF_DEPTH 8
static uintptr_t (*g_prof)[PROF_DEPTH];
static volatile unsigned g_prof_n;
static unsigned g_prof_room;
static pthread_t g_prof_thread;
static const char* g_prof_path;
static void on_profile(int sig, siginfo_t* info, void* context) {
    void* at[PROF_DEPTH + 4];
    (void)sig, (void)info;
    if (!pthread_equal(pthread_self(), g_prof_thread) || g_prof_n >= g_prof_room) return;
    uintptr_t pc = (uintptr_t)((ucontext_t*)context)->uc_mcontext.gregs[REG_RIP], *to = g_prof[g_prof_n];
    int n = backtrace(at, PROF_DEPTH + 4), from = 0, k = 1;
    while (from < n && (uintptr_t)at[from] != pc) from++;        /* past this handler and the way into it */
    to[0] = pc;
    for (int i = from + 1; i < n && k < PROF_DEPTH; i++) to[k++] = (uintptr_t)at[i];
    while (k < PROF_DEPTH) to[k++] = 0;
    g_prof_n++;
}
static void profile_start(const char* path) {
    struct sigaction sa;
    struct itimerval every = { { 0, 1000 }, { 0, 1000 } };
    void* warm[4];
    backtrace(warm, 4);               /* the unwinder loads itself at its first use: not inside a signal */
    g_prof_room = 1u << 20;
    g_prof = malloc((size_t)g_prof_room * sizeof *g_prof);
    if (!g_prof) return;
    g_prof_path = path, g_prof_thread = pthread_self();
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_profile;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    setitimer(ITIMER_PROF, &every, NULL);
}
static void profile_restart(void) { g_prof_n = 0; }
static void profile_write(void) {
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    char line[512];
    if (!g_prof_path) return;
    setitimer(ITIMER_PROF, &off, NULL);
    FILE* out = fopen(g_prof_path, "w");
    FILE* maps = fopen("/proc/self/maps", "r");
    if (out && maps) {
        while (fgets(line, sizeof line, maps)) if (strstr(line, " r-xp ") || strstr(line, "trails-portable")) fprintf(out, "map %s", line);
        for (unsigned i = 0; i < g_prof_n; i++) {
            for (int k = 0; k < PROF_DEPTH && g_prof[i][k]; k++) fprintf(out, k ? " %lx" : "%lx", (unsigned long)g_prof[i][k]);
            fputc('\n', out);
        }
        fprintf(stderr, "[profile] %u samples of the game's thread in %s\n", g_prof_n, g_prof_path);
    }
    if (out) fclose(out);
    if (maps) fclose(maps);
    g_prof_path = NULL;
}
#elif defined(__SWITCH__)
/* The Switch has no such timer. A thread of the host's own, on another core,
 * stops the game's thread five hundred times a second for as long as it takes
 * to read its registers (the kernel hands them out only for a paused thread),
 * and lets it go on: where it was, where that returns to, and the callers
 * above as far as the frame records lead inside the thread's stack. These
 * are samples of the clock, not of work: a thread that waits is found where
 * it waits. Every ten seconds they are added to the file, so that a run ended
 * from the console's menu leaves what it had. The file starts with where the
 * program was loaded; tools/profile_portable.py names the addresses from
 * trails.elf, the driver's functions among them (it is linked in). */
#define PROF_DEPTH 8
static uintptr_t (*g_prof)[PROF_DEPTH];
static volatile unsigned g_prof_n;
static unsigned g_prof_room;
static Handle g_prof_thread;
static const char* g_prof_path;
static uintptr_t g_prof_lo, g_prof_hi;        /* the game thread's stack */
static pthread_mutex_t g_prof_lock = PTHREAD_MUTEX_INITIALIZER;
extern char _start[], __end__[];      /* the program as loaded: its first byte, and past its last */
static void profile_flush(void) {
    pthread_mutex_lock(&g_prof_lock);
    FILE* out = g_prof_n ? fopen(g_prof_path, "a") : NULL;
    if (out) {
        for (unsigned i = 0; i < g_prof_n; i++) {
            for (int k = 0; k < PROF_DEPTH && g_prof[i][k]; k++) fprintf(out, k ? " %lx" : "%lx", (unsigned long)g_prof[i][k]);
            fputc('\n', out);
        }
        fclose(out);
    }
    g_prof_n = 0;
    pthread_mutex_unlock(&g_prof_lock);
}
static void* profile_sampler(void* arg) {
    unsigned since = 0;
    (void)arg;
    hle_thread_aside();
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);       /* ahead of what it shares its core with: a sample is due when it is due */
    for (;;) {
        ThreadContext c;
        svcSleepThread(2000000);
        if (g_prof_n < g_prof_room && R_SUCCEEDED(svcSetThreadActivity(g_prof_thread, ThreadActivity_Paused))) {
            if (R_SUCCEEDED(svcGetThreadContext3(&c, g_prof_thread))) {
                uintptr_t* to = g_prof[g_prof_n];
                uintptr_t fp = (uintptr_t)c.fp;
                int k = 0;
                to[k++] = (uintptr_t)c.pc.x;
                to[k++] = (uintptr_t)c.lr;
                while (k < PROF_DEPTH && fp >= g_prof_lo && fp + 16 <= g_prof_hi && !(fp & 15)) {
                    uintptr_t next = ((const uintptr_t*)fp)[0], ret = ((const uintptr_t*)fp)[1];
                    if (ret && ret != to[k - 1]) to[k++] = ret;
                    if (next <= fp) break;
                    fp = next;
                }
                while (k < PROF_DEPTH) to[k++] = 0;
                g_prof_n++;
            }
            svcSetThreadActivity(g_prof_thread, ThreadActivity_Runnable);
        }
        if (++since >= 5000) since = 0, profile_flush();
    }
    return NULL;
}
static void profile_start(const char* path) {
    MemoryInfo mi;
    u32 page;
    pthread_t t;
    FILE* out = fopen(path, "w");
    if (!out) { fprintf(stderr, "[profile] cannot write %s\n", path); return; }
    fprintf(out, "map %lx-%lx r-xp 00000000 00:00 0 trails.elf\n", (unsigned long)(uintptr_t)_start, (unsigned long)(uintptr_t)__end__);
    fclose(out);
    g_prof_room = 8192;
    g_prof = malloc((size_t)g_prof_room * sizeof *g_prof);
    if (!g_prof || R_FAILED(svcQueryMemory(&mi, &page, (u64)(uintptr_t)&mi))) return;
    g_prof_lo = (uintptr_t)mi.addr, g_prof_hi = (uintptr_t)(mi.addr + mi.size);
    g_prof_path = path, g_prof_thread = threadGetCurHandle();
    if (pthread_create(&t, NULL, profile_sampler, NULL)) { g_prof_path = NULL; return; }
    pthread_detach(t);
    fprintf(stderr, "[profile] the game's thread is sampled into %s (the program is at %lx)\n", path, (unsigned long)(uintptr_t)_start);
}
static void profile_restart(void) { g_prof_n = 0; }
static void profile_write(void) { if (g_prof_path) profile_flush(); }
#else
static void profile_start(const char* path) { (void)path; }
static void profile_write(void) {}
static void profile_restart(void) {}
#endif

/* ---- where the game is, said from a thread of the host's own -------------------- */
#include <pthread.h>
#include <time.h>
static unsigned g_watch;
static void* watch_thread(void* arg) {
    struct timespec nap = { (time_t)g_watch, 0 };
    unsigned long last[HLE_THREADS_SEEN] = { 0 };
    (void)arg;
    hle_thread_aside();
    for (unsigned t = g_watch;; t += g_watch) {
        nanosleep(&nap, NULL);
        fprintf(stderr, "[watch] %u s: frame %u, %lu draws, the renderer is %s\n", t, g_frame_now, hle_d3d9_draws(), hle_d3d9_doing);
        for (int i = 0; i < HLE_THREADS_SEEN; i++) {
            unsigned long calls = hle_threads[i].calls;
            if (!hle_threads[i].shim) continue;
            fprintf(stderr, "[watch]   thread %d: %lu calls out since the last line; the last was %s from sub_%08X, %s\n", i,
                    calls - last[i], hle_threads[i].shim, hle_threads[i].from, hle_threads[i].inside ? "not yet back" : "returned");
            last[i] = calls;
        }
    }
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "\n[not-lifted] sub_%08X (called from 0x%08X)\n", va, g_cur_func);
    hle_exit(2);
}

static int g_unanswered;
static uint32_t resolve_and_count(const char* name) {
    uint32_t va = hle_resolve(name);
    if (va) return va;
    g_unanswered++;
    fprintf(stderr, "  unanswered: %s\n", name);
    return hle_resolve_or_stub(name);
}

static void usage(void) {
    fprintf(stderr,
        "usage: trails-portable [options] [path to ed6_win_DX9.exe]\n"
        "  the game's folder is the working directory; with no path, the game is looked for there and\n"
        "  then beside this program. On the Switch the options are read from args.txt beside it.\n"
        "  --saves DIR          the game's Saved Games folder (default: save)\n"
        "  --fps N              frames a second (0 = no limit); also the game's Frame Rate option\n"
        "  --original-limiter   the game's own frame waits, and its own Frame Rate row\n"
        "  --mods DIR           the folder of mods (src/mods/vfs.c); on the Switch, mods beside the program\n"
        "  --voice-log          say which voice file each voiced line plays\n"
        "  --movie-log          a line a second while a movie plays\n"
        "  --headless           draw with no window (a test run)\n"
        "  --frames N           leave after N frames have been presented\n"
        "  --script FILE        play a script of button presses (tests/*.txt)\n"
        "  --script-in-movies   let a script's buttons reach a movie too\n"
        "  --shots DIR          save the picture there as PNG: the first frame,\n"
        "  --shot-every N         and every Nth after it\n"
        "  --perf S             say every S seconds where the frames' time went (the Switch: every 10; 0 = not)\n"
        "  --perf-detail        the drawing's part of that, split up\n"
        "  --profile FILE       sample the game's thread (tools/profile_portable.py names the samples)\n"
        "  --watch S            say every S seconds where the game is\n"
        "  --hot-check          check every answer of the game functions done natively against the original\n"
        "  --no-hot             the original game functions only\n"
        "  --no-stream          vertices in a buffer a draw, not one stream\n"
        "  --no-aside           the host's threads left on the core they start on\n"
        "  --pictures N, --no-pictures   how many pictures GL keeps to use again (default 512)\n"
        "  --read-quarter       read pictures back a quarter at a time (in-game pictures under Ryujinx)\n"
        "  --d3d-log            list render targets, viewports and what is read back\n"
        "  --audio-out FILE     write everything the game's mixer puts out to a WAV file\n"
        "  --trace PREFIX       list every call out whose name starts so (IDirect3D...)\n"
        "  --imports            list the imports with no answer and stop\n");
}

int main(int argc, char** argv) {
    const char* exe = NULL;
    const char* saves = "save";       /* --saves DIR: where the game's Saved Games folder is */
    const char* mods = NULL;
    int list_only = 0, voice_log = 0, perf_detail = 0;
    const char* profile = NULL;
#ifdef __SWITCH__
    static char text[2048];
    static char* words[64];
    switch_start(argc, argv);
    /* The console gives a core's time by turns only to threads of priority
     * 59, which is where the C library starts every thread but this one. The
     * main thread starts above them, and wherever the game polls for another
     * thread's work it would never let that thread run. */
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    hle_on_exit = log_done;
    atexit(log_done_at_exit);
    saves = g_saves;
    hle_d3d9_vsync = 1;
    {
        FILE* f = fopen(g_args, "r");
        size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f) fclose(f);
        text[n] = 0;
        argc = 1, argv = words;
        for (char* w = strtok(text, " \t\r\n"); w && argc < 63; w = strtok(NULL, " \t\r\n")) words[argc++] = w;
    }
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--imports")) list_only = 1;
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves = argv[++i];
        else if (!strcmp(argv[i], "--headless")) hle_d3d9_headless = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_frames = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) g_shots = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_shot_every = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) g_watch = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--perf") && i + 1 < argc) g_perf_s = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--perf-detail")) perf_detail = 1;
        else if (!strcmp(argv[i], "--no-stream")) hle_d3d9_stream = 0;
        else if (!strcmp(argv[i], "--read-quarter")) hle_d3d9_read_quarter = 1;
        else if (!strcmp(argv[i], "--no-pictures")) hle_d3d9_pictures = 0;
        else if (!strcmp(argv[i], "--pictures") && i + 1 < argc) hle_d3d9_pictures = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-aside")) hle_threads_aside = 0;
        else if (!strcmp(argv[i], "--script-in-movies")) g_script_in_movies = 1;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_fps_limit = atoi(argv[++i]), g_fps_given = 1;
        else if (!strcmp(argv[i], "--original-limiter")) g_fps_limit = -1, g_game_limiter = 1;
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(argv[i], "--d3d-log")) hle_d3d9_log = 1;
        else if (!strcmp(argv[i], "--audio-out") && i + 1 < argc) wav_open(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) load_script(argv[++i]);
        else if (!strcmp(argv[i], "--mods") && i + 1 < argc) mods = argv[++i];
        else if (!strcmp(argv[i], "--voice-log")) voice_log = 1;
        else if (!strcmp(argv[i], "--movie-log")) { extern int fc_movie_log; fc_movie_log = 1; }
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) setenv("HLE_SHIMTRACE", argv[++i], 1);
        /* the game's functions done natively (src/common/hot.c): each answer checked against the original, or none */
        else if (!strcmp(argv[i], "--hot-check")) setenv("FC_HOT_CHECK", "1", 1);
        else if (!strcmp(argv[i], "--no-hot")) setenv("FC_HOT_OFF", "1", 1);
        else if (argv[i][0] == '-') fprintf(stderr, "[host] %s is not an option here: left out (docs/building.md lists them)\n", argv[i]);
        else exe = argv[i];
    }
#ifdef __SWITCH__
    if (!exe) exe = g_exe;
#else
    /* With no exe named: the game's in the working directory, or in the
     * program's own folder (a player's install, the program put beside the
     * game's exe), which then becomes the working directory. */
    if (!exe) {
        static char own[1024];
        struct stat st;
        ssize_t n = readlink("/proc/self/exe", own, sizeof own - 1);
        char* cut = n > 0 ? (own[n] = 0, strrchr(own, '/')) : NULL;
        if (stat(FC_GAME_EXE, &st) == 0) exe = FC_GAME_EXE;
        else if (cut && (*cut = 0, chdir(own) == 0) && stat(FC_GAME_EXE, &st) == 0) exe = FC_GAME_EXE;
    }
#endif
#ifdef __SWITCH__
    if (g_fps_limit > 0 && g_fps_limit <= 60 && 60 % g_fps_limit == 0) hle_d3d9_vsync = 60 / g_fps_limit, g_fps_limit = 0;
#endif
    fprintf(stderr, "[host] frames: %s; vertices %s; other threads %s%s\n",
            g_fps_limit < 0 ? "the game's own limiter" : g_fps_limit > 0 ? "the host's limiter" :
            hle_d3d9_vsync > 1 ? "every second or third of the display's (--fps)" : "no limit but the display's",
            hle_d3d9_stream ? "in one stream" : "in a buffer a draw (--no-stream)",
            hle_threads_aside ? "kept off the first one's core" : "left where they start (--no-aside)",
            hle_d3d9_pictures ? "; pictures GL has used again" : "; every picture sent (--no-pictures)");
    if (hle_d3d9_pictures) fprintf(stderr, "[host] up to %d pictures kept by GL (--pictures N)\n", hle_d3d9_pictures);
    if (g_shots) mkdir(g_shots, 0755);
    /* The game's folder is the working directory (the game counts on it):
     * SoraVoice's files are looked for there, then the mod folders. */
    mods_scan_game(".");
#ifdef __SWITCH__
    if (!mods) { static char beside[400]; snprintf(beside, sizeof beside, "%s/mods", g_home); mods = beside; }
#endif
    if (mods) mods_scan(mods);
    fc_voice_init(voice_log);
    hle_d3d9_title = "Trails in the Sky FC";
    hle_d3d9_frame_hook = on_frame;
    hle_perf_on = g_perf_s != 0;
    hle_perf_phases = hle_perf_on && perf_detail;
    if (g_watch) {
        pthread_t t;
        hle_threads_watched = 1;
        pthread_create(&t, NULL, watch_thread, NULL);
    }
#ifndef __SWITCH__
    setvbuf(stderr, NULL, _IONBF, 0);
#endif
    catch_faults();
    if (!exe) {
        usage();
        return 1;
    }
    mkdir(saves, 0755);
    fc_set_saves_folder(saves);
    win32hle_register(fc_win32_wide);       /* first: this game's own answers win */
    recomp_host_init();
    pe_image img;
    if (recomp_pe_map(exe, &img) != 0) { fprintf(stderr, "cannot map %s\n", exe); return 1; }
    {
        char guest[1024];
        hle_guest_path(exe, guest, sizeof guest);
        hle_module_add(guest, img.base, img.resource_rva, img.resource_size, 1);
        hle_set_command_line(guest);
    }
    fprintf(stderr, "Trails in the Sky FC, portable host: image at 0x%08X, entry 0x%08X, %u lifted functions\n",
            img.base, img.entry, recomp_dispatch_count);
    recomp_pe_bind(&img, resolve_and_count);
    fprintf(stderr, "%d imports have no answer yet\n", g_unanswered);
    if (list_only) return 0;
    fprintf(stderr, "entering 0x%08X\n\n", fc_entry_va);
    if (profile) profile_start(profile);
    uint32_t rc = hle_call_guest(fc_entry_va, 0, NULL);
    fprintf(stderr, "\nentry returned eax=%08X\n", rc);
#ifdef __SWITCH__
    log_done((int)rc);
#endif
    return (int)rc;
}
