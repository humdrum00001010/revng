# Public binary validation for issue 653

The complete, independently authored executable built from `source/program.c` reproduces the **post-inline GEP verifier failure** through the ordinary official binary-input CLI. It does **not yet establish the specific stale-policy lifecycle cause claimed in [issue 653](https://github.com/revng/revng/issues/653)**. That attribution requires evidence of the relevant helper's body and `revng.inline.policy` before and after flattening; a verifier abort alone does not supply it.

The fresh validation is recorded in [`runs/official-quick-20261006-public-cli/result.json`](runs/official-quick-20261006-public-cli/result.json). Compilation and linking both exit 0. The compiler emits genuine `ldmxcsr` and `stmxcsr` instructions, and the linked input is a complete static ELF32/i386 executable with entry point `_start`. Its SHA-256 is `39ade80af9a92f98ecfee7ffd50d34a71314de1ecd10f22e0abfd697fc03606c`, identical to the previously built public executable. Only this C executable is supplied to rev.ng; the helper-fixture sources and `build-supported-helper-case.sh` are not used by this validation.

The exact command inside the official container is:

```sh
revng quick artifact emit-recompilable-archive /inputs/program.elf -o recompilable.tar.gz
```

The native diagnostic in [`quick.stderr.log`](runs/official-quick-20261006-public-cli/quick.stderr.log) is:

```text
Abort at /builds/gitlab/revng/orchestra/orchestra/sources/revng/lib/InlineHelpers/PostInlineHelpersVerifyPass.cpp:28:

post-inline-helpers-verify: getelementptr in Isolated function local_0x4010e0_Code_x86
```

The backtrace shows `DeleteHelperBodiesPass::runOnModule` calling `PostInlineHelpersVerifyPass::runOnModule`. No recompilable archive is produced. After printing the native fatal assertion, the process remains running under Rosetta. The runner records its state and process command, waits five seconds after detecting the assertion, and stops only the newly created public probe. The Docker command and container exit **137** therefore record that observer stop; they are not the native assertion's natural exit code. [`quick-terminal-state.stdout.log`](runs/official-quick-20261006-public-cli/quick-terminal-state.stdout.log) records `OOMKilled: false`.

The validation uses the pinned official image `revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce`, with the stock helper bundle and stock pipeline. [`revng-system-info.stdout.log`](runs/official-quick-20261006-public-cli/revng-system-info.stdout.log) identifies rev.ng source revision `ba8cdb0278084da07b2655f19d1e5f1ce2ebbbdf` and Python 3.14.6. [`revng-opt-version.stdout.log`](runs/official-quick-20261006-public-cli/revng-opt-version.stdout.log) records LLVM 16.0.1, an optimized build with assertions. The host build uses Homebrew Clang 17.0.6 and LLD 17.0.6. Version commands, complete outputs, executable hashes, build commands, the Docker invocation, stop command and individual exit statuses are saved in [`commands.txt`](runs/official-quick-20261006-public-cli/commands.txt), the stage logs, and the JSON receipt.

From this directory, repeat the validation with a fresh automatically named results directory:

```sh
LLVM_BIN=/opt/homebrew/opt/llvm@17/bin python3 validate.py
```

The recorded invocation was `python3 validate.py --variant official-quick-20261006-public-cli`. `validate.py` refuses to overwrite an existing run directory. It builds the complete source with the following flags, then invokes the official quick command with the entire executable mounted read-only. The exact output paths used in the recorded run are retained in `commands.txt`.

```sh
/opt/homebrew/opt/llvm@17/bin/clang --target=i386-unknown-linux-gnu -msse -O1 -g \
  -ffreestanding -fno-stack-protector -fno-pic -fdebug-compilation-dir=/case \
  -c source/program.c -o runs/official-quick-20261006-public-cli/program.o
/opt/homebrew/opt/llvm@17/bin/ld.lld -m elf_i386 --entry=_start --build-id=none \
  -o runs/official-quick-20261006-public-cli/program.elf \
  runs/official-quick-20261006-public-cli/program.o
```

The container has networking disabled and is capped at one CPU and 4 GiB. The runner has a 300-second limit and retains the stopped container for inspection. It supplies no function selectors, modified model, policy metadata, synthetic LLVM/Clift input, or replacement helper modules. The original `produced/` and `observed/` evidence is preserved, and unrelated running containers are left running. Nothing is published.

For public review, host package paths in receipt arguments and binary-tool headers are normalized to `<PACKAGE>`, meaning this directory. The official in-container quick command, binary, generated archive and native failure diagnostics are unchanged. Unmodified local receipts remain separately preserved. Direct helper and IR fixtures are excluded from this principal evidence packet. `review-manifest.json` inventories and hashes every included evidence file.
