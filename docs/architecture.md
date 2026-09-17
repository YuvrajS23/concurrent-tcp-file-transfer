# Server architecture

`tcp_file_server` uses a single `poll(2)` loop. The listening socket and every accepted client socket are non-blocking, so the event loop does not wait on a slow peer.

## Per-client states

```text
AwaitingRequest
  ├─ GET → SendingGetHeader → SendingFile → close
  └─ PUT → SendingReady → ReceivingFile → SendingResult → close

Any state → SendingError → close
```

`poll(2)` requests `POLLIN` while a client needs to provide a request or upload bytes, and `POLLOUT` while the server has a response or download chunk ready. A writable download event advances at most one buffered socket write before the loop returns to polling; this gives other ready clients a chance to progress.

## Storage model

The server creates the configured root if necessary. It accepts only flat, safe remote names, so no request can address a parent directory or an arbitrary absolute path. GET requests reject symbolic links and non-regular files. PUT requests are written to a temporary file within the root and renamed only after the expected byte count arrives.

## Lifecycle and limits

- Default bind address: `127.0.0.1`
- Maximum request line: 4 KiB
- Transfer chunk: 64 KiB
- Maximum concurrently tracked clients: 256
- Idle connection timeout: 60 seconds
- Maximum accepted upload declaration: 16 GiB

The service is intentionally a compact teaching example, not a hardened file-storage system. See the security section of the main README before using it outside a local trusted environment.
