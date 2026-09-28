/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Ms. Pac-Man (Atari, 1983).
 *
 * What the game draws with what, on the 228 rows Stella shows of it:
 *
 *   rows  12..182  the maze: the background is its floor, the playfield its walls and its
 *                  wafers (two rows high). The four mazes have colours of their own.
 *   Ms. Pac-Man    player 0, ten rows high, colour 2A
 *   ghosts         player 1, ten rows high, two of the four a frame at most, each in a
 *                  colour of its own: 46 (red), 58 (pink), B8 (cyan), 26 (orange). All are
 *                  96 while they can be eaten, and 0C and 96 in turns while that is about
 *                  to end. Eaten, a ghost is a pair of eyes five rows high in its own
 *                  colour.
 *   power pills    the ball: the two at the left and the two at the right in turns
 *   rows 184..     her lives (copies of player 0) and the fruit of the maze (player 1),
 *                  and below them the score, of both players
 *
 * The game moves its objects on many lines (HMOVE), which blanks eight pixels at the left
 * of each: what is there of an object is cut into pieces. They are coloured as the object
 * is, by its colour.
 *
 * Of its memory ($80 is 0):
 *
 *   6..9     the ghosts' columns, 12 more than where they are drawn: orange, cyan, pink, red
 *   10       her column, likewise       16       her row, 12 less than where she is drawn
 *   12..15   the ghosts' rows, likewise
 *   119      the wafers eaten
 *
 * Not known: what the fruit is drawn with while it moves through the maze. What is in the
 * maze and none of the above shows as the game draws it.
 */
#include "pac_family.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * What the game is like
 * ------------------------------------------------------------------------- */

#define MAZE_TOP   12
#define MAZE_END   183    /* the row after its last */

#define RAM_GHOST_X 6
#define RAM_HER_X   10
#define RAM_GHOST_Y 12
#define RAM_WAFERS  119
#define RAM_LESS    12     /* what memory has of a place is this far from where it is drawn */
#define WAFERS      150    /* in a maze, more or less: the mazes differ */

#define COLOR_HER     0x2A
#define COLOR_SCARED  0x96
#define COLOR_PALE    0x0C
#define SHAPE_EYES    0xE69CFF64u

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

static const char *const md5[] = {
   "87e79cd41ce136fd4f72cc6e2c161bee",   /* Ms. Pac-Man (USA) */
   NULL
};

static const char *const fx[] = {
   "glow", "high",
   "width", "50",
   "reverb", "small",
   NULL
};

#define OPT_COLORS "proteus_mp_colors"
#define OPT_MAZE   "proteus_mp_maze"
#define OPT_WAFERS "proteus_mp_wafers"
#define OPT_SPARKS "proteus_mp_sparks"
#define OPT_SOUND  "proteus_mp_sound"
#define OPT_SIREN  "proteus_mp_siren"

static const char *const colors[] = { "arcade", "The arcade's", "original", "The game's own", NULL };
static const char *const mazes[] = { "neon", "Outlines on black", "solid", "Walls on black",
   "original", "The game's own", NULL };
static const char *const wafers[] = { "dots", "Round dots", "original", "The game's dashes", NULL };
/* In the order of the options' values. */
enum { COLORS_ARCADE = 0, COLORS_ORIGINAL };
enum { MAZE_NEON = 0, MAZE_SOLID, MAZE_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours",
     "The ghosts in the red, pink, cyan and orange of the arcade, with eyes, blue while they can be eaten and pale before that ends; Ms. Pac-Man in yellow. Or the game's own colours.",
     "arcade", colors },
   { OPT_MAZE, "Maze",
     "The maze as glowing outlines on black, in the colour the game gives it; as solid walls on black; or as the game draws it.",
     "neon", mazes },
   { OPT_WAFERS, "Wafers",
     "What Ms. Pac-Man eats as round dots, or as the dashes the game draws.", "dots", wafers },
   { OPT_SPARKS, "Explosions",
     "Sparks where a ghost is eaten and where Ms. Pac-Man is caught, and a flash when she eats a power pill.",
     "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own for eating, the power pill and a ghost eaten, each where it happens between left and right, and the game's tunes in softer voices. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { OPT_SIREN, "Siren",
     "A siren under the chase that rises as the wafers get fewer, and another while the ghosts can be eaten, where the game is silent. Needs the sounds to be Proteus's.",
     "enabled", px_kit_toggle },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   /* The options. */
   unsigned colors, maze;
   bool dots, sparks, own_sound, siren;

   uint32_t frame;                  /* counts the pictures that advance */

   /* The picture. */
   unsigned mode;                   /* PAC_IDLE and the like */
   unsigned scared_for;             /* frames since a ghost was seen that can be eaten */
   unsigned chased_for;             /* and since one was seen in its own colour */
   bool eyes[PAC_GHOSTS];           /* a ghost was a pair of eyes when it was drawn last */
   uint8_t *small;                  /* where the wafers are: an entry a captured pixel */
   int her_x, her_y;                /* where she was seen last; -1: nowhere yet */

   /* What one of the two hooks found out for the other. */
   bool pill_eaten;                 /* the picture saw it: the sound is to be played */
   bool caught;                     /* the sound heard it: the picture is to burst */

   /* The sounds. */
   px_kit_tia tia;                  /* the game's two voices */
   pac_voices voices;               /* what of Proteus's sounds goes on */
   bool heard[2];                   /* a voice was to be heard in the frame before */
   unsigned eaten_for;              /* frames since a ghost was eaten; 0: none was */
   unsigned moved;                  /* frames since a ghost last moved */
   uint8_t ghost_at[2 * PAC_GHOSTS];
   int pac_at;                      /* her column, for where a sound is; -1: not known */
} mp;

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

