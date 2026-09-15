# lazyusf2 in Proteus Retune

Plays Nintendo 64 USF/miniUSF rips.

- Source: https://gitlab.com/kode54/lazyusf2 (Christopher Snowhill), commit
  `421f00bcaa1988b8e1825e91780129f24fbd1aa0` (2022-03-09), unmodified.
- Left out: `test/`, `prj/` and the Makefile. The recompilers are kept for their headers but not
  built: Proteus uses the cached interpreter (`r4300/empty_dynarec.c`). Added here: `COPYING` (the
  GPL-2.0 text) and `LICENSE-n64_cic_nus_6105.txt` (that file's BSD notice, for binary releases).

## License

lazyusf2 is a modified Mupen64Plus core. Its Mupen64Plus-derived files are licensed under the
GNU General Public License version 2 or (at your option) any later version, as their headers say;
`COPYING` is that license's text. The work as a whole, including the files lazyusf2 adds without
headers of their own (`usf/`), is distributed under those terms, as GPL-2.0 section 2(b) requires
of a work based on Mupen64Plus. Proteus Retune, which includes it, is GPL-3.0-or-later.

Other notices kept with their files:

- `rsp_lle/`: the RSP interpreter by Iconoclast, CC0 1.0 (public domain dedication).
- `si/n64_cic_nus_6105.c`: Copyright 2011 X-Scale, BSD 2-clause (the notice is in the file).
