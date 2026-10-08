#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
image="${IMAGE:-revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce}"
mode="${1:-plain}"

case "$mode" in
  plain)
    memory_limit=256m
    revng_command='revng quick artifact emit-recompilable-archive /input/repro.dll -o /output/recompilable.tar.gz > /output/revng.log 2>&1'
    ;;
  verify)
    memory_limit=1g
    revng_command='revng quick artifact emit-recompilable-archive /input/repro.dll -o /output/recompilable.tar.gz -- --debug-log=verify > /output/revng.log 2>&1'
    ;;
  *)
    printf 'usage: %s [plain|verify]\n' "$0" >&2
    exit 2
    ;;
esac

output_dir="$(mktemp -d "${TMPDIR:-/tmp}/revng-dla-cycle.XXXXXX")"
log_file="$output_dir/revng.log"
docker_log="$output_dir/docker.log"
container_name="revng-dla-cycle-$mode-$$"

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
  --memory "$memory_limit" \
  --memory-swap "$memory_limit" \
  --pids-limit 4096 \
  --mount "type=bind,src=$script_dir/repro.dll,dst=/input/repro.dll,readonly" \
  --mount "type=bind,src=$output_dir,dst=/output" \
  --workdir /tmp \
  --entrypoint /bin/bash \
  "$image" \
  -lc "$revng_command" \
  >"$docker_log" 2>&1
status=$?
set -e

oom_killed="$(docker inspect --format '{{.State.OOMKilled}}' "$container_name")"

if [[ "$mode" == plain ]]; then
  if [[ "$status" -ne 137 || "$oom_killed" != true ]]; then
    cat "$docker_log" >&2
    [[ ! -s "$log_file" ]] || cat "$log_file" >&2
    printf 'unexpected plain result: exit=%s OOMKilled=%s\n' \
      "$status" "$oom_killed" >&2
    exit 1
  fi

  printf '%s\n' \
    'revng quick artifact emit-recompilable-archive repro.dll -o recompilable.tar.gz'
  printf 'reproduced: exit=137 OOMKilled=true\n'
else
  if [[ "$status" -ne 134 || "$oom_killed" != false ]] \
     || ! grep -Fq 'TS.verifyPointerDAG() and TS.verifyDAG() and TS.verifyUnions()' "$log_file"; then
    cat "$docker_log" >&2
    [[ ! -s "$log_file" ]] || cat "$log_file" >&2
    printf 'unexpected verify result: exit=%s OOMKilled=%s\n' \
      "$status" "$oom_killed" >&2
    exit 1
  fi

  sed -n '1,4p' "$log_file"
  printf 'reproduced: exit=134 at the pointer-DAG assertion\n'
fi
