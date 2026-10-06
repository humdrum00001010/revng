# Issue 652: public normal-command validation

**Not reproduced by this complete public binary.** The independently authored `source/program.c` is compiled and linked into a static ELF32/i386 executable with entry point `_start`. The pinned official tool's normal quick command exits **0**, creates `recompilable.tar.gz`, and emits no `PostInlineHelpersVerifyPass.cpp:28` or post-inline GEP diagnostic. This classification applies to this binary candidate only.

The candidate has a concrete limitation: Clang folds `public_values[public_selector]` to a constant store of `9`, as shown in [`binary-disassembly.stdout.log`](runs/official-quick-20261006-public-cli/binary-disassembly.stdout.log). The binary therefore does not preserve the source lookup operation. Guest-program constants also do not establish that rev.ng's own helper globals undergo the initializer-removal lifecycle described in [issue 652](https://github.com/revng/revng/issues/652). This successful run does not validate that helper-splitting contract.

The exact command inside the container is:

```sh
revng quick artifact emit-recompilable-archive /inputs/program.elf -o recompilable.tar.gz
```

[`runs/official-quick-20261006-public-cli/result.json`](runs/official-quick-20261006-public-cli/result.json) records every command and exit status, source/binary/archive SHA-256 hashes, and the actual Docker terminal state (`ExitCode: 0`, `OOMKilled: false`). Compilation and linking both exit 0. [`commands.txt`](runs/official-quick-20261006-public-cli/commands.txt) preserves the exact build commands and Docker invocation; separate stdout/stderr logs preserve their complete output. The generated archive is retained under [`project/`](runs/official-quick-20261006-public-cli/project/).

The build uses Homebrew Clang 17.0.6 and LLD 17.0.6. The official image is `revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce`; `revng system-info` records source revision `ba8cdb0278084da07b2655f19d1e5f1ce2ebbbdf` and Python 3.14.6, and `revng opt --version` records LLVM 16.0.1 with assertions enabled. Their commands, outputs and executable hashes are retained with the receipt.

To repeat from this directory in a fresh results directory:

```sh
LLVM_BIN=/opt/homebrew/opt/llvm@17/bin python3 validate.py
```

The recorded invocation was `python3 validate.py --variant official-quick-20261006-public-cli`. The runner refuses to overwrite existing run directories. It builds the complete C source with `--target=i386-unknown-linux-gnu -msse -O1 -g -ffreestanding -fno-stack-protector -fno-pic -fdebug-compilation-dir=/case`, then links with `ld.lld -m elf_i386 --entry=_start --build-id=none`. The stock official container runs with networking disabled, one CPU and 4 GiB, and receives only the entire executable mounted read-only. No function selectors, model edits, IR/Clift inputs, supplied policy metadata, replacement helper modules, or helper-fixture scripts are used. Prior files are preserved, unrelated jobs remain running, and nothing is published.

For public review, host package paths in receipt arguments and binary-tool headers are normalized to `<PACKAGE>`, meaning this directory. The official in-container quick command, binary, generated archive and native failure diagnostics are unchanged. Unmodified local receipts remain separately preserved. Direct helper and IR fixtures are excluded from this principal evidence packet. `review-manifest.json` inventories and hashes every included evidence file.
