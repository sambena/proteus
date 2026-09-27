/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Proteus capture interface: what an emulator core tells Proteus about a frame beyond its
 * pixels. For the Atari 2600 that is which TIA objects were present at every pixel, each
 * object's own colour there, the register writes that built the frame and the two audio
 * voices before they were mixed.
 *
 * A core implements it with two extra exports:
 *
 *    void retro_proteus_capture_enable(unsigned flags);
 *    const struct pxc_frame *retro_proteus_capture(unsigned abi_version);
 *
 * retro_proteus_capture returns the capture of the frame the last retro_run() completed, or
 * NULL when capture is off, the versions differ or no frame completed. The pointers inside
 * stay valid until the next retro_run().
 *
 * This file is shared between the core (GPL-2.0-or-later) and Proteus (GPL-3.0-or-later).
 */
#ifndef PROTEUS_CAPTURE_H
#define PROTEUS_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PXC_ABI_VERSION 1u
#define PXC_W           160
#define PXC_MAX_H       320

/* retro_proteus_capture_enable flags */
#define PXC_ENABLE_VIDEO  1u   /* tags, winner, colour planes, aux */
#define PXC_ENABLE_WRITES 2u   /* register write log */
#define PXC_ENABLE_AUDIO  4u   /* per-voice volumes */

/* Tag plane: which objects emit at a pixel (all of them, not only the one on top). */
#define PXC_P0     0x01u
#define PXC_M0     0x02u
#define PXC_P1     0x04u
#define PXC_M1     0x08u
#define PXC_BL     0x10u
#define PXC_PF     0x20u
#define PXC_BLANK  0x40u   /* no picture: VBLANK, HMOVE comb, RSYNC fill or a line never drawn */
#define PXC_SPRITES (PXC_P0 | PXC_M0 | PXC_P1 | PXC_M1 | PXC_BL)

/* Colour planes, and the order of pxc_frame.color[]. */
enum
{
   PXC_L_BK = 0,
   PXC_L_PF,
   PXC_L_BL,
   PXC_L_P0,
   PXC_L_M0,
   PXC_L_P1,
   PXC_L_M1,
   PXC_LAYERS
};

/* Aux plane */
#define PXC_AUX_P0_COPY(a)  ((a) & 3u)          /* 0: P0 is not scanning here, else copy 1..3 */
#define PXC_AUX_P1_COPY(a)  (((a) >> 2) & 3u)
#define PXC_AUX_PRIORITY(a) (((a) >> 4) & 3u)   /* PXC_PRI_* */
#define PXC_PRI_NORMAL 0u   /* P0/M0, P1/M1, BL/PF, BK */
#define PXC_PRI_PFP    1u   /* BL/PF, P0/M0, P1/M1, BK */
#define PXC_PRI_SCORE  2u   /* P0/M0, PF, P1/M1, BL, BK */

struct pxc_regwrite
{
   uint16_t scanline;   /* line of the frame, counted from VSYNC */
   int16_t  y;          /* row of the picture, or -1 outside it */
   uint8_t  hclock;     /* colour clock 0..227 of the write */
   uint8_t  reg;        /* TIA register 0x00..0x3F */
   uint8_t  value;
   uint8_t  flags;      /* reserved, 0 */
};

struct pxc_audio
{
   uint32_t rate_x1000;   /* samples a second, times 1000 */
   uint32_t count;        /* samples this frame */
   const uint8_t *v0;     /* voice 0: volume 0..15, averaged over each sample */
   const uint8_t *v1;
};

struct pxc_frame
{
   uint32_t abi_version;
   uint32_t struct_size;
   uint32_t frame_serial;      /* counts completed frames; 0 never */
   uint16_t width;             /* PXC_W */
   uint16_t height;            /* rows in the picture */
   uint16_t scanlines_total;   /* lines in the frame, blanking included */
   uint8_t  pal;               /* 0: 60 Hz timing, 1: 50 Hz */
   uint8_t  reserved;
   uint32_t flags;             /* PXC_ENABLE_* in effect */

   /* [height * width] each. */
   const uint8_t *tags;
   const uint8_t *winner;               /* the core's own output: an index into palette */
   const uint8_t *color[PXC_LAYERS];    /* valid where the tag bit is set; BK everywhere */
   const uint8_t *aux;

   const uint32_t *palette;             /* 256 XRGB8888 entries */

   const struct pxc_regwrite *writes;
   uint32_t write_count;
   uint32_t writes_dropped;             /* writes beyond the log's size */

   struct pxc_audio audio;
};

#ifdef __cplusplus
}
#endif

#endif
