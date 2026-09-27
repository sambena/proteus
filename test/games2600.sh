#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs games through Stella, its capture build and Proteus, and checks that what Proteus is
# given and what it shows without effects is what Stella shows. Bring your own games.
#
#   test/games2600.sh <test dir> <out dir> <rom>...
#
# <test dir> is where make test2600 put the harness and the cores. For every game this
# writes <out dir>/<n>_stella.bmp, <n>_proteus.bmp, <n>_objects.bmp and <n>.wav.
#
# Stella as its authors build it ignores "phosphor mode: off" when a game starts, and
# blends the frames of games whose properties ask for it. So it is compared with the capture
# build with phosphor left to the game, and everything else with phosphor off, which the
# capture build does turn off.

set -u
DIR=$1
OUT=$2
shift 2
FRAMES=${FRAMES:-1200}
H=$DIR/harness2600
FAIL=0
N=0

mkdir -p "$OUT"
OFF="--opt proteus_fx_glow=off --opt proteus_fx_shadow=disabled --opt proteus_fx_smooth=disabled \
 --opt proteus_fx_flicker=disabled --opt proteus_fx_trails=disabled --opt proteus_fx_background=disabled \
 --opt proteus_fx_reactive=disabled --opt proteus_fx_scanlines=disabled --opt proteus_fx_bars=disabled --opt proteus_fx_audio=disabled"
COMMON="--quiet --input --sysdir $OUT"
PLAIN="--opt stella_phosphor=off"

sum() { sha1sum < "$1" | cut -c1-40; }

check() {   # label, a, b
   if [ -s "$OUT/$N.$2.txt" ] && [ "$(sum "$OUT/$N.$2.txt")" = "$(sum "$OUT/$N.$3.txt")" ]; then
      printf '   %-52s ok\n' "$1"
   else
      printf '   %-52s FAIL\n' "$1"
      FAIL=$((FAIL + 1))
   fi
}

for ROM in "$@"; do
   N=$((N + 1))
   echo "$N: $(basename "$ROM")"
   # Stella starts games with memory of chance; every run starts from this state instead.
   "$H" "$DIR/stock/stella_libretro.dll" "$ROM" 30 --quiet --sysdir "$OUT" \
      --state-out "$OUT/$N.state" > "$OUT/$N.state.log" 2>&1
   COMMON="--quiet --input --sysdir $OUT --state-in $OUT/$N.state"
   "$H" "$DIR/stock/stella_libretro.dll" "$ROM" "$FRAMES" $COMMON \
      --hashes "$OUT/$N.stock.txt" > "$OUT/$N.stock.log" 2>&1
   "$H" "$DIR/px/stellapx_libretro.dll" "$ROM" "$FRAMES" $COMMON \
      --hashes "$OUT/$N.same.txt" > "$OUT/$N.same.log" 2>&1
   "$H" "$DIR/px/stellapx_libretro.dll" "$ROM" "$FRAMES" $COMMON $PLAIN \
      --hashes "$OUT/$N.px.txt" --bmp "$OUT/${N}_stella.bmp" > "$OUT/$N.px.log" 2>&1
   "$H" "$DIR/px/stellapx_libretro.dll" "$ROM" "$FRAMES" $COMMON $PLAIN --capture \
      --hashes "$OUT/$N.capture.txt" > "$OUT/$N.capture.log" 2>&1
   captured=$?
   "$H" "$DIR/px/proteus_stellapx_libretro.dll" "$ROM" "$FRAMES" $COMMON $OFF --native \
      --hashes "$OUT/$N.plain.txt" > "$OUT/$N.plain.log" 2>&1
   "$H" "$DIR/px/proteus_stellapx_libretro.dll" "$ROM" "$FRAMES" $COMMON \
      --bmp "$OUT/${N}_proteus.bmp" --wav "$OUT/$N.wav" > "$OUT/$N.full.log" 2>&1
   "$H" "$DIR/px/proteus_stellapx_libretro.dll" "$ROM" "$FRAMES" $COMMON \
      --opt proteus_fx_audio=disabled --opt proteus_fx_view=instances \
      --bmp "$OUT/${N}_objects.bmp" > "$OUT/$N.objects.log" 2>&1

   if [ ! -s "$OUT/$N.stock.txt" ]; then
      echo "   Stella did not run it: FAIL"
      head -3 "$OUT/$N.stock.log" | sed 's/^/   /'
      FAIL=$((FAIL + 1))
      continue
   fi
   if [ "$captured" = 0 ]; then r=ok; else r=FAIL; FAIL=$((FAIL + 1)); fi
   printf '   %-52s %s\n' "the capture agrees with every frame" "$r"
   # Some games are not the same twice in Stella even from a state: what Pitfall II shows
   # depends on how long frames take to emulate. Runs of those cannot be compared, since
   # capturing and drawing take time. Stella is run once more as it is and once held up.
   steady=yes
   "$H" "$DIR/stock/stella_libretro.dll" "$ROM" "$FRAMES" $COMMON \
      --hashes "$OUT/$N.again.txt" > "$OUT/$N.again.log" 2>&1
   [ "$(sum "$OUT/$N.stock.txt")" = "$(sum "$OUT/$N.again.txt")" ] || steady=no
   "$H" "$DIR/stock/stella_libretro.dll" "$ROM" "$FRAMES" $COMMON --slow 3 \
      --hashes "$OUT/$N.again.txt" > "$OUT/$N.again.log" 2>&1
   [ "$(sum "$OUT/$N.stock.txt")" = "$(sum "$OUT/$N.again.txt")" ] || steady=no
   if [ "$steady" = no ]; then
      echo "   Stella's frames depend on the time they take: runs are not compared"
   else
      check "the capture build's frames are Stella's" stock same
      check "capturing changes no frame" px capture
      check "Proteus without effects shows Stella's frames" px plain
   fi
   grep -E '^capture pixels|^objects' "$OUT/$N.capture.log" | sed 's/^/   /'
   grep -E '^video|^time|^sound' "$OUT/$N.full.log" | sed 's/^/   /'
done

if [ "$FAIL" = 0 ]; then
   echo "PASSED (0 failures)"
else
   echo "FAILED ($FAIL failures)"
fi
exit "$FAIL"
