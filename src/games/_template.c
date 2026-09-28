/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A game module to begin from. Copy it to a file of the game's name, give `template` and
 * `tp` the game's, add the game to games.h, and fill in what is marked. `make lint-games`
 * says what is missing; docs/GAME_MODULES.md has the whole of it.
 *
 * <Title> (<publisher>, <year>).
 *
 * What the game draws with what, on the rows Stella shows of it:
 *
 *   rows  ..       <the score: playfield? players?>
 *   rows  ..       <the enemies: which object, how many copies, in turns or all at once>
 *   rows  ..       <the player>
 *   shots          <missiles? the ball?>
 *
 * Of its memory ($80 is 0): <byte> has <what>.
 *
 * Its sounds are listed where they are told apart, below.
 */
#include "../kit.h"

#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * The ROMs, the defaults and the options
 * ------------------------------------------------------------------------- */

/* Every release the module is for: `md5sum` of the file. */
static const char *const md5[] = {
   "00000000000000000000000000000000",   /* <Title> (<region>) */
   NULL
};

/* What suits the game of the effects every game gets (fx.h), where that is not the
 * default. */
static const char *const fx[] = {
   "glow", "high",
   NULL
};

/* The module's own options are proteus_<two letters of the game>_<what>. These four are
 * what a player looks for in every game; a game that has no use for one leaves it out. */
#define OPT_COLORS   "proteus_tp_colors"
#define OPT_BACKDROP "proteus_tp_backdrop"
#define OPT_SPARKS   "proteus_tp_sparks"
#define OPT_SOUND    "proteus_tp_sound"

static const char *const colors[] = { "proteus", "Proteus's", "original", "The game's own", NULL };
static const char *const backdrop[] = { "on", "On", "off", "Off", NULL };
/* In the order of the option's values. */
enum { COLORS_PROTEUS = 0, COLORS_ORIGINAL };

static const px_game_option options[] = {
   { OPT_COLORS, "Colours", "Colours of Proteus's choosing, or the game's own.", "proteus", colors },
   { OPT_BACKDROP, "Backdrop", "What is behind the game where its background is black.", "on", backdrop },
   { OPT_SPARKS, "Explosions", "Sparks where something is hit.", "enabled", px_kit_toggle },
   { OPT_SOUND, "Sounds",
     "Sounds of Proteus's own in place of the game's, each where it happens between left and right. Needs the enhanced sound.",
     "proteus", px_kit_sounds },
   { NULL, NULL, NULL, NULL, NULL }
};

/* ---------------------------------------------------------------------------
 * What the module keeps from frame to frame
 * ------------------------------------------------------------------------- */

typedef struct
{
   /* The options. */
   unsigned colors;
   bool backdrop, sparks, own_sound;

   uint32_t frame;      /* counts the frames that advance */
   px_kit_tia tia;      /* the game's two voices */
   px_kit_tags known;   /* what is known of the objects on the screen */
   int lives;           /* as memory had it; -1 before the first frame */
} game;

/* ---------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------- */

/* What an object is in the game, from where it is and what it is drawn with. */
static unsigned role_of(const px_instance *in)
{
   (void)in;
   return PX_ROLE_NONE;
}

static void frame(void *state, px_scene *s)
{
   game *g = (game*)state;
   px_objects *o = s->objects;

   /* A frame that is drawn again (the game is paused) is to look as it did: nothing moves
    * on, nothing is counted, nothing bursts. */
   if (s->advance)
      g->frame++;

   for (unsigned i = 0; i < o->count; i++)
   {
      px_instance *in = &o->inst[i];
      in->role = (uint8_t)role_of(in);
      if (g->colors == COLORS_ORIGINAL)
         continue;
      switch (in->role)
      {
         case PX_ROLE_ENEMY: px_scene_tint(s, in, 0xFF5FD2); break;
         default:            break;
      }
   }

   if (s->advance)
      g->lives = px_kit_ram(s->ram, s->ram_size, 0);
}

/* ---------------------------------------------------------------------------
 * The sounds
 *
 *   voice 0   <what>: waveform <n> at pitch <n>, <how long>
 *   voice 1   <what>: ...
 *
 * tools/2600/sounds.sh lists them from a run of the game.
 * ------------------------------------------------------------------------- */

static void sound(void *state, px_sound *s)
{
   game *g = (game*)state;

   px_kit_tia_hear(&g->tia, s->frame);

   /* A sound Proteus plays in the game's place: the game's voice is silent for as long as
    * it plays what is known. What is not known is heard as the game plays it. */
   if (g->own_sound && px_kit_tia_began(&g->tia, 0) && g->tia.wave[0] == 8)
   {
      static const px_tone boom[2] = {
         /* wave          freq  to   glide  attack  hold   decay  gain   cutoff to */
         { PX_WAVE_NOISE, 7000, 900, 0.25f, 0,      0.02f, 0.30f, 0.46f, 5000, 500, 0, 0 },
         { PX_WAVE_SINE,   140,  45, 0.12f, 0.001f, 0.02f, 0.25f, 0.70f, 0, 0, 0, 0 }
      };
      px_kit_play(s, boom, 2, px_kit_pan(-1));
      px_sound_rumble(s, 22000, 30000, 8);
   }
   if (g->own_sound && g->tia.wave[0] == 8)
      s->voice[0] = 0.0f;
}

/* ---------------------------------------------------------------------------
 * The module
 * ------------------------------------------------------------------------- */

/* The game was reset, or a state was loaded: what was known is not so any more. */
static void reset(void *state)
{
   game *g = (game*)state;
   g->lives = -1;
   px_kit_tia_reset(&g->tia);
   px_kit_tags_reset(&g->known);
}

static void *create(void)
{
   game *g = (game*)calloc(1, sizeof(game));
   if (g)
      reset(g);
   return g;
}

static void destroy(void *state)
{
   free(state);
}

static void configure(void *state, const char *(*get)(const char *key))
{
   game *g = (game*)state;
   g->colors    = px_kit_pick(get, OPT_COLORS, colors);
   g->backdrop  = px_kit_on(get, OPT_BACKDROP);
   g->sparks    = px_kit_on(get, OPT_SPARKS);
   g->own_sound = px_kit_on(get, OPT_SOUND);
}

const px_game px_game_template = {
   "Template", md5, fx, options, create, destroy, reset, configure, frame, sound
};
