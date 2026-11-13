#!/usr/bin/env python3
"""Run the emulator headless over a suite of images and compare the hashes with a reference.

Each case starts a fresh copy of the app's config, runs the machine for a fixed number of
frames with --bench-hash and records the frame, sound, RAM and CPU hashes it prints. Two
builds that print the same hashes ran the machine identically.

  regress.py run [suite...] [-k PATTERN]   compare with the reference, exit 1 on a difference
  regress.py bless [suite...] [-k PATTERN] take the current results as the reference
  regress.py list [suite...]
  regress.py quick [bench...] [-k PATTERN]  the headless libxpeccy benches, no app needed
  regress.py drawcheck [suite...] [--every N] each case with every frame drawn and with only
                                            every Nth drawn: the machine must not see the difference

The images and the references are not in this repository: --corpus/XPECCY_CORPUS and
--golden/XPECCY_GOLDEN point at them.
"""

import argparse
import concurrent.futures as cf
import copy
import fnmatch
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUITES = Path(__file__).resolve().parent / "suites"
EXE = "xpeccy-plus.exe" if os.name == "nt" else "xpeccy-plus"
FIELDS = ("frames", "sound", "ram", "cpu", "pc", "T")
HASH_RE = re.compile(r"^hash: frames (\w+) sound (\w+) ram (\w+) cpu (\w+) pc (\w+) T (-?\d+)", re.M)


HOWS = ("auto", "open", "boot")
OPTS = ("cfg", "mac", "nvram", "sd", "hdd", "put", "keys", "eeprom", "card1", "card2", "cardimg1", "cardimg2",
        "romfile", "sdimg")
PINNED = 946684800  # 2000-01-01: a folder card's FAT carries the host mtimes


class Case:
    def __init__(self, suite, name, machine, how, skip, frames, file, extra):
        self.suite, self.name, self.machine, self.how = suite, name, machine, how
        self.skip, self.frames, self.file = int(skip), int(frames), file
        # name:value options are the case's setup, anything else goes to the emulator
        self.opts, self.extra = [], []
        for e in extra:
            k, sep, v = e.partition(":")
            if sep and k in OPTS:
                self.opts.append((k, v))
            else:
                self.extra.append(e)

    @property
    def key(self):
        return f"{self.suite}/{self.name}"

    @property
    def cost(self):
        return (self.skip + self.frames) * (3 if "--bench-full" in self.extra else 1)


def ini_lines(pairs):
    """[(section.key, value)] as ini text, one block per section."""
    secs = {}
    for k, v in pairs:
        sec, _, key = k.partition(".")
        secs.setdefault(sec, []).append(f"{key} = {v}")
    return "".join(f"[{s}]\n" + "\n".join(lines) + "\n\n" for s, lines in secs.items())


def pin(top):
    """One mtime for a folder card: its FAT carries the host's dates, which a checkout moves."""
    for root, dirs, files in os.walk(top):
        for n in dirs + files:
            os.utime(os.path.join(root, n), (PINNED, PINNED))
    os.utime(top, (PINNED, PINNED))


