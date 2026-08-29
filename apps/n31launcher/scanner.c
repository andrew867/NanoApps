/*
 * scanner.c — see scanner.h.
 *
 * One worker, one mutex, one condition variable, and two lists: the one the
 * worker is filling and the one waiting to be collected. The main thread only
 * ever holds the mutex long enough to copy a pointer's worth of state, so it
 * cannot be made to wait on the filesystem even when the worker is deep in a
 * NAND read.
 */

#include "scanner.h"

#include <pthread.h>
#include <string.h>

static pthread_t       s_thread;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_wake = PTHREAD_COND_INITIALIZER;

static bool s_running;          /* the worker should keep going */
static bool s_started;          /* the thread exists */
static bool s_requested;        /* a scan has been asked for */
static bool s_busy;             /* the worker is in a scan right now */
static bool s_ready;            /* s_result holds something to collect */
static bool s_invalidate;       /* forget the fingerprint before the next one */

/* Filled by the worker, read by the main thread, both under the lock. It is a
   couple of kilobytes; copying it is far cheaper than the scan that made it. */
static n31_app_list_t s_result;

/*
 * The worker's own scan state. Only the worker touches it, which is the point
 * of the state being caller-owned rather than a static inside apps.c.
 */
static n31_scan_state_t s_state;

static void *worker(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&s_lock);
    while (s_running) {
        if (!s_requested) {
            pthread_cond_wait(&s_wake, &s_lock);
            continue;
        }
        s_requested = false;
        s_busy = true;

        bool forget = s_invalidate;
        s_invalidate = false;

        /* Everything below happens with the lock DROPPED. It is the whole
           reason this thread exists: a scan can sit in a NAND read for a long
           time, and holding the lock across it would block the main thread at
           its next collect - which is exactly the stall being avoided. */
        pthread_mutex_unlock(&s_lock);

        if (forget) s_state.valid = false;

        n31_app_list_t local;
        bool changed = n31_apps_scan_into(&local, &s_state);

        pthread_mutex_lock(&s_lock);
        if (changed) {
            s_result = local;
            s_ready = true;
        }
        s_busy = false;
    }
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

bool n31_scanner_start(void)
{
    if (s_started) return true;

    s_running = true;
    if (pthread_create(&s_thread, NULL, worker, NULL) != 0) {
        s_running = false;
        return false;
    }
    s_started = true;
    return true;
}

void n31_scanner_stop(void)
{
    if (!s_started) return;

    pthread_mutex_lock(&s_lock);
    s_running = false;
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_lock);

    pthread_join(s_thread, NULL);
    s_started = false;
}

void n31_scanner_request(void)
{
    if (!s_started) return;

    pthread_mutex_lock(&s_lock);
    s_requested = true;
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_lock);
}

void n31_scanner_invalidate(void)
{
    if (!s_started) return;

    pthread_mutex_lock(&s_lock);
    s_invalidate = true;
    s_requested = true;
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_lock);
}

bool n31_scanner_busy(void)
{
    if (!s_started) return false;

    pthread_mutex_lock(&s_lock);
    bool busy = s_busy || s_requested;
    pthread_mutex_unlock(&s_lock);
    return busy;
}

bool n31_scanner_collect(void)
{
    if (!s_started) return false;

    n31_app_list_t list;

    pthread_mutex_lock(&s_lock);
    if (!s_ready) {
        pthread_mutex_unlock(&s_lock);
        return false;
    }
    list = s_result;
    s_ready = false;
    pthread_mutex_unlock(&s_lock);

    /*
     * The worker reports a changed fingerprint, which is not quite the same as
     * a changed list - a folder touched without being altered would do it. So
     * compare before telling the caller to redraw, or the selection would be
     * recomputed and the screen repainted for nothing.
     */
    bool same = (list.count == n31_app_count) &&
                memcmp(list.apps, n31_apps,
                       sizeof(n31_app_t) * list.count) == 0;

    n31_apps_publish(&list);
    return !same;
}
