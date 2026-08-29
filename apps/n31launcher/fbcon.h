/*
 * fbcon.h — sharing the screen with the kernel console.
 *
 * The framebuffer console and the launcher draw to the same pixels, so every
 * kernel message lands on top of the UI. Turning console logging off entirely
 * would be the wrong fix: the console is how a boot that goes wrong is
 * diagnosed, and that has to keep working right up until there is something
 * else on the screen.
 *
 * So the console is only detached, and only once the launcher is ready to draw.
 * Boot logs to it exactly as before. On the way out it is reattached, and the
 * kernel messages from the whole session are still in the ring buffer, so
 * exiting the launcher gives back a working console rather than a blank one.
 *
 * It is also lent back mid-session, for apps that ARE terminal programs. Those
 * draw through the console, so for them the console is not something in the way
 * - it is the display. Lending is separate from restoring because the launcher
 * has to take the screen again afterwards, and needs to remember that it was
 * the one holding it.
 *
 * Only the framebuffer console is touched. A serial console is a different
 * console and keeps everything throughout, which is what you want when the
 * thing you are debugging is the launcher.
 */

#ifndef N31_LAUNCHER_FBCON_H
#define N31_LAUNCHER_FBCON_H

#include <stdbool.h>

/* Detach the framebuffer console. Returns false if it could not be done, in
   which case nothing has changed and the console still owns the screen. */
bool n31_fbcon_detach(void);

/* Give it back for a console app to draw through, and take it again after.
   Both are no-ops if we never held it. */
void n31_fbcon_lend(void);
void n31_fbcon_reclaim(void);

/* Blank the terminal, so an app that is about to draw through it does not
   start on top of whatever was last printed there. */
void n31_fbcon_clear(void);

/*
 * Take the console back if something rebound it behind us.
 *
 * There is no event for this, so it is a poll: cheap enough to run alongside
 * everything else, and the alternative is that one module load or mode set
 * hands the screen back to the console for the rest of the session, with every
 * kernel message drawing over whatever is running.
 *
 * Only ever re-takes a console this process detached in the first place, and
 * does nothing while it is lent to a console app. Returns true if it had to
 * take it back, which means the console has been drawing and the screen needs
 * repainting.
 */
bool n31_fbcon_reassert(void);

/* Reattach whatever detach took, if anything. Safe to call unconditionally. */
void n31_fbcon_restore(void);

#endif /* N31_LAUNCHER_FBCON_H */