def set_up(case, corpus, cfg, tmp):
    """Apply a case's options to its fresh config; returns the emulator arguments they add."""
    args, cards = [], []
    ini = {"cfg": [], "mac": []}
    for k, v in case.opts:
        if k in ini:
            ini[k].append(tuple(v.split("=", 1)))
        elif k == "nvram":
            (cfg / "nvram").mkdir(exist_ok=True)
            shutil.copy(corpus / v, cfg / "nvram" / f"{case.machine}.cmos")
        elif k in ("sd", "hdd"):
            src, dst = corpus / v, tmp / f"{k}-{Path(v).name}"
            # a copy, so the corpus is never written
            if src.is_dir():
                shutil.copytree(src, dst)
            else:
                shutil.copy(src, dst)
            cards.append(dst)
            if k == "sd":
                args += ["--sdcard", str(dst)]
            else:
                ini["cfg"].append(("MEDIA.hdd.master", str(dst)))
        elif k == "put":
            path, src = v.split("=", 1)
            if not cards:
                raise OSError("put: needs an sd: or hdd: folder before it")
            shutil.copy(corpus / src, cards[-1] / path)
        elif k == "keys":
            args += ["--bench-keys", v]
        elif k == "romfile":		# a ROM the bundle has not, among the config's: mac:rom.rom0=<its name>
            shutil.copy(corpus / v, cfg / "roms" / Path(v).name)
        elif k == "sdimg":		# the slot's card: a FAT16 image made of a folder (mksd.py)
            dst = (tmp / f"sd-{Path(v).name}").with_suffix(".img")
            subprocess.run([sys.executable, str(ROOT / "tests" / "core" / "divmmc" / "mksd.py"), str(dst), str(corpus / v)], check=True)
            args += ["--sdcard", str(dst)]
        elif k == "eeprom":		# what the divMMC's EEPROM holds, kept for all machines
            (cfg / "nvram").mkdir(exist_ok=True)
            shutil.copy(corpus / v, cfg / "nvram" / "divmmc.eeprom")
        elif k in ("card1", "card2", "cardimg1", "cardimg2"):
            # a divMMC slot: an image or a folder as it is, or (cardimg) a FAT16 image
            # made of a folder by tests/core/divmmc/mksd.py
            src, slot = corpus / v, k[-1]
            dst = tmp / f"card{slot}-{Path(v).name}"
            if k.startswith("cardimg"):
                dst = dst.with_suffix(".img")
                subprocess.run([sys.executable, str(ROOT / "tests" / "core" / "divmmc" / "mksd.py"), str(dst), str(src)], check=True)
            elif src.is_dir():
                shutil.copytree(src, dst)
            else:
                shutil.copy(src, dst)
            cards.append(dst)
            ini["cfg"].append((f"MEDIA.divmmc.card{slot}", str(dst)))
    for card in cards:
        if card.is_dir():
            pin(card)
    # the references are of folders served read-only; a writable one is a bigger volume
    folder_ro = any(card.is_dir() for card in cards)
    if ini["cfg"]:
        if folder_ro:
            ini["cfg"].append(("GENERAL.folder.write", "no"))
        (cfg / "config.conf").write_text(ini_lines(ini["cfg"]), encoding="utf-8")
    elif folder_ro:
        # no settings of its own: the config a fresh start writes, with that one line in
        base = (ROOT / "res" / "fallback" / "config.conf").read_text(encoding="utf-8")
        (cfg / "config.conf").write_text(base.replace("[GENERAL]\n", "[GENERAL]\nfolder.write = no\n", 1), encoding="utf-8")
    if ini["mac"]:
        (cfg / "machines").mkdir(exist_ok=True)
        (cfg / "machines" / f"{case.machine}.conf").write_text(ini_lines(ini["mac"]), encoding="utf-8")
    return args


def load_suite(name):
    path = SUITES / f"{name}.txt"
    cases = []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = shlex.split(line)
        # boot runs the firmware from power-on and takes no file
        nf = 5 if len(f) > 2 and f[2] == "boot" else 6
        if len(f) < nf or f[2] not in HOWS:
            sys.exit(f"{path}:{n}: expected: name machine auto|open skip frames file [opts...]"
                     " or name machine boot skip frames [opts...]")
        cases.append(Case(name, f[0], f[1], f[2], f[3], f[4], f[5] if nf == 6 else "", f[nf:]))
    return cases


def all_suites():
    return sorted(p.stem for p in SUITES.glob("*.txt"))


def find_app(arg):
    """The staged dist folder to run: given, or the newest dev build in build/dist."""
    if arg:
        app = Path(arg)
    else:
        dist = ROOT / "build" / "dist"
        cands = [d for d in dist.glob("*-dev+*") if (d / EXE).exists()] if dist.exists() else []
        if not cands:
            sys.exit("no dev build in build/dist; pass --app <staged folder>")
        app = max(cands, key=lambda d: (d / EXE).stat().st_mtime)
    if not (app / EXE).exists() or not (app / "config").is_dir():
        sys.exit(f"{app}: not a staged build (needs {EXE} and config/)")
    return app


