# Issue 649: complete public PE export-root reproduction

Status: **reproduced_supported_path** on the pristine pinned official image.
The [reported issue](https://github.com/revng/revng/issues/649) is reproduced by independently authored C compiled into complete PE32 DLLs, processed by the normal whole-binary quick and project commands.
No model, prototype, function selector, proprietary DLL, source fragment, or private IR is supplied.
This package is prepared for root review; it has not been uploaded.

## Build and run

Use Clang/LLD 17.0.6 and Python 3 with PyYAML. Put the LLVM tools on PATH, or pass their directory explicitly:

```sh
python3 build.py --llvm-bin /path/to/llvm-17/bin
python3 run.py
```

`build.py` executes these ordinary whole-DLL compiler/linker commands:

```sh
clang --target=i686-pc-windows-msvc -O2 -ffreestanding -c src/export_matrix.c -o build/export_matrix.obj
lld-link /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X86 /OPT:NOICF /OPT:NOREF /timestamp:0 /DEF:src/export_matrix.def /OUT:build/export_matrix.dll build/export_matrix.obj
llvm-readobj --file-headers --sections --coff-exports build/export_matrix.dll
```

The normal supported invocations performed for every complete fixture are:

```sh
revng quick artifact emit-recompilable-archive fixture.dll -o quick.tar.gz
revng quick analyze fixture.dll -o quick-model.yml
mkdir project
cd project
revng project init ../fixture.dll
revng project artifact emit-recompilable-archive -o project.tar.gz
```

The runner mounts only this public package at `/case`, disables container networking, and pins:

```text
revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce
rev.ng source revision: ba8cdb0278084da07b2655f19d1e5f1ce2ebbbdf
```

`run.py` records the exact portable Docker argument vectors, actual image ID, process exit codes, complete stdout/stderr, automatic models and archives.
An additional producer must be explicitly classified with `--baseline-class`; it cannot silently be called pristine upstream.

## Source-complete export truth and observation

`export_matrix.dll` is a 2,560-byte complete i386 DLL with no entrypoint and no static COFF symbols.
Its source/DEF describe named code exports, two names at one code RVA, an ordinal-only code export, a data export, a forwarder, and unused zero-RVA ordinal slots.
`shared_ordinal_alias.dll` is an independently authored complete PE-table variant in which both alias names use the same EAT ordinal index.
`build.py` records that standard synthetic table edit; it never edits an automatic model.
`pe_exports.py` walks the entire EAT and all name/ordinal entries, including unnamed entries and aliases; it does not rely on a first-name-only iterator.

Both original DLLs contain **three unique executable export targets**.
Both quick and normal project pipelines return exit 0 but produce **zero model functions** and a **52-byte functions.c containing only the two includes**.
Thus successful pipeline exit does not establish exported API recovery.

| Complete fixture | Binary SHA-256 | Expected code roots | Quick / project model functions |
|---|---|---:|---:|
| export_matrix.dll | a7d30d11535ef8af6c25f6d4f85a0426acd2bbdd2c9d5f0a4786f6ee90fa910a | 3 | 0 / 0 |
| shared_ordinal_alias.dll | 4c9ae52a27491ac9f9682d410775f2d201c2c76c972575425d7055d022e64952 | 3 | 0 / 0 |

The pinned importer seeds the entrypoint/static symbol table and does not seed executable EAT entries.
The current public `develop` source snapshot at `f29ed6fa36c82d523813dd613865076c833e6f03` is retained in `provenance/`; the current-upstream source observation is separate from a current-upstream binary test.
No claim of current-upstream binary validation or original program equivalence is made here.

## Artifacts

`src/` contains the complete authored C and DEF.
`build/` contains both full DLLs, compiler/linker logs, full PE headers/sections/exports, source/export truth JSON, and the build receipt.
`results/pristine-pinned-upstream/` contains the exact ordinary commands, complete logs, automatic models, full archives, generated functions C, and classification receipt.
`provenance/` contains public issue/latest-comment snapshots and pinned public source snapshots. The fresh public API check found zero comments on this issue.

The package contains no private application data or workspace identifiers. Review is required before sharing it or updating the issue.
