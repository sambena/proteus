#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs games through Stella, its capture build and Proteus, and checks that what Proteus is
# given and what it shows without effects is what Stella shows. Bring your own games.
#
#   test/games2600.sh <test dir> <out dir> <rom>...
#
# <test dir> is where make test2600 put the harness and the cores. For every game this
# writes <out dir>/<n>_stella.bmp, <n>_proteus.bmp, <n>_any.bmp (without what Proteus knows
# of the game in particular), <n>_objects.bmp and <n>.wav.
#
# Give the ROMs as C:/... and not as /c/...: MSYS2 leaves arguments with brackets in them
# as they are.
#
# Stella as its authors build it ignores "phosphor mode: off" when a game starts, and
# blends the frames of games whose properties ask for it. So it is compared with the capture
# build with phosphor left to the game, and everything else with phosphor off, which the
# capture build does turn off.
#
# Stella does not run every game the same way twice, even from a saved state: what Pitfall II
# shows depends on how long frames take to emulate, and Pitfall! is another game once in a
# while. Two runs that are the same were the same game, which chance does not bring about
# in 1200 frames; two that differ are run again before they count as different.

set -u
DIR=$1
OUT=$2
shift 2
FRAMES=${FRAMES:-1200}
H=$DIR/harness2600
# dll, so or dylib: as the cores make test2600 left there are named
E=dll
for e in so dylib; do [ -f "$DIR/px/stellapx_libretro.$e" ] && E=$e; done
FAIL=0
N=0

mkdir -p "$OUT"
OFF="--opt proteus_fx_glow=off --opt proteus_fx_shadow=disabled --opt proteus_fx_smooth=disabled \
 --opt proteus_fx_flicker=disabled --opt proteus_fx_trails=disabled --opt proteus_fx_background=disabled \
 --opt proteus_fx_reactive=disabled --opt proteus_fx_scanlines=disabled --opt proteus_fx_bars=disabled \
 --opt proteus_fx_game=disabled --opt proteus_fx_audio=disabled"
PLAIN="--opt stella_phosphor=off"
SILENT="--opt proteus_fx_audio=disabled"
STELLA=$DIR/stock/stella_libretro.$E
PX=$DIR/px/stellapx_libretro.$E
PROTEUS=$DIR/px/proteus_stellapx_libretro.$E

sum() { sha1sum < "$1" | cut -c1-40; }

run() {   # name, core, options
   name=$1; core=$2; shift 2
   "$H" "$core" "$ROM" "$FRAMES" $COMMON --hashes "$OUT/$N.$name.txt" "$@" > "$OUT/$N.$name.log" 2>&1
}

variant() {   # name
   case $1 in
      stock)   run stock "$STELLA" ;;
      slow)    run slow "$STELLA" --slow 3 ;;
      same)    run same "$PX" ;;
      px)      run px "$PX" $PLAIN --bmp "$OUT/${N}_stella.bmp" ;;
      capture) run capture "$PX" $PLAIN --capture ;;
      plain)   run plain "$PROTEUS" $OFF --native ;;
      full)    run full "$PROTEUS" --bmp "$OUT/${N}_proteus.bmp" --wav "$OUT/$N.wav" ;;
      game)    run game "$PROTEUS" $SILENT ;;
      any)     run any "$PROTEUS" $SILENT --opt proteus_fx_game=disabled --bmp "$OUT/${N}_any.bmp" ;;
      objects) run objects "$PROTEUS" $SILENT --opt proteus_fx_view=instances --bmp "$OUT/${N}_objects.bmp" ;;
   esac
}

equal() { [ -s "$OUT/$N.$1.txt" ] && [ "$(sum "$OUT/$N.$1.txt")" = "$(sum "$OUT/$N.$2.txt")" ]; }

check() {   # label, a, b
   tries=1
   until equal "$2" "$3" || [ "$tries" -ge 4 ]; do
      variant "$2"
      variant "$3"
      tries=$((tries + 1))
   done
   if equal "$2" "$3"; then
      if [ "$tries" -gt 1 ]; then note=" (at try $tries)"; else note=; fi
      printf '   %-52s ok%s\n' "$1" "$note"
   else
      printf '   %-52s FAIL\n' "$1"
      FAIL=$((FAIL + 1))
   fi
}

for ROM in "$@"; do
   N=$((N + 1))
   echo "$N: $(basename "$ROM")"
   # Stella starts games with memory of chance; every run starts from this state instead.
   "$H" "$STELLA" "$ROM" 30 --quiet --sysdir "$OUT" \
      --state-out "$OUT/$N.state" > "$OUT/$N.state.log" 2>&1
   COMMON="--quiet --input --sysdir $OUT --state-in $OUT/$N.state"

   variant stock
   if [ ! -s "$OUT/$N.stock.txt" ]; then
      echo "   Stella did not run it: FAIL"
      head -3 "$OUT/$N.stock.log" | sed 's/^/   /'
      FAIL=$((FAIL + 1))
      continue
   fi
   for v in same px capture plain full game any objects slow; do
      variant $v
   done

   if grep -q '^capture ok' "$OUT/$N.capture.log"; then r=ok; else r=FAIL; FAIL=$((FAIL + 1)); fi
   printf '   %-52s %s\n' "the capture agrees with every frame" "$r"
   if ! equal stock slow; then
      echo "   Stella's frames depend on the time they take: runs are not compared"
   else
      check "the capture build's frames are Stella's" stock same
      check "capturing changes no frame" px capture
      check "Proteus without effects shows Stella's frames" px plain
   fi
   if ! equal game any; then
      echo "   Proteus knows this game: ${N}_proteus.bmp is with what it knows, ${N}_any.bmp without"
   else
      echo "   Proteus draws it as any game"
   fi
   grep -E '^capture pixels|^objects' "$OUT/$N.capture.log" | sed 's/^/   /'
   grep -E '^video|^time|^sound|^rumble' "$OUT/$N.full.log" | sed 's/^/   /'
done

if [ "$FAIL" = 0 ]; then
   echo "PASSED (0 failures)"
else
   echo "FAILED ($FAIL failures)"
fi
exit "$FAIL"