static bool in_maze(const px_instance *in)
{
   return in->y + (int)in->h > MAZE_TOP && in->y < MAZE_END;
}

/* Which ghost has a colour: in the order of the arcade's. -1: none. */
static int ghost_of_color(unsigned color)
{
   switch (color)
   {
      case 0x46: return 0;
      case 0x58: return 1;
      case 0xB8: return 2;
      case 0x26: return 3;
      default:   return -1;
   }
}

static bool is_ghost_color(unsigned color)
{
   return ghost_of_color(color) >= 0 || color == COLOR_SCARED || color == COLOR_PALE;
}

/* Which ghost is drawn at a place, by where memory has them: for those that have one
 * colour while they can be eaten. -1: none of them is near. */
static int ghost_at(const px_glance *g, const px_instance *in)
{
   /* Memory has them in this order. */
   static const int8_t order[PAC_GHOSTS] = { 3, 2, 1, 0 };
   int best = -1, best_far = 10;
   for (unsigned i = 0; i < PAC_GHOSTS; i++)
   {
      const int x = px_kit_ram(g->ram, g->ram_size, RAM_GHOST_X + i);
      const int y = px_kit_ram(g->ram, g->ram_size, RAM_GHOST_Y + i);
      int far;
      if (x < 0 || y < 0)
         return -1;
      far = abs(x - RAM_LESS - in->x) + abs(y + RAM_LESS - in->y);
      if (far < best_far)
      {
         best_far = far;
         best     = order[i];
      }
   }
   return best;
}

/* The ghosts cross each other and leave the maze at its sides to come back at the other:
 * each is who its colour says, or who memory has where it is drawn, wherever that is. The
 * power pills are there in turns, the left and the right. */
static unsigned who(void *state, const px_glance *g, const px_instance *in)
{
   (void)state;
   if (!in_maze(in))
      return 0;
   if (in->cls == PXC_L_P1 && is_ghost_color(in->color) && in->h >= 5)
   {
      int which = ghost_of_color(in->color);
      if (which < 0)
         which = ghost_at(g, in);
      return which < 0 ? 0 : PX_WHO_ANYWHERE | (unsigned)(which + 1);
   }
   return in->cls == PXC_L_BL ? 1 : 0;
}

static bool is_eyes(const px_instance *in)
{
   return in->h == 5 && in->hash == SHAPE_EYES;
}

/* Roles, and what the ghosts are about. */
static void find_things(mp *g, px_scene *s)
{
   px_objects *o = s->objects;
   bool scared = false, pale = false, chased = false;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role = PX_ROLE_NONE;
      if (!in_maze(in))
      {
         if (in->y >= MAZE_END)
            in->role = PX_ROLE_HUD;
         continue;
      }
      if (in->cls == PXC_L_P0 && in->color == COLOR_HER)
      {
         in->role = PX_ROLE_PLAYER;
         if (!in->ghost && in->h >= 9 && s->advance)
         {
            g->her_x = in->x + 4;
            g->her_y = in->y + 5;
         }
      }
      else if (in->cls == PXC_L_BL)
         in->role = PX_ROLE_BONUS;
      else if (in->cls == PXC_L_P1 && is_ghost_color(in->color))
      {
         in->role  = PX_ROLE_ENEMY;
         in->group = in->who & 0x7F ? (uint8_t)((in->who & 0x7F) - 1) : 0;
         if (in->ghost || is_eyes(in))
            continue;
         scared = scared || in->color == COLOR_SCARED;
         pale   = pale || in->color == COLOR_PALE;
         chased = chased || ghost_of_color(in->color) >= 0;
      }
   }

   if (!s->advance)
      return;
   /* The ghosts are shown in turns: what none is seen to be about for a while is over. */
   g->scared_for = scared || pale ? 0 : g->scared_for + 1;
   g->chased_for = chased ? 0 : g->chased_for + 1;
   if (pale)
      g->mode = PAC_WARNING;
   else if (scared && g->mode != PAC_WARNING)
      g->mode = PAC_SCARED;
   else if (g->scared_for > 8)
      g->mode = g->chased_for > 8 ? PAC_IDLE : PAC_CHASE;
}

