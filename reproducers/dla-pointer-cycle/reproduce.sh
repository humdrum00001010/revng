#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
image="${IMAGE:-revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce}"
output_dir="$(mktemp -d "${TMPDIR:-/tmp}/revng-dla-cycle.XXXXXX")"
log_file="$output_dir/revng.log"
container_name="revng-dla-cycle-$$"

cleanup() {
  docker rm -f "$container_name" >/dev/null 2>&1 || true
  rm -rf "$output_dir"
}
trap cleanup EXIT

set +e
docker run \
  --name "$container_name" \
  --platform linux/amd64 \
  --network none \
  --cpus 2 \
  --memory 1g \
  --memory-swap 1g \
  --pids-limit 4096 \
  --mount "type=bind,src=$script_dir/repro.dll,dst=/input/repro.dll,readonly" \
  --mount "type=bind,src=$output_dir,dst=/output" \
  --workdir /tmp \
  --entrypoint /bin/bash \
  "$image" \
  -lc 'revng quick artifact emit-recompilable-archive /input/repro.dll -o /output/recompilable.tar.gz -- --debug-log=verify > /output/revng.log 2>&1' \
  >/dev/null 2>&1
status=$?
set -e

if [[ "$status" -ne 134 ]] \
   || ! grep -Fq 'TS.verifyPointerDAG() and TS.verifyDAG() and TS.verifyUnions()' "$log_file"; then
  cat "$log_file" >&2
  printf 'unexpected result: exit %s\n' "$status" >&2
  exit 1
fi

sed -n '1,4p' "$log_file"
printf 'reproduced: rev.ng exited 134 at the pointer-DAG assertion\n'
