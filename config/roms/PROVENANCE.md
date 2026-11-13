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
| `scorpion295.rom` | Scorpion ZS 256 | Scorpion / MOA | grey, given away by the maker | ROM 2.95 of 26.11.1994, 64K, CRC32 99F57CE1 (the image others call 2.94), `23add41c9c4a3f7cca845d3d5cf51367` |
| `prof39f.rom` | Scorpion ZS 256 | ProfROM authors | grey | ProfROM 3.9f, 128K, `dcb8ebbe2d2f4c4afa58c43507987b9c` |
| `prof401.rom` | Scorpion ZS 256 Turbo+ | MOA (Andrew Larchenko) / Scorpion | grey | ProfROM 4.01 of 13.07.1997, 256K, ROM disk "FirmWare BonusPak", CRC32 847A66E4, `b124c52cd462ce36d79086747115896f` |
| `profi-kramis02.rom` | Profi v3 | JV "Kramis" / Kondor | grey | the v3 boards' own: BIOS JV "KRAMIS" V.02 of 10.1990 with TR-DOS 5.03, CRC32 77327F52, `720f23bec22581f37fb55bd7a928fde2` |
| `profi-bios10.rom` | Profi v5 | Micco Software / Kondor | grey | ROM Bios 1.0 of 21.09.93, read off a Kondor 5.04 board; TR-DOS 5.04T, CRC32 10DA289A, `f4ab0dd91cd7d207879767d4fe5bf30e` |
| `profi-bios20.rom` | Profi v5 | Micco Software / Kondor | grey | ROM Bios 2.0 of 17.04.94, TR-DOS 5.04T, CRC32 36F5F7BD, `02877e403f22d10d12ef0296ccb96f60` |
| `atm2.rom` | ATM Turbo 2+ | MicroART | grey | `28ce89a88089417db4d3057de942a1bb` |
| `zxevo-fe.rom` | ZX Evo (BaseConf) | NedoPC group | grey | EVO Reset Service 0.61 FE, `4d440af2f2d746094527aa6789390705` |
| `tsconf.rom` | ZX Evo (TSConf) | TS-Labs | grey | TS-BIOS build 2024-07-28, `pentevo/rom/bin/ts-bios.rom` of the zx-evo repository (9ce7544), `9a0a7f6c10ffa0a556748c4b40088554` |
| `phoenix.rom` | ZXM-Phoenix | ZXM-Phoenix authors | grey | BIOS 5.03, `892a393093f373ad3b4c9453f529b523` |
| `alf.rom` | ALF TV Game | SKB Zapad, Brest | grey | 32K: games menu in bank 0, 48 BASIC in bank 1, `f879e22ae833687ccdfc4745aa56b804` |

Source: `256s-*.rom` from <https://github.com/trufanov-nok/fuse-extra-roms>; `alf.rom`
from <https://zxbyte.ru/alf.htm>, which publishes it as a dump of the console's 27C256;
`scorpion295.rom` is `ROM2_95.512` from the maker's own download,
<https://web.archive.org/web/20060812213917/http://www.scorpion.ru:80/spectrum/files/rom2_95.zip>;
the "2.95" circulating as `scorp295.rom` (CRC 0C6C1EF6) is a different image.
`profi-*.rom` from <https://speccy4ever.speccy.org/_PR.htm> (`PJV02`, `PB10`, `PB20`, named
there by CRC). The `profi.rom` shipped before was V.02's BIOS page with someone's TR-DOS 5.04T
in place of the 5.03 the board came with.
`prof401.rom` from <https://speccy4ever.speccy.org/_SC.htm>, the CRC MAME's `scorpiontb` lists;
it is the last ProfROM MOA built for the real board (4.02 runs only in MOA's own emulator, and
the 4.xx.0NN builds are a later community fork). The rest from the images that have circulated
with these machines and with Xpeccy itself.

`alf.rom` is the original firmware, not the patched one that also circulates: it checks
it is running on a genuine console and prints "Не укради" if it is not, and this
emulator passes that check - which makes it a better image to ship and a useful test.

## Tier 4 - rights holder defunct or unknown, bundled by convention