/* The colour the game gives the maze, as light: as bright as it gets. */
static uint32_t maze_color(const px_scene *s)
{
   const struct pxc_frame *f = s->frame;
   for (unsigned y = MAZE_TOP; y < MAZE_END && y < f->height; y++)
      for (unsigned x = 8; x < PXC_W; x += 4)
      {
         const size_t i = (size_t)y * PXC_W + x;
         if ((f->tags[i] & (PXC_PF | PXC_BLANK)) == PXC_PF)
         {
            const uint32_t rgb = f->palette[f->color[PXC_L_PF][i]] & 0xFFFFFFu;
            const unsigned r = (rgb >> 16) & 0xFF, gr = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
            const unsigned most = r > gr ? (r > b ? r : b) : (gr > b ? gr : b);
            return most ? px_rgb_scale(rgb, 255u * 256u / most) : PAC_RGB_WALL;
         }
      }
   return PAC_RGB_WALL;
}

static void draw_maze(mp *g, px_scene *s)
{
   if (!g->small)
      return;
   px_kit_small_playfield(s, MAZE_TOP, MAZE_END, 3, g->small);

   if (g->maze != MAZE_ORIGINAL)
   {
      const uint32_t wall = maze_color(s);
      px_kit_background(s, 0, s->frame->height, 0x000000);
      if (g->maze == MAZE_NEON)
         px_kit_outline(s, MAZE_TOP, MAZE_END, wall, px_rgb_scale(wall, 36),
               px_rgb_scale(wall, 110), g->small);
      else
         px_kit_outline(s, MAZE_TOP, MAZE_END, wall, wall, 0, g->small);
   }
   if (g->dots || g->colors == COLORS_ARCADE)
      px_kit_dots(s, g->small, MAZE_TOP, MAZE_END, g->colors == COLORS_ARCADE, PAC_RGB_WAFER,
            g->dots);
}

static void color_things(mp *g, px_scene *s)
{
   px_objects *o = s->objects;

   if (g->colors != COLORS_ARCADE)
      return;
   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      switch (in->role)
      {
         case PX_ROLE_ENEMY:
         {
            /* A ghost is about what its colour says: one that was eaten and is back is
             * chased by none while the others still are. The game makes them pale and
             * blue in turns itself. */
            const unsigned about = in->color == COLOR_SCARED ? PAC_SCARED
                  : in->color == COLOR_PALE ? PAC_WARNING : PAC_CHASE;
            uint32_t eyes = 0;
            px_scene_tint(s, in, pac_ghost_color(about, 8, in->group, is_eyes(in), &eyes));
            if (eyes && in->h >= 9)
               px_kit_fill_holes(s, in, eyes);
            break;
         }
         case PX_ROLE_PLAYER:
            px_scene_tint(s, in, PAC_RGB_PAC);
            break;
         case PX_ROLE_BONUS:
            px_scene_tint(s, in, pac_pill_color(g->frame));
            px_scene_energy(s, in, true);
            break;
         case PX_ROLE_HUD:
            /* Her lives, which are of her colour but for its brightness. */
            if (in->cls == PXC_L_P0 && in->y < MAZE_END + 12)
               px_scene_tint(s, in, PAC_RGB_PAC);
            break;
         default:
            break;
      }
   }
}

