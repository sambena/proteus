#!/bin/sh
# Which bytes of a game's memory say where its objects are: for every class of object, the
# bytes that are its column or its row in three frames of four or more (a game may move a
# thing a frame before it draws it there), give or take a constant. Rows
# are tried at one and at two lines to the count, as games keep them.
#
#   tools/2600/ram-where.sh <objects file> <ram file> [first frame] [last frame] [first row] [last row]
#
# Where a class is several things in turns (four ghosts, one a frame), the bytes of each are
# found by narrowing the frames or the rows to where only one of them is.
set -u
if [ $# -lt 2 ]; then
   sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
   exit 2
fi
gawk -v a="${3:-0}" -v z="${4:-999999}" -v top="${5:-0}" -v bottom="${6:-999}" '
FNR == NR {
   if ($1 == "frame") { f = $2 + 0; delete had; next }
   if (/from its track/) next
   if (match($0, /^ +([a-z0-9]+) copy [0-9] at +(-?[0-9]+), *(-?[0-9]+) /, m) && m[3] + 0 >= top && m[3] + 0 <= bottom)
   {
      # Only frames in which a class is one thing.
      if (m[1] in had) { many[f, m[1]] = 1 } else { had[m[1]] = 1; x[f, m[1]] = m[2] + 0; y[f, m[1]] = m[3] + 0; classes[m[1]] = 1 }
   }
   next
}
{
   f = $1 + 0
   if (f < a || f > z) next
   for (c in classes)
   {
      if (!((f, c) in x) || ((f, c) in many)) continue
      frames[c]++
      if (!((c, "x", x[f, c]) in seen)) { seen[c, "x", x[f, c]] = 1; places[c, "x"]++ }
      if (!((c, "y", y[f, c]) in seen)) { seen[c, "y", y[f, c]] = 1; places[c, "y"]++ }
      for (i = 2; i <= NF; i++)
      {
         v = strtonum("0x" $i)
         hx[c, i - 2, v - x[f, c]]++
         hy[c, i - 2, y[f, c] - v]++
         hy2[c, i - 2, y[f, c] - 2 * v]++
      }
   }
}
# What stands still says nothing: every byte that stands still too would go with it.
function report(h, what, axis,    k, p) {
   for (k in h)
   {
      split(k, p, SUBSEP)
      if (frames[p[1]] >= 40 && places[p[1], axis] >= 4 && h[k] >= 0.75 * frames[p[1]])
         printf "%-3s %-22s byte %3d ($%02X)  %s %d   in %d of %d frames\n", p[1], what, p[2], p[2] + 128,
               what ~ /column/ ? "less" : "plus", p[3], h[k], frames[p[1]]
   }
}
END {
   report(hx, "column", "x")
   report(hy, "row", "y")
   report(hy2, "row, two lines a count", "y")
   for (c in classes)
      printf "%-3s seen alone in %d frames, in %d columns and %d rows\n", c, frames[c], places[c, "x"], places[c, "y"]
}' "$1" "$2" | sort