| File | Machine | Rights holder | Status | Note |
| --- | --- | --- | --- | --- |
| `trdos.rom` | Beta Disk / TR-DOS 5.03 | Technology Research Ltd (UK) | proprietary, holder long gone | last official release, used by the Sinclair profiles; shipped by Fuse and ZEsarUX as well, `0da70a5d2a0e733398e005b96b7e4ba6` |
| `trdos504t.rom` | Beta Disk / TR-DOS 5.04T | Technology Research Ltd + unknown patch authors | as above | the version the clones actually shipped with, used by the Pentagon profiles, `b4c9634312b796063015450daef13dfa` |
| `gluck.rom` | Pentagon service ROM | Gluck service ROM authors | unknown | Mr Gluk Reset Service 6.3R, the Pentagons' until 2026.11; it stays for a config that names it, `d5869034604dbfd2c1d54170e874fd0a` |
| `gluk661.rom` | Pentagon service ROM | Mr Gluk Reset Service 6.61 (10 Jan 2010): Renat Mamedov (Mr Gluk), Roman Gavrilov (Reanimator), Dmitry Bystrov (Alone Coder) | unknown, published with its sources | `glukpen.rom` of Alone Coder's `GLUK661.rar` (alonecoder.nedopc.com/zx), the same file as UnrealSpeccy's `glukpen.rom`; used by the Pentagon profiles, `412f3f3a655334ca2777709a168051f6` |
| `sgen.rom` | ATM Turbo 2+, ZX Evo | ATM / General Sound authors | unknown | 2K character generator for the text mode, `bd4e0f6f9177f38a71f081882ba3c454` |
| `gs104.rom` | General Sound 1.04 | General Sound authors | unknown | `6cb34f369d32b2788bd631937b1cf3eb` |
| `gs105a.rom` | General Sound 1.05a | General Sound authors | unknown | `1801e94780ad2a914b070b7128d86c39` |
| `gs105b.rom` | General Sound 1.05b | General Sound authors | unknown | latest release, used by the bundled romsets, `c6244f36ce2e782d4a4be9d72b0b4dd4` |
| `gs105a.txt` | - | General Sound authors | unknown | release notes shipped with the firmware |
| `gs105b.txt` | - | General Sound authors | unknown | release notes shipped with the firmware |
| `neogs111.rom` | NeoGS | NedoPC (loader by savelij, firmware fixes by psb and Evgeny Muchkin on the General Sound ROM) | unknown, published with the card's sources | 512K flash image: loader 0.21 at #00000, firmware 1.11 of 07.02.2026 at #10000; `z80/create_update/full_ngs.rom` of NedoPC's NeoGS SVN (svn.nedopc.com/ngs), CRC32 9E14798B, `0e65e0182bf568348ba5276d9ba2088f` |

The General Sound firmware sources are published at
<https://github.com/psbhlw/gs-firmware> (sources only, no built images); the binaries
bundled here are the original firmware releases.

## ZX Spectrum Next - the System/Next distribution

No Next ROMs are kept here: the machine's firmware loads them from `machines/next/` on its
SD card, as the real one does. The program does not ship that card: it makes one from the
official System/Next release, downloaded from its GitLab repository when the user asks
(`src/xgui/nextcard.cpp`), or takes a card the user has. System/Next is under the Next
License (SpecNext Ltd, `LICENSE.md` in it). A test build may stage the release pinned in
`packaging/deps.json` beside the program (`make-dist.ps1 -SysNext`), whole and unchanged.

## esxDOS - permission of the rights holder

`../esxdos/` holds esxDOS 0.8.5, 0.8.6, 0.8.7, 0.8.8 and 0.8.9 by Papaya Dezign, each release
in a folder of its own: the divMMC firmware (`ESXMMC.BIN`; `ESXMMC.ROM` in 0.8.6, as that
release names it), the `SYS` and `BIN` folders a card needs, and the release's `README.TXT`
and `.NFO` - unmodified, from the official zips. Bundled with the permission of Papaya Dezign;
thanks to Miguel Guerreiro / Papaya Dezign. Their terms are in `../esxdos/LICENSE`.

### esxDOS 0.8.5

`../esxdos/0.8.5/`, from <http://www.esxdos.org/files/esxdos085.zip> (sha256 `ae960bbcd0d31f687152cde03401e1e02bff0f85f2880698b6fe3d27de3d009a`).

