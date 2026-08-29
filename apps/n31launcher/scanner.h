/*
 * scanner.h — looking for apps, off the thread that draws.
 *
 * Finding apps means reading directories and manifests, and on this device
 * those live on NAND behind an FTL. A read that takes a moment on tmpfs can
 * take a great deal longer there, especially just after a mount when nothing
 * is cached - and every one of those moments is a frame the launcher did not
 * draw and a button press it did not answer.
 *
 * So the filesystem work happens on a worker thread and the result is handed
 * over as a finished list. The worker never touches LVGL or any global the UI
 * reads: LV_USE_OS is LV_OS_NONE in this build, so LVGL is not thread-safe and
 * only the main thread is allowed near it. The division is absolute - the
 * worker produces a n31_app_list_t and nothing else.
 */

#ifndef N31_LAUNCHER_SCANNER_H
#define N31_LAUNCHER_SCANNER_H

#include <stdbool.h>

#include "apps.h"

/*
 * Start the worker. Returns false if the thread could not be created, in which
 * case the caller should fall back to scanning inline - a launcher that stutters
 * is better than one with no apps in it.
 */
bool n31_scanner_start(void);
void n31_scanner_stop(void);

/* Ask for a scan. Returns immediately; ignored if one is already in flight. */
void n31_scanner_request(void);

/*
 * Throw away what the worker remembers, so the next scan does the full work.
 * For the moment a volume finishes mounting: folders that were unreadable a
 * second ago say nothing about what is on them now.
 */
void n31_scanner_invalidate(void);

/*
 * Publish a finished scan into the globals the UI reads, if one is waiting.
 * Returns true if the app list actually changed and the screen needs redrawing.
 * Main thread only, and it never blocks.
 */
bool n31_scanner_collect(void);

/* Is a scan in flight? The caller uses this to avoid asking again. */
bool n31_scanner_busy(void);

#endif /* N31_LAUNCHER_SCANNER_H */
