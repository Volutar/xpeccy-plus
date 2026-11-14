"""The quick tier: libxpeccy's headless benches, built by tests/CMakeLists.txt with no Qt.

Driven by `regress.py quick`. Each bench prints text; expected.txt beside it holds that
text cut into `== <case>` sections, and a case passes when its section is the same.

  z80       tools/z80test: Fuse's per-instruction tests, verdict by its compare.py
  tsconf    tsconf-bench over the Z80 programs in core/tsconf/bin (cases.txt)
  baseconf  baseconf-bench over core/baseconf/bin (cases.txt)
  divmmc    divmmc-bench over core/divmmc/bin (cases.txt): a 48K with a divMMC
  tape      tape-bench over the images core/tape/mktape.py writes
  pok       pok-bench: the .pok reader over core/pok/files, pokes in and out of 16/48/128K
  vfat      vfat-bench: a folder volume taking writes, FatFs as the guest, on a scratch folder
  zxnext    zxnext-bench: debugger memory views, and a Next then a 48K with the Next's hooks gone
  kbd       kbd-bench over core/kbd/bin (cases.txt): host keys that are combinations, shift first;
            the Kempston mouse's ports
  esp       esp-bench over core/esp/bin (cases.txt): the emulated ESP-01 on the Next's UART, a
            server on 127.0.0.1
  siblings  siblings-bench over core/siblings/folders.txt: which file names are sides of a set
  brk       brk-bench: breakpoints against the Z80's bus cycles (Read, Fetch, Interrupt) on a 48K
  dsk       dsk-bench: EXTENDED DSK images the bench writes, read by the uPD765 through the +3's ports;
            a 1 MB TRD through the Beta disk's WD1793
  gs        gs-bench: the General Sound mix, original board and NeoGS, on fixed samples and volumes;
            the firmwares in config/roms: their idle loop skipped, the card as when it is not
  beeper    beeper-bench: the beeper's filter by type - time constants, -3 dB points, the piezo's ring

The programs are committed assembled; core/mkbins.py rebuilds them with sjasmplus.
"""

import concurrent.futures as cf
import difflib
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CORE = Path(__file__).resolve().parent
ROOT = CORE.parent.parent
BUILD = ROOT / "build" / "out" / "tests"
BENCHES = ("z80", "tsconf", "baseconf", "divmmc", "tape", "pok", "vfat", "zxnext", "kbd", "esp", "siblings", "brk", "dsk", "gs", "beeper")
EXE = ".exe" if os.name == "nt" else ""
# the qt6-x64 toolchain of packaging/make-dist.ps1, used when nothing else is given
WIN_MINGW = Path(r"C:\Qt\Tools\mingw1120_64")
WIN_CMAKE = Path(r"C:\Qt\Tools\CMake_64\bin")


def parse_cases(bench):
    """bin lines -> [(file, source, [defines])], run lines -> [(case, file, frames, {env})]"""
    path = CORE / bench / "cases.txt"
    bins, runs = [], []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        f = shlex.split(line)
        if f[0] == "bin" and len(f) >= 3:
            bins.append((f[1], f[2], f[3:]))
        elif f[0] == "run" and len(f) >= 4:
            env = dict(e.split("=", 1) for e in f[4:])
            runs.append((f[1], f[2], int(f[3]), env))
        else:
            sys.exit(f"{path}:{n}: expected 'bin file source [DEF=V...]' or 'run case file frames [ENV=V...]'")
    return bins, runs


def read_sections(text):
    """`== name` headed sections of a bench's output or of expected.txt, in order"""
    out, cur = {}, None
    for line in text.splitlines():
        if line.startswith("== "):
            cur = line[3:].strip()
            out[cur] = []
        elif cur is not None:
            out[cur].append(line)
    return {k: "\n".join(v) for k, v in out.items()}


