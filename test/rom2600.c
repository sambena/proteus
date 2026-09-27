/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Writes pxtest.a26, a 4K Atari 2600 program for testing Proteus without a commercial game.
 * It is laid out like an early shooter so that it exercises what such games do to the TIA:
 *
 *   - a band of playfield "score" blocks at the top
 *   - three rows of invaders: both players, three copies each, placed again with RESP0/RESP1
 *     and HMOVE for every row, with a two-frame animation and a slow march to the left
 *   - reflected playfield shields
 *   - missile 0 moving right one pixel a frame, and the ball standing still
 *   - a cannon (player 0) and a saucer (player 1) that is drawn on odd frames only: flicker
 *   - a strip of background colour as the ground
 *   - voice 0 holding a tone, voice 1 holding another one that is on for 32 frames in 64
 *
 *   rom2600 <out.a26> [pal]
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum
{
   VSYNC = 0x00, VBLANK, WSYNC, RSYNC, NUSIZ0, NUSIZ1, COLUP0, COLUP1, COLUPF, COLUBK, CTRLPF,
   REFP0, REFP1, PF0, PF1, PF2, RESP0, RESP1, RESM0, RESM1, RESBL, AUDC0, AUDC1, AUDF0, AUDF1,
   AUDV0, AUDV1, GRP0, GRP1, ENAM0, ENAM1, ENABL, HMP0, HMP1, HMM0, HMM1, HMBL, VDELP0, VDELP1,
   VDELBL, RESMP0, RESMP1, HMOVE, HMCLR, CXCLR
};

/* RAM */
enum { FRAME = 0x80, MARCH = 0x81, PATTERN = 0x82, FLICKER = 0x84 };

#define ORG 0xF000u

static uint8_t rom[4096];
static unsigned pc;
static int pass;
/* 312 lines a frame instead of 262: 45 of blanking, 228 of picture, 36 of overscan. */
static int pal;

static struct { const char *name; unsigned addr; } labels[64];
static unsigned label_count;
static int errors;

static void label(const char *name)
{
   for (unsigned i = 0; i < label_count; i++)
      if (!strcmp(labels[i].name, name))
      {
         if (pass == 1 && labels[i].addr != ORG + pc)
         {
            fprintf(stderr, "label %s moved\n", name);
            errors++;
         }
         labels[i].addr = ORG + pc;
         return;
      }
   labels[label_count].name = name;
   labels[label_count].addr = ORG + pc;
   label_count++;
}

static unsigned addr_of(const char *name)
{
   for (unsigned i = 0; i < label_count; i++)
      if (!strcmp(labels[i].name, name))
         return labels[i].addr;
   if (pass == 1)
   {
      fprintf(stderr, "unknown label %s\n", name);
      errors++;
   }
   return ORG;
}

static void b(unsigned v) { rom[pc++ & 0xFFF] = (uint8_t)v; }
static void op0(unsigned op) { b(op); }
static void op1(unsigned op, unsigned v) { b(op); b(v); }
static void op2(unsigned op, unsigned a) { b(op); b(a & 0xFF); b(a >> 8); }

static void branch(unsigned op, const char *to)
{
   int d = (int)addr_of(to) - (int)(ORG + pc + 2);
   if (pass == 1 && (d < -128 || d > 127))
   {
      fprintf(stderr, "branch to %s out of range\n", to);
      errors++;
   }
   b(op);
   b((unsigned)d & 0xFF);
}

#define SEI()        op0(0x78)
#define CLD()        op0(0xD8)
#define TXS()        op0(0x9A)
#define DEX()        op0(0xCA)
#define DEY()        op0(0x88)
#define NOP()        op0(0xEA)
#define ASL()        op0(0x0A)
#define RTS()        op0(0x60)
#define LDA_I(v)     op1(0xA9, v)
#define LDX_I(v)     op1(0xA2, v)
#define LDY_I(v)     op1(0xA0, v)
#define AND_I(v)     op1(0x29, v)
#define LDA_Z(a)     op1(0xA5, a)
#define AND_Z(a)     op1(0x25, a)
#define STA(a)       op1(0x85, a)
#define STA_ZX(a)    op1(0x95, a)
#define INC_Z(a)     op1(0xE6, a)
#define LDA_IY(a)    op1(0xB1, a)
#define LDA_AX(l)    op2(0xBD, addr_of(l))
#define LDA_AY(l)    op2(0xB9, addr_of(l))
#define JMP(l)       op2(0x4C, addr_of(l))
#define JSR(l)       op2(0x20, addr_of(l))
#define BNE(l)       branch(0xD0, l)
#define BEQ(l)       branch(0xF0, l)
#define BPL(l)       branch(0x10, l)

