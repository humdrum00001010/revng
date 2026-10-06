# Issue 651: supported binary candidates do not reproduce the assertion

[Upstream issue](https://github.com/revng/revng/issues/651) is open. **Not reproduced by either complete binary candidate.** These negative results do not qualify as positive binary reproduction for the issue.

The independently authored complete C DLL accesses three packed records: a byte followed by `long double`, an unsigned `_BitInt(24)`, or `_Bool`. The x86 Windows GNU target retains actual x87 long-double operations and the packed byte offsets. A second whole DLL retains DWARF. Neither uses injected LLVM, Clift, or model definitions.

Expected if this binary reaches the reported defect: normal C emission aborts on `llvm::alignTo(ByteSize, Alignment) == StructByteSize` or `FieldSizeInBits % 8 == 0`. Actual: **official quick archive generation exits 0 and compilation of the complete unchanged generated C exits 0 for both DLLs.** The normal pipeline reconstructs memory accesses and helper calls; the authored C aggregate layout does not establish that its original LLVM struct reaches the Clifter.

The [maintainer's response](https://github.com/revng/revng/issues/651#issuecomment-6011875851) requires reproduction through the regular binary pipeline. Earlier synthetic importer fixtures are not included or counted here. Evidence is `runs/official-quick-1446/result.json` and `runs/official-quick-dwarf/result.json`, including complete generated files, compiler logs and COFF objects. Undefined runtime helper symbols are recorded; a successful object compilation does not establish DLL linking or runtime correctness.

For the second whole-binary candidate:

```sh
python3 build.py --preserve-dwarf
python3 run.py --quick --binary repro-with-dwarf.dll --variant another-fresh-dwarf-run
```

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
