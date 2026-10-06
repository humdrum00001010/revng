# Issue 647: binary reachability remains unconfirmed

[Upstream issue](https://github.com/revng/revng/issues/647) is open. **Not reproduced by these binary candidates.** This package is an honest negative result, not a qualifying positive binary reproduction for that issue.

The independently authored complete C DLL passes a top-level-const pointer typedef through two noinline functions. The ordinary PE candidate and a second PE retaining complete DWARF both complete supported whole-binary pipeline and whole generated C compilation with exit 0. The retained ordinary project model contains no const-qualified type definitions or `FixedByteView` typedef; the quick DWARF result likewise does not reproduce the diagnostic. These candidates do not establish the original implicit-bitcast condition. The previous IR-only fixture is not used or included in this package. Binary reachability of that condition remains unconfirmed.

Evidence is `runs/official-1446/result.json` and `runs/official-quick-dwarf-clean/result.json`. To build/run the DWARF variant:

```sh
python3 build.py --preserve-dwarf
python3 run.py --quick --binary repro-with-dwarf.dll --variant another-fresh-dwarf-run
```

The ordinary candidate used project init/artifact; the clean DWARF candidate used the quick command. Both consume the complete DLL.

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