static unsigned anon;

/* `n` lines: each STA WSYNC ends one. */
static void lines(unsigned n)
{
   static char names[64][8];
   char *name = names[anon++ & 63];
   snprintf(name, 8, "L%u", anon);
   LDY_I(n);
   label(name);
   STA(WSYNC);
   DEY();
   BNE(name);
}

static void program(void)
{
   anon = 0;
   pc   = 0;

   label("Reset");
   SEI();
   CLD();
   LDX_I(0xFF);
   TXS();
   LDA_I(0);
   label("Clear");
   STA_ZX(0);
   DEX();
   BNE("Clear");

   LDA_I(0x21); STA(CTRLPF);           /* reflected playfield, ball 4 wide */
   LDA_I(0x04); STA(AUDC0);
   LDA_I(0x0F); STA(AUDF0);
   LDA_I(0x08); STA(AUDV0);
   LDA_I(0x0C); STA(AUDC1);
   LDA_I(0x07); STA(AUDF1);

   /* Missile 0 near the left edge, the ball near the middle. */
   STA(WSYNC);
   for (unsigned i = 0; i < 10; i++) NOP();
   STA(RESM0);
   for (unsigned i = 0; i < 10; i++) NOP();
   STA(RESBL);

   label("Frame");
   LDA_I(2);
   STA(VBLANK);
   STA(VSYNC);
   STA(WSYNC);
   STA(WSYNC);
   STA(WSYNC);
   LDA_I(0);
   STA(VSYNC);

   INC_Z(FRAME);

   /* Voice 1 sounds while bit 5 of the frame count is set. */
   LDA_Z(FRAME);
   AND_I(0x20);
   BEQ("Quiet");
   LDA_I(0x06);
   label("Quiet");
   STA(AUDV1);

   /* The invaders march a pixel to the left every 8 frames, 8 steps, then start again. */
   LDA_Z(FRAME);
   AND_I(0x38);
   ASL();
   STA(MARCH);
   STA(WSYNC);                         /* 1 */

   /* Their two poses swap every 16 frames. */
   LDA_Z(FRAME);
   AND_I(0x10);
   BEQ("PoseA");
   LDA_I(addr_of("InvaderB") & 0xFF);
   JMP("PoseSet");
   label("PoseA");
   LDA_I(addr_of("InvaderA") & 0xFF);
   label("PoseSet");
   STA(PATTERN);
   LDA_I(addr_of("InvaderA") >> 8);
   STA(PATTERN + 1);

   /* The saucer is there on odd frames. */
   LDA_Z(FRAME);
   AND_I(0x01);
   BEQ("NoSaucer");
   LDA_I(0xFF);
   label("NoSaucer");
   STA(FLICKER);

   /* Missile 0 moves right by one; nothing else moves here. */
   LDA_I(0);
   STA(HMP0);
   STA(HMP1);
   STA(HMBL);
   LDA_I(0xF0);
   STA(HMM0);
   STA(WSYNC);                         /* 2 */
   STA(HMOVE);
   lines(pal ? 42 : 34);               /* 36, PAL 44 */
   LDA_I(0);
   STA(HMM0);
   STA(WSYNC);                         /* 37 */
   STA(VBLANK);

   /* Score band: 16 lines */
   LDA_I(0x1E); STA(COLUPF);
   LDA_I(0xA5); STA(PF1);
   LDA_I(0x5A); STA(PF2);
   lines(8);
   LDA_I(0);
   STA(PF1);
   STA(PF2);
   lines(8);

   /* Invaders: 3 rows of 24 lines */
   LDX_I(0); JSR("Row");
   LDX_I(1); JSR("Row");
   LDX_I(2); JSR("Row");

   /* Shields: 32 lines */
   lines(8);
   LDA_I(0xC6); STA(COLUPF);
   LDA_I(0x1C); STA(PF1);
   LDA_I(0x70); STA(PF2);
   lines(16);
   LDA_I(0);
   STA(PF1);
   STA(PF2);
   lines(8);

   /* Shot and ball: 16 lines */
   LDA_I(0x10); STA(NUSIZ0);           /* one missile, 2 wide */
   LDA_I(0x0E); STA(COLUP0);
   LDA_I(2);
   STA(ENAM0);
   STA(ENABL);
   lines(8);
   LDA_I(0);
   STA(ENAM0);
   STA(ENABL);
   lines(8);

   /* Cannon and saucer: 24 lines */
   STA(WSYNC);                         /* 1 */
   LDA_I(0);
   STA(NUSIZ0);
   STA(NUSIZ1);
   LDA_I(0xC8); STA(COLUP0);
   LDA_I(0x46); STA(COLUP1);
   for (unsigned i = 0; i < 9; i++) NOP();
   STA(RESP0);
   for (unsigned i = 0; i < 4; i++) NOP();
   STA(RESP1);
   LDY_I(7);
   label("CannonRow");
   STA(WSYNC);
   LDA_AY("Cannon");
   STA(GRP0);
   LDA_AY("Saucer");
   AND_Z(FLICKER);
   STA(GRP1);
   STA(WSYNC);
   DEY();
   BPL("CannonRow");                   /* 17 */
   STA(WSYNC);                         /* 18 */
   LDA_I(0);
   STA(GRP0);
   STA(GRP1);
   lines(6);                           /* 24 */

   /* Ground: 32 lines */
   lines(4);
   LDA_I(0xF4); STA(COLUBK);
   lines(4);
   LDA_I(0);
   STA(COLUBK);
   lines(pal ? 60 : 24);

   /* Overscan: 30 lines, PAL 36 */
   LDA_I(2);
   STA(VBLANK);
   lines(pal ? 36 : 30);
   JMP("Frame");

   /* One row of invaders, colour X: 24 lines */
   label("Row");
   STA(WSYNC);                         /* 1 */
   LDA_AX("RowColor");
   STA(COLUP0);
   STA(COLUP1);
   LDA_I(0x06);                        /* three copies, medium spacing */
   STA(NUSIZ0);
   STA(NUSIZ1);
   LDA_Z(MARCH);
   STA(HMP0);
   STA(HMP1);
   STA(RESP0);
   NOP();
   STA(RESP1);
   STA(WSYNC);                         /* 2 */
   STA(HMOVE);
   LDY_I(7);
   label("RowLine");
   STA(WSYNC);
   LDA_IY(PATTERN);
   STA(GRP0);
   STA(GRP1);
   STA(WSYNC);
   DEY();
   BPL("RowLine");                     /* 18 */
   STA(WSYNC);                         /* 19 */
   LDA_I(0);
   STA(GRP0);
   STA(GRP1);
   lines(5);                           /* 24 */
   RTS();

   label("RowColor");
   b(0x46); b(0x86); b(0xD6);

   /* Patterns, bottom row first. */
   label("InvaderA");
   b(0xA5); b(0x5A); b(0x24); b(0xFF); b(0xDB); b(0x7E); b(0x3C); b(0x18);
   label("InvaderB");
   b(0x42); b(0x81); b(0x5A); b(0xFF); b(0xDB); b(0x7E); b(0x3C); b(0x18);
   label("Cannon");
   b(0xFE); b(0xFE); b(0xFE); b(0xFE); b(0x7C); b(0x38); b(0x38); b(0x10);
   label("Saucer");
   b(0x00); b(0x24); b(0x7E); b(0xFF); b(0xDB); b(0x7E); b(0x3C); b(0x00);

   if (pc > 0xFFA)
   {
      fprintf(stderr, "program too long\n");
      errors++;
   }

   rom[0xFFA] = rom[0xFFC] = rom[0xFFE] = ORG & 0xFF;
   rom[0xFFB] = rom[0xFFD] = rom[0xFFF] = ORG >> 8;
}

int main(int argc, char **argv)
{
   FILE *f;
   if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "pal")))
   {
      fprintf(stderr, "usage: rom2600 <out.a26> [pal]\n");
      return 2;
   }
   pal = argc == 3;
   memset(rom, 0xFF, sizeof(rom));
   for (pass = 0; pass < 2; pass++)
      program();
   if (errors)
      return 1;
   if (!(f = fopen(argv[1], "wb")) || fwrite(rom, 1, sizeof(rom), f) != sizeof(rom))
   {
      fprintf(stderr, "cannot write %s\n", argv[1]);
      return 1;
   }
   fclose(f);
   printf("wrote %s: %u bytes of program\n", argv[1], pc);
   return 0;
}
