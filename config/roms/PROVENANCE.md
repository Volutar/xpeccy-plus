# Provenance of the bundled ROM images

Every file in `config/roms/` is listed here. Nothing else is bundled, and no image
was modified - the copyright messages inside them are intact. See `LICENSE` in this
directory for the distribution terms, and `AMSTRAD.copyright` for the canonical
Sinclair/Amstrad notice (copied verbatim from the Fuse emulator; it also mentions a
few images that are not bundled here).

File names follow Fuse where Fuse carries the same image, so a set can be compared
file by file with `fuse-extra-roms`. Images that Fuse does not carry keep a plain
name of their own.

MD5 is given so a rebuild of the bundle can be checked against this table.

## Tier 1 - explicit permission

Copyright Amstrad, who allow distribution of the ROMs but retain the copyright.

| File | Machine | Rights holder | Status | Note |
| --- | --- | --- | --- | --- |
| `48.rom` | ZX Spectrum 48K | Amstrad | permitted | `4c42a2f075212361c3117015b107ff68` |
| `128-0.rom` | ZX Spectrum 128K | Amstrad | permitted | `b4d2692115a9f2924df92a3cbfb358fb` |
| `128-1.rom` | ZX Spectrum 128K | Amstrad | permitted | `6e09e5d3c4aef166601669feaaadc01c` |
| `plus2-0.rom` | ZX Spectrum +2 | Amstrad | permitted | `4ed7af4636308b8a48d7a35e6c5b546b` |
| `plus2-1.rom` | ZX Spectrum +2 | Amstrad | permitted | `b3db95931cc844efaeb82db9c171b9f3` |
| `plus3-0.rom` | ZX Spectrum +2A / +3, v4.0 | Amstrad | permitted | `9833b8b73384dd5fa3678377ff00a2bb` |
| `plus3-1.rom` | ZX Spectrum +2A / +3, v4.0 | Amstrad | permitted | `0f711ceb5ab801b4701989982e0f334c` |
| `plus3-2.rom` | ZX Spectrum +2A / +3, v4.0 | Amstrad | permitted | `3b6dd659d5e4ec97f0e2f7878152c987` |
| `plus3-3.rom` | ZX Spectrum +2A / +3, v4.0 | Amstrad | permitted | `a148bcc575e51389e84fdf5d555c3196` |
| `plus3-41.rom` | ZX Spectrum +3, v4.1 | Amstrad | permitted | later official revision, all four 16K pages in one file, `7e00ed3562abfd188d0d4da03e80bc0a` |

Source: `48.rom` .. `plus3-3.rom` taken from
<https://github.com/trufanov-nok/fuse-extra-roms>. `plus3-41.rom` comes from a
long-circulating dump of the v4.1 ROM set, which Fuse does not carry.

## Tier 2 - derivative of Amstrad code, attributed

| File | Machine | Rights holder | Status | Note |
| --- | --- | --- | --- | --- |
| `128p-0.rom` | Pentagon 128 | Amstrad + unknown authors of the Pentagon patch | derivative, distributed with every ZX emulator | `a249565f03b98d004ee7f019570069cd` |
| `128p-1.rom` | Pentagon 128 | Amstrad | permitted | byte-identical to `128-1.rom` (the 48 BASIC page), `6e09e5d3c4aef166601669feaaadc01c` |

Source: <https://github.com/trufanov-nok/fuse-extra-roms>.

## Tier 3 - clone firmware, derivative or unclear terms

Firmware of Soviet / post-Soviet ZX clones. The 128 BASIC part inside them descends
from the Amstrad code, the rest was written by the clone authors. No formal license
was ever published for any of these; they have been distributed with the machines
and with every emulator of them for decades.

