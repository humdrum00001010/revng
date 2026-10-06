# Issue 648: public authored-C binary probes

Status: **not_reproduced** with the two complete authored binaries in this package.
The separately reported backend model-width loss does **not yet have a qualifying supported C-to-binary reproduction here**.
This result must not be replaced by the old direct DLA graph test's result.
The package is prepared for root review and has not been uploaded.

## Build and normal pipeline

Use Clang/LLD 17.0.6, Python 3 and PyYAML:

```sh
python3 build.py --llvm-bin /path/to/llvm-17/bin
python3 run.py
```

`src/pointer_scalar_views.c` is newly authored, self-contained C.
It overlays an eight-byte scalar view and four-byte pointer views whose targets are distinct four/eight-byte structs.
The full standalone i386 ELF executable has a genuine entrypoint and five authored functions; it needs no libc, supplied rev.ng model, custom prototype, direct graph or IR input.
The build produces raw and embedded-DWARF variants, with generic `/source` debug paths.

Representative complete binary build commands are:

```sh
clang --target=i386-linux-gnu -O1 -fno-inline -fno-strict-aliasing -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-asynchronous-unwind-tables -c src/pointer_scalar_views.c -o build/raw.o
ld.lld -m elf_i386 -e _start -o build/raw.elf build/raw.o
```

The debug variant adds ordinary compiler-produced DWARF (`-g -gdwarf-4`) and public debug path mapping, then uses the same complete link.
Exact commands and all compiler/disassembly output are in `build/`.
Both complete binaries are passed to the standard default whole-binary path:

```sh
revng quick artifact emit-recompilable-archive fixture.elf -o quick.tar.gz
revng quick analyze fixture.elf -o quick-model.yml
```

No analysis lists, injected LLVM, supplied model, target selection or prototype override is used.
The pristine pinned official image is:

```text
revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce
rev.ng source revision: ba8cdb0278084da07b2655f19d1e5f1ce2ebbbdf
```

## Observation and limits

Both raw and debug ordinary quick pipelines complete with exit 0 and recover all five function entries.
The scalar storage view remains a generic 64-bit field.
The debug model retains the authored `Small` four-byte and `Wide` eight-byte types and their eight-byte `Target` union.
It does not demonstrate the claimed overlapping model-owned pointer-union shape collapsing from eight bytes to four bytes.
The distinct model-owned aggregate pointee condition required by the previously posted direct graph has not been established for these binary outputs.

| Complete authored binary | SHA-256 | Supported-path result |
|---|---|---|
| raw.elf | c030c6b89ad838f979060159e6d99910f1291280979841ae9868d916edf466ae | completes; target width loss not reproduced |
| debug.elf | 3a38a477730b1d7314d764e736c60494b7588ac4ecbd93df091aaaf07f638b28 | completes; target width loss not reproduced |

These results establish neither original-program semantic equivalence nor absence of the reported bug for every possible binary.
They establish that the clean supported probes included here do not reproduce it.

## Existing upstream fix versus the new width-change claim

[b482edd3d3b77e134bf21979c36f59e0a7502c95](https://github.com/revng/revng/commit/b482edd3d3b77e134bf21979c36f59e0a7502c95) fixes the older large-scalar pointer assertion.
[c634c39387932bc9f0267c0455c737cf281214c5](https://github.com/revng/revng/commit/c634c39387932bc9f0267c0455c737cf281214c5) fixes model-owned node initialization.
Those public changes are separate from issue 648's proposed retention of an additional wide scalar union member during model conversion.
The existing direct graph can exercise that separate backend claim, but custom graph/model construction is not a supported binary reproduction and is not included as qualifying evidence in this package.
The authored binaries here do not isolate an observable binary-path difference between those upstream changes and the new width change; no such A/B success claim is made.

Current public source snapshots at `f29ed6fa36c82d523813dd613865076c833e6f03` are retained under `provenance/`.
A current source snapshot is not a pristine current-upstream binary result.
The fresh public API check found zero comments on issue 648.

`build/` retains complete binaries, full symbols/disassembly, exact build commands and hashes.
`results/pristine-pinned-upstream/` retains complete supported-path logs, generated archives/C/model, every automatic type definition, and explicit result/condition classifications.
There are no proprietary source/binaries, private IR/fragments, or private workspace identifiers. Root review is required before sharing or updating the issue.
