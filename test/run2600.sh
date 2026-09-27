#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks Proteus for the Atari 2600 against Stella, with the test program of rom2600.c.
#
#   test/run2600.sh <test dir> <stella_libretro> <stellapx_libretro> <proteus_libretro> [rom]
#
# <stella_libretro> is Stella as its authors build it, <stellapx_libretro> the build with the
# capture interface. Every check compares hashes of the frames of two runs, or states what
# it measures.

set -u
DIR=$1
STOCK=$2
PX=$3
PROTEUS=$4
ROM=${5:-$DIR/pxtest.a26}
FRAMES=${FRAMES:-600}
H=$DIR/harness2600
FAIL=0

# The wrapper finds its inner core next to itself, by its own name.
mkdir -p "$DIR/stock" "$DIR/px" "$DIR/out"
cp "$STOCK" "$DIR/stock/stella_libretro.dll"
cp "$PX" "$DIR/px/stellapx_libretro.dll"
cp "$PROTEUS" "$DIR/stock/proteus_stella_libretro.dll"
cp "$PROTEUS" "$DIR/px/proteus_stellapx_libretro.dll"

COMMON="--quiet --input --sysdir $DIR/out --opt stella_phosphor=off"

run() {   # name, core, options...
   name=$1; core=$2; shift 2
   "$H" "$core" "$ROM" "$FRAMES" $COMMON --hashes "$DIR/out/$name.txt" "$@" > "$DIR/out/$name.log" 2>&1
   echo "$?" > "$DIR/out/$name.rc"
}

same() {   # label, a, b
   if [ -s "$DIR/out/$2.txt" ] && [ "$(sha1sum < "$DIR/out/$2.txt")" = "$(sha1sum < "$DIR/out/$3.txt")" ]; then
      printf '  %-58s ok\n' "$1"
   else
      printf '  %-58s FAIL\n' "$1"
      FAIL=$((FAIL + 1))
   fi
}

differ() {   # label, a, b
   if [ -s "$DIR/out/$2.txt" ] && [ "$(sha1sum < "$DIR/out/$2.txt")" != "$(sha1sum < "$DIR/out/$3.txt")" ]; then
      printf '  %-58s ok\n' "$1"
   else
      printf '  %-58s FAIL\n' "$1"
      FAIL=$((FAIL + 1))
   fi
}

passed() {   # label, name
   if [ "$(cat "$DIR/out/$2.rc")" = 0 ]; then
      printf '  %-58s ok\n' "$1"
   else
      printf '  %-58s FAIL\n' "$1"
      FAIL=$((FAIL + 1))
   fi
}

line() {   # name, word
   grep "^$2" "$DIR/out/$1.log" | head -1
}

OFF="--opt proteus_fx_glow=off --opt proteus_fx_shadow=disabled --opt proteus_fx_smooth=disabled \
 --opt proteus_fx_flicker=disabled --opt proteus_fx_trails=disabled --opt proteus_fx_background=disabled \
 --opt proteus_fx_reactive=disabled --opt proteus_fx_scanlines=disabled --opt proteus_fx_bars=disabled"
NOSOUND="--opt proteus_fx_audio=disabled"

echo "Atari 2600: $FRAMES frames of $(basename "$ROM")"

run stock      "$DIR/stock/stella_libretro.dll"
run px_off     "$DIR/px/stellapx_libretro.dll"
run px_on      "$DIR/px/stellapx_libretro.dll" --capture --layers "$DIR/out/layers"
run wrap_stock "$DIR/stock/proteus_stella_libretro.dll" $NOSOUND
run native     "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND --opt proteus_fx_scale=native
run plain      "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND $OFF --native
run plain1920  "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND $OFF --native --opt proteus_fx_scale=1920
run disabled   "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND --opt proteus_fx_video=disabled
run full       "$DIR/px/proteus_stellapx_libretro.dll" --bmp "$DIR/out/full.bmp" --wav "$DIR/out/full.wav"
run fused      "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND $OFF --native --opt proteus_fx_flicker=enabled
run layers     "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND --opt proteus_fx_view=layers --bmp "$DIR/out/view_layers.bmp"
run boxes      "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND --opt proteus_fx_view=instances --bmp "$DIR/out/view_instances.bmp"
run mono       "$DIR/px/proteus_stellapx_libretro.dll" --opt proteus_fx_width=0 --opt proteus_fx_reverb=off
run wide       "$DIR/px/proteus_stellapx_libretro.dll" --opt proteus_fx_width=100 --opt proteus_fx_reverb=off
run noglow     "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND --opt proteus_fx_glow=off --opt proteus_fx_shadow=disabled

