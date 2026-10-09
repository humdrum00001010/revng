#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
clang_bin="${CLANG:-clang}"
linker_bin="${MINGW_LD:-i686-w64-mingw32-ld}"
object_file="$(mktemp "${TMPDIR:-/tmp}/revng-dla-cycle.XXXXXX.obj")"
trap 'rm -f "$object_file"' EXIT

export LC_ALL=C
export SOURCE_DATE_EPOCH=0

"$clang_bin" \
  --target=i686-w64-windows-gnu \
  -std=c11 \
  -ffreestanding \
  -fno-builtin \
  -fno-stack-protector \
  -O0 \
  -c "$script_dir/repro.c" \
  -o "$object_file"

"$linker_bin" \
  -shared \
  -e _pointer_cycle \
  --subsystem windows \
  --export-all-symbols \
  --no-insert-timestamp \
  -o "$script_dir/repro.dll" \
  "$object_file"

chmod 0644 "$script_dir/repro.dll"
