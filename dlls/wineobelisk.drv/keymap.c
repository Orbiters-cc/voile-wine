/*
 * wineobelisk.drv keyboard layout: the Windows layout of the keyboard layout selected on the Mac
 *
 * wsd sends what each key types in the Mac's layout (WSD_EV_KEYMAP). This file gives each key its Windows virtual
 * key from those characters, the way winemac.drv does (keyboard.c, macdrv_compute_keyboard_layout, whose tables
 * these are), and translates a key into characters, dead keys included (macdrv_ToUnicodeEx, without UCKeyTranslate:
 * wsd read the layout for us). It depends on nothing of Wine but its virtual key numbers, and is tested natively
 * against the Mac's own input sources (voile-lkd, tests/keyboard).
 *
 * Copyright 1993 Bob Amstadt
 * Copyright 1996 Albert den Haan
 * Copyright 1997 David Faure
 * Copyright 1998 Morten Welinder
 * Copyright 1998 Ulrich Weigand
 * Copyright 1999 Ove Kåven
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
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

#if 0
#pragma makedep unix
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef OBELISK_KEYMAP_TEST
/* built outside Wine: the virtual keys this file names */
#define VK_CANCEL 0x03
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0d
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1b
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2d
#define VK_DELETE 0x2e
#define VK_LWIN 0x5b
#define VK_RWIN 0x5c
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6a
#define VK_ADD 0x6b
#define VK_SEPARATOR 0x6c
#define VK_SUBTRACT 0x6d
#define VK_DECIMAL 0x6e
#define VK_DIVIDE 0x6f
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7a
#define VK_F12 0x7b
#define VK_F13 0x7c
#define VK_F14 0x7d
#define VK_F15 0x7e
#define VK_F16 0x7f
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F24 0x87
#define VK_OEM_NEC_EQUAL 0x92
#define VK_LSHIFT 0xa0
#define VK_RSHIFT 0xa1
#define VK_LCONTROL 0xa2
#define VK_RCONTROL 0xa3
#define VK_LMENU 0xa4
#define VK_RMENU 0xa5
#define VK_VOLUME_MUTE 0xad
#define VK_VOLUME_DOWN 0xae
#define VK_VOLUME_UP 0xaf
#define VK_OEM_1 0xba
#define VK_OEM_PLUS 0xbb
#define VK_OEM_COMMA 0xbc
#define VK_OEM_MINUS 0xbd
#define VK_OEM_PERIOD 0xbe
#define VK_OEM_2 0xbf
#define VK_OEM_3 0xc0
#define VK_OEM_4 0xdb
#define VK_OEM_5 0xdc
#define VK_OEM_6 0xdd
#define VK_OEM_7 0xde
#define VK_OEM_8 0xdf
#define VK_OEM_102 0xe2
#define VK_ICO_CLEAR 0xe6
#define VK_OEM_RESET 0xe9
#define VK_OEM_CLEAR 0xfe
#else
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#endif

#include "keymap.h"
#include "wsd_protocol.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* mac keycodes this file names (HIToolbox, Events.h) */
enum
{
    kVK_ISO_Section = 0x0a, kVK_ANSI_Equal = 0x18, kVK_ANSI_Minus = 0x1b, kVK_ANSI_RightBracket = 0x1e,
    kVK_ANSI_LeftBracket = 0x21, kVK_ANSI_Quote = 0x27, kVK_ANSI_Semicolon = 0x29, kVK_ANSI_Backslash = 0x2a,
    kVK_ANSI_Comma = 0x2b, kVK_ANSI_Slash = 0x2c, kVK_ANSI_Period = 0x2f, kVK_Space = 0x31, kVK_ANSI_Grave = 0x32,
    kVK_ANSI_KeypadDecimal = 0x41,
};

/* Mac virtual keycode -> virtual key and scan code, by the key's place on a US keyboard: winemac.drv's table
 * (keyboard.c, default_map), with Command as the Windows keys and Option as Alt. The keys that are not fixed take
 * their virtual key from the layout. */
