# DataLayoutAnalysis pointer-cycle reproducer

This package contains an independently authored, four-line C input and the
complete PE32 DLL built from it. The DLL is only parsed by rev.ng.

```c
void pointer_cycle(unsigned *slot) {
  unsigned middle = *(unsigned *) *slot;
  *slot = *(unsigned *) middle;
}
```

## Reproduce

Run the bounded reproducer with Docker:

```sh
./reproduce.sh
```

The script defaults to the pinned public rev.ng image recorded in
`results.json`. To use a locally built `develop` image:

```sh
IMAGE=your-revng-develop-image ./reproduce.sh
```

The underlying supported CLI command is:

```sh
revng quick artifact emit-recompilable-archive \
  /input/repro.dll -o /tmp/recompilable.tar.gz -- --debug-log=verify
```

It exits 134 at the existing DLA invariant:

```text
TS.verifyPointerDAG() and TS.verifyDAG() and TS.verifyUnions()
```

`--debug-log=verify` enables the existing invariant checks and bounds the
failure before recursive model construction consumes memory. With the same
DLL and no logger argument, a 256 MiB, no-swap run exits 137 with
`OOMKilled=true`.

## Cause boundary

Immediately before `MergePointeesOfPointerUnion`, the relevant pointer chain
is `13 -> 20 -> 26`. Two same-offset pointer children have nodes 13 and 26 as
their pointees. The pass merges those pointees and retargets `20 -> 26` to
`20 -> 13`, creating the pointer-only cycle `13 -> 20 -> 13`.

The verifier detects that cycle in `DLAMakeModelTypes.cpp`. Without the
verifier, `getOrCreateUpcastableScalarType` follows the pointer cycle before
either cache entry is populated and allocates recursive coroutine frames until
the process runs out of memory.

Three source reductions are negative controls: removing one pointer load,
removing the final store, or collapsing the two statements into one expression
all make the quick command exit 0 with the tested `-O0` compiler. The named
`middle` value, three loads, and final write are the observed source boundary.

## Build

The checked-in DLL is reproducible from `repro.c` with a Clang frontend and a
GNU MinGW linker:

```sh
CLANG=clang MINGW_LD=i686-w64-mingw32-ld ./build.sh
```

The recorded build used Clang 23.1.2 and GNU ld 2.47.20260726. The linker emits
a zero PE timestamp, so repeated builds with those tools produce the DLL hash
listed in `SHA256SUMS`.