| File | Size | MD5 |
| --- | ---: | --- |
| `ESXMMC.BIN` | 8192 | `d58a3f24f40cd3f5a897faf40610a740` |
| `BIN/CD` | 512 | `50c40f96a11161cbad1b312d7517e8e3` |
| `BIN/CHMOD` | 512 | `fb04875adc42097837b7839432f5103c` |
| `BIN/CP` | 512 | `1e5a00d1a8acb2f5d46f8ff1b1347e80` |
| `BIN/DIVIDEO` | 917 | `1e3a152f884d9a310496870a3bc58766` |
| `BIN/DSKPROBE` | 2837 | `d5336db574873f68edbfdbec72c2f404` |
| `BIN/DUMPMEM` | 512 | `e9d02ffab7560f07adfc0739a1e5baf0` |
| `BIN/FILE` | 1024 | `3dc7b278ed9f52ff6df9122066149791` |
| `BIN/GRAMON` | 2560 | `930ec960019d5802b957d591ddcd7164` |
| `BIN/HEXDUMP` | 512 | `cd4f68ae03402de1a1299fcc91edde83` |
| `BIN/LS` | 638 | `a4b24245348fd51e46f8a5d1faf406bc` |
| `BIN/LSTAP` | 1024 | `90c937f5d8e847b286dfb5fbb9dd850d` |
| `BIN/MKDIR` | 512 | `7c2505a761948bf221f5083924eff968` |
| `BIN/MORE` | 512 | `4a7beb3c741b49f51f5f11579d70bcf2` |
| `BIN/MV` | 512 | `48ebee514da2b7e803fafa72507a5fe6` |
| `BIN/PARTINFO` | 512 | `b6b734af5bf31e7023ad3f3925b5e59a` |
| `BIN/PLAYPT3` | 2549 | `9ec0e9664bd546cc31d805f6facfcf68` |
| `BIN/PLAYSQT` | 1759 | `db65aad44667f1d3bd606abcf97e1ff6` |
| `BIN/PLAYSTC` | 1475 | `fadead590fd9b02f32e20c1fd35664b7` |
| `BIN/PLAYWAV` | 1536 | `9d617d24638646cc43cccba9f2c8c445` |
| `BIN/SNAPLOAD` | 6656 | `e587f0ad0e05eb9c6aa0da38f53e2b24` |
| `BIN/SPEAKCZ` | 1536 | `cd3b8cf7d0daa65051cb2877ef1a7031` |
| `BIN/TAPEIN` | 512 | `297ef7a55c655139b06aeb678abe56ee` |
| `BIN/TAPEOUT` | 512 | `7ee30393fbbc9e581496b500cf9a46e1` |
| `BIN/VDISK` | 512 | `3fb12c5773ae39c77a47c8d8bf9b077b` |
| `ESX085.NFO` | 3804 | `8248680812cce36a288778c494cdb15b` |
| `README.TXT` | 10556 | `2ba9a334a1151f8da2bc04a15e87dbb9` |
| `SYS/BDIR.KO` | 1024 | `ad3e5608d0017fb546b22d6c92277823` |
| `SYS/BETADISK.SYS` | 2163 | `afc7236fd8f8e03abe1d28c8978e7db8` |
| `SYS/BFILE.KO` | 1024 | `5ab18a1d55ef4b6047ffef264a34b2fe` |
| `SYS/CONFIG/TRDOS.CFG` | 602 | `1277b815d1f125eae8c20c566c79700a` |
| `SYS/ERRMSG.KO` | 512 | `62a5a9dd6ac8d1e048baa70073b7fecd` |
| `SYS/ESXDOS.SYS` | 3556 | `3af4100b54e71fe60f0369329693b4e3` |
| `SYS/NMI.SYS` | 3554 | `a631121a77387c92c107ca6ba8cbd76b` |
| `SYS/NMI_HELP.SCR` | 6912 | `d427bb8a4a21efc53067e2b6589da7fa` |
| `SYS/TAPE.KO` | 1024 | `6dc247ed1387ae5eecd15b03bc3a78be` |
| `SYS/TRDOS54T.KO` | 13285 | `6e14f8e7c43746c0b4dbd8fe77a60d11` |

### esxDOS 0.8.6

`../esxdos/0.8.6/`, from <http://www.esxdos.org/files/esxdos086.zip> (sha256 `643a16bcde212886caed395291e47f45e86b82e03bb6d9176730593c64e725ef`).

