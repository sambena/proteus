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
cp deps/viogsf/vbam/gpl.txt "$out/licenses/viogsf-COPYING.txt"
cp deps/viogsf/vbam/License.txt "$out/licenses/viogsf-VBA-LICENSE.txt"
cp deps/viogsf/PROTEUS.md "$out/licenses/viogsf-NOTICE.md"
cp deps/vio2sf/desmume/COPYING "$out/licenses/vio2sf-COPYING.txt"
cp deps/vio2sf/PROTEUS.md "$out/licenses/vio2sf-NOTICE.md"
cp deps/sseqplayer/LICENSE.TXT "$out/licenses/SSEQPlayer-LICENSE.txt"
cp deps/sseqplayer/PROTEUS.md "$out/licenses/SSEQPlayer-NOTICE.md"
cp deps/lakesnes/LICENSE.txt "$out/licenses/LakeSnes-LICENSE.txt"
cp deps/lakesnes/PROTEUS.md "$out/licenses/LakeSnes-NOTICE.md"
cp deps/vgmstream/COPYING "$out/licenses/vgmstream-COPYING.txt"
cp deps/vgmstream/PROTEUS.md "$out/licenses/vgmstream-NOTICE.md"
cp deps/dmc_unrar/COPYING "$out/licenses/dmc_unrar-COPYING.txt"

# The Atari 2600 needs Stella built from the proteus-capture branch of sambena/stella. Ship that
# build when there is one: STELLAPX names the dll, else a stella checkout beside this one.
stellapx="${STELLAPX:-../stella/src/os/libretro/stellapx_libretro.dll}"
if [ -f "$stellapx" ]; then
  stella="$(dirname "$stellapx")/../../.."
  mkdir -p "$out/stellapx"
  cp "$stellapx" "$out/stellapx/"
  cp tools/stellapx_libretro.info "$out/stellapx/"
  cp "$stella/License.txt" "$out/licenses/Stella-LICENSE.txt"
  cp "$stella/Copyright.txt" "$out/licenses/Stella-Copyright.txt"
  echo "stellapx: $(git -C "$stella" rev-parse --short HEAD)" > "$out/stellapx/COMMIT.txt"
else
  echo "warning: no stellapx_libretro.dll; the zip leaves out the Atari 2600 core" >&2
fi
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

  Atari 2600, with the picture drawn anew and new sounds: copy stellapx\\stellapx_libretro.dll
  into RetroArch's cores folder and stellapx\\stellapx_libretro.info into its info folder, then
    powershell -ExecutionPolicy Bypass -File .\\install-core.ps1 -RetroArch D:\\RetroArch -Core stellapx
  RetroArch lists it as "Atari - 2600 (Proteus Retune + Stella)"; the options are under
  Proteus 2600 in the core options.

Replace D:\\RetroArch with your RetroArch folder.

ProteusStudio.exe finds a game's songs and writes its profile. Choose your RetroArch
folder under Settings the first time. proteus-cli.exe does the same work from the
command line. README.md has the full guide.

Super Mario 64 and Ocarina of Time: open them in Studio (choose Mupen64Plus-Next as the N64 core
in Settings), or write their profiles, every song listed by name, with
    .\\proteus-cli.exe n64 D:\\RetroArch\\cores\\mupen64plus_next_libretro.dll "D:\\Roms\\Nintendo 64" D:\\RetroArch\\system\\proteus
then pick replacements under Quick Menu > Core Options > Proteus.

No games, ROMs or music are included. Use games you own.

Proteus Retune is GPL-3.0-or-later; the source is at the address above. Bundled
libraries keep their own licenses (see licenses\\): libgme (LGPL-2.1), lazyusf2
(GPL-2.0-or-later; its RSP interpreter CC0, its CIC-NUS-6105 code BSD-2-Clause), psflib (MIT),
viogsf and vio2sf (GPL-2.0-or-later), SSEQPlayer (WTFPL-2.0), LakeSnes (MIT), vgmstream
(ISC-style), dmc_unrar (GPL-2.0-or-later), Stella in stellapx\\ (GPL-2.0-or-later; source at
https://github.com/sambena/stella/tree/proteus-capture), Dear ImGui (MIT), SDL2 (zlib), zlib (zlib), dr_wav / dr_mp3 (public domain or MIT-0),
stb_vorbis (public domain or MIT), emu2413 (MIT).
EOF

(cd build && rm -f "$name.zip" && powershell -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$name.zip'")
echo "build/$name.zip"
