/*
 * klog.h — the last thing the kernel said about storage.
 *
 * Bringing the volume up takes about a minute and happens before this program
 * has anything to list. During that minute the launcher would otherwise show
 * "None yet" and look like it had finished and found nothing, which is exactly
 * the impression to avoid - so it shows the kernel's own running commentary
 * instead, and stops once a volume is mounted.
 *
 * /dev/kmsg rather than shelling out to dmesg: no process per poll, no output
 * to parse twice, and it can be positioned at the end so only new messages
 * arrive. Reads never block.
 *
 * Filtered to storage, because everything else is noise here. A line about the
 * touch panel scrolling past while someone waits for their music is worse than
 * no line at all.
 */

#ifndef N31_LAUNCHER_KLOG_H
#define N31_LAUNCHER_KLOG_H

#include <stdbool.h>
#include <stddef.h>

/* Open the kernel log positioned at the end, so only what happens from now on
   is reported. False if it is not readable, which is not fatal - there is
   simply nothing to show. */
bool n31_klog_open(void);
void n31_klog_close(void);

/*
 * The most recent storage-related line since the last call, written to `out`.
 * Returns false when there is nothing new, leaving `out` alone. Never blocks.
 */
bool n31_klog_poll(char *out, size_t cap);

#endif /* N31_LAUNCHER_KLOG_H */