def toolchain(args):
    """Environment and cmake configure arguments for the build."""
    env = dict(os.environ)
    cfg = []
    cmake = shutil.which("cmake")
    if os.name == "nt":
        mingw = Path(args.mingw or os.environ.get("XPECCY_MINGW") or WIN_MINGW)
        if (mingw / "bin" / "gcc.exe").exists():
            env["PATH"] = f"{mingw / 'bin'};{env['PATH']}"
            cfg += ["-G", "MinGW Makefiles"]
            zlib = next(mingw.glob("*-w64-mingw32/lib/libz.a"), None)
            if zlib:
                inc = zlib.parent.parent / "include"
                cfg += [f"-DZLIB_LIBRARY={zlib.as_posix()}", f"-DZLIB_INCLUDE_DIR={inc.as_posix()}"]
        elif not shutil.which("gcc"):
            return None, None, f"no MinGW at {mingw}: pass --mingw or set XPECCY_MINGW"
        if not cmake and (WIN_CMAKE / "cmake.exe").exists():
            cmake = str(WIN_CMAKE / "cmake.exe")
    if not cmake:
        return None, None, "no cmake on PATH"
    return env, [cmake] + cfg, None


def build(args):
    env, cfg, err = toolchain(args)
    if err:
        print(f"ERROR    build: {err}")
        return False
    cmake = cfg[0]
    t0 = time.monotonic()
    steps = []
    if not (BUILD / "CMakeCache.txt").exists():
        steps.append(cfg + ["-DCMAKE_BUILD_TYPE=Release", "-S", str(ROOT / "tests"), "-B", str(BUILD)])
    steps.append([cmake, "--build", str(BUILD), "-j", str(os.cpu_count() or 4)])
    for cmd in steps:
        p = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace")
        if p.returncode:
            print(f"ERROR    build: {' '.join(cmd[:2])} failed")
            print("\n".join((p.stdout + p.stderr).strip().splitlines()[-15:]))
            return False
    print(f"built in {time.monotonic() - t0:.1f}s", flush=True)
    return True


def run_prog(bench, case):
    """One run of a mailbox bench, in a folder of its own."""
    name, file, frames, cenv = case
    exe = BUILD / f"{bench}-bench{EXE}"
    with tempfile.TemporaryDirectory(prefix="xquick-") as tmp:
        env = dict(os.environ)
        env["XPECCY_ROOT"] = str(ROOT)
        for k, v in cenv.items():
            if v.endswith(".img"):
                (Path(tmp) / v).write_bytes(bytes(1 << 20))
            env[k] = v
        cmd = [str(exe), str(CORE / bench / "bin" / file), str(frames)]
        p = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True, errors="replace", timeout=120)
    out = p.stdout.rstrip("\n")
    if p.returncode:
        out += f"\nexit {p.returncode}: {p.stderr.strip()}"
    return name, out


def bench_prog(bench, jobs):
    _, runs = parse_cases(bench)
    with cf.ThreadPoolExecutor(jobs) as pool:
        return dict(pool.map(lambda c: run_prog(bench, c), runs))


def run_sections(bench, args, rc=True, timeout=120, **kw):
    """A bench that prints its own sections; a failed run (rc: a nonzero exit too) is one of them."""
    p = subprocess.run([str(BUILD / f"{bench}-bench{EXE}")] + args, capture_output=True, text=True,
                       errors="replace", timeout=timeout, **kw)
    res = read_sections(p.stdout)
    if (rc and p.returncode) or not res:
        res[f"{bench}-bench"] = f"exit {p.returncode}: {p.stderr.strip()}"
    return res


def bench_tape():
    with tempfile.TemporaryDirectory(prefix="xquick-") as tmp:
        subprocess.run([sys.executable, str(CORE / "tape" / "mktape.py"), tmp], check=True)
        return run_sections("tape", [tmp])


def bench_pok():
    return run_sections("pok", sorted(str(f) for f in (CORE / "pok" / "files").glob("*.pok")))


def bench_siblings():
    return run_sections("siblings", [str(CORE / "siblings" / "folders.txt")], encoding="utf-8")


def bench_brk():
    return run_sections("brk", [], rc=False)


def bench_dsk():
    return run_sections("dsk", [], timeout=300)


def bench_gs():
    roms = ROOT / "config" / "roms"
    return run_sections("gs", [str(roms / "gs105b.rom"), str(roms / "neogs111.rom")], rc=False)


