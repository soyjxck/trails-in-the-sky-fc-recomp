/* The sampling profiler (profile.c): --profile FILE. */
#ifndef FC_PROFILE_H
#define FC_PROFILE_H
#include <windows.h>

/* Start sampling `thread`; the report goes to out_path when the run ends. */
void profile_start(HANDLE thread, const char* out_path);

/* Stop and write the report. Safe to call when not profiling, and more than once. */
void profile_report(void);

#endif
