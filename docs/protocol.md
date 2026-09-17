# Wire protocol

The service uses one request and one transfer per TCP connection. Control records are UTF-8-compatible ASCII lines ending in `\n`; file data is opaque binary data.

## Download

```text
Client → Server: GET <filename>\n
Server → Client: OK <byte-count>\n
Server → Client: <exactly byte-count binary bytes>
Server → Client: connection close
```

`<filename>` must be a flat name of up to 255 characters composed of letters, digits, `.`, `_`, and `-`.

## Upload

```text
Client → Server: PUT <filename> <byte-count>\n
Server → Client: READY\n
Client → Server: <exactly byte-count binary bytes>
Server → Client: OK <byte-count>\n
Server → Client: connection close
```

The client waits for `READY` before sending file bytes. This handshake is important: TCP is a byte stream, so separate `send` calls are not preserved as separate messages. The byte count tells the receiver exactly where an upload ends.

## Errors

The server may return:

```text
ERR <reason>\n
```

Current reasons include `bad-request`, `unsafe-filename`, `not-found`, `not-readable`, `invalid-size`, `storage-failure`, and `protocol-error`.

## Transfer behavior

- The server uses 64 KiB transfer buffers and handles short socket writes.
- File counts are represented as unsigned 64-bit values.
- Uploads write to a temporary file in the storage root, then rename it after the declared byte count is received.
- Downloads and successful transfers close their connection after the response; clients do not pipeline multiple requests on one socket.
- A completed upload replaces an existing remote file. Concurrent uploads to the same name have last-completed-upload-wins semantics.