| File | Machine | Rights holder | Status | Note |
| --- | --- | --- | --- | --- |
| `256s-0.rom` | Scorpion ZS 256 | Sergey Zonov / ZS Scorpion authors | grey, distributed by convention | `b9fda5b6a747ff037365b0e2d8c4379a` |
| `256s-1.rom` | Scorpion ZS 256 | as above | grey | `643861ad34831b255bf2eb64e8b6ecb8` |
| `256s-2.rom` | Scorpion ZS 256 | as above | grey | service ROM, `d8ad507b1c915a9acfe0d73957082926` |
| `256s-3.rom` | Scorpion ZS 256 | Technology Research Ltd | grey | TR-DOS page of the set, `ce0723f9bc02f4948c15d3b3230ae831` |
| `prof39f.rom` | Scorpion ZS 256 | ProfROM authors | grey | ProfROM 3.9f, 128K, `dcb8ebbe2d2f4c4afa58c43507987b9c` |
| `profi.rom` | Profi | Profi authors (KONDOR) | grey | Profi v0.2 with TR-DOS 5.04T, `65dff86e995761ffaffd0fc137f31fb2` |
| `atm2.rom` | ATM Turbo 2+ | MicroART | grey | `28ce89a88089417db4d3057de942a1bb` |
| `zxevo-fe.rom` | ZX Evo (BaseConf) | NedoPC group | grey | EVO Reset Service 0.61 FE, `4d440af2f2d746094527aa6789390705` |
| `tsconf.rom` | ZX Evo (TSConf) | TS-Labs | grey | TS-BIOS, shipped elsewhere as `ts-bios.rom`, `d5f199df3832dc749fe0d12f1ce8f26f` |
| `phoenix.rom` | ZXM-Phoenix | ZXM-Phoenix authors | grey | BIOS 5.03, `892a393093f373ad3b4c9453f529b523` |
| `alf.rom` | ALF TV Game | SKB Zapad, Brest | grey | 32K: games menu in bank 0, 48 BASIC in bank 1, `f879e22ae833687ccdfc4745aa56b804` |

Source: `256s-*.rom` from <https://github.com/trufanov-nok/fuse-extra-roms>; `alf.rom`
from <https://zxbyte.ru/alf.htm>, which publishes it as a dump of the console's 27C256;
the rest from the images that have circulated with these machines and with Xpeccy itself.

`alf.rom` is the original firmware, not the patched one that also circulates: it checks
it is running on a genuine console and prints "Не укради" if it is not, and this
emulator passes that check - which makes it a better image to ship and a useful test.

## Tier 4 - rights holder defunct or unknown, bundled by convention

| File | Machine | Rights holder | Status | Note |
| --- | --- | --- | --- | --- |
| `trdos.rom` | Beta Disk / TR-DOS 5.03 | Technology Research Ltd (UK) | proprietary, holder long gone | last official release, used by the Sinclair profiles; shipped by Fuse and ZEsarUX as well, `0da70a5d2a0e733398e005b96b7e4ba6` |
| `trdos504t.rom` | Beta Disk / TR-DOS 5.04T | Technology Research Ltd + unknown patch authors | as above | the version the clones actually shipped with, used by the Pentagon profiles, `b4c9634312b796063015450daef13dfa` |
| `gluck.rom` | Pentagon service ROM | Gluck service ROM authors | unknown | `d5869034604dbfd2c1d54170e874fd0a` |
| `sgen.rom` | ATM Turbo 2+, ZX Evo | ATM / General Sound authors | unknown | 2K character generator for the text mode, `bd4e0f6f9177f38a71f081882ba3c454` |
| `gs104.rom` | General Sound 1.04 | General Sound authors | unknown | `6cb34f369d32b2788bd631937b1cf3eb` |
| `gs105a.rom` | General Sound 1.05a | General Sound authors | unknown | `1801e94780ad2a914b070b7128d86c39` |
| `gs105b.rom` | General Sound 1.05b | General Sound authors | unknown | latest release, used by the bundled romsets, `c6244f36ce2e782d4a4be9d72b0b4dd4` |
| `gs105a.txt` | - | General Sound authors | unknown | release notes shipped with the firmware |
| `gs105b.txt` | - | General Sound authors | unknown | release notes shipped with the firmware |

The General Sound firmware sources are published at
<https://github.com/psbhlw/gs-firmware> (sources only, no built images); the binaries
bundled here are the original firmware releases.

## Other files

| File | What it is |
| --- | --- |
| `AMSTRAD.copyright` | canonical Amstrad notice, verbatim copy from the Fuse distribution |
| `LICENSE` | distribution terms for this directory |
| `PROVENANCE.md` | this file |
| `../boot.$B` | Dimon boot 2024 by Dmitry Yurinov, appended to TR-DOS images that carry no boot file - <https://zxart.ee/prod/540777> |
