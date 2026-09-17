# Legacy phase snapshots

This directory preserves the eight supplied Phase 1–4 client/server files as historical coursework artifacts (with only whitespace normalization). They are **not** part of the build and are not the implementation described by the main README.

| Phase | Historical milestone |
| --- | --- |
| 1 | Single client receives one server-selected file. |
| 2 | Adds a `get` request and iterative server loop. |
| 3 | Adds a fixed-array `poll(2)` prototype for GET requests. |
| 4 | Adds prototype `get` and `put` commands. |

The snapshots retain their original behavior. They do not provide safe TCP framing, partial-write handling, streaming large files, path isolation, or non-blocking transfer states; on this macOS toolchain, their server variants also need a `::bind` qualification to compile. The canonical `src/` implementation corrects those issues while preserving the project’s Phase 4 intent.
