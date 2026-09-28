/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The games Proteus knows in particular. A game is added by a file of its name in this
 * directory, which the Makefile finds by itself, and a line here. docs/GAME_MODULES.md has
 * the rest. */
#ifndef PROTEUS_GAMES_H
#define PROTEUS_GAMES_H

#define PX_GAMES(X) \
   X(space_invaders) \
   X(pac_man) \
   X(adventure)

#endif
