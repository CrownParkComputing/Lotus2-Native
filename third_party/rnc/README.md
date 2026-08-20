# RNC ProPack — FORMER runtime dep for `src/host/rnc2.c` (superseded)

**Superseded 2026-08-20.**  `src/host/rnc2.c` is now a self-contained
pure-C port of the method-2 unpack path from the public RNC ProPack
decompilation (lab313ru/rnc_propack_source); no assembled object is
needed and this directory is kept only for this note.  The text below
is retained as a record of the approach that was tried first.

The shim in `src/host/rnc2.c` calls `rnp_unpack()` from the original
RNC ProPack by T.F. Ralph.  The RNC ProPack source is **not** vendored
here — you build it once and place the object file in this directory.

## One-time setup

1. Grab `rnp38.lha` (or whatever the current version is) from aminet:
   ```
   aminet:dev/pack/rnp38.lha
   ```
   Direct mirror: <http://aminet.net/package/dev/pack/rnp38>

2. Extract somewhere outside the project (e.g. `/tmp/rnp_src/`).

3. Build just the unpacker.  The Makefile in the archive is for Amiga;
   the unpacker is plain C and compiles with a normal C compiler.
   The function we need is `rnp_unpack()` in `sources/RNC_Unpack.c`
   (or equivalent).  Compile just that one TU:
   ```
   cc -O2 -c -o rnp.o sources/RNC_Unpack.c
   ```
   If the archive also has an `rnp.h`, it should declare
   `int rnp_unpack(const void *in, void *out, unsigned int outsize);`
   — the shim in `src/host/rnc2.c` forward-declares the same prototype
   with `extern`, so a missing header is fine.

4. Copy `rnp.o` into this directory:
   ```
   cp rnp.o /home/jon/Lotus2-Native/third_party/rnc/rnp.o
   ```

5. Rebuild the host.  The Makefile picks up `third_party/rnc/rnp.o`
   automatically:
   ```
   cd /home/jon/Lotus2-Native && make build/lotus2
   ```

## Why a thin wrapper, not a vendored port

The earlier clean-room port (the 206-line `rnc2.c` that returned
`rc=-5` on the Lotus 2 install body) had its bit-encoding wrong on a
subcommand the install body hits in its first command.  The cookbook
says: "Every shared fix needs a focused unit test and the affected
title integration route. Success-return stubs do not count as
implementations."  A hand-rolled decoder that fails loudly is honest;
a hand-rolled decoder that *silently* returns plausible-looking output
on a compressed install body is worse.  The wrapper keeps the
integration path identical to what an unmodified RNC ProPack provides
to every other Amiga title that uses the format.

## Licence

The RNC ProPack is the work of T.F. Ralph.  The source lives outside
this project; only the compiled `rnp.o` lands here.  Do not commit
the source.