/* What happened since the frame before. */
static void find_events(mp *g, px_scene *s, unsigned mode_before)
{
   px_objects *o = s->objects;

   for (unsigned i = 0; i < o->count; i++)
   {
      const px_instance *in = &o->inst[i];
      if (in->ghost || in->role != PX_ROLE_ENEMY || ghost_of_color(in->color) < 0)
         continue;
      if (is_eyes(in) && !g->eyes[in->group & 3] && g->sparks)
      {
         px_scene_burst(s, in->x + 4, in->y + 3, pac_ghost_colors[in->group & 3], 26, 340);
         px_scene_burst(s, in->x + 4, in->y + 3, PAC_RGB_SCARED, 14, 220);
      }
      if (in->h == 5 || in->h >= 9)
         g->eyes[in->group & 3] = is_eyes(in);
   }

   if (g->mode == PAC_SCARED && mode_before != PAC_SCARED && mode_before != PAC_WARNING)
   {
      g->pill_eaten = true;
      if (g->sparks)
         px_scene_flash(s, PAC_RGB_SCARED, 80);
   }
   if (g->caught)
   {
      if (g->sparks && g->her_x >= 0)
      {
         px_scene_flash(s, 0xFF3020, 90);
         px_scene_burst(s, g->her_x, g->her_y, PAC_RGB_PAC, 36, 400);
      }
      g->caught = false;
   }
}

