# SpaceGuard project guidance

## Project overview

SpaceGuard is a Qt/C++ desktop tool for explaining why free space decreases on large filesystems. It captures a
filesystem-tree snapshot, later scans the same root, compares the two trees, and reports significant positive size
changes without repeating the same growth at every ancestor.

The product-specific code is currently small and lives in `app/src/`:

- `snapshot.{h,cpp}` owns the filesystem tree, recursive scan, comparison, and snapshot serialization.
- `mainwindow.{h,cpp,ui}` owns the current manual save/load/compare workflow and result table.
- Snapshot files use the `.spaceguard` extension and are serialized with `QDataStream` plus `qCompress`.

The current implementation is a prototype. Important planned improvements are trustworthy scan-error reporting,
off-main-thread multithreaded scanning, explicit link/reparse handling, volume free-space measurements, allocated-size
accounting, and progress/cancellation. Performance and memory consumption should be kept in mind but are not primary
design drivers. Snapshot format versioning is not currently a priority.

Windows is the primary platform. macOS and Linux support are desired.

## Project structure

The repository is a qmake `TEMPLATE = subdirs` project using modern C++ and Qt Widgets. `SpaceGuard.pro` is the root
project and `app/app.pro` builds the application.

Top-level components:

- `app/`: SpaceGuard executable and all product-specific code.
- `thin_io/`: native low-level file and filesystem I/O. Use it where Qt filesystem APIs cannot provide the required
  correctness or native metadata. Keep recursion, threading, cancellation, aggregation, and Qt adapters out of this
  library. See `thin_io/README.md` for its completed API contract and `doc/plan.md` for SpaceGuard integration.
- `cpputils/`: general C++ utilities. Relevant here are `CWorkerThreadPool`, `CInterruptableThread`, and
  `CExecutionQueue`; use these for background and multithreaded scan orchestration.
- `qtutils/`: reusable Qt helpers, settings, widgets, dialogs, and platform integration.
- `cpp-template-utils/`: header-only generic/template utilities used by the other components.

The dependency directories above are Git submodules. Treat each as a separate repository and do not make incidental
changes inside one while working on SpaceGuard. A task that changes a submodule should keep that submodule's diff
self-contained and reviewable.

## Filesystem design direction

- `thin_io` supplies synchronous single-operation native primitives; SpaceGuard supplies scan policy and concurrency.
- On Windows, every `thin_io` path-taking API should have a primary `const wchar_t*` overload and a UTF-8 `const char*`
  forwarding overload. Existing APIs must follow the same convention. On POSIX, `const char*` is the native byte path.
- SpaceGuard should pass `QString` UTF-16 storage to native-wide Windows overloads through a small adapter outside
  `thin_io`, avoiding a UTF-8 round trip.
- Do not traverse symbolic links, junctions, mount points, or other directory reparse targets while accounting for a
  selected filesystem. Report the entry, but keep traversal within the chosen tree/volume.
- A failed or incomplete directory enumeration must never be treated as an empty, authoritative subtree.
- Build a scan result completely off-thread and publish it to the UI only when complete and still current.
- Keep the existing in-memory tree and snapshot serialization until correctness work creates a concrete reason to
  replace them.

## Related reference project

`E:/Development/Projects/Personal/file-commander/` contains established filesystem traversal and concurrency patterns.
Read its root `AGENTS.md` and the documents it points to before borrowing a design. Particularly relevant references
are `doc/threading.md`, `doc/core-engine.md`, the parallel directory queue in
`file-commander-core/src/filesystemhelpers/filestatistics.cpp`, and link-cycle handling in
`file-commander-core/src/directoryscanner.cpp`. Reuse the patterns deliberately; do not couple SpaceGuard to File
Commander's core library.
