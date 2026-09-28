#!/bin/sh
# Plays a game for a while and writes down what it does, to study it:
#
#   tools/2600/record.sh <rom> <name> <frames> [options of the harness]
#
#   build/study/<name>.objects.txt   every frame's objects: class, place, size, colour, shape
#   build/study/<name>.sound.txt     the audio registers whenever they change
#   build/study/<name>.ram.txt       the console's 128 bytes after every frame
#   build/study/<name>.wav           what was heard
#   build/study/<name>.bmp           the last frame
#
# The stick is moved for you: --input (left, right, fire: shooters) or --roam (all four
# directions: mazes), with --seed <n> for another way through. --press <frame>:<button> and
# --poke <frame>:<byte>:<value> stage what does not happen by itself. The game runs in the
# capture build of Stella, STELLAPX, without Proteus around it.
set -u
cd "$(dirname "$0")/../.." || exit 1
case "$(uname -s)" in
   MINGW*|MSYS*|CYGWIN*) EXT=dll; EXE=.exe ;;
   Darwin*)              EXT=dylib; EXE= ;;
   *)                    EXT=so; EXE= ;;
esac
STELLAPX=${STELLAPX:-../cores/px/stellapx_libretro.$EXT}
if [ $# -lt 3 ]; then
   sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
   exit 2
fi
ROM=$1; NAME=$2; FRAMES=$3
shift 3
make "build/test2600/harness2600$EXE" > /dev/null || exit 1
OUT=build/study
mkdir -p "$OUT"
"build/test2600/harness2600$EXE" "$STELLAPX" "$ROM" "$FRAMES" --quiet --sysdir "$OUT" \
   --opt stella_phosphor=off --capture --objects "$OUT/$NAME.objects.txt" \
   --sound "$OUT/$NAME.sound.txt" --ram "$OUT/$NAME.ram.txt" --wav "$OUT/$NAME.wav" \
   --bmp "$OUT/$NAME.bmp" "$@" > "$OUT/$NAME.log" 2>&1
grep -E '^core|capture ok|capture FAIL|cannot|missing|not <' "$OUT/$NAME.log"
printf 'ROM     %s\n' "$(md5sum < "$ROM" | cut -c1-32)"
ls "$OUT/$NAME".*