const struct obelisk_key obelisk_default_keys[OBELISK_KEYS] =
{
    {'A',0x1e},{'S',0x1f},{'D',0x20},{'F',0x21},{'H',0x23},{'G',0x22},{'Z',0x2c},{'X',0x2d},          /* 0x00 */
    {'C',0x2e},{'V',0x2f},{VK_OEM_102,0x56},{'B',0x30},{'Q',0x10},{'W',0x11},{'E',0x12},{'R',0x13},  /* 0x08 */
    {'Y',0x15},{'T',0x14},{'1',0x02},{'2',0x03},{'3',0x04},{'4',0x05},{'6',0x07},{'5',0x06},          /* 0x10 */
    {VK_OEM_PLUS,0x0d},{'9',0x0a},{'7',0x08},{VK_OEM_MINUS,0x0c},{'8',0x09},{'0',0x0b},{VK_OEM_6,0x1b},{'O',0x18}, /* 0x18 */
    {'U',0x16},{VK_OEM_4,0x1a},{'I',0x17},{'P',0x19},{VK_RETURN,0x1c,1},{'L',0x26},{'J',0x24},{VK_OEM_7,0x28}, /* 0x20 */
    {'K',0x25},{VK_OEM_1,0x27},{VK_OEM_5,0x2b},{VK_OEM_COMMA,0x33},{VK_OEM_2,0x35},{'N',0x31},{'M',0x32},
    {VK_OEM_PERIOD,0x34},                                                                              /* 0x28 */
    {VK_TAB,0x0f,1},{VK_SPACE,0x39,1},{VK_OEM_3,0x29},{VK_BACK,0x0e,1},{0,0},{VK_ESCAPE,0x01,1},{VK_RWIN,0x15c,1},
    {VK_LWIN,0x15b,1},                                                                                 /* 0x30 */
    {VK_LSHIFT,0x2a,1},{VK_CAPITAL,0x3a,1},{VK_LMENU,0x38,1},{VK_LCONTROL,0x1d,1},{VK_RSHIFT,0x36,1},{VK_RMENU,0x138,1},
    {VK_RCONTROL,0x11d,1},{0,0} /* Fn */,                                                               /* 0x38 */
    {VK_F17,0x68,1},{VK_DECIMAL,0x53,1},{0,0},{VK_MULTIPLY,0x37,1},{0,0},{VK_ADD,0x4e,1},{0,0},{VK_OEM_CLEAR,0x59,1}, /* 0x40 */
    {VK_VOLUME_UP,0x100,1},{VK_VOLUME_DOWN,0x100,1},{VK_VOLUME_MUTE,0x100,1},{VK_DIVIDE,0x135,1},{VK_RETURN,0x11c,1},
    {0,0},{VK_SUBTRACT,0x4a,1},{VK_F18,0x69,1},                                                        /* 0x48 */
    {VK_F19,0x6a,1},{VK_OEM_NEC_EQUAL,0x10d,1},{VK_NUMPAD0,0x52,1},{VK_NUMPAD1,0x4f,1},{VK_NUMPAD2,0x50,1},
    {VK_NUMPAD3,0x51,1},{VK_NUMPAD4,0x4b,1},{VK_NUMPAD5,0x4c,1},                                       /* 0x50 */
    {VK_NUMPAD6,0x4d,1},{VK_NUMPAD7,0x47,1},{VK_F20,0x6b,1},{VK_NUMPAD8,0x48,1},{VK_NUMPAD9,0x49,1},
    {0xff,0x7d,1} /* JIS Yen */,{0xc1,0x73,1} /* JIS Underscore */,{VK_SEPARATOR,0x7e,1} /* JIS Keypad Comma */, /* 0x58 */
    {VK_F5,0x3f,1},{VK_F6,0x40,1},{VK_F7,0x41,1},{VK_F3,0x3d,1},{VK_F8,0x42,1},{VK_F9,0x43,1},
    {0xff,0x72,1} /* JIS Eisu */,{VK_F11,0x57,1},                                                      /* 0x60 */
    {VK_OEM_RESET,0x71,1} /* JIS Kana */,{VK_F13,0x64,1},{VK_F16,0x67,1},{VK_F14,0x65,1},{0,0},{VK_F10,0x44,1},{0,0},
    {VK_F12,0x58,1},                                                                                   /* 0x68 */
    {0,0},{VK_F15,0x66,1},{VK_INSERT,0x152,1} /* Help */,{VK_HOME,0x147,1},{VK_PRIOR,0x149,1},{VK_DELETE,0x153,1},
    {VK_F4,0x3e,1},{VK_END,0x14f,1},                                                                   /* 0x70 */
    {VK_F2,0x3c,1},{VK_NEXT,0x151,1},{VK_F1,0x3b,1},{VK_LEFT,0x14b,1},{VK_RIGHT,0x14d,1},{VK_DOWN,0x150,1},
    {VK_UP,0x148,1},{0,0},                                                                             /* 0x78 */
};

