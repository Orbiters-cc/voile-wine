/*
 * wineobelisk.drv keyboard layout: the Windows layout of the keyboard layout selected on the Mac
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_OBELISKDRV_KEYMAP_H
#define __WINE_OBELISKDRV_KEYMAP_H

#include <stddef.h>

#define OBELISK_KEYS 128  /* mac virtual keycodes */

/* a key by its place on a US keyboard */
struct obelisk_key
{
    unsigned short vkey;   /* 0: not a key Windows knows */
    unsigned short scan;   /* 0x100: extended */
    unsigned char  fixed;  /* the same virtual key in every layout (Return, the arrows, the keypad...) */
};
extern const struct obelisk_key obelisk_default_keys[OBELISK_KEYS];

/* the modifiers a character is typed with, as bits of a level */
enum { OBELISK_LEVEL_SHIFT = 1, OBELISK_LEVEL_ALTGR = 2, OBELISK_LEVEL_CAPS = 4, OBELISK_LEVELS = 8 };

struct obelisk_keymap_composition { unsigned short dead, base, composed; };

struct obelisk_keymap
{
    unsigned int   serial;
    int            iso;                               /* the keyboard is an ISO one */
    int            altgr;                             /* the right Option key is AltGr (KLLF_ALTGR) */
    unsigned short vkey[OBELISK_KEYS];                /* mac keycode -> virtual key, 0 when unknown */
    unsigned short scan[OBELISK_KEYS];                /* mac keycode -> scan code (0x100: extended) */
    unsigned short chars[OBELISK_KEYS][OBELISK_LEVELS]; /* character keys only, 0 for none */
    unsigned short dead[OBELISK_KEYS];                /* bit n: a dead key at level n */
    unsigned char  key_of_vkey[256];                  /* virtual key -> mac keycode of a character key, 0xff none */
    unsigned int   composition_count;
    struct obelisk_keymap_composition *compositions;  /* in the same allocation */
    char           source[64];                        /* the Mac's input source */
};

/* The Windows layout of a WSD_EV_KEYMAP blob, to be freed with free(). NULL for a blob that is not one. */
extern struct obelisk_keymap *obelisk_keymap_create( const void *blob, size_t size, unsigned int serial );

/* ToUnicodeEx for the character keys. `level` holds OBELISK_LEVEL_* bits; `pending` is the caller's dead key state
 * (the accent waiting for its letter, 0 for none), left as it is when `keep` is set. Returns the count of characters
 * written to `out`, -1 for a dead key (its accent in out[0]), and -2 for a key that types nothing the layout knows
 * of, which is the caller's to translate. */
extern int obelisk_keymap_to_unicode( const struct obelisk_keymap *keymap, unsigned int vkey, unsigned int level,
                                      int keep, unsigned short *pending, unsigned short *out, int size );

#endif
