#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds everything and packs build/ProteusRetune-<version>-win64.zip for a GitHub release.
# Run from an MSYS2 UCRT64 shell at the repository root: tools/package-release.sh 0.1.0
set -e
version="${1:?usage: tools/package-release.sh <version>}"
name="ProteusRetune-$version-win64"
out="build/$name"

make
make studio
make cli

rm -rf "$out" "build/$name.zip"
mkdir -p "$out/licenses"
cp build/proteus_libretro.dll build/proteus_dsp.dll build/ProteusStudio.exe build/proteus-cli.exe "$out/"
cp tools/install-core.ps1 README.md "$out/"
cp LICENSE "$out/licenses/Proteus-LICENSE.txt"
cp deps/gme/LICENSE "$out/licenses/libgme-LICENSE.txt"
cp deps/imgui/LICENSE.txt "$out/licenses/DearImGui-LICENSE.txt"
cp deps/lazyusf2/COPYING "$out/licenses/lazyusf2-COPYING.txt"
cp deps/lazyusf2/LICENSE-n64_cic_nus_6105.txt "$out/licenses/lazyusf2-n64_cic_nus_6105-LICENSE.txt"
cp deps/lazyusf2/PROTEUS.md "$out/licenses/lazyusf2-NOTICE.md"
cp deps/psflib/LICENSE "$out/licenses/psflib-LICENSE.txt"
prefix="${MINGW_PREFIX:-/ucrt64}"
cp "$prefix/share/licenses/SDL2/LICENSE.txt" "$out/licenses/SDL2-LICENSE.txt"
cp "$prefix/share/licenses/zlib/LICENSE" "$out/licenses/zlib-LICENSE.txt"

cat > "$out/READ ME FIRST.txt" <<EOF
Proteus Retune $version for Windows (64-bit)
https://github.com/sambena/proteus

Install into RetroArch (close RetroArch first). In PowerShell, from this folder:

  DSP plugin, works with your usual core:
    powershell -ExecutionPolicy Bypass -File .\\install-core.ps1 -RetroArch D:\\RetroArch -Dsp
    Then in RetroArch: Settings > Audio > DSP Plugin > Proteus.dsp

  Wrapper core around a core you have installed ("Proteus Retune + Snes9x"):
    powershell -ExecutionPolicy Bypass -File .\\install-core.ps1 -RetroArch D:\\RetroArch -Core snes9x
    NES:  -Core fceumm
    N64:  -Core mupen64plus_next   (N64 is tested with the wrapper core)

Replace D:\\RetroArch with your RetroArch folder.

ProteusStudio.exe finds a game's songs and writes its profile. Choose your RetroArch
folder under Settings the first time. proteus-cli.exe does the same work from the
command line. README.md has the full guide.

Super Mario 64 and Ocarina of Time: write their profiles, every song listed by name, with
    .\\proteus-cli.exe n64 D:\\RetroArch\\cores\\mupen64plus_next_libretro.dll "D:\\Roms\\Nintendo 64" D:\\RetroArch\\system\\proteus
then pick replacements under Quick Menu > Core Options > Proteus.

No games, ROMs or music are included. Use games you own.

Proteus Retune is GPL-3.0-or-later; the source is at the address above. Bundled
libraries keep their own licenses (see licenses\\): libgme (LGPL-2.1), lazyusf2
(GPL-2.0-or-later; its RSP interpreter CC0, its CIC-NUS-6105 code BSD-2-Clause), psflib (MIT),
Dear ImGui (MIT), SDL2 (zlib), zlib (zlib), dr_wav / dr_mp3 (public domain or MIT-0),
stb_vorbis (public domain or MIT), emu2413 (MIT).
EOF

(cd build && rm -f "$name.zip" && powershell -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$name.zip'")
echo "build/$name.zip"