def bench_beeper():
    return run_sections("beeper", [], rc=False)


def bench_vfat():
    with tempfile.TemporaryDirectory(prefix="xquick-") as tmp:
        return run_sections("vfat", [tmp, "4", "120"], timeout=300)


def bench_zxnext():
    res = {}
    for case in ("switch", "debugger", "reset", "ram"):
        p = subprocess.run([str(BUILD / f"zxnext-bench{EXE}"), f"--{case}"], capture_output=True, text=True, errors="replace", timeout=120)
        sections = read_sections(p.stdout)
        res.update(sections)
        if p.returncode or not sections:
            res[f"zxnext-{case}"] = f"exit {p.returncode}: {p.stderr.strip()}"
    return res


def bench_z80():
    """Fuse's tests: (passed, the summary line, the full report)"""
    z80 = ROOT / "tools" / "z80test"
    out = BUILD / "z80-out.txt"
    with open(out, "w") as f:
        p = subprocess.run([str(BUILD / f"z80-coretest{EXE}"), str(z80 / "tests" / "tests.in")], stdout=f, timeout=120)
    if p.returncode:
        return False, f"coretest exit {p.returncode}", ""
    p = subprocess.run([sys.executable, str(z80 / "compare.py"), str(out), str(z80 / "tests" / "tests.expected")],
                       capture_output=True, text=True, errors="replace")
    report = (p.stdout + p.stderr).strip()
    return p.returncode == 0, (report.splitlines() or ["no output"])[0], report


def show_diff(exp, got):
    lines = list(difflib.unified_diff(exp.splitlines(), got.splitlines(), "expected", "got", n=0, lineterm=""))
    for line in lines[2:10]:
        print(f"           {line}")
    if len(lines) > 10:
        print(f"           ... {len(lines) - 10} more")


def run(args, matches):
    """matches(name, key, patterns): regress.py's -k test"""
    benches = args.suite or list(BENCHES)
    bad = [b for b in benches if b not in BENCHES]
    if bad:
        sys.exit(f"no such bench: {', '.join(bad)} (there are: {', '.join(BENCHES)})")
    if not args.no_build and not build(args):
        return 2
    t0 = time.monotonic()
    passed = failed = 0
    for bench in benches:
        if bench == "z80":
            if not matches("fuse", "z80/fuse", args.k):
                continue
            ok, summary, report = bench_z80()
            print(f"{'PASS' if ok else 'FAIL':8} z80/fuse  {summary}")
            if not ok and args.verbose:
                print(report)
            passed, failed = passed + ok, failed + (not ok)
            continue
        got = {"tape": bench_tape, "pok": bench_pok, "vfat": bench_vfat, "zxnext": bench_zxnext,
               "siblings": bench_siblings, "brk": bench_brk, "dsk": bench_dsk, "gs": bench_gs,
               "beeper": bench_beeper}.get(bench, lambda: bench_prog(bench, args.jobs))()
        got = {k: v for k, v in got.items() if matches(k, f"{bench}/{k}", args.k)}
        exp_path = CORE / bench / "expected.txt"
        exp = read_sections(exp_path.read_text(encoding="utf-8")) if exp_path.exists() else {}
        if args.bless:
            exp.update(got)
            exp_path.write_text("".join(f"== {k}\n{v}\n" for k, v in exp.items()), encoding="utf-8")
            print(f"blessed  {bench}: {len(got)} cases")
            continue
        for name, out in got.items():
            key = f"{bench}/{name}"
            if name not in exp:
                failed += 1
                print(f"FAIL     {key}  no expected output")
            elif out == exp[name]:
                passed += 1
                print(f"PASS     {key}")
            else:
                failed += 1
                print(f"FAIL     {key}")
                show_diff(exp[name], out)
        if not args.k:
            for name in exp:
                if name not in got:
                    failed += 1
                    print(f"FAIL     {bench}/{name}  not run")
    if args.bless:
        return 0
    print(f"\n{passed} passed, {failed} failed in {time.monotonic() - t0:.1f}s")
    return 1 if failed else 0