| File | Size | MD5 |
| --- | ---: | --- |
| `ESXMMC.ROM` | 8192 | `e951363a13b45e9aecddd01be788a64a` |
| `BIN/128` | 512 | `255451e44525d1752386efec2d827917` |
| `BIN/CD` | 512 | `50c40f96a11161cbad1b312d7517e8e3` |
| `BIN/CHMOD` | 512 | `fb04875adc42097837b7839432f5103c` |
| `BIN/CP` | 512 | `1e5a00d1a8acb2f5d46f8ff1b1347e80` |
| `BIN/DIVIDEO` | 917 | `1e3a152f884d9a310496870a3bc58766` |
| `BIN/DRIVES` | 1008 | `9a8bc288f48b399974e5676a1dbd6bdc` |
| `BIN/DSKPROBE` | 2837 | `d5336db574873f68edbfdbec72c2f404` |
| `BIN/DUMPMEM` | 512 | `e9d02ffab7560f07adfc0739a1e5baf0` |
| `BIN/FILE` | 1024 | `3dc7b278ed9f52ff6df9122066149791` |
| `BIN/GRAMON` | 2560 | `930ec960019d5802b957d591ddcd7164` |
| `BIN/HEXDUMP` | 512 | `cd4f68ae03402de1a1299fcc91edde83` |
| `BIN/HEXVIEW` | 2767 | `489543ce2372ebe07e3f292379660f7c` |
| `BIN/LAUNCHER` | 1024 | `978313757a735574065f7c5ef932bae5` |
| `BIN/LS` | 1189 | `8ceef180fe87f9a0de000721b7e01fc0` |
| `BIN/LSTAP` | 1024 | `90c937f5d8e847b286dfb5fbb9dd850d` |
| `BIN/MKDIR` | 512 | `7c2505a761948bf221f5083924eff968` |
| `BIN/MKTRD` | 512 | `c9449b0a8cb9b3ad7f6f11123681bb50` |
| `BIN/MORE` | 512 | `4a7beb3c741b49f51f5f11579d70bcf2` |
| `BIN/MV` | 512 | `48ebee514da2b7e803fafa72507a5fe6` |
| `BIN/PARTINFO` | 512 | `a35fbbf8a4b54e5dfbb4604a0dcb5b36` |
| `BIN/PLAYPT3` | 3642 | `bfabb96b23412808f561e7666b1811a5` |
| `BIN/PLAYSQT` | 1759 | `db65aad44667f1d3bd606abcf97e1ff6` |
| `BIN/PLAYSTC` | 1475 | `fadead590fd9b02f32e20c1fd35664b7` |
| `BIN/PLAYTFM` | 2232 | `c2fc51b9e118a57423bc9cf925908be8` |
| `BIN/PLAYWAV` | 1536 | `9d617d24638646cc43cccba9f2c8c445` |
| `BIN/RM` | 1289 | `b6d06591057859e061ac0a382f6ab3e8` |
| `BIN/SNAPLOAD` | 6656 | `e587f0ad0e05eb9c6aa0da38f53e2b24` |
| `BIN/SPEAKCZ` | 1536 | `cd3b8cf7d0daa65051cb2877ef1a7031` |
| `BIN/TAPEIN` | 512 | `5db0a65f475f9d9eb91b0eda452bae8e` |
| `BIN/TAPEOUT` | 512 | `b30b9f2fd6fdc78eed514fb15dcb3bfd` |
| `BIN/VDISK` | 512 | `3fb12c5773ae39c77a47c8d8bf9b077b` |
| `ESX086.NFO` | 3726 | `d11fc8553f67ccecca1b7dd7ef3bc067` |
| `README.TXT` | 6476 | `e1e5ca1cd68e3f606a4d7a694476e599` |
| `SYS/BDIR.KO` | 1024 | `93520050b6b2ea9044685ed2fb0d911c` |
| `SYS/BETADISK.SYS` | 2233 | `0cedd4ca07bacaf0dfd963d213a5634d` |
| `SYS/BFILE.KO` | 1024 | `3188d51ab24e7999fc6205f1579b868f` |
| `SYS/CONFIG/ESXDOS.CFG` | 181 | `b975b292246f0f57be8a84d656919254` |
| `SYS/CONFIG/TRDOS.CFG` | 875 | `3e4870421391bdf18759eab6ae77aa3b` |
| `SYS/ERRMSG.KO` | 512 | `62a5a9dd6ac8d1e048baa70073b7fecd` |
| `SYS/ESXDOS.SYS` | 3809 | `f1b5a5d86cb13822be992f11d3c2061d` |
| `SYS/NMI.SYS` | 3544 | `f16773b341322175f69d750d8d486c55` |
| `SYS/NMI_HELP.SCR` | 6912 | `d427bb8a4a21efc53067e2b6589da7fa` |
| `SYS/TAPE.KO` | 1024 | `ff8ca407ba862d04790ab097a70ab711` |
| `SYS/TRDBOOT.BIN` | 14592 | `3bf4a800eecc95b8026e06c595630f31` |
| `SYS/TRDOS54T.KO` | 13638 | `a850a565eb92f1c9709568ca6fec3e84` |

### esxDOS 0.8.7

`../esxdos/0.8.7/`, from <http://www.esxdos.org/files/esxdos087.zip> (sha256 `6f5b523ad34bc1f57c816d69279fd044cb923b35b88fe9b6a33879462b847a19`).