/* A character and the virtual key Windows layouts give the key that types it: winemac.drv's symbol_vkeys, in its
 * order. `mac_key` is the key that must type it, -1 for any. */
static const struct { unsigned short wchar; unsigned short vkey; int mac_key; } symbol_vkeys[] =
{
    { '-', VK_OEM_PLUS, kVK_ANSI_Equal },
    { '-', VK_OEM_MINUS, -1 },
    { '+', VK_OEM_PLUS, -1 },
    { '_', VK_OEM_MINUS, -1 },
    { ',', VK_OEM_COMMA, -1 },
    { '.', VK_OEM_PERIOD, -1 },
    { '=', VK_OEM_8, kVK_ANSI_Slash },
    { '=', VK_OEM_PLUS, -1 },
    { '!', VK_OEM_8, kVK_ANSI_Slash },
    { 0x00f9, VK_OEM_3, kVK_ANSI_Quote }, /* French u grave */
    { '$', VK_OEM_1, kVK_ANSI_RightBracket },
    { ':', VK_OEM_2, kVK_ANSI_Period },
    { '*', VK_OEM_5, kVK_ANSI_Backslash },
    { '`', VK_OEM_5, kVK_ANSI_Backslash },
    { ';', VK_OEM_PERIOD, kVK_ANSI_Comma },
    { ')', VK_OEM_4, kVK_ANSI_Minus },
    { '>', VK_OEM_PERIOD, -1 },
    { '<', VK_OEM_COMMA, -1 },
    { '|', VK_OEM_5, -1 },
    { '\\', VK_OEM_5, -1 },
    { '`', VK_OEM_3, -1 },
    { '[', VK_OEM_4, -1 },
    { '~', VK_OEM_3, -1 },
    { 0x00df, VK_OEM_4, kVK_ANSI_Minus }, /* German sharp s */
    { 0x00fc, VK_OEM_1, kVK_ANSI_LeftBracket }, /* German u umlaut */
    { 0x00f6, VK_OEM_3, kVK_ANSI_Semicolon }, /* German o umlaut */
    { 0x00e4, VK_OEM_7, kVK_ANSI_Quote }, /* German a umlaut */
    { '?', VK_OEM_2, -1 },
    { ']', VK_OEM_6, -1 },
    { '/', VK_OEM_2, -1 },
    { ':', VK_OEM_1, -1 },
    { '}', VK_OEM_6, -1 },
    { '{', VK_OEM_4, -1 },
    { ';', VK_OEM_1, -1 },
    { '\'', VK_OEM_7, -1 },
    { ':', VK_OEM_PERIOD, -1 },
    { ';', VK_OEM_COMMA, -1 },
    { '"', VK_OEM_7, -1 },
    { 0x00b4, VK_OEM_4, kVK_ANSI_Equal }, /* acute accent */
    { '\'', VK_OEM_2, -1 },
    { 0x00a7, VK_OEM_5, -1 }, /* section sign */
    { '*', VK_OEM_PLUS, -1 },
    { 0x00b4, VK_OEM_7, -1 },
    { '`', VK_OEM_4, -1 },
    { '[', VK_OEM_6, -1 },
    { '/', VK_OEM_5, -1 },
    { '^', VK_OEM_5, kVK_ISO_Section }, /* not in winemac.drv's table: the key left of 1 on German PC keyboards */
    { '^', VK_OEM_6, -1 },
    { '*', VK_OEM_2, -1 },
    { '{', VK_OEM_6, -1 },
    { 0x00b4, VK_OEM_6, -1 },
    { '~', VK_OEM_1, -1 },
    { '?', VK_OEM_PLUS, -1 },
    { '?', VK_OEM_4, -1 },
    { 0x00b4, VK_OEM_3, -1 },
    { '?', VK_OEM_COMMA, -1 },
    { '~', VK_OEM_PLUS, -1 },
    { ']', VK_OEM_4, -1 },
    { '\'', VK_OEM_3, -1 },
    { 0x00a7, VK_OEM_7, -1 },
    { '<', VK_OEM_102, -1 },
    /* not in winemac.drv's table */
    { 0x00b2, VK_OEM_7, -1 }, /* superscript two, the key left of 1 on French and Belgian PC keyboards */
};