def run_case(case, app, corpus, shots, timeout):
    image = corpus / case.file
    if case.file and not image.exists():
        return case, {"error": f"missing image {image}"}, 0.0
    with tempfile.TemporaryDirectory(prefix="xreg-") as tmp:
        # A fresh config each time: the app restores the last media and machine on start.
        cfg = Path(tmp) / "config"
        shutil.copytree(app / "config", cfg)
        cmd = [str(app / EXE), "--confdir", str(cfg), "-m", case.machine]
        if case.how == "auto":
            cmd += ["--autostart", str(image)]
        elif case.how == "open":
            cmd += [str(image)]
        try:
            cmd += set_up(case, corpus, cfg, Path(tmp))
        except OSError as e:
            return case, {"error": f"setup: {e}"}, 0.0
        cmd += ["--bench-skip", str(case.skip), "--bench", str(case.frames), "--bench-hash"]
        if shots:
            cmd += ["--bench-shot", str(shots / f"{case.suite}-{case.name}.ppm")]
        cmd += case.extra
        t0 = time.monotonic()
        try:
            p = subprocess.run(cmd, cwd=app, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return case, {"error": f"timeout after {timeout}s"}, time.monotonic() - t0
        dt = time.monotonic() - t0
    out = p.stdout.decode("latin-1", "replace")
    m = HASH_RE.search(out)
    if not m:
        tail = (out + p.stderr.decode("latin-1", "replace")).strip().splitlines()[-3:]
        return case, {"error": f"no hash line (exit {p.returncode}): {' | '.join(tail)}"}, dt
    return case, dict(zip(FIELDS, m.groups())), dt


def golden_path(golden, suite):
    return golden / f"{suite}.json"


def load_golden(golden, suite):
    p = golden_path(golden, suite)
    return json.loads(p.read_text(encoding="utf-8")) if p.exists() else {}


def matches(name, key, patterns):
    """-k: a case is picked by its name or its suite/name, against any pattern"""
    return not patterns or any(fnmatch.fnmatch(name, k) or fnmatch.fnmatch(key, k) for k in patterns)


def pick(args):
    suites = args.suite or all_suites()
    cases = [c for s in suites for c in load_suite(s)]
    if args.k:
        cases = [c for c in cases if matches(c.name, c.key, args.k)]
    if not cases:
        sys.exit("no cases match")
    return cases


def env_path(arg, var, what):
    v = arg or os.environ.get(var)
    if not v:
        sys.exit(f"no {what}: pass --{what} or set {var}")
    return Path(v)


def execute(args, cases):
    app = find_app(args.app)
    corpus = env_path(args.corpus, "XPECCY_CORPUS", "corpus")
    shots = Path(args.shots).resolve() if args.shots else None
    if shots:
        shots.mkdir(parents=True, exist_ok=True)
    print(f"app {app.name}, {len(cases)} cases, {args.jobs} at a time", flush=True)
    results = {}
    t0 = time.monotonic()
    with cf.ThreadPoolExecutor(args.jobs) as pool:
        # the longest first, so they do not finish last
        futs = [pool.submit(run_case, c, app, corpus, shots, args.timeout)
                for c in sorted(cases, key=lambda c: -c.cost)]
        for f in cf.as_completed(futs):
            case, res, dt = f.result()
            results[case.key] = res
            if args.verbose:
                print(f"  {case.key:28} {dt:6.1f}s {res.get('error', '')}", flush=True)
    print(f"done in {time.monotonic() - t0:.0f}s", flush=True)
    return results


def compare(cases, results, ref_of):
    """A line for each case that fails or differs from ref_of(case) (None: no reference yet);
    the counts (same, changed, new, failed)"""
    same = changed = new = failed = 0
    for c in cases:
        r, ref = results[c.key], ref_of(c)
        err = r.get("error") or (ref or {}).get("error")
        if err:
            failed += 1
            print(f"FAIL     {c.key:28} {err}")
        elif ref is None:
            new += 1
            print(f"NEW      {c.key:28} no reference yet")
        elif all(r[f] == ref.get(f) for f in FIELDS):
            same += 1
        else:
            changed += 1
            which = ", ".join(f for f in FIELDS if r[f] != ref.get(f))
            print(f"DIFFERS  {c.key:28} {which}")
    return same, changed, new, failed


def cmd_run(args):
    cases = pick(args)
    golden = env_path(args.golden, "XPECCY_GOLDEN", "golden")
    results = execute(args, cases)
    if args.twice:
        again = execute(args, cases)
        for k, r in again.items():
            if r != results[k] and "error" not in r:
                results[k] = {"error": "not deterministic: two runs differ"}
    refs = {s: load_golden(golden, s) for s in {c.suite for c in cases}}
    same, changed, new, failed = compare(cases, results, lambda c: refs[c.suite].get(c.name))
    print(f"\n{same} same, {changed} differ, {new} new, {failed} failed")
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1), encoding="utf-8")
    return 1 if changed or failed else 0