| File | Size | MD5 |
| --- | ---: | --- |
| `ESXMMC.BIN` | 8192 | `9575a489a444d17243d6f6855313755c` |
| `BIN/128` | 512 | `255451e44525d1752386efec2d827917` |
| `BIN/CD` | 512 | `50c40f96a11161cbad1b312d7517e8e3` |
| `BIN/CHMOD` | 512 | `fb04875adc42097837b7839432f5103c` |
| `BIN/CP` | 512 | `1e5a00d1a8acb2f5d46f8ff1b1347e80` |
| `BIN/DFWLOAD` | 1024 | `6c9fec59ad152fb55db55b3fc20f49c1` |
| `BIN/DIVIDEO` | 917 | `1e3a152f884d9a310496870a3bc58766` |
| `BIN/DRIVES` | 1012 | `ada452345221f871918bd7eff09a776e` |
| `BIN/DSKPROBE` | 2837 | `d5336db574873f68edbfdbec72c2f404` |
| `BIN/DUMPMEM` | 1024 | `0248920e86df0dd1f97715cde16d4ff6` |
| `BIN/FILE` | 1024 | `3dc7b278ed9f52ff6df9122066149791` |
| `BIN/GRAMON` | 2560 | `930ec960019d5802b957d591ddcd7164` |
| `BIN/HEXDUMP` | 1024 | `54c58aa8d5f44617defcb35b122f32df` |
| `BIN/HEXVIEW` | 2767 | `489543ce2372ebe07e3f292379660f7c` |
| `BIN/LAUNCHER` | 1024 | `978313757a735574065f7c5ef932bae5` |
| `BIN/LS` | 1189 | `8ceef180fe87f9a0de000721b7e01fc0` |
| `BIN/LSTAP` | 1024 | `90c937f5d8e847b286dfb5fbb9dd850d` |
| `BIN/MKDIR` | 512 | `7c2505a761948bf221f5083924eff968` |
| `BIN/MKTRD` | 512 | `c9449b0a8cb9b3ad7f6f11123681bb50` |
| `BIN/MORE` | 512 | `841c8214f79371c268dbae277166d9e0` |
| `BIN/MV` | 512 | `48ebee514da2b7e803fafa72507a5fe6` |
| `BIN/PARTINFO` | 512 | `a35fbbf8a4b54e5dfbb4604a0dcb5b36` |
| `BIN/PLAYPT3` | 3642 | `bfabb96b23412808f561e7666b1811a5` |
| `BIN/PLAYSQT` | 1759 | `db65aad44667f1d3bd606abcf97e1ff6` |
| `BIN/PLAYSTC` | 1475 | `fadead590fd9b02f32e20c1fd35664b7` |
| `BIN/PLAYTFM` | 2232 | `c2fc51b9e118a57423bc9cf925908be8` |
| `BIN/PLAYWAV` | 1536 | `9d617d24638646cc43cccba9f2c8c445` |
| `BIN/RM` | 1293 | `c1f6ec95a0caf5568e35664362233378` |
| `BIN/SERCP` | 3072 | `e5175048a94fdfe58bb4ceab370fa16c` |
| `BIN/SNAPLOAD` | 6656 | `e587f0ad0e05eb9c6aa0da38f53e2b24` |
| `BIN/SPEAKCZ` | 1536 | `cd3b8cf7d0daa65051cb2877ef1a7031` |
| `BIN/TAPEIN` | 512 | `5db0a65f475f9d9eb91b0eda452bae8e` |
| `BIN/TAPEOUT` | 512 | `b30b9f2fd6fdc78eed514fb15dcb3bfd` |
| `BIN/TSPLAY` | 3584 | `bbefe2eefd9adac8c6051741ce1977ba` |
| `BIN/VDISK` | 512 | `3fb12c5773ae39c77a47c8d8bf9b077b` |
| `ESX087.NFO` | 3728 | `a134f3f82e94e85debeb1ca4c66a9e15` |
| `README.TXT` | 3204 | `38e765140a895ed3250ba9610204b110` |
| `SYS/BDIR.KO` | 1024 | `ae94a386534c96bb22e238ab0d7711ee` |
| `SYS/BETADISK.SYS` | 2233 | `1f8b88148b57793c27c36d2848eac7e6` |
| `SYS/BFILE.KO` | 1024 | `a5e937d1c3c5c7f08191419a856e3e0b` |
| `SYS/CONFIG/ESXDOS.CFG` | 252 | `22217dca75b0be50aa93a76720ea1ecf` |
| `SYS/CONFIG/TRDOS.CFG` | 875 | `3e4870421391bdf18759eab6ae77aa3b` |
| `SYS/ERRMSG.KO` | 512 | `62a5a9dd6ac8d1e048baa70073b7fecd` |
| `SYS/ESXDOS.SYS` | 3891 | `53ef5b1367f873cc08fa60d466ee1c77` |
| `SYS/NMI.SYS` | 3540 | `02ed3c53c2806642ee1bba583f890fd6` |
| `SYS/NMI_HELP.SCR` | 6912 | `d427bb8a4a21efc53067e2b6589da7fa` |
| `SYS/TAPE.KO` | 1024 | `67626b2f05053d9446c038f7d1cd3702` |
| `SYS/TRDBOOT.BIN` | 14592 | `3bf4a800eecc95b8026e06c595630f31` |
| `SYS/TRDOS54T.KO` | 13652 | `e01265ed175d7ade70f9f2129c9480e0` |

