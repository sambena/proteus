#!/bin/sh
# Which bytes of a game's memory change when a sound begins, and seldom else: what the game
# counts when it makes that sound (the dots eaten, the enemies left, the lives).
#
#   tools/2600/ram-events.sh <sound file> <ram file> <voice> <waveform> [first frame] [last frame]
set -u
if [ $# -lt 4 ]; then
   sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'
   exit 2
fi
gawk -v voice="$3" -v wave="$4" -v a="${5:-0}" -v z="${6:-999999}" '
FNR == NR {
   if ($0 !~ /voice 0:/) next
   f = $1 + 0
   c = voice == 0 ? $5 : $13; v = voice == 0 ? $9 : $17
   on = (c == wave && v + 0 > 0)
   if (on && !was && f >= a && f <= z) { ev[f] = 1; events++ }
   was = on
   next
}
{
   f = $1 + 0
   for (i = 2; i <= NF; i++)
   {
      b = i - 2
      v = strtonum("0x" $i)
      if (FNR > 1 && v != prev[b] && f >= a && f <= z)
      {
         changes[b]++
         near = 0
         for (d = -3; d <= 3; d++) if ((f + d) in ev) near = 1
         if (near) { hits[b]++; if (!(b in eg)) eg[b] = sprintf("from %02X to %02X in frame %d", prev[b], v, f) }
      }
      prev[b] = v
   }
}
END {
   printf "the sound began %d times\n", events
   for (b = 0; b < 128; b++)
      if (events && hits[b] >= events * 0.8 && changes[b] <= hits[b] * 1.5)
         printf "byte %3d ($%02X) changed %d times, %d of them with the sound; first %s\n",
               b, b + 128, changes[b], hits[b], eg[b]
}' "$1" "$2"