/* virtual keys any key may take, when none of the above fits */
static const struct { unsigned short first, last; } vkey_ranges[] =
{
    { 'A', 'Z' },
    { '0', '9' },
    { VK_OEM_1, VK_OEM_3 },
    { VK_OEM_4, VK_ICO_CLEAR },
    { 0xe9, 0xf5 },
    { VK_OEM_NEC_EQUAL, VK_OEM_NEC_EQUAL },
    { VK_F1, VK_F24 },
    { 0, 0 }
};

/* the levels of the blob the virtual keys are looked for at, in winemac.drv's order: no modifier, Shift, Command,
 * Shift+Command, Option, Shift+Option */
static const unsigned char assign_levels[] =
{
    WSD_KEYMAP_BASE, WSD_KEYMAP_SHIFT, WSD_KEYMAP_COMMAND, WSD_KEYMAP_COMMAND_SHIFT, WSD_KEYMAP_OPTION,
    WSD_KEYMAP_OPTION | WSD_KEYMAP_SHIFT,
};

/* The letter of the Latin alphabet a character is, in upper case, whatever its case and width. winemac.drv compares
 * with a collator of the Mac (case, width and composition insensitive); these are the characters keyboards type. */
static unsigned short letter_of( unsigned short c )
{
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    if (c >= 0xff21 && c <= 0xff3a) return c - 0xff21 + 'A';  /* fullwidth */
    if (c >= 0xff41 && c <= 0xff5a) return c - 0xff41 + 'A';
    return c;
}

/* The same without its diacritical mark: the letter an accented Latin letter decomposes to (Latin-1 Supplement
 * and Latin Extended-A), as the Mac's diacritic insensitive collator takes it. */
static unsigned short base_letter_of( unsigned short c )
{
    static const char latin1[] = /* U+00C0..U+00FF */
        "AAAAAA.CEEEEIIII.NOOOOO..UUUUY.."
        "AAAAAA.CEEEEIIII.NOOOOO..UUUUY.Y";
    static const char extended_a[] = /* U+0100..U+017F */
        "AAAAAACCCCCCCCDD..EEEEEEEEEEGGGG"
        "GGGGHH..IIIIIIIII...JJKK.LLLLLL."
        "...NNNNNN...OOOOOO..RRRRRRSSSSSS"
        "SSTTTT..UUUUUUUUUUUUWWYYYZZZZZZ.";
    char base = 0;

    if (c >= 0xc0 && c <= 0xff) base = latin1[c - 0xc0];
    else if (c >= 0x100 && c <= 0x17f) base = extended_a[c - 0x100];
    if (base && base != '.') return base;
    return letter_of( c );
}

/* The key at the place `key` has on an ANSI keyboard. On an ISO keyboard kVK_ISO_Section is the key left of 1 and
 * kVK_ANSI_Grave the one right of the left Shift: each has the other's place, and so its scan code (winemac.drv
 * swaps their scan codes the same way). */
static unsigned int place_of( unsigned int key, int iso )
{
    if (iso && (key == kVK_ISO_Section || key == kVK_ANSI_Grave)) return key ^ kVK_ISO_Section ^ kVK_ANSI_Grave;
    return key;
}

/* The keys that take their virtual key from what they type. kVK_ISO_Section is one on an ISO keyboard only: an ANSI
 * keyboard has no such key, and what the layout holds for it must not take a virtual key from a real one. */
