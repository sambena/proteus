# SSEQPlayer in Proteus Retune

Plays Nintendo DS NCSF/miniNCSF rips (`src/ncsf_play.cpp`): it reads the SDAT a rip carries and
plays its sequences directly, as the game's sound driver would, rather than emulating the DS.

- Source: https://github.com/kode54/SSEQPlayer (Naram Qashat, adapted from fincs's FeOS Sound
  System; maintained by Christopher Snowhill), commit `77222d3657adff358fb4e610d3e56bb7ada8ec24`
  (2022-01-30), unmodified.
- Left out: the Visual Studio project and the Makefile.
- Built with `-D_LIBCPP_VERSION` (as Kodi's build of it is) so `Player.cpp` does not define two
  `std::codecvt` ids that libstdc++ already has. Plain C++11, no threads.
- It throws C++ exceptions on a malformed SDAT, which `src/ncsf_play.cpp` catches. A WebAssembly
  build needs exceptions enabled (`-fexceptions`) for that; without them a bad rip aborts.

## License

`LICENSE.TXT`: the WTFPL version 2, which places no conditions on use. It is included in Proteus
Retune under GPL-3.0-or-later.
