# Issue 645: supported whole-binary reproduction

[Upstream issue](https://github.com/revng/revng/issues/645) is open. **Reproduced** through the official binary-input pipeline.

The clean DLL calls public GDI32 `CreateDIBSection` and `CreatePalette`, causing normal binary analysis to recover public bitmap/palette type definitions. Expected: the complete emitted C compiles. Actual: whole archive generation exits 0, then whole C compilation exits 1 because arrays precede complete typedef element definitions:

```text
types-and-globals.h:85:20: error: array has incomplete element type 'RGBQUAD' (aka 'struct tagRGBQUAD')
types-and-globals.h:112:27: error: array has incomplete element type 'PALETTEENTRY' (aka 'struct tagPALETTEENTRY')
```

Both `runs/official-1446/result.json` (normal project init/artifact) and `runs/official-quick-1446/result.json` (requested quick command) prove the failure. The latter includes `whole-compile.log` and all unchanged generated files. This package supplies the whole standalone DLL, C source, and public GDI import library definition.

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

`clean-build-verification.json` confirms fresh builds from this source and script produce byte-identical recorded DLLs, including the clean DWARF variant. Intermediate source-build objects are not needed and are excluded from this package.
