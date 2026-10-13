/*
 * pace.c -- the display's rate, or half of it (pace.h).
 */
#include "pace.h"

#define FULL_WINDOW 120u            /* frames counted at the display's rate: two seconds */
#define FULL_LATE   18u             /* this many of them late, and it is half the rate from here */
#define HALF_WINDOW 90u             /* frames counted at half the rate: three seconds */
#define HALF_BUSY   4u              /* at most this many of them too long for one refresh, for the window to be calm */
#define NEED_FIRST  2u              /* calm windows before going back up */
#define NEED_MOST   16u
#define SOON_S      20.0            /* falling again within this of going up doubles the wait */
#define SETTLED_S   60.0            /* and this long at the display's rate forgets it */

void fc_pace_init(fc_pace* p) {
    p->interval = 1;
    p->frames = p->late = p->busy = p->calm = 0;
    p->need = NEED_FIRST;
    p->rose_at = -1.0;
    p->work_sum = p->fell_work = 0.0;
}

int fc_pace_frame(fc_pace* p, double now_s, double took_ms, double work_ms, double refresh_ms) {
    p->frames++;
    p->work_sum += work_ms;
    if (p->interval == 1) {
        if (took_ms > refresh_ms * 1.2) p->late++;
        if (p->late >= FULL_LATE) {
            if (p->rose_at >= 0.0 && now_s - p->rose_at < SOON_S) p->need = p->need * 2 > NEED_MOST ? NEED_MOST : p->need * 2;
            p->fell_work = p->work_sum / p->frames;
            p->interval = 2;
            p->frames = p->late = p->busy = p->calm = 0;
            p->work_sum = 0.0;
        } else if (p->frames >= FULL_WINDOW) {
            if (p->rose_at >= 0.0 && now_s - p->rose_at > SETTLED_S) p->need = NEED_FIRST;
            p->frames = p->late = 0;
            p->work_sum = 0.0;
        }
    } else {
        if (work_ms > refresh_ms * 0.78) p->busy++;
        if (p->frames >= HALF_WINDOW) {
            /* a scene much lighter than the one it fell in is another scene: no reason left to be wary */
            if (p->work_sum / p->frames < p->fell_work * 0.7) p->need = NEED_FIRST;
            p->calm = p->busy <= HALF_BUSY ? p->calm + 1 : 0;
            p->frames = p->busy = 0;
            p->work_sum = 0.0;
            if (p->calm >= p->need) {
                p->interval = 1;
                p->rose_at = now_s;
                p->frames = p->late = p->calm = 0;
                p->work_sum = 0.0;
            }
        }
    }
    return p->interval;
}