static void frame(void *state, px_scene *s)
{
   mp *g = (mp*)state;
   const unsigned mode_before = g->mode;

   if (s->advance)
      g->frame++;

   find_things(g, s);
   draw_maze(g, s);
   color_things(g, s);
   if (s->advance)
      find_events(g, s, mode_before);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 * The game leaves the volume at 15 and makes a voice silent by pitch 0 of waveform 4 or 5,
 * which is higher than is heard. It has:
 *
 *   both      its tunes, in notes of 8 and 16 frames of waveform 4: before a game (272
 *             frames; voice 0 has the melody at pitch 15 to 27, voice 1 what goes with it
 *             at 23 to 31), and when Ms. Pac-Man is caught (after 32 frames of silence:
 *             voice 0 at 11, 12, 13, 14, 15 and voice 1 at 15, 17, 18, 20, 18)
 *   voice 0   a wafer eaten: waveform 5 at pitch 30, 26, 23 and 0, a frame each, twice
 *   voice 0   a ghost eaten: waveform 4 at pitch 24 and 16, eight frames each, voice 1
 *             silent
 *
 * A power pill eaten sounds as a wafer does, and the game is silent while the ghosts can
 * be eaten and while they chase, where the arcade had its sirens.
 * ------------------------------------------------------------------------- */

/* A voice is to be heard. */
static bool sounds(const px_kit_tia *t, unsigned voice)
{
   return t->volume[voice] && !((t->wave[voice] == 4 || t->wave[voice] == 5) && !t->pitch[voice]);
}

static bool plays_note(const px_kit_tia *t, unsigned voice)
{
   return t->wave[voice] == 4 && sounds(t, voice);
}

/* Whether the game is on: the ghosts move. They stand while a tune plays. */
static bool ghosts_move(mp *g, const px_sound *s)
{
   bool moved = false;
   for (unsigned i = 0; i < 2 * PAC_GHOSTS; i++)
   {
      const unsigned at = i < PAC_GHOSTS ? RAM_GHOST_X + i : RAM_GHOST_Y + i - PAC_GHOSTS;
      const int v = px_kit_ram(s->ram, s->ram_size, at);
      if (v < 0)
         return false;
      if (g->ghost_at[i] != (uint8_t)v)
         moved = true;
      g->ghost_at[i] = (uint8_t)v;
   }
   g->moved = moved ? 0 : g->moved + 1;
   return g->moved < 12;
}

static void sound(void *state, px_sound *s)
{
   mp *g = (mp*)state;
   const bool could_eat = g->mode == PAC_SCARED || g->mode == PAC_WARNING;
   bool note[2], tune, running, scared;
   int eaten;

   /* Where she is: from the picture before, or from memory. */
   g->pac_at = -1;
   for (unsigned i = 0; s->objects && i < s->objects->count; i++)
      if (s->objects->inst[i].role == PX_ROLE_PLAYER && !s->objects->inst[i].ghost)
         g->pac_at = s->objects->inst[i].x + 4;
   if (g->pac_at < 0 && px_kit_ram(s->ram, s->ram_size, RAM_HER_X) >= RAM_LESS)
      g->pac_at = px_kit_ram(s->ram, s->ram_size, RAM_HER_X) - RAM_LESS + 4;

   g->voices.ticks++;
   px_kit_tia_hear(&g->tia, s->frame);
   note[0] = plays_note(&g->tia, 0);
   note[1] = plays_note(&g->tia, 1);

   /* A wafer: at the first of its two times four pitches. */
   if (g->tia.wave[0] == 5 && g->tia.volume[0] && g->tia.pitch[0] == 30
         && !(g->tia.was_wave[0] == 5 && g->tia.was_volume[0]))
   {
      if (g->own_sound)
         pac_play_wafer(&g->voices, s, g->pac_at);
      PAC_RUMBLE_WAFER(s);
   }

   /* A ghost eaten: two notes that begin out of silence while ghosts can be eaten. Without
    * the picture that is not known, and they are played as the notes they are. */
   if (g->eaten_for && ++g->eaten_for > 18)
      g->eaten_for = 0;
   if (note[0] && !g->heard[0] && !note[1] && g->tia.pitch[0] == 24 && could_eat)
   {
      g->eaten_for = 1;
      if (g->own_sound)
         pac_play_eaten(s, g->pac_at);
      PAC_RUMBLE_EATEN(s);
   }

   /* The tunes, note by note: the melody an octave down, what goes with it two. */
   tune = (note[0] || note[1]) && !g->eaten_for;
   if (tune)
   {
      /* She is caught: the first notes of that tune. */
      if (note[0] && note[1] && g->tia.pitch[0] == 11 && g->tia.pitch[1] == 15
            && !(g->heard[0] && g->tia.was_pitch[0] == 11))
      {
         g->caught = true;
         PAC_RUMBLE_CAUGHT(s);
      }
      for (unsigned v = 0; v < 2 && g->own_sound; v++)
         if (note[v] && (!g->heard[v] || g->tia.pitch[v] != g->tia.was_pitch[v]))
            pac_play_note(s, v == 0, px_kit_tia_hz(4, g->tia.pitch[v]) / (v == 0 ? 2.0f : 4.0f));
   }
   g->heard[0] = note[0];
   g->heard[1] = note[1];

   /* A power pill, which the picture saw eaten. */
   if (g->pill_eaten)
   {
      if (g->own_sound)
         pac_play_pill(s, g->pac_at);
      PAC_RUMBLE_PILL(s);
      g->pill_eaten = false;
   }

   running = ghosts_move(g, s) && !tune;
   scared  = running && could_eat;
   eaten   = px_kit_ram(s->ram, s->ram_size, RAM_WAFERS);
   /* Faster when it is about to end. */
   pac_warble(&g->voices, s, g->own_sound && g->siren && scared,
         (float)(g->voices.ticks % (g->mode == PAC_WARNING ? 10 : 20))
               / (g->mode == PAC_WARNING ? 10.0f : 20.0f));
   pac_siren(&g->voices, s, g->own_sound && g->siren && running && g->mode == PAC_CHASE,
         eaten < 0 ? 0.0f : eaten >= WAFERS ? 1.0f : (float)eaten / (float)WAFERS);

   if (g->own_sound)
   {
      /* Both voices play what is known, or nothing. */
      if (!sounds(&g->tia, 0) || g->tia.wave[0] == 5 || note[0])
         s->voice[0] = 0.0f;
      if (!sounds(&g->tia, 1) || note[1])
         s->voice[1] = 0.0f;
   }
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

static void reset(void *state)
{
   mp *g = (mp*)state;
   px_kit_tia_reset(&g->tia);
   pac_voices_reset(&g->voices);
   memset(g->eyes, 0, sizeof(g->eyes));
   memset(g->ghost_at, 0, sizeof(g->ghost_at));
   g->mode       = PAC_IDLE;
   g->scared_for = g->chased_for = 1000;
   g->her_x      = g->her_y = -1;
   g->pill_eaten = g->caught = false;
   g->heard[0]   = g->heard[1] = false;
   g->eaten_for  = 0;
   g->moved      = 1000;
   g->pac_at     = -1;
}

static void *create(void)
{
   mp *g = (mp*)calloc(1, sizeof(mp));
   if (g)
   {
      g->small = (uint8_t*)calloc((size_t)PXC_W * PXC_MAX_H, 1);
      g->dots = g->sparks = g->own_sound = g->siren = true;
      reset(g);
   }
   return g;
}

static void destroy(void *state)
{
   mp *g = (mp*)state;
   if (g)
      free(g->small);
   free(g);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   mp *g = (mp*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->maze      = px_kit_pick(get, OPT_MAZE, mazes);
   g->dots      = px_kit_on(get, OPT_WAFERS);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
   g->siren     = px_kit_on(get, OPT_SIREN);
}

const px_game px_game_ms_pac_man = {
   "Ms. Pac-Man", md5, fx, options, create, destroy, reset, configure, frame, sound, who
};
