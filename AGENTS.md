# SpaceGuard project guidance

## Project overview

SpaceGuard is a Qt/C++ desktop tool for explaining why free space decreases on large filesystems. It captures a
filesystem-tree snapshot, later scans the same root, compares the two trees, and reports significant positive size
changes without repeating the same growth at every ancestor.

The product-specific code is currently small and lives in `app/src/`:

- `snapshot.{h,cpp}` owns the factual native-name tree, derived accounting state, and validated atomic persistence.
- `snapshot_comparison.{h,cpp}` owns exact-only tree comparison, root eligibility, and free-space reconciliation.
- `snapshot_scanner.{h,cpp}` owns the synchronous native scan API, single/parallel traversal, and typed completed,
  failed, or canceled outcomes.
- `snapshot_scan_runner.{h,cpp}` owns the scan pool, nonblocking cancellation, generation-tagged
  immutable publication, and coalesced progress through a caller-owned execution queue.
- `mainwindow.{h,cpp,ui}` owns the create-baseline/find-growth/inspect-usage/cancel workflow, UI-thread publication
  draining, snapshot file dialogs, threshold-only recalculation, and the two top-level result views.
- `snapshot_usage_widget.{h,cpp,ui}` owns lazy presentation of a completed snapshot's current allocated-space tree,
  including exact/lower-bound/unknown totals, percentages, boundary states, hard-link accounting explanations, authoritative
  snapshot-path search, and lazy expansion to a selected native path. It initially expands the root and materializes
  that one level; deeper directories remain lazy.
- Snapshot files use the `.spaceguard` extension and a versioned, platform-marked `QDataStream`/`qCompress` format
  written through `QSaveFile`. Legacy prototype snapshots are rejected.

The prototype replacement is complete in source: the final snapshot model, persistence, deterministic
accounting/comparison, native multithreaded scan, asynchronous runner, UI workflow, hardening, and cross-platform
readiness pass are integrated. The growth-first comparison UI, current-space usage view, cross-view navigation,
authoritative path search, and source-level UI polish are integrated. Remaining work is runtime UI inspection and
optional performance tuning. Performance and memory consumption should be kept in mind but are not primary design
concerns at the current estimates.

Windows is the primary platform. macOS and Linux support are desired.

## Project structure

The repository is a qmake `TEMPLATE = subdirs` project using modern C++ and Qt Widgets. `SpaceGuard.pro` is the root
project and `app/app.pro` builds the application.

Top-level components:

- `app/`: SpaceGuard executable and all product-specific code.
- `thin_io/`: native low-level file and filesystem I/O. Use it where Qt filesystem APIs cannot provide the required
  correctness or native metadata. Keep recursion, threading, cancellation, aggregation, and Qt adapters out of this
  library. See `thin_io/README.md` for its completed API contract and `doc/plan.md` for SpaceGuard integration.
- `cpputils/`: general C++ utilities. Relevant here are `CWorkerThreadPool` and `CExecutionQueue`; use these for
  background scan orchestration and UI-thread publication.
- `qtutils/`: reusable Qt helpers, settings, widgets, dialogs, and platform integration.
- `cpp-template-utils/`: header-only generic/template utilities used by the other components.
- `tests/`: separate qmake test project; `tests/spaceguard_testapp/` is the Catch2 application for non-UI SpaceGuard
  code. Tests must not be part of the main `SpaceGuard.pro` build. GitHub Actions must build and run the separate test
  project on Windows, macOS, and Linux whenever tests are added or changed.

The dependency directories above are Git submodules. Treat each as a separate repository and do not make incidental
changes inside one while working on SpaceGuard. A task that changes a submodule should keep that submodule's diff
self-contained and reviewable.

SpaceGuard product types live in the global namespace: this executable is already the top-level application boundary,
so a redundant application-wide namespace adds noise without disambiguating library clients. Keep focused internal
namespaces such as `SnapshotInternal` where they genuinely group an implementation detail.

`app/src/filesystem_access.h` is a stateless static adapter that forwards directly to `thin_io`; production scanning
has no filesystem object, virtual dispatch, or adapter lifetime. The separate test project recompiles
`snapshot_scanner.cpp` with `SPACEGUARD_TEST_FILESYSTEM_ACCESS`, selecting its callback-backed test adapter so fake
filesystems remain deterministic without adding substitution machinery to the application.

## Filesystem design direction

- `thin_io` supplies synchronous single-operation native primitives; SpaceGuard supplies scan policy and concurrency.
- On Windows, every `thin_io` path-taking API should have a primary `const wchar_t*` overload and a UTF-8 `const char*`
  forwarding overload. Existing APIs must follow the same convention. On POSIX, `const char*` is the native byte path.
- SpaceGuard should pass `QString` UTF-16 storage to native-wide Windows overloads through a small adapter outside
  `thin_io`, avoiding a UTF-8 round trip.
- Resolve a factual descendant into path components with `nativeDescendantComponents()`; it validates separator
  boundaries and preserves native component spelling/bytes without a display-string round trip.
- Do not traverse symbolic links, junctions, mount points, or other directory reparse targets while accounting for a
  selected filesystem. Report the entry, but keep traversal within the chosen tree/volume.
- Linux traversal compares the scan-local `statx` mount ID as well as filesystem identity so bind mounts are boundaries
  even when they retain the target's device ID. Mount identity is traversal-only and is not persisted across scans.
- A failed or incomplete directory enumeration must never be treated as an empty, authoritative subtree.
- Build a scan result completely off-thread and publish it to the UI only when complete and still current.
- Use the scanner's explicit single-thread overload for deterministic semantic tests. Use its pool overload for
  concurrency/equivalence tests and through `SnapshotScanRunner` in production.
- Persist factual observations only; rebuild coverage, allocation totals, hard-link groups, and other derived state
  after scanning or loading.
- Derived accounting keeps exact subtree allocation separate from a conservative known subtotal. Incomplete coverage
  clears exactness but retains observed contributions for `>=` presentation; comparison continues to use exact totals
  only. The usage view calculates percentages from displayed exact/known values and qualifies the percentage columns as
  `% known` when the root is incomplete; those values are shares of observed allocation, not estimates of unknown shares.
- Access-denied and transient scan issues are routine on system volumes. Keep native diagnostics available in details,
  but use lower-bound notation and tooltips instead of repeating warning labels on every affected usage-tree row.
- Count ordinary one-link regular files directly. Build derived hard-link groups only for files reporting multiple links;
  comparison must still correlate one-to-many and many-to-one alias transitions through a surviving common path.
- Store each snapshot entry's children in the separate-key/value `flat_map`. Scanning reserves and finalizes the complete
  unsorted sibling batch before publishing child pointers; loading reserves and strictly appends the persisted sorted names.

## Related reference project

`E:/Development/Projects/Personal/file-commander/` contains established filesystem traversal and concurrency patterns.
Read its root `AGENTS.md` and the documents it points to before borrowing a design. Particularly relevant references
are `doc/threading.md`, `doc/core-engine.md`, the parallel directory queue in
`file-commander-core/src/filesystemhelpers/filestatistics.cpp`, and link-cycle handling in
`file-commander-core/src/directoryscanner.cpp`. Reuse the patterns deliberately; do not couple SpaceGuard to File
Commander's core library.
