/*
 * tg_audio_hb.h — tg_audq's operations on RetailOS.
 *
 * The four firmware entry points the queue needs, at the addresses NanoApps'
 * own sdk/hb_audio.c and Entrain use: the descriptor constructor, the
 * player singleton, its play method, and the voice's two fields.
 */

#ifndef TINYGB_AUDIO_HB_H
#define TINYGB_AUDIO_HB_H

#include "tg_audq.h"

const tg_audq_ops *tg_audio_hb_ops(void);

#endif /* TINYGB_AUDIO_HB_H */
