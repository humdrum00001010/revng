# Issue 646: intended undefined-value behavior

[Upstream issue](https://github.com/revng/revng/issues/646) is **closed as intended design**. [Maintainer resolution](https://github.com/revng/revng/issues/646#issuecomment-6011579959): undefined values can reach the emitted C; the consumer supplies `undef_value` semantics appropriate to its application.

The independent complete C DLL uses ordinary inline assembly with an unspecified output register and a hardware division flag that the architecture leaves undefined. The official complete-binary project pipeline exits 0; complete generated C compiles to a COFF object with exit 0. `llvm-nm --undefined-only whole.obj` reports:

```text
         U _undef_value
```

**The symbol pattern is reproduced; this does not establish an unfixed bug.** No generated runtime provider is supplied here, since doing so would choose semantics. The retained supported-path evidence is `runs/official-1446/result.json` and `whole-undefined-symbols.txt`. The quick command below is an equivalent reproduction recipe, not an additional recorded quick run.

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
