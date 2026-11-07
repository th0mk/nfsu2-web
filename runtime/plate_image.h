/* plate_image.h -- a license plate's picture from its text (plates.c): the
 * game's own plate (its LICENSE_PLATE texture, 128x64: "Need for Speed" over
 * "UNDRGND 2", four screws), its number painted over and the text drawn in
 * its place in raised letters like the game's.  In the game's texture layout:
 * every mip level, BGRA (D3DFMT_A8R8G8B8), the largest first.  No game state
 * here: tests/plates_unit.c builds it alone. */
#ifndef PLATE_IMAGE_H
#define PLATE_IMAGE_H
#include <stdint.h>
#include <stddef.h>

#define PLATE_W         128           /* the game's plate (GLOBALB.BUN's LICENSE_PLATE, DXT1), and ours */
#define PLATE_H         64
#define PLATE_MIPS      4             /* 128x64 down to 16x8, as the game's */
#define PLATE_BYTES     ((128 * 64 + 64 * 32 + 32 * 16 + 16 * 8) * 4)
#define PLATE_TEXT_MAX  8             /* roam_core.h RC_PLATE_MAX */

/* a DXT1 picture of w x h (multiples of 4) to BGRA */
void plate_dxt1(const uint8_t *src, int w, int h, uint8_t *bgra);
/* text's plate (n capitals and digits, 1..PLATE_TEXT_MAX), every level, into out (PLATE_BYTES); stock: the game's
 * plate, PLATE_W x PLATE_H BGRA, or 0 (a plain grey plate with a dark rim stands in).  0: not a plate's text */
int  plate_image(const char *text, size_t n, const uint8_t *stock, uint8_t *out);

#endif
