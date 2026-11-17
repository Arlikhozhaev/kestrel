#!/usr/bin/env bash
# Open-loop wrk2 harness. -R is the offered rate. This script does not invent a latency.
# kestreld pins each reactor to its own physical core. Do not taskset the process.
# The bundled mock is a stand-in. Set UPSTREAM=IPv4:PORT to use a real model server.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KESTRELD=${KESTRELD:-"$ROOT/build/release/kestreld"}
WRK2=${WRK2:-wrk2}
PYTHON=${PYTHON:-python3}
BIND=${BIND:-127.0.0.1}
PORT=${PORT:-18081}
MOCK_PORT=${MOCK_PORT:-18080}
CORES=${CORES:-1}
DURATION_SEC=${DURATION:-15}
THREADS=${THREADS:-2}
CONNECTIONS=${CONNECTIONS:-64}
POOL=${POOL:-32}
DELAY_MS=${UPSTREAM_DELAY_MS:-0}
RATES=${RATES:-"2000 5000 10000"}
PAYLOADS=${PAYLOADS:-"128 1024"}
HIT_RATIOS=${HIT_RATIOS:-"0 1"}
UPSTREAM=${UPSTREAM:-}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT=${OUT:-"$ROOT/bench/out/$STAMP"}
CLK_TCK=$(getconf CLK_TCK)

MOCK_PID=""
KESTREL_PID=""
FAILED=0
UNIQ_BASE=1000000

usage() {
  echo "usage: bench/run_benchmark.sh" >&2
  echo "  KESTRELD, WRK2, UPSTREAM, DURATION, RATES, PAYLOADS, HIT_RATIOS, CORES, CONNECTIONS" >&2
}

