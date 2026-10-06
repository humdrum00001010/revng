#!/bin/sh
set -eu
cd "$(dirname "$0")"
: "${FIXTURE_CLANG:=clang}"
: "${FIXTURE_LINK:=lld-link}"
"$FIXTURE_CLANG" --target=i686-pc-windows-msvc -O2 -ffreestanding \
  -c public-test.c -o public-test.obj
# Preserve the public fixture's original PE timestamp and export-directory name.
"$FIXTURE_LINK" /DLL /NOENTRY /MACHINE:X86 /NODEFAULTLIB \
  /TIMESTAMP:1791238794 /OUT:stack.dll public-test.obj
mv stack.dll public-test.dll