### esxDOS 0.8.8

`../esxdos/0.8.8/`, from <http://www.esxdos.org/files/esxdos088.zip> (sha256 `8bee2a0a0108c6a94f29870fa703a74e4711a9e5c8bba3c90b555f715845187b`).

| File | Size | MD5 |
| --- | ---: | --- |
| `ESXMMC.BIN` | 8192 | `720b5bf903d912dc6dfb4967d957723a` |
| `BIN/128` | 512 | `9632dc64cbe81eb10b2028a925621dae` |
| `BIN/CAL` | 603 | `5b858161928fc91e6563c25e67a7d5c5` |
| `BIN/CD` | 512 | `50c40f96a11161cbad1b312d7517e8e3` |
| `BIN/CHMOD` | 512 | `fb04875adc42097837b7839432f5103c` |
| `BIN/CP` | 512 | `1e5a00d1a8acb2f5d46f8ff1b1347e80` |
| `BIN/DATE` | 388 | `49281f0d780bcee65a8697d54ea12549` |
| `BIN/DFWLOAD` | 1024 | `6c9fec59ad152fb55db55b3fc20f49c1` |
| `BIN/DIVIDEO` | 917 | `1e3a152f884d9a310496870a3bc58766` |
| `BIN/DRIVES` | 1012 | `ada452345221f871918bd7eff09a776e` |
| `BIN/DSKPROBE` | 2837 | `d5336db574873f68edbfdbec72c2f404` |
| `BIN/DUMPMEM` | 512 | `e9d02ffab7560f07adfc0739a1e5baf0` |
| `BIN/FILE` | 1024 | `3dc7b278ed9f52ff6df9122066149791` |
| `BIN/GRAMON` | 2560 | `930ec960019d5802b957d591ddcd7164` |
| `BIN/HEXDUMP` | 512 | `cd4f68ae03402de1a1299fcc91edde83` |
| `BIN/HEXVIEW` | 2767 | `489543ce2372ebe07e3f292379660f7c` |
| `BIN/LAUNCHER` | 1024 | `978313757a735574065f7c5ef932bae5` |
| `BIN/LS` | 1189 | `8ceef180fe87f9a0de000721b7e01fc0` |
| `BIN/LSTAP` | 1024 | `90c937f5d8e847b286dfb5fbb9dd850d` |
| `BIN/MKDIR` | 512 | `7c2505a761948bf221f5083924eff968` |
| `BIN/MKTRD` | 512 | `c9449b0a8cb9b3ad7f6f11123681bb50` |
| `BIN/MORE` | 417 | `f414a55ee44f00286a50a231bd29054a` |
| `BIN/MV` | 512 | `48ebee514da2b7e803fafa72507a5fe6` |
| `BIN/PARTINFO` | 512 | `a35fbbf8a4b54e5dfbb4604a0dcb5b36` |
| `BIN/PLAYPT3` | 3642 | `bfabb96b23412808f561e7666b1811a5` |
| `BIN/PLAYSQT` | 1759 | `db65aad44667f1d3bd606abcf97e1ff6` |
| `BIN/PLAYSTC` | 1475 | `fadead590fd9b02f32e20c1fd35664b7` |
| `BIN/PLAYTFM` | 2232 | `c2fc51b9e118a57423bc9cf925908be8` |
| `BIN/PLAYWAV` | 1536 | `9d617d24638646cc43cccba9f2c8c445` |
| `BIN/RM` | 1293 | `c1f6ec95a0caf5568e35664362233378` |
| `BIN/SCL2TRD` | 4075 | `601cd775f8646cdcaaeaf7d0e63b3e05` |
| `BIN/SERCP` | 3072 | `94673dbd6a9d4275d99f7f9f15f0bff3` |
| `BIN/SNAPLOAD` | 6656 | `e587f0ad0e05eb9c6aa0da38f53e2b24` |
| `BIN/SPEAKCZ` | 1536 | `cd3b8cf7d0daa65051cb2877ef1a7031` |
| `BIN/STRINGS` | 1045 | `e4d1d1ff1608736d82a07d7ea9442418` |
| `BIN/TAPEIN` | 512 | `5db0a65f475f9d9eb91b0eda452bae8e` |
| `BIN/TAPEOUT` | 512 | `b30b9f2fd6fdc78eed514fb15dcb3bfd` |
| `BIN/TSPLAY` | 3584 | `bbefe2eefd9adac8c6051741ce1977ba` |
| `BIN/VDISK` | 512 | `3fb12c5773ae39c77a47c8d8bf9b077b` |
| `ESX088.NFO` | 2964 | `69f16f4f6f637f4f185afac706b0f4ce` |
| `README.TXT` | 3485 | `03740da28901541724a80a475b154307` |
| `SYS/BDIR.KO` | 1024 | `00907f03854e120d097e645be4c7fb88` |
| `SYS/BETADISK.SYS` | 2233 | `806b40b5d5acb16caea26b33f13149a9` |
| `SYS/BFILE.KO` | 1024 | `57a846cecb9de3f98fea6f142cb90bf6` |
| `SYS/CONFIG/ESXDOS.CFG` | 198 | `fbb28aca7eb642ef4e93b552347ca1f9` |
| `SYS/CONFIG/TRDOS.CFG` | 875 | `3e4870421391bdf18759eab6ae77aa3b` |
| `SYS/ERRMSG.KO` | 512 | `62a5a9dd6ac8d1e048baa70073b7fecd` |
| `SYS/ESXDOS.SYS` | 3994 | `d674fecf5abc36cbf4bed5edd081e831` |
| `SYS/NMI.SYS` | 3540 | `02ed3c53c2806642ee1bba583f890fd6` |
| `SYS/NMI_HELP.SCR` | 6912 | `d427bb8a4a21efc53067e2b6589da7fa` |
| `SYS/TAPE.KO` | 1024 | `f5ed25b0d909cc326822105d403752bd` |
| `SYS/TRDOS54T.KO` | 13652 | `3eb5971db4de4903e12ef182c718d736` |

