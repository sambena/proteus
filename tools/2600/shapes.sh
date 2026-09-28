#!/bin/sh
# What a game draws with which object: the colours and the shapes of every class, and the
# frames they were seen in. Objects a game shows in turns show as colours that go with the
# frame's number (its phase, of four).
#
#   tools/2600/shapes.sh <objects file> [first row] [last row]
#
# The rows narrow it to a part of the picture: the score apart from the game.
set -u
if [ $# -lt 1 ]; then
   sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
   exit 2
fi
gawk -v top="${2:-0}" -v bottom="${3:-999}" '
/^frame/ { f = $2 + 0; next }
/from its track/ { next }
match($0, /^ +([a-z0-9]+) copy ([0-9]) at +(-?[0-9]+), *(-?[0-9]+) size +([0-9]+)x *([0-9]+) colour ([0-9A-F]+) shape ([0-9A-F]+)/, m) {
   y = m[4] + 0
   if (y < top || y > bottom) next
   k = m[1] " colour " m[7] " phase " (f % 4)
   n[k]++
   if (!(k in first)) first[k] = f
   last[k] = f
   k = m[1] " " m[5] "x" m[6] " shape " m[8]
   sn[k]++
   if (!(k in sfirst)) sfirst[k] = f
   slast[k] = f
}
END {
   print "--- colours"
   for (k in n) printf "%-28s %6d times, frames %d..%d\n", k, n[k], first[k], last[k] | "sort"
   close("sort")
   print "--- shapes"
   for (k in sn) printf "%-28s %6d times, frames %d..%d\n", k, sn[k], sfirst[k], slast[k] | "sort"
}' "$1"