# A profile for the game, in the system directory, says the same as noglow's options.
mkdir -p "$DIR/out_profile/proteus"
printf '[fx]\nglow = off\nshadow = disabled\naudio = disabled\n' > "$DIR/out_profile/proteus/$(basename "$ROM" .a26).ini"
COMMON="--quiet --input --sysdir $DIR/out_profile --opt stella_phosphor=off"
run profile    "$DIR/px/proteus_stellapx_libretro.dll"
COMMON="--quiet --input --sysdir $DIR/out --opt stella_phosphor=off"

# 50 Hz
PAL_ROM=$(dirname "$ROM")/pxtest_pal.a26
if [ -f "$PAL_ROM" ]; then
   NTSC_ROM=$ROM
   ROM=$PAL_ROM
   run pal_stock "$DIR/stock/stella_libretro.dll"
   run pal_plain "$DIR/px/proteus_stellapx_libretro.dll" $NOSOUND $OFF --native
   run pal_full  "$DIR/px/proteus_stellapx_libretro.dll" --bmp "$DIR/out/pal_full.bmp"
   ROM=$NTSC_ROM
fi

# "objects L to M in a frame, T tracks in all; G drawn from their tracks in F frames"
objects() {
   label=$1
   set -- $(grep '^objects' "$DIR/out/px_on.log" | tr -cd '0-9 \n')
   most=${2:-0}; tracks=${3:-999}; ghost_frames=${5:-0}
   # 18 invaders, the cannon, the saucer, the shot and the ball; the shot is two while it
   # leaves at one side and enters at the other.
   if [ "$most" -ge 22 ] && [ "$most" -le 23 ] && [ "$tracks" -le 40 ] \
         && [ "$ghost_frames" -ge $((FRAMES / 2 - 20)) ]; then
      printf '  %-58s ok\n' "$label"
   else
      printf '  %-58s FAIL\n' "$label"
      FAIL=$((FAIL + 1))
   fi
}

# "sound  rms left A right B, of their difference D; mean left M right N; peak P"
apart() {   # label, name, least difference, most difference
   label=$1; least=$3; most=$4
   set -- $(grep '^sound' "$DIR/out/$2.log" | tr -cd '0-9 \n')
   if [ "${3:-0}" -ge "$least" ] && [ "${3:-0}" -le "$most" ]; then
      printf '  %-58s ok\n' "$label"
   else
      printf '  %-58s FAIL\n' "$label"
      FAIL=$((FAIL + 1))
   fi
}

has() {   # label, name, text
   if grep -q "$3" "$DIR/out/$2.log"; then
      printf '  %-58s ok\n' "$1"
   else
      printf '  %-58s FAIL\n' "$1"
      FAIL=$((FAIL + 1))
   fi
}

echo "The capture build of Stella"
same   "its frames are Stella's, capture off"  stock px_off
same   "its frames are Stella's, capture on"   stock px_on
passed "the capture agrees with every frame"   px_on
echo "Proteus"
same   "around Stella: the frames pass through"           stock wrap_stock
same   "native size: the frames are Stella's"             stock native
same   "enhanced picture off: the frames are Stella's"    stock disabled
same   "1280 wide, no effects: Stella's frames, enlarged" stock plain
same   "1920 wide, no effects: Stella's frames, enlarged" stock plain1920
differ "flicker fusion draws what Stella leaves out"      stock fused
differ "the effects change the picture"                   plain full
objects "objects are found, followed, and fused when they flicker"
same   "a profile's [fx] is what the options would be"   noglow profile
apart  "stereo width 0: both voices in the middle"        mono 0 0
apart  "stereo width 100: a voice a side"                 wide 2000 30000
if [ -f "$PAL_ROM" ]; then
   has    "50 Hz: Stella runs the program at 50 frames"      pal_stock "50.000 fps"
   same   "50 Hz: no effects, Stella's frames, enlarged"    pal_stock pal_plain
   has    "50 Hz: the picture is 1280 wide"                  pal_full "last 1280x"
fi

for n in stock native full; do
   printf '  %-10s %s\n' "$n" "$(line $n video)"
   printf '  %-10s %s\n' "" "$(line $n time)"
done
for n in stock full mono wide; do
   printf '  %-10s %s\n' "$n" "$(line $n sound)"
done

if [ "$FAIL" = 0 ]; then
   echo "PASSED (0 failures)"
else
   echo "FAILED ($FAIL failures)"
fi
exit "$FAIL"
