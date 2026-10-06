#!/usr/bin/env bash
set -euo pipefail
REVNG=${REVNG:-revng}
mkdir -p observed
"$REVNG" quick artifact emit-recompilable-archive produced/program.elf \
  -o observed/recompilable-archive.tar > observed/quick.stdout.log \
  2> observed/quick.stderr.log
