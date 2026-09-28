#!/bin/sh
# What a game's two voices play: a line for every stretch of one waveform on one voice, with
# the pitches and the volumes it went through. A sound is told by these.
#
#   tools/2600/sounds.sh <sound file> [first frame] [last frame]
set -u
if [ $# -lt 1 ]; then
   sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'
   exit 2
fi
gawk -v a="${2:-0}" -v b="${3:-999999}" '
function flush(v) {
   if (len[v])
      printf "%6d  voice %d  waveform %2d  %4d frames  pitch %s  volume %s\n",
            start[v], v, wave[v], len[v], pitches[v], volumes[v]
   len[v] = 0
}
function note(v, f, c, p, vol) {
   if (!vol || (len[v] && c != wave[v])) flush(v)
   if (!vol) return
   if (!len[v]) { start[v] = f; wave[v] = c; pitches[v] = p; volumes[v] = vol; lp[v] = p; lv[v] = vol; np[v] = 1; nv[v] = 1 }
   else
   {
      if (p != lp[v])   { if (np[v] < 16) pitches[v] = pitches[v] "," p; else if (np[v] == 16) pitches[v] = pitches[v] ",.."; np[v]++; lp[v] = p }
      if (vol != lv[v]) { if (nv[v] < 16) volumes[v] = volumes[v] "," vol; else if (nv[v] == 16) volumes[v] = volumes[v] ",.."; nv[v]++; lv[v] = vol }
   }
   len[v]++
}
/voice 0:/ {
   f = $1 + 0
   c0[f] = $5; p0[f] = $7; l0[f] = $9; c1[f] = $13; p1[f] = $15; l1[f] = $17
   if (f > last) last = f
}
END {
   for (f = 0; f <= last; f++)
   {
      if (f in c0) { C0 = c0[f]; P0 = p0[f]; L0 = l0[f]; C1 = c1[f]; P1 = p1[f]; L1 = l1[f] }
      if (f < a || f > b) continue
      note(0, f, C0, P0, L0 + 0)
      note(1, f, C1, P1, L1 + 0)
   }
   flush(0); flush(1)
}' "$1" | sort -n
