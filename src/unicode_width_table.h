#ifndef PROV_UNICODE_WIDTH_TABLE_H
#define PROV_UNICODE_WIDTH_TABLE_H

/*
 * Code-point width ranges for prov_char_width() (SPEC.md §5, §12.3).
 *
 * This is a curated best-effort table for Milestone 1, not a full UAX #11
 * dump. Regenerate a complete table from the Unicode data files with
 * scripts/gen_unicode_width.py (East Asian Width W/F -> wide; general
 * categories Mn/Me + zero-width Cf -> zero). Ranges are inclusive [lo, hi]
 * and MUST stay sorted ascending and non-overlapping (binary searched).
 */

#include "proven/types.h"

/* Zero-width: nonspacing combining marks and zero-width format controls. */
static const proven_u32 prov_width_zero[][2] = {
    {0x00000300, 0x0000036F}, {0x00000483, 0x00000487},
    {0x00000591, 0x000005BD}, {0x000005BF, 0x000005BF},
    {0x000005C1, 0x000005C2}, {0x000005C4, 0x000005C5},
    {0x000005C7, 0x000005C7}, {0x00000610, 0x0000061A},
    {0x0000064B, 0x0000065F}, {0x00000670, 0x00000670},
    {0x000006D6, 0x000006DC}, {0x000006DF, 0x000006E4},
    {0x000006E7, 0x000006E8}, {0x000006EA, 0x000006ED},
    {0x00000711, 0x00000711}, {0x00000730, 0x0000074A},
    {0x000007A6, 0x000007B0}, {0x000007EB, 0x000007F3},
    {0x00000816, 0x00000819}, {0x0000081B, 0x00000823},
    {0x00000825, 0x00000827}, {0x00000829, 0x0000082D},
    {0x00000859, 0x0000085B}, {0x00000900, 0x00000902},
    {0x0000093A, 0x0000093A}, {0x0000093C, 0x0000093C},
    {0x00000941, 0x00000948}, {0x0000094D, 0x0000094D},
    {0x00000951, 0x00000957}, {0x00000962, 0x00000963},
    {0x00000E31, 0x00000E31}, {0x00000E34, 0x00000E3A},
    {0x00000E47, 0x00000E4E}, {0x00000EB1, 0x00000EB1},
    {0x00000EB4, 0x00000EBC}, {0x00000EC8, 0x00000ECD},
    {0x00001AB0, 0x00001AFF}, {0x00001DC0, 0x00001DFF},
    {0x0000200B, 0x0000200F}, {0x0000202A, 0x0000202E},
    {0x00002060, 0x00002064}, {0x0000206A, 0x0000206F},
    {0x000020D0, 0x000020FF}, {0x0000FE00, 0x0000FE0F},
    {0x0000FE20, 0x0000FE2F}, {0x0000FEFF, 0x0000FEFF},
    {0x000E0100, 0x000E01EF},
};

/* Wide: East Asian Wide (W) and Fullwidth (F). */
static const proven_u32 prov_width_wide[][2] = {
    {0x00001100, 0x0000115F}, {0x00002329, 0x0000232A},
    {0x00002E80, 0x0000303E}, {0x00003041, 0x000033FF},
    {0x00003400, 0x00004DBF}, {0x00004E00, 0x00009FFF},
    {0x0000A000, 0x0000A4CF}, {0x0000A960, 0x0000A97F},
    {0x0000AC00, 0x0000D7A3}, {0x0000F900, 0x0000FAFF},
    {0x0000FE10, 0x0000FE19}, {0x0000FE30, 0x0000FE6F},
    {0x0000FF00, 0x0000FF60}, {0x0000FFE0, 0x0000FFE6},
    {0x0001B000, 0x0001B16F}, {0x0001F200, 0x0001F2FF},
    {0x0001F300, 0x0001F64F}, {0x0001F900, 0x0001F9FF},
    {0x0001FA00, 0x0001FAFF}, {0x00020000, 0x0002FFFD},
    {0x00030000, 0x0003FFFD},
};

#define PROV_WIDTH_ZERO_COUNT (sizeof(prov_width_zero) / sizeof(prov_width_zero[0]))
#define PROV_WIDTH_WIDE_COUNT (sizeof(prov_width_wide) / sizeof(prov_width_wide[0]))

#endif /* PROV_UNICODE_WIDTH_TABLE_H */
