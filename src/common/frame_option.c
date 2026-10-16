/* The game's own frame-rate option, done again.
 *
 * The game's OPTION screen has a row, Frame Rate: Automatic, 30 FPS Limit,
 * 60 FPS Limit (FpsMode in config.ini, the byte at 0x007A8591: 0, 1, 2). The
 * game keeps a limit with a wait in its frame loop just before Present:
 * Sleep(0) until more than 32 or 16 whole milliseconds have passed since the
 * last Present returned, which comes out at an uneven 30 and at 57.5 to 57.8
 * a second (a refresh shown twice, twice a second). Automatic is the 125 a
 * second of the game's other wait, which the host keeps already.
 *
 * Here the row is the host's frame rate, the one the Windows settings menu
 * (F2) and --fps also set: Uncapped, 120, 60 and 30 FPS Limit on a PC; 60
 * and 30 on the Switch, whose display shows 60. The game's wait is told
 * there is nothing to wait for and the host paces the frames, exactly or by
 * the display. The byte says which rate is in force and the game saves it as
 * it saves the rest: 0 Uncapped, 1 30, 2 60, 3 120, 4 a rate the settings
 * menu or --fps chose that the row has no place for (shown by its number). At
 * start the byte the game read from its settings is the rate where nothing
 * else (--fps, the Windows settings) has said one.
 *
 * The lifted game calls here from three places (run_lift.py PATCHES):
 *   0x004738B0  a frame is about to be presented, the byte read: fc_fps_mode
 *   0x004E3FCD  the row was pressed and the game has stored its next value:
 *   0x004E3FE9  fc_fps_next stores the host's next instead
 *   0x004E4E54  the row's label has been picked: fc_fps_label
 * With --original-limiter the game's waits and its row are as they were.
 *
 * Each host has the rate (src/runtime/host.c, src/portable/main.c):
 *   int  fc_rate(void)            frames a second, 0 none, -1 the game's own waits
 *   void fc_rate_choose(int fps)  the player chose it on the row: at once, and kept
 *   void fc_rate_start(int fps)   what the saved byte asks for at start (-1 nothing)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"

#define FC_FPS_OPTION   0x007A8591u   /* byte: FpsMode */

int  fc_rate(void);
void fc_rate_choose(int fps);
void fc_rate_start(int fps);

/* The row's rates, in the order a press steps through them: from the top
 * down, as the game's own went (Automatic, 60, 30, Automatic). */
#ifdef __SWITCH__
static const int k_rates[] = { 60, 30 };
#else
static const int k_rates[] = { 0, 120, 60, 30 };
#endif
#define RATES ((int)(sizeof k_rates / sizeof k_rates[0]))

static int option_of(int fps) { return fps == 0 ? 0 : fps == 30 ? 1 : fps == 60 ? 2 : fps == 120 ? 3 : 4; }
static int fps_of(int option) { return option == 0 ? 0 : option == 1 ? 30 : option == 2 ? 60 : option == 3 ? 120 : -1; }

int fc_fps_mode(int option) {
    static int started;
    if (!started) started = 1, fc_rate_start(fps_of(option));
    int fps = fc_rate();
    if (fps < 0) return option;
    MEM8(FC_FPS_OPTION) = (uint8_t)option_of(fps);
    return 0;                                       /* the game's own wait: nothing to wait for */
}

void fc_fps_next(void) {
    int fps = fc_rate(), next = k_rates[0];
    if (fps < 0) return;
    for (int i = 0; i < RATES; i++)
        if (k_rates[i] == fps) { next = k_rates[(i + 1) % RATES]; goto chosen; }
    for (int i = 0; i < RATES; i++)                 /* one the row has no place for: the next below it */
        if (k_rates[i] != 0 && (fps == 0 || k_rates[i] < fps)) { next = k_rates[i]; break; }
chosen:
    fc_rate_choose(next);
    MEM8(FC_FPS_OPTION) = (uint8_t)option_of(next);
}

/* Where a label is handed to the game: guest memory of the host's. The game
 * copies it out at once (into the line it draws), so one will do. */
#if defined(HLE_WITH_D3D9)
uint32_t hle_guest_alloc(uint32_t n);
static uint32_t label_at(void) {
    static uint32_t va;
    if (!va) va = hle_guest_alloc(32);
    return va;
}
#else
static uint32_t label_at(void) {                    /* the guest's addresses are the host's own */
    static char label[32];
    return (uint32_t)(uintptr_t)label;
}
#endif

uint32_t fc_fps_label(uint32_t picked, int english) {
    int fps = fc_rate();
    uint32_t va = label_at();
    char text[32];
    if (fps < 0 || !va) return picked;
    if (fps == 0) snprintf(text, sizeof text, "%s", english ? "Uncapped" : "\x96\xb3\x90\xa7\x8c\xc0");   /* 無制限 */
    else snprintf(text, sizeof text, english ? "%d FPS Limit" : "%dFPS\x8c\xc5\x92\xe8", fps);       /* 固定, as the game's own */
    for (uint32_t i = 0; i <= (uint32_t)strlen(text); i++) MEM8(va + i) = (uint8_t)text[i];
    return va;
}
