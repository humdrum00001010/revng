#!/bin/bash
set -euo pipefail
source /revng/environment
ulimit -c 0
case "${1:-}" in
  natural)
    exec timeout --signal=TERM --kill-after=5 90 \
      revng quick artifact emit-recompilable-archive \
      /fixture/public-test.dll -o /work/recompilable.tar.gz
    ;;
  controlled)
    exec timeout --signal=TERM --kill-after=5 90 \
      env LD_PRELOAD=/fixture/occupy-low-page.so \
      /revng/root/bin/python /revng/root/bin/revng \
      quick artifact emit-recompilable-archive \
      /fixture/public-test.dll -o /work/recompilable.tar.gz
    ;;
  *)
    echo 'usage: run-case.sh natural|controlled' >&2
    exit 2
    ;;
esac
