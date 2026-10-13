/*
 * pace.h -- the display's rate, or half of it.
 *
 * Where Present waits for the display (the Switch), a frame that is not ready
 * in time is shown a refresh late. A scene the machine can almost draw at the
 * display's rate then comes out as frames of one refresh and of two, mixed:
 * forty-odd a second on paper, and to the eye a stutter. Half the display's
 * rate, held evenly, looks smoother than that.
 *
 * This decides which of the two to ask for, from how the frames have been
 * going: down to every second refresh when frames keep coming late, back up
 * when the work in a frame has for a while been short enough to fit in one
 * refresh with room to spare. Going back up and falling again at once (work
 * that only looked short: time spent waiting on the graphics chip is not
 * seen) makes it wait twice as long before the next try.
 */
#ifndef FC_PACE_H
#define FC_PACE_H

typedef struct {
    int interval;                   /* refreshes a frame: 1 or 2 */
    unsigned frames, late, busy;    /* in the window being counted */
    unsigned calm, need;            /* calm windows in a row, and how many it takes to go back up */
    double rose_at;                 /* when it last went back up, in seconds; below 0: never */
    double work_sum, fell_work;     /* the window's work so far; a frame's work when it last fell */
} fc_pace;

void fc_pace_init(fc_pace* p);
/* A frame has been presented: `took_ms` since the one before, `work_ms` of it
 * not spent waiting in Present; `now_s` is any clock in seconds, `refresh_ms`
 * the display's period. Returns the interval to present at from here on. */
int fc_pace_frame(fc_pace* p, double now_s, double took_ms, double work_ms, double refresh_ms);

#endif
