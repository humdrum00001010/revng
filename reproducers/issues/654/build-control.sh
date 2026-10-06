#!/bin/sh
set -eu
cd "$(dirname "$0")"
: "${CONTROL_CC:=cc}"
"$CONTROL_CC" -std=c11 -O2 -fPIC -shared -Wall -Wextra -Werror \
  -nostartfiles -nodefaultlibs -Wl,--build-id=none -Wl,-z,defs \
  occupy-low-page.c -lc -o occupy-low-page.so
