#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <server-binary> <client-binary>" >&2
  exit 2
fi

server_bin=$1
client_bin=$2
if ! command -v nc >/dev/null 2>&1; then
  echo "netcat (nc) is required for the raw protocol validation" >&2
  exit 2
fi
test_root=$(mktemp -d "${TMPDIR:-/tmp}/tcp-file-transfer.XXXXXX")
server_pid=""

cleanup() {
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

server_root="$test_root/server-root"
client_root="$test_root/client-root"
mkdir -p "$server_root" "$client_root"

printf 'A small upload that is checked byte-for-byte.\n' > "$client_root/small.txt"
: > "$client_root/empty.txt"
dd if=/dev/urandom of="$client_root/large.bin" bs=65536 count=64 2>/dev/null

"$server_bin" --port 0 --root "$server_root" > "$test_root/server.log" 2>&1 &
server_pid=$!

port=""
for _ in {1..100}; do
  port=$(sed -n 's/^Listening on .*:\([0-9][0-9]*\) with storage root .*$/\1/p' "$test_root/server.log" | head -n 1)
  if [[ -n "$port" ]]; then
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$test_root/server.log" >&2
    echo "server exited before becoming ready" >&2
    exit 1
  fi
  sleep 0.05
done

if [[ -z "$port" ]]; then
  cat "$test_root/server.log" >&2
  echo "server did not report its ephemeral port" >&2
  exit 1
fi

"$client_bin" put --host 127.0.0.1 --port "$port" --local "$client_root/small.txt" --remote small.txt &
upload_small_pid=$!
"$client_bin" put --host 127.0.0.1 --port "$port" --local "$client_root/large.bin" --remote large.bin &
upload_large_pid=$!
wait "$upload_small_pid"
wait "$upload_large_pid"
cmp "$client_root/small.txt" "$server_root/small.txt"
cmp "$client_root/large.bin" "$server_root/large.bin"

"$client_bin" get --host 127.0.0.1 --port "$port" --remote large.bin --local "$client_root/slow-copy.bin" --delay-ms 10 &
slow_download_pid=$!
sleep 0.1
"$client_bin" put --host 127.0.0.1 --port "$port" --local "$client_root/small.txt" --remote parallel.txt
wait "$slow_download_pid"
cmp "$client_root/large.bin" "$client_root/slow-copy.bin"
cmp "$client_root/small.txt" "$server_root/parallel.txt"

"$client_bin" put --host 127.0.0.1 --port "$port" --local "$client_root/empty.txt" --remote empty.txt
"$client_bin" get --host 127.0.0.1 --port "$port" --remote empty.txt --local "$client_root/empty-copy.txt"
cmp "$client_root/empty.txt" "$client_root/empty-copy.txt"

if "$client_bin" get --host 127.0.0.1 --port "$port" --remote missing.txt --local "$client_root/missing.txt"; then
  echo "missing-file request unexpectedly succeeded" >&2
  exit 1
fi

unsafe_response=$(printf 'GET ../outside\n' | nc 127.0.0.1 "$port")
if [[ "$unsafe_response" != "ERR unsafe-filename" ]]; then
  echo "server did not reject an unsafe filename: $unsafe_response" >&2
  exit 1
fi

echo "integration test passed"
