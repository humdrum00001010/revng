The authored three-export PE32 DLL exercises the normal whole-binary quick
artifact path. The failure depends on the host process's address layout: the
binary alone does not trigger it in the recorded natural run.

`occupy-low-page.c` is a Linux C environment control. Its shared-library
constructor reserves one anonymous `PROT_NONE` page at `0x8000` with
`MAP_FIXED_NOREPLACE`, verifies that `0x9000` is still available, and releases the
second page. It neither interposes an allocator nor calls or edits QEMU/rev.ng
state. Loading it into the unmodified CLI process preserves the first
reservation. Exit 125 means that the required layout could not be established.

The ordinary CLI command is:

```sh
revng quick artifact emit-recompilable-archive \
  public-test.dll -o recompilable.tar.gz
```

Build the DLL with LLVM 17, and build the control on Linux:

```sh
FIXTURE_CLANG=clang FIXTURE_LINK=lld-link ./build-public-dll.sh
CONTROL_CC=cc ./build-control.sh
```

The DLL build preserves the historical public fixture's PE timestamp and
export-directory name so its bytes can be compared with the prior proof. The
source and DLL contain three authored functions, no imports, no entry point,
and no fragments from an application binary.

In a fresh writable directory, after loading the image's `/revng/environment`,
the controlled invocation uses the authentic CLI entry point:

```sh
LD_PRELOAD=/fixture/occupy-low-page.so \
  /revng/root/bin/python /revng/root/bin/revng \
  quick artifact emit-recompilable-archive \
  /fixture/public-test.dll -o /work/recompilable.tar.gz
```

The official baseline is
`revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce`.
The recorded host is macOS 26.1 arm64, Docker Desktop with a Linux aarch64 daemon
and amd64/Rosetta processes. `profiles.json` distinguishes this shipped
baseline from locally rebuilt baseline/fixed comparisons.

The fresh C-controlled check in `self-check.json` builds the control with the
official image, then runs the exact quick artifact command. The official
baseline produces a complete archive naturally and fails with the control at
`linux-user/i386/cpu_loop.c:453`, `env->idt.base != -1`. A matching public-source
fixed image produces complete archives in both layouts. The input DLL remains
byte-identical in every run. The controlled baseline exits 134; all three
successful runs exit 0 and include `decompiled/functions.c`.

Run `python3 self-check.py` to compile the C control and test the official image
in two fresh projects. Pass `--fixed-image IMAGE` for the optional fixed
comparison. Each job is offline, sequential, limited to one CPU and 2 GiB, and
has a 90-second observation cap. The script does not manage existing containers
or start Docker. It records sanitized commands, hashes, public logs and actual
results in `self-check.json` and `logs/`. `checks.json` records the current
verification boundary.

Issue: https://github.com/revng/revng/issues/654

Status: **reproduced through the supported quick command with an explicit C
environment control**. The binary alone is not claimed to fail. This package
does not identify the layout or cause of an unrelated application crash. No
publication is authorized by this package.