static int is_character_key( const struct obelisk_keymap *keymap, unsigned int key )
{
    if (key == kVK_ISO_Section && !keymap->iso) return 0;
    return obelisk_default_keys[key].scan && !obelisk_default_keys[key].fixed;
}

/* Gives each character key its virtual key from what it types: macdrv_compute_keyboard_layout. `map` holds the
 * blob's characters. */
static void assign_vkeys( struct obelisk_keymap *keymap, const uint16_t *map, unsigned int levels )
{
#define MAP(key, combo) (map[(key) * levels + assign_levels[combo]])
    unsigned char used[256] = { 0 };
    unsigned int key, combo, i, vkey, range, pass;

    for (key = 0; key < OBELISK_KEYS; key++)
    {
        const struct obelisk_key *place = &obelisk_default_keys[place_of( key, keymap->iso )];
        keymap->scan[key] = place->scan;
        keymap->vkey[key] = 0;
        if (!is_character_key( keymap, key ))
        {
            keymap->vkey[key] = obelisk_default_keys[key].vkey;
            used[keymap->vkey[key] & 0xff] = 1;
        }
    }

    /* The letters A to Z: without a modifier first, then with each. A first pass takes the plain letter, a second
     * one a letter with a diacritical mark. */
    for (pass = 0; pass < 2; pass++)
        for (combo = 0; combo < ARRAY_COUNT(assign_levels); combo++)
            for (vkey = 'A'; vkey <= 'Z'; vkey++)
            {
                if (used[vkey]) continue;
                for (key = 0; key < OBELISK_KEYS; key++)
                {
                    unsigned short c = MAP( key, combo );
                    if (!is_character_key( keymap, key ) || keymap->vkey[key] || !c) continue;
                    if ((pass ? base_letter_of( c ) : letter_of( c )) != vkey) continue;
                    keymap->vkey[key] = vkey;
                    used[vkey] = 1;
                    break;
                }
            }

    /* the digits */
    for (combo = 0; combo < ARRAY_COUNT(assign_levels); combo++)
        for (vkey = '0'; vkey <= '9'; vkey++)
        {
            if (used[vkey]) continue;
            for (key = 0; key < OBELISK_KEYS; key++)
            {
                if (!is_character_key( keymap, key ) || keymap->vkey[key] || MAP( key, combo ) != vkey) continue;
                keymap->vkey[key] = vkey;
                used[vkey] = 1;
                break;
            }
        }

    /* common punctuation, to the OEM virtual key Windows layouts give it ('.' to VK_OEM_PERIOD) */
    for (combo = 0; combo < ARRAY_COUNT(assign_levels); combo++)
        for (i = 0; i < ARRAY_COUNT(symbol_vkeys); i++)
        {
            vkey = symbol_vkeys[i].vkey;
            if (used[vkey]) continue;
            for (key = 0; key < OBELISK_KEYS; key++)
            {
                if (!is_character_key( keymap, key ) || keymap->vkey[key] || !MAP( key, combo )) continue;
                if (symbol_vkeys[i].mac_key != -1 && symbol_vkeys[i].mac_key != (int)key) continue;
                if (MAP( key, combo ) != symbol_vkeys[i].wchar) continue;
                keymap->vkey[key] = vkey;
                used[vkey] = 1;
                break;
            }
        }

    /* the virtual key of the key's place on a US keyboard, if it is still free */
    for (key = 0; key < OBELISK_KEYS; key++)
    {
        vkey = obelisk_default_keys[place_of( key, keymap->iso )].vkey;
        if (!is_character_key( keymap, key ) || keymap->vkey[key] || used[vkey]) continue;
        keymap->vkey[key] = vkey;
        used[vkey] = 1;
    }

    /* a key at a letter's place whose letter was taken: another letter; the same for the digits */
    vkey = 'A';
    for (key = 0; key < OBELISK_KEYS; key++)
    {
        unsigned int place = obelisk_default_keys[key].vkey;
        if (place < 'A' || place > 'Z' || !is_character_key( keymap, key ) || keymap->vkey[key]) continue;
        while (vkey <= 'Z' && used[vkey]) vkey++;
        if (vkey > 'Z') break;
        keymap->vkey[key] = vkey;
        used[vkey] = 1;
    }
    vkey = '0';
    for (key = 0; key < OBELISK_KEYS; key++)
    {
        unsigned int place = obelisk_default_keys[key].vkey;
        if (place < '0' || place > '9' || !is_character_key( keymap, key ) || keymap->vkey[key]) continue;
        while (vkey <= '9' && used[vkey]) vkey++;
        if (vkey > '9') break;
        keymap->vkey[key] = vkey;
        used[vkey] = 1;
    }

    /* last, any virtual key that is free */
    range = 0;
    vkey = vkey_ranges[range].first;
    for (key = 0; key < OBELISK_KEYS; key++)
    {
        if (!is_character_key( keymap, key ) || keymap->vkey[key]) continue;
        while (vkey && used[vkey])
        {
            if (vkey == vkey_ranges[range].last) vkey = vkey_ranges[++range].first;
            else vkey++;
        }
        if (!vkey) break;
        keymap->vkey[key] = vkey;
        used[vkey] = 1;
    }
#undef MAP
}

