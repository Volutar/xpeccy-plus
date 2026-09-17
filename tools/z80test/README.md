# Z80 core timing test

Runs Xpeccy's Z80 against Fuse's per-instruction test suite. For every one of the
1356 tests it checks the tstate each bus cycle happened on, the order of those
cycles, the final registers and flags, and which bytes of memory changed.

```
./run.sh
CC=C:/Qt/Tools/mingw1120_64/bin/gcc.exe ./run.sh      # windows, qt toolchain
```

It exits 0 when the run matches `known-failures.txt` - a list of state failures
that have been looked at and left, with the reason beside each - and 1 as soon as
a timing failure turns up or that list changes either way. So a nonzero exit
means *something moved*, not *the core is imperfect*.

Fuse's data is from 2017 and a couple of its expectations have been overtaken by
later research, so it is not the last word. Patrik Rak's
[Zilog Z80 CPU Test Suite](https://github.com/raxoft/z80test) compares against
CRCs taken off a real 48K instead; it runs inside the emulator as a `.tap` and is
what settles a disagreement with the vectors here.

Failures come out in two groups, because they mean different things:

- **timing** - a bus cycle on the wrong tstate, in the wrong order, or a wrong
  tick count. This is what the ULA sees, so it is what breaks contention.
- **state** - registers, flags, MEMPTR or memory. Same timing, wrong result.

Two differences are expected and filtered out. Fuse's harness prints port
contention (`PC`) lines that we cannot produce - port contention lives in the
machine here, not in the cpu core. And on a conditional jump that is not taken
Fuse contends the displacement read but throws the byte away, so it prints the
bus cycle without the read; the cycle itself is the same.

## Licensing

**This is a development tool and is not part of the emulator.** Nothing here is
compiled into `xpeccy-plus`, which stays under its own MIT license.

`coretest.c` derives from `z80/coretest.c` of
[Fuse](https://fuse-emulator.sourceforge.net/), Copyright (c) 2003-2017 Philip
Kendall, and `tests/` holds Fuse's test data unchanged. Both are GNU GPL v2 or
later, so this folder is too - see `LICENSE`.