if [[ "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -ne 0 ]]; then
  usage
  exit 2
fi

cleanup() {
  set +e
  if [[ -n "$KESTREL_PID" ]]; then
    kill "$KESTREL_PID" 2>/dev/null
    wait "$KESTREL_PID" 2>/dev/null
  fi
  if [[ -n "$MOCK_PID" ]]; then
    kill "$MOCK_PID" 2>/dev/null
    wait "$MOCK_PID" 2>/dev/null
  fi
  if [[ -n "${WAL:-}" && -d "$WAL" ]]; then
    rm -rf "$WAL"
  fi
}
trap cleanup EXIT

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing $1" >&2
    exit 1
  fi
}

need "$PYTHON"
need curl
if [[ "$WRK2" == */* ]]; then
  if [[ ! -x "$WRK2" ]]; then
    echo "wrk2 is not executable: $WRK2" >&2
    exit 1
  fi
elif ! command -v "$WRK2" >/dev/null 2>&1; then
  echo "wrk2 is required. The closed-loop wrk binary is not a substitute." >&2
  exit 1
fi
if [[ ! -x "$KESTRELD" ]]; then
  echo "kestreld is not built: $KESTRELD" >&2
  exit 1
fi
if [[ "$CONNECTIONS" -gt 128 ]]; then
  echo "CONNECTIONS=$CONNECTIONS exceeds kestreld's default connection cap of 128" >&2
  exit 1
fi

case "$DURATION_SEC" in
  ''|*[!0-9]*)
    echo "DURATION must be a whole number of seconds" >&2
    exit 2
    ;;
esac

mkdir -p "$OUT"
WAL=$(mktemp -d "${TMPDIR:-/tmp}/kestrel-bench.XXXXXX")
MANIFEST="$OUT/manifest.tsv"
: >"$MANIFEST"

wait_http() {
  local url=$1
  local i
  for i in $(seq 1 100); do
    if curl -sf --max-time 1 "$url" >/dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "timed out waiting for $url" >&2
  return 1
}

cpu_seconds() {
  local pid=$1
  if [[ -z "$pid" || ! -r "/proc/$pid/stat" ]]; then
    echo ""
    return 0
  fi
  awk -v hz="$CLK_TCK" '{printf "%.6f", ($14 + $15) / hz}' "/proc/$pid/stat"
}

cpu_delta() {
  local before=$1
  local after=$2
  if [[ -z "$before" || -z "$after" ]]; then
    echo ""
    return 0
  fi
  awk -v a="$before" -v b="$after" 'BEGIN { d = b - a; if (d < 0) d = 0; printf "%.6f", d }'
}

start_mock() {
  if [[ -n "$UPSTREAM" ]]; then
    return 0
  fi
  "$PYTHON" "$ROOT/bench/mock_upstream.py" --bind "$BIND" --port "$MOCK_PORT" --delay-ms "$DELAY_MS" \
    >"$OUT/mock.log" 2>&1 &
  MOCK_PID=$!
  wait_http "http://${BIND}:${MOCK_PORT}/health" || {
    echo "mock failed to listen; log follows" >&2
    cat "$OUT/mock.log" >&2 || true
    exit 1
  }
  if ! grep -q "mock listening" "$OUT/mock.log"; then
    echo "mock log has no listen line" >&2
    exit 1
  fi
}

upstream_addr() {
  if [[ -n "$UPSTREAM" ]]; then
    echo "$UPSTREAM"
  else
    echo "${BIND}:${MOCK_PORT}"
  fi
}

start_kestrel() {
  local upstream
  upstream=$(upstream_addr)
  "$KESTRELD" --bind "$BIND" --port "$PORT" --cores "$CORES" --wal "$WAL" --upstream "$upstream" \
    >"$OUT/kestrel.log" 2>&1 &
  KESTREL_PID=$!
  wait_http "http://${BIND}:${PORT}/health" || {
    echo "kestreld failed to listen; log follows" >&2
    cat "$OUT/kestrel.log" >&2 || true
    exit 1
  }
  if ! grep -q "listening" "$OUT/kestrel.log"; then
    echo "kestrel log has no listen line" >&2
    exit 1
  fi
}

warmup() {
  local payload=$1
  local ratio=$2
  local last id body
  awk -v r="$ratio" 'BEGIN { exit !(r <= 0) }' && return 0
  if awk -v r="$ratio" 'BEGIN { exit !(r >= 1) }'; then
    last=0
  else
    last=$((POOL - 1))
  fi
  for id in $(seq 0 "$last"); do
    body=$(mktemp)
    "$PYTHON" "$ROOT/bench/prompt.py" --id "$id" --bytes "$payload" >"$body"
    curl -sf --max-time 5 -o /dev/null \
      -H "Content-Type: application/json" \
      --data-binary "@${body}" \
      "http://${BIND}:${PORT}/v1/chat/completions" || {
      rm -f "$body"
      echo "warmup request failed" >&2
      return 1
    }
    rm -f "$body"
  done
}

record() {
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$MANIFEST"
}

run_wrk() {
  local url=$1
  local rate=$2
  local payload=$3
  local ratio=$4
  local name=$5
  local log="$OUT/${name}.wrk"
  local before_cpu_k after_cpu_k before_cpu_u after_cpu_u
  local metrics_before="" metrics_after="" note=""
  local cpu_k="" cpu_u=""

  if [[ "$url" == *":${PORT}" || "$url" == *":${PORT}/"* ]]; then
    warmup "$payload" "$ratio"
    sleep 0.2
    metrics_before="$OUT/${name}.metrics.before"
    curl -sf --max-time 5 "http://${BIND}:${PORT}/metrics" >"$metrics_before"
  fi

  before_cpu_k=$(cpu_seconds "$KESTREL_PID")
  before_cpu_u=$(cpu_seconds "$MOCK_PID")
  export KESTREL_HIT_RATIO="$ratio"
  export KESTREL_PAYLOAD="$payload"
  export KESTREL_POOL="$POOL"
  export KESTREL_UNIQ_BASE="$UNIQ_BASE"
  set +e
  "$WRK2" -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION_SEC}s" -R"$rate" --latency \
    -s "$ROOT/bench/lua/chat_completion.lua" \
    "$url" >"$log" 2>&1
  local status=$?
  set -e
  UNIQ_BASE=$((UNIQ_BASE + rate * DURATION_SEC + 100000))
  after_cpu_k=$(cpu_seconds "$KESTREL_PID")
  after_cpu_u=$(cpu_seconds "$MOCK_PID")
  cpu_k=$(cpu_delta "$before_cpu_k" "$after_cpu_k")
  cpu_u=$(cpu_delta "$before_cpu_u" "$after_cpu_u")
  if [[ "$status" -ne 0 ]]; then
    note="wrk2 exit ${status}"
    FAILED=1
  fi
  if [[ -n "$metrics_before" ]]; then
    sleep 0.2
    metrics_after="$OUT/${name}.metrics.after"
    curl -sf --max-time 5 "http://${BIND}:${PORT}/metrics" >"$metrics_after"
  fi
  local shown_hit=$ratio
  if [[ "$name" == direct_* ]]; then
    shown_hit="-"
  fi
  record "$name" "$rate" "$payload" "$shown_hit" "$log" "$metrics_before" "$metrics_after" "$cpu_k" "$cpu_u" "$note"
}

start_mock

UP=$(upstream_addr)
for rate in $RATES; do
  for payload in $PAYLOADS; do
    run_wrk "http://${UP}/" "$rate" "$payload" "0" "direct_R${rate}_P${payload}"
  done
done

start_kestrel
for rate in $RATES; do
  for payload in $PAYLOADS; do
    for ratio in $HIT_RATIOS; do
      tag=${ratio//./p}
      run_wrk "http://${BIND}:${PORT}/" "$rate" "$payload" "$ratio" "kestrel_R${rate}_P${payload}_H${tag}"
    done
  done
done

"$PYTHON" "$ROOT/bench/scrape_metrics.py" --manifest "$MANIFEST" --out "$OUT/report.md"
echo "wrote $OUT/report.md"
exit "$FAILED"