/* Whether a Windows layout of these characters has an AltGr key: when some ASCII character can only be typed with
 * Option, as the braces of the French and German layouts. The right Option key is then AltGr, as on a PC keyboard
 * of the layout, and the left one stays Alt. A layout that types all of ASCII without Option (US, Dvorak) keeps two
 * Alt keys, as its Windows layout does. */
static int needs_altgr( const struct obelisk_keymap *keymap )
{
    unsigned char plain[128] = { 0 };
    unsigned int key, level;
    int ret = 0;

    for (key = 0; key < OBELISK_KEYS; key++)
        for (level = 0; level < OBELISK_LEVELS; level++)
        {
            unsigned short c = keymap->chars[key][level];
            if (c > 0x20 && c < 0x7f && !(level & OBELISK_LEVEL_ALTGR)) plain[c] = 1;
        }
    for (key = 0; key < OBELISK_KEYS; key++)
        for (level = 0; level < OBELISK_LEVELS; level++)
        {
            unsigned short c = keymap->chars[key][level];
            if (c > 0x20 && c < 0x7f && (level & OBELISK_LEVEL_ALTGR) && !plain[c]) ret = 1;
        }
    return ret;
}

struct obelisk_keymap *obelisk_keymap_create( const void *blob, size_t size, unsigned int serial )
{
    struct wsd_keymap header;
    struct obelisk_keymap *keymap;
    const uint8_t *bytes = blob;
    uint16_t *map, *dead;
    size_t map_size, need;
    unsigned int key, level, i;

    if (size < sizeof(header)) return NULL;
    memcpy( &header, blob, sizeof(header) );
    if (header.version != WSD_KEYMAP_VERSION || header.keys != OBELISK_KEYS) return NULL;
    if (header.levels < WSD_KEYMAP_LEVELS || header.levels > 64 || header.compositions > 0x10000) return NULL;
    map_size = (size_t)header.keys * header.levels * sizeof(uint16_t);
    need = sizeof(header) + map_size + header.keys * sizeof(uint16_t) +
           (size_t)header.compositions * sizeof(struct wsd_keymap_composition);
    if (size < need) return NULL;

    if (!(keymap = calloc( 1, sizeof(*keymap) + map_size + header.keys * sizeof(uint16_t) +
                           header.compositions * sizeof(*keymap->compositions) ))) return NULL;
    keymap->compositions = (void *)(keymap + 1);
    map = (uint16_t *)(keymap->compositions + header.compositions);
    dead = map + header.keys * header.levels;
    memcpy( map, bytes + sizeof(header), map_size + header.keys * sizeof(uint16_t) );
    bytes += sizeof(header) + map_size + header.keys * sizeof(uint16_t);
    for (i = 0; i < header.compositions; i++)
    {
        struct wsd_keymap_composition c;
        memcpy( &c, bytes + i * sizeof(c), sizeof(c) );
        if (!c.dead || !c.base || !c.composed) continue;
        keymap->compositions[keymap->composition_count].dead = c.dead;
        keymap->compositions[keymap->composition_count].base = c.base;
        keymap->compositions[keymap->composition_count].composed = c.composed;
        keymap->composition_count++;
    }

