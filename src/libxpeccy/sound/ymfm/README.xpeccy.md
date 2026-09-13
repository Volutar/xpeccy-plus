# ymfm, vendored

Aaron Giles' Yamaha FM cores, <https://github.com/aaronsgiles/ymfm>, BSD-3-Clause
(see `LICENSE`). Taken from upstream commit `81aec25` (2026-07-27).

Only the OPN half is here - `ymfm_opn.*` is what the YM2203 needs, and it pulls in
`ymfm_adpcm.*` and `ymfm_pcm.*` for its OPNA/OPNB relatives. The OPL, OPM, OPQ, OPZ
and the standalone SSG files are not copied.

What uses it: `../ym-2203.cpp`, which drives `fm_engine_base<opn_registers>` directly
rather than the `ymfm::ym2203` class - see the comment at the top of that file for why.

**One change to the vendored source**, marked in place at the end of `ymfm_opn.cpp`:
the two explicit instantiations that let the engine be used from another file. That is
how `ymfm_opl.cpp` already exports its own engine, so it is the library's own pattern,
not a patch to its behaviour. Re-apply it when updating.