### esxDOS 0.8.9

`../esxdos/0.8.9/`, from <http://www.esxdos.org/files/esxdos089.zip> (sha256 `d455888361cd13d455e362a4c841452b6f4568d30f9069443044affdd40de9a4`).

| File | Size | MD5 |
| --- | ---: | --- |
| `ESXMMC.BIN` | 8192 | `fa50b0258e52b8d72bd83cc2fb6e1013` |
| `BIN/128` | 512 | `9632dc64cbe81eb10b2028a925621dae` |
| `BIN/CAL` | 603 | `5b858161928fc91e6563c25e67a7d5c5` |
| `BIN/CD` | 512 | `50c40f96a11161cbad1b312d7517e8e3` |
| `BIN/CHMOD` | 512 | `fb04875adc42097837b7839432f5103c` |
| `BIN/CP` | 512 | `1e5a00d1a8acb2f5d46f8ff1b1347e80` |
| `BIN/DATE` | 388 | `49281f0d780bcee65a8697d54ea12549` |
| `BIN/DFWLOAD` | 1024 | `6c9fec59ad152fb55db55b3fc20f49c1` |
| `BIN/DIVIDEO` | 917 | `1e3a152f884d9a310496870a3bc58766` |
| `BIN/DRIVES` | 1008 | `9a8bc288f48b399974e5676a1dbd6bdc` |
| `BIN/DSKPROBE` | 2837 | `d5336db574873f68edbfdbec72c2f404` |
| `BIN/DUMPMEM` | 512 | `e9d02ffab7560f07adfc0739a1e5baf0` |
| `BIN/FILE` | 1024 | `3dc7b278ed9f52ff6df9122066149791` |
| `BIN/GRAMON` | 2560 | `930ec960019d5802b957d591ddcd7164` |
| `BIN/GSC` | 4896 | `2e4f5e414e0b620f2ce566e5a32afeb6` |
| `BIN/HEXDUMP` | 512 | `cd4f68ae03402de1a1299fcc91edde83` |
| `BIN/HEXVIEW` | 2767 | `489543ce2372ebe07e3f292379660f7c` |
| `BIN/LAUNCHER` | 1160 | `76a10020fffc4f672690f5496baaadfa` |
| `BIN/LS` | 1189 | `8ceef180fe87f9a0de000721b7e01fc0` |
| `BIN/LSTAP` | 1024 | `90c937f5d8e847b286dfb5fbb9dd850d` |
| `BIN/MKDIR` | 512 | `7c2505a761948bf221f5083924eff968` |
| `BIN/MKTRD` | 512 | `c9449b0a8cb9b3ad7f6f11123681bb50` |
| `BIN/MORE` | 417 | `f414a55ee44f00286a50a231bd29054a` |
| `BIN/MV` | 512 | `48ebee514da2b7e803fafa72507a5fe6` |
| `BIN/PARTINFO` | 512 | `a35fbbf8a4b54e5dfbb4604a0dcb5b36` |
| `BIN/PLAYPT3` | 3642 | `bfabb96b23412808f561e7666b1811a5` |
| `BIN/PLAYSQT` | 1759 | `db65aad44667f1d3bd606abcf97e1ff6` |
| `BIN/PLAYSTC` | 1475 | `fadead590fd9b02f32e20c1fd35664b7` |
| `BIN/PLAYTFM` | 2232 | `c2fc51b9e118a57423bc9cf925908be8` |
| `BIN/PLAYWAV` | 1536 | `9d617d24638646cc43cccba9f2c8c445` |
| `BIN/RM` | 1289 | `b6d06591057859e061ac0a382f6ab3e8` |
| `BIN/SCL2TRD` | 4075 | `601cd775f8646cdcaaeaf7d0e63b3e05` |
| `BIN/SERCP` | 3072 | `94673dbd6a9d4275d99f7f9f15f0bff3` |
| `BIN/SNAPLOAD` | 6656 | `e587f0ad0e05eb9c6aa0da38f53e2b24` |
| `BIN/SPEAKCZ` | 1536 | `cd3b8cf7d0daa65051cb2877ef1a7031` |
| `BIN/STRINGS` | 1045 | `e4d1d1ff1608736d82a07d7ea9442418` |
| `BIN/TAPEIN` | 512 | `5db0a65f475f9d9eb91b0eda452bae8e` |
| `BIN/TAPEOUT` | 512 | `b30b9f2fd6fdc78eed514fb15dcb3bfd` |
| `BIN/TAR` | 2691 | `dcab1baa4907675ee3a845986fd53685` |
| `BIN/TE` | 1024 | `bc73a3c7299b9e57a74b49a698343f49` |
| `BIN/TSPLAY` | 3584 | `bbefe2eefd9adac8c6051741ce1977ba` |
| `BIN/VDISK` | 512 | `3fb12c5773ae39c77a47c8d8bf9b077b` |
| `ESX089.NFO` | 2965 | `8a17f0a54c0d1af43d38be5d14e1100c` |
| `README.TXT` | 4320 | `740928f5527378845ef25b49c0bb1e18` |
| `SYS/BDIR.KO` | 1024 | `d1a602cbc49f2200c0de7e9f855894eb` |
| `SYS/BETADISK.SYS` | 2233 | `64a7ea5a4690c76bedcaed67874a0505` |
| `SYS/BFILE.KO` | 1024 | `1f6c192703e8ef104f4efe5831eb042d` |
| `SYS/CONFIG/ESXDOS.CFG` | 247 | `0eea6ed61b759a6c7a8285d2859c0bdf` |
| `SYS/CONFIG/TRDOS.CFG` | 875 | `3e4870421391bdf18759eab6ae77aa3b` |
| `SYS/ERRMSG.KO` | 512 | `62a5a9dd6ac8d1e048baa70073b7fecd` |
| `SYS/ESXDOS.SYS` | 4090 | `484135f5803e399e09ef01b290f2f39e` |
| `SYS/NMI.SYS` | 3539 | `7744e8f88ad5a0a38ae16132cdcbca1c` |
| `SYS/NMI_HELP.SCR` | 6912 | `7c1fbd5d11172b79a8042334479a34c4` |
| `SYS/TAPE.KO` | 1024 | `0dc2206e6ceaffa0810e7f99e2ac7c9a` |
| `SYS/TRDBOOT.BIN` | 15360 | `313590e963124f498a5b3763aea66c7d` |
| `SYS/TRDOS54T.KO` | 16412 | `2e0ef5b18368c4d432ec983cb34c6616` |
| `SYS/VDISK.KO` | 512 | `7f8f7ed83b6c7ad1eee4aadc27c68f22` |

## Other files

| File | What it is |
| --- | --- |
| `AMSTRAD.copyright` | canonical Amstrad notice, verbatim copy from the Fuse distribution |
| `LICENSE` | distribution terms for this directory |
| `PROVENANCE.md` | this file |
| `../esxdos/LICENSE` | terms of the bundled esxDOS |
| `../boot.$B` | Dimon boot 2024 by Dmitry Yurinov, appended to TR-DOS images that carry no boot file - <https://zxart.ee/prod/540777> |