    keymap->serial = serial;
    keymap->iso = !!(header.flags & WSD_KEYMAP_ISO);
    memcpy( keymap->source, header.source, sizeof(keymap->source) );
    keymap->source[sizeof(keymap->source) - 1] = 0;

    assign_vkeys( keymap, map, header.levels );

    /* the characters: of the character keys and of the space bar (Option+Space is a no-break space, and a dead key
     * then the space bar types the accent); the keypad's decimal key keeps its one character, a comma in French */
    memset( keymap->key_of_vkey, 0xff, sizeof(keymap->key_of_vkey) );
    for (key = 0; key < OBELISK_KEYS; key++)
    {
        if (key == kVK_ANSI_KeypadDecimal)
        {
            unsigned short c = map[key * header.levels];
            keymap->chars[key][0] = (c > 0x20 && c < 0x7f) ? c : 0;
            continue;
        }
        if (!is_character_key( keymap, key ) && key != kVK_Space) continue;
        if (!keymap->vkey[key]) continue;
        keymap->key_of_vkey[keymap->vkey[key] & 0xff] = key;
        for (level = 0; level < OBELISK_LEVELS; level++)
        {
            unsigned short c = map[key * header.levels + level];
            /* control characters are Windows' own business (Ctrl+letter), not the Mac layout's */
            if (c < 0x20 || c == 0x7f) c = 0;
            keymap->chars[key][level] = c;
        }
        if (key != kVK_Space) keymap->dead[key] = dead[key] & ((1u << OBELISK_LEVELS) - 1);
    }
    for (key = 0; key < OBELISK_KEYS; key++)  /* a dead key without its accent cannot be told from another */
        for (level = 0; level < OBELISK_LEVELS; level++)
            if (!keymap->chars[key][level]) keymap->dead[key] &= ~(1u << level);
    keymap->altgr = needs_altgr( keymap );
    return keymap;
}

static unsigned short compose( const struct obelisk_keymap *keymap, unsigned short dead, unsigned short base )
{
    unsigned int i;

    for (i = 0; i < keymap->composition_count; i++)
        if (keymap->compositions[i].dead == dead && keymap->compositions[i].base == base)
            return keymap->compositions[i].composed;
    return 0;
}

int obelisk_keymap_to_unicode( const struct obelisk_keymap *keymap, unsigned int vkey, unsigned int level,
                               int keep, unsigned short *pending, unsigned short *out, int size )
{
    unsigned int key = keymap->key_of_vkey[vkey & 0xff];
    unsigned short c, composed, accent = *pending;
    int count = 0;

    if (vkey > 0xff || key == 0xff) return -2;
    level &= OBELISK_LEVELS - 1;
    if (!keymap->altgr) level &= ~OBELISK_LEVEL_ALTGR;
    c = keymap->chars[key][level];
    /* Caps Lock changes nothing for this key at this level */
    if (!c && (level & OBELISK_LEVEL_CAPS))
    {
        level &= ~OBELISK_LEVEL_CAPS;
        c = keymap->chars[key][level];
    }
    if (!c) return 0;  /* the key types nothing there, and a waiting accent goes on waiting */

    if (keymap->dead[key] & (1u << level))
    {
        if (!accent)
        {
            if (!keep) *pending = c;
            if (size > 0) out[0] = c;
            return -1;
        }
        /* a dead key after a dead key: both accents, as Windows types them */
        if (!keep) *pending = 0;
        if (count < size) out[count++] = accent;
        if (count < size) out[count++] = c;
        return count;
    }
    if (!accent)
    {
        if (size > 0) out[count++] = c;
        return count;
    }
    if (!keep) *pending = 0;
    if ((composed = compose( keymap, accent, c )))
    {
        if (count < size) out[count++] = composed;
        return count;
    }
    /* no such letter: the accent, then the character */
    if (count < size) out[count++] = accent;
    if (count < size) out[count++] = c;
    return count;
}
