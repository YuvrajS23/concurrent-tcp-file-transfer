# Concurrent TCP File Transfer Service

A C++17, TCP-based file-transfer service with a polling event loop on the server. It supports streamed uploads and downloads over a small, documented application protocol.

> This is an FTP-inspired educational project, **not** an implementation of RFC 959 FTP. It is designed for local development and trusted networks only.

## What it demonstrates

- Bidirectional file transfer: the client provides `put` and `get` commands.
- A single server event loop using `poll(2)` and non-blocking sockets to manage multiple active clients.
- Per-connection state machines, so a slow upload or download does not block unrelated connections.
- Fixed 64 KiB streaming buffers, 64-bit byte counts, and partial-read/partial-write handling; files are never loaded wholly into memory.
- A request/response protocol with explicit sizes and error responses.
- A storage-root boundary and flat, validated remote file names to reject path traversal.

The original coursework snapshots are retained under [`legacy/`](legacy/README.md). The runnable implementation is the rewritten code in [`src/`](src/), not the legacy prototypes.

## Architecture

```text
tcp_file_client ── TCP ──> tcp_file_server ──> storage root
                              │
                              └─ poll(2) + non-blocking, per-client state machines
```

The listener accepts connections without blocking. Each client moves through request parsing, response-header delivery, streamed upload/download, and final acknowledgement states. The server returns to `poll(2)` between file chunks, allowing other ready sockets to make progress. See [`docs/architecture.md`](docs/architecture.md) for the state flow.

## Requirements

- macOS or Linux (POSIX sockets and `poll(2)`)
- A C++17 compiler such as Apple Clang, Clang, or GCC
- `make` for the simplest local build
- CMake 3.20+ only when using the optional CMake build

## Build

```sh
git clone https://github.com/YuvrajS23/concurrent-tcp-file-transfer.git
cd concurrent-tcp-file-transfer
make
```

This creates:

```text
build/tcp_file_server
build/tcp_file_client
```

If CMake is installed, this equivalent flow is also supported:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Run locally

Use two terminals. The server binds to `127.0.0.1` by default, which keeps it local to the machine.

Terminal 1:

```sh
mkdir -p demo/server-root demo/client-data
./build/tcp_file_server --port 9000 --root ./demo/server-root
```

Terminal 2, upload a file:

```sh
printf 'hello over TCP\n' > demo/client-data/hello.txt
./build/tcp_file_client put \
  --host 127.0.0.1 --port 9000 \
  --local demo/client-data/hello.txt --remote hello.txt
```

Download it under a new local name:

```sh
./build/tcp_file_client get \
  --host 127.0.0.1 --port 9000 \
  --remote hello.txt --local demo/client-data/hello-downloaded.txt

cmp demo/client-data/hello.txt demo/client-data/hello-downloaded.txt
```

For a checksum-based manual check on macOS:

```sh
shasum -a 256 demo/client-data/hello.txt demo/client-data/hello-downloaded.txt
```

### Options

```text
tcp_file_server --port <0-65535> --root <storage-directory> [--bind <address>]

tcp_file_client get --host <host> --port <1-65535> \
  --remote <filename> --local <path> [--delay-ms <0-60000>]

tcp_file_client put --host <host> --port <1-65535> \
  --local <path> --remote <filename> [--delay-ms <0-60000>]
```

`--port 0` asks the server to select an ephemeral port and prints the chosen value. `--delay-ms` intentionally slows each client chunk; it is useful for demonstrating that another client can continue transferring while one client is slow.

Remote names are deliberately limited to a single, flat filename containing letters, numbers, `.`, `_`, or `-`. This prevents `../` traversal and keeps every transfer inside the server root.

A completed `put` atomically replaces an existing remote file. If clients deliberately upload to the same remote name at the same time, the last completed upload wins; use distinct names when concurrent writers must not replace one another.

## Protocol

The protocol is line-framed for control messages and size-delimited for binary file data:

| Operation | Exchange |
| --- | --- |
| Download | Client: `GET <name>\n` → Server: `OK <bytes>\n` followed by exactly `<bytes>` raw file bytes |
| Upload | Client: `PUT <name> <bytes>\n` → Server: `READY\n` → Client: exactly `<bytes>` raw bytes → Server: `OK <bytes>\n` |
| Error | Server: `ERR <reason>\n` |

The explicit `READY` response prevents a command and upload payload from being mistaken for one TCP message. The full protocol notes are in [`docs/protocol.md`](docs/protocol.md).

## Test

```sh
make test
```

The integration test starts the server on a temporary loopback port and verifies:

- parallel small and multi-megabyte uploads;
- a slow download running alongside an upload;
- byte-for-byte upload/download comparisons;
- zero-byte files;
- missing-file and unsafe-filename failures.

The test uses the standard `nc`/netcat utility to send one raw unsafe request, confirming that the server—not only the CLI—rejects traversal attempts.

## Project layout

```text
.
├── src/                    # Canonical, runnable client and server
├── tests/                  # Local TCP integration test
├── docs/                   # Protocol and event-loop notes
├── legacy/                 # Original Phase 1–4 coursework snapshots (not built)
├── Makefile                # No-dependency build and test commands
└── CMakeLists.txt          # Optional CMake build
```

## Security and scope

This project intentionally omits TLS, authentication, authorization, checksums, resumable transfers, quotas, and production hardening. Do not expose it to the public Internet or use it to transfer sensitive data. `--bind 0.0.0.0` should be used only on a trusted network. The server root must also be controlled by trusted local users; protecting against malicious local filesystem races is outside this compact teaching example’s scope.

No license has been selected for this repository yet. Add one only if you own or are authorized to license all included source material.
