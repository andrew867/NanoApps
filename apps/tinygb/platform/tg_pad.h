/*
 * tg_pad.h — the on-screen controls, and the one place their geometry lives.
 *
 * The emulator draws 160x144 scaled by three halves, which is 240x216 on a
 * 240x432 panel - so exactly half the screen is the game and the other half
 * has been empty since the port started. tg_fb_layout already puts the picture
 * hard against the top for this reason. This is what goes underneath.
 *
 * Drawing and hit-testing are the same rectangles, which is why they are in
 * one file. Two copies of a button's position is a button that looks like it
 * is somewhere it is not, and the failure is a control that misses by ten
 * pixels in a way nobody can see - only feel.
 *
 * MULTITOUCH IS NOT OPTIONAL HERE. A Game Boy is held with two thumbs and
 * almost every game needs a direction and A at the same time; a pad that reads
 * one contact is a pad you cannot run and jump with. So the caller feeds every
 * active contact through tg_pad_hit and ORs the results, and the driver's
 * multitouch slots - not its single-touch emulation - are what it reads.
 *
 * Diagonals come free from the shape of the d-pad: it is hit-tested as a
 * three-by-three grid rather than as four arms, so a thumb in a corner cell
 * presses two directions exactly as a real cross does.
 */

#ifndef TINYGB_PAD_H
#define TINYGB_PAD_H

#include <stdbool.h>
#include <stdint.h>

#include "tg_fb.h"

/*
 * Which buttons a contact at (x, y) is pressing, as TG_* bits from tg_core.h.
 *
 * Zero for a touch on the picture, in the gaps between buttons, or anywhere
 * else. Panel coordinates, which is what the driver reports.
 */
unsigned tg_pad_hit(int x, int y);

/*
 * Draw the pad, with `held` shown pressed.
 *
 * Cheap to call every frame: it returns immediately when nothing has changed
 * since the last draw. Pass `force` after anything else has painted over the
 * bottom of the screen - clearing the framebuffer, or coming back from the
 * menu - because this cannot see that happen.
 */
void tg_pad_draw(tg_fb *fb, unsigned held, bool force);

/* Forget what was last drawn, so the next draw paints everything. */
void tg_pad_invalidate(void);

#endif /* TINYGB_PAD_H */