def cmd_bless(args):
    cases = pick(args)
    golden = env_path(args.golden, "XPECCY_GOLDEN", "golden")
    results = execute(args, cases)
    golden.mkdir(parents=True, exist_ok=True)
    bad = 0
    for s in sorted({c.suite for c in cases}):
        ref = load_golden(golden, s)
        for c in (c for c in cases if c.suite == s):
            r = results[c.key]
            if "error" in r:
                bad += 1
                print(f"FAIL     {c.key:28} {r['error']} - reference kept")
                continue
            if ref.get(c.name) != r:
                print(f"blessed  {c.key}")
            ref[c.name] = r
        golden_path(golden, s).write_text(json.dumps(dict(sorted(ref.items())), indent=1) + "\n", encoding="utf-8")
    return 1 if bad else 0


def cmd_drawcheck(args):
    """Frames left undrawn must change nothing the machine does: the same case run with every
    frame drawn and with only every Nth drawn, both hashing those Nth frames, agrees in every
    field. Needs no reference."""
    cases = pick(args)
    n = str(args.every)
    def variant(flag):
        out = []
        for c in cases:
            v = copy.copy(c)
            v.extra = c.extra + [flag, n]
            out.append(v)
        return out
    drawn = execute(args, variant("--bench-hash-every"))
    sparse = execute(args, variant("--bench-draw-every"))
    same, changed, _, failed = compare(cases, sparse, lambda c: drawn[c.key])
    print(f"\n{same} same, {changed} differ, {failed} failed (every {n})")
    return 1 if changed or failed else 0


def cmd_list(args):
    cases = pick(args)
    for c in cases:
        print(f"{c.key:28} {c.machine:12} {c.how:4} {c.skip:>5}+{c.frames:<5} {c.file}")
    # every machine the build defines, and how many cases run it
    count = {m.stem: 0 for m in (ROOT / "res" / "machines").glob("*.conf")}
    for c in cases:
        count[c.machine] = count.get(c.machine, 0) + 1
    print()
    for m in sorted(count):
        print(f"{m:14} {count[m]:3}{'  < 3' if count[m] < 3 else ''}")
    return 0


def cmd_quick(args):
    sys.path.insert(0, str(Path(__file__).resolve().parent / "core"))
    sys.dont_write_bytecode = True
    import quick
    return quick.run(args, matches)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("run", "bless", "list", "quick", "drawcheck"))
    ap.add_argument("suite", nargs="*", help="suites in tests/suites (default: all)")
    ap.add_argument("-k", action="append", help="only cases whose name matches (glob, repeatable)")
    ap.add_argument("--app", help="staged build folder (default: newest dev build in build/dist)")
    ap.add_argument("--corpus", help="image folder (default: $XPECCY_CORPUS)")
    ap.add_argument("--golden", help="reference folder (default: $XPECCY_GOLDEN)")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--timeout", type=int, default=300, help="seconds per case")
    ap.add_argument("--shots", help="write each case's last frame here as .ppm")
    ap.add_argument("--twice", action="store_true", help="run every case twice and flag any that differ")
    ap.add_argument("--json", help="write the raw results here")
    ap.add_argument("--every", type=int, default=3, help="drawcheck: draw every Nth frame (default 3)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--no-build", action="store_true", help="quick: use what build/out/tests holds")
    ap.add_argument("--bless", action="store_true", help="quick: take the current output as expected")
    ap.add_argument("--mingw", help="quick, Windows: MinGW folder (default: $XPECCY_MINGW, then the qt6-x64 one)")
    args = ap.parse_args()
    return {"run": cmd_run, "bless": cmd_bless, "list": cmd_list, "quick": cmd_quick,
            "drawcheck": cmd_drawcheck}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
