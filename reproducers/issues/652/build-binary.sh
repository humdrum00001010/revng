#!/usr/bin/env bash
set -euo pipefail
COMPILER=${COMPILER:-clang}
LINKER=${LINKER:-ld.lld}
mkdir -p produced
"$COMPILER" --version > produced/compiler-version.txt
"$LINKER" --version > produced/linker-version.txt
"$COMPILER" --target=i386-unknown-linux-gnu -msse -O1 -g -ffreestanding \
  -fno-stack-protector -fno-pic -fdebug-compilation-dir=/case -c source/program.c -o produced/program.o
"$LINKER" -m elf_i386 --entry=_start --build-id=none \
  -o produced/program.elf produced/program.o
