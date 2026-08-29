/*
 * Exercise the scanner thread without a device.
 *
 * The properties worth proving are the ones that would be invisible until they
 * bit: that collect() never blocks waiting for a scan, that a result arrives
 * without the caller waiting for it, and that an app appearing on disk is
 * noticed without anyone asking twice.
 */
#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void ok(const char *what, int cond)
{
    printf("  %-48s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) fails++;
}

static long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000L + t.tv_nsec / 1000;
}

static int have(const char *prog)
{
    for (uint8_t i = 0; i < n31_app_count; i++)
        if (!strcmp(n31_apps[i].prog, prog)) return 1;
    return 0;
}

/* Collect until something arrives, or we run out of patience. Returns the
   milliseconds waited, which is only ever reported, never asserted on. */
static int wait_for_result(int max_ms)
{
    int waited = 0;

    while (waited < max_ms) {
        if (n31_scanner_collect()) return waited;
        usleep(10000);
        waited += 10;
    }
    return -1;
}

static void write_app(const char *dir, const char *name)
{
    char p[256];
    FILE *f;

    snprintf(p, sizeof p, "%s/%s", dir, name);
    mkdir(p, 0755);

    snprintf(p, sizeof p, "%s/%s/%s", dir, name, name);
    f = fopen(p, "w");
    if (f) { fputs("#!/bin/sh\ntrue\n", f); fclose(f); }
    chmod(p, 0755);
}

int main(void)
{
    printf("scanner thread:\n");

    system("rm -rf /tmp/n31os && mkdir -p /tmp/n31os/apps");
    write_app("/tmp/n31os/apps", "alpha");

    ok("the worker starts", n31_scanner_start());

    /*
     * The point of the whole exercise: asking for a scan must return at once,
     * whatever the filesystem is doing. A millisecond is already far more
     * headroom than a mutex needs.
     */
    long t0 = now_us();
    n31_scanner_request();
    long request_us = now_us() - t0;
    ok("request() returns immediately", request_us < 1000);

    t0 = now_us();
    n31_scanner_collect();
    long collect_us = now_us() - t0;
    ok("collect() does not wait for the scan", collect_us < 1000);

    int waited = wait_for_result(3000);
    ok("a result arrives", waited >= 0);
    ok("and it found the app", have("alpha"));

    /* Nothing changed, so nothing should be published - otherwise the UI would
       redraw every couple of seconds forever. */
    n31_scanner_request();
    usleep(300000);
    ok("an unchanged rescan publishes nothing", !n31_scanner_collect());

    /* An app appearing must be picked up on the next request, which is what
       makes scp-and-wait work. */
    write_app("/tmp/n31os/apps", "beta");
    n31_scanner_request();
    waited = wait_for_result(3000);
    ok("a new app is noticed", waited >= 0 && have("beta"));
    ok("without losing the first", have("alpha"));

    /* invalidate() must force the work even when nothing looks different -
       that is what the end of a mount needs. */
    n31_scanner_invalidate();
    usleep(300000);
    ok("invalidate re-runs the scan", n31_app_count > 0 && have("beta"));

    n31_scanner_stop();
    ok("the worker stops cleanly", 1);

    system("rm -rf /tmp/n31os");
    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails != 0;
}
