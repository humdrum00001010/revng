# Issue 655: supported binary candidate does not reproduce the const write

[Upstream issue](https://github.com/revng/revng/issues/655) is open. **Not reproduced by this complete binary candidate.** This negative result does not qualify as positive binary reproduction for the issue.

The independently authored complete C DLL changes the public `LOGFONTW.lfHeight`, increments `lfWidth`, decrements `lfWeight`, then calls genuine WinGDI `CreateFontIndirectW(const LOGFONTW *)`. The standalone DLL contains a real GDI import, not a replacement helper or imported application fragment.

Expected if this binary reaches the reported defect: compilation of the complete emitted C fails on a write through a const-qualified record. Actual: **official quick archive generation exits 0 and compilation of the complete unchanged generated C exits 0.** The writer receives a writable inferred `struct_23 *`; normal analysis preserves the readonly WinGDI declaration and inserts `(const LOGFONTW *) argument_0` at its call. Therefore the binary does not establish the illegal const member write from the earlier synthetic Clift fixture. That fixture is not included or counted here.

Evidence is `runs/official-quick-1446/result.json`, complete generated source/headers, `whole-compile.log` and the whole COFF object. The expected external GDI symbol remains unresolved at object stage. This result does not claim DLL linking or runtime correctness.

## Reproduce

Requires Python 3, Docker with linux/amd64 support, and LLVM 17 (`clang`, `lld-link`, `llvm-dlltool`, `llvm-nm`). Set `LLVM_BIN` to the LLVM bin directory; its default is the Homebrew LLVM 17 path. Run from this package directory:

```sh
python3 build.py
python3 run.py --quick --variant another-fresh-run
```

`run.py` uses this official pinned image, capped at 1 CPU / 4 GiB with networking disabled:

```sh
IMAGE=revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce
mkdir quick-output
docker run --rm --platform linux/amd64 --network none --cpus 1 --memory 4g --memory-swap 4g \
  --mount type=bind,src="$PWD/repro.dll",dst=/inputs/repro.dll,readonly \
  --mount type=bind,src="$PWD/quick-output",dst=/work --workdir /work "$IMAGE" \
  revng quick artifact emit-recompilable-archive /inputs/repro.dll -o recompilable.tar.gz
tar -xzf quick-output/recompilable.tar.gz -C quick-output
"$LLVM_BIN/clang" --target=i686-w64-windows-gnu -std=gnu2x -ffreestanding -O0 \
  -I quick-output/decompiled -c quick-output/decompiled/functions.c -o quick-output/whole.obj
```

This is the entire DLL and entire generated translation unit. There are no function selectors, model edits, injected LLVM/Clift files, source rewrites, or private input fragments. `build.py` builds the independently authored complete C DLL. Compiler/linker commands, binary hashes, actual Docker terminal states, archive/generated-source hashes and complete compiler logs are retained in the JSON receipts. `tool-versions.json` pins LLVM executable hashes and the official image/source revision.

`clean-build-verification.json` confirms fresh builds from this source and script produce byte-identical recorded DLLs. Intermediate source-build objects are not needed and are excluded from this package.
