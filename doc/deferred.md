# SpaceGuard Deferred Work

## Purpose and status

This is the canonical backlog of product, UI, and optional scanner-performance work deliberately excluded from the
completed implementation. None of these items is required to complete the current product: SpaceGuard can capture a
baseline, compare it with a later scan, keep positive growth visible independently of net free-space direction, inspect
current occupied space, search the current snapshot, and navigate between the growth and usage views.

An item being listed here is not an implementation commitment or an indication that the current design is incomplete.
Promote one into active work only when real use, measurements, or a concrete workflow demonstrates enough value to
justify its behavior and complexity. Define its semantics and acceptance criteria before implementation; several items
interact with incomplete scans, hard-link accounting, native paths, or destructive filesystem operations.

## Product principles that deferred work must preserve

- Growth comparison remains the primary result; current occupied-space analysis remains secondary.
- Positive growth must remain visible even when larger deletions make the tree smaller or increase free space.
- Allocated space is the primary metric. Logical size must not silently replace it.
- Incomplete scans retain known allocation as a conservative lower bound. The UI must distinguish exact values, known
  values, unknown values, and overflow.
- Hard links must not be double-counted, and path aliases must remain navigable.
- Search, filtering, and navigation operate on authoritative native snapshot paths, not only materialized GUI rows or
  decoded display strings.
- Snapshot data is historical. Any action against the live filesystem must expect that paths and identities may have
  changed since the scan.

## Visualization and windowing

### Treemap synchronized with the usage tree

**Value:** Provide rapid visual recognition of the largest current consumers while retaining the tree as the precise,
accessible source of names and values.

**Promote when:** Users can find exact entries in the tree but routinely struggle to recognize the dominant regions or
compare similarly sized siblings.

**Required design decisions:**

- Define how lower bounds, unknown allocation, overflow, hard-link aliases, and directory-local allocation appear.
- Keep selection and navigation synchronized in both directions without eagerly materializing the entire tree widget.
- Avoid implying that rectangle area is exact when the underlying allocation is only known or unavailable.
- Measure layout and memory cost on snapshots with very large sibling sets before choosing an implementation.

### Separate or detachable result windows

**Value:** Allow side-by-side comparison of growth and current usage, or retain results on another monitor.

**Promote when:** Repeated use shows that tab switching materially hinders investigation.

**Required design decisions:**

- Define which window owns scan controls, immutable snapshots, comparison results, and application lifetime.
- Preserve cross-view navigation when the target view is detached, hidden, or closed.
- Decide whether window placement and detached state persist between sessions.
- Keep cancellation, stale-generation rejection, and result replacement under one unambiguous owner.

### Baseline/current selector in the usage view

**Value:** Inspect where space was occupied at either side of a comparison without loading or rescanning separately.

**Promote when:** Users frequently need to understand the old location and composition of a reported change, not merely
the current tree.

**Required design decisions:**

- Show the selected snapshot and timestamp unmistakably so historical data is never mistaken for the live filesystem.
- Decide whether search state, expansion, selection, and scroll position are shared or retained separately per side.
- Define what cross-view navigation selects when a path exists on only one side.
- Retain only snapshots already required by the active workflow; do not introduce an implicit snapshot-history manager.

## Analysis and filtering

### Exact gross-positive accumulated-size total

**Value:** Answer how much allocation appeared or expanded in total, independently of deletions.

**Why it is not the sum of visible growth rows:** Comparison rows deliberately suppress repeated ancestor growth and may
represent aggregate directories. Summing them can double-count the same physical allocation. Hard-link alias changes
and incomplete coverage make an apparently simple sum still less reliable.

**Promote when:** Users need one aggregate positive-growth figure in addition to the existing list of actionable growth
regions.

**Required design decisions:**

- Define a non-overlapping accounting frontier whose positive deltas can be summed exactly.
- Preserve hard-link correlation across one-to-many and many-to-one alias transitions.
- Specify when the result is unavailable because either snapshot lacks exact accounting.
- Name and explain the metric so it is not confused with net tree growth or whole-filesystem usage change.

### Minimum-size filtering in the usage tree

**Value:** Suppress small entries so large contributors are easier to inspect.

**Promote when:** Sorting alone proves insufficient in directories with many immediate children.

**Required design decisions:**

- Apply the filter to immutable snapshot data, not only currently materialized `QTreeWidgetItem` objects.
- Decide whether the threshold uses exact allocation, known allocation, or both, and how unknown entries remain reachable.
- Preserve ancestors needed to reach matching descendants.
- Define interaction with search, cross-view selection, hard-link aliases, and lazy expansion.
- Distinguish filtering from pagination: a user-selected threshold intentionally hides entries, while pagination must
  still expose the complete set.

### Background usage-search or navigation index

**Value:** Reduce repeated full-snapshot traversal when `Find next` becomes noticeably slow.

**Current behavior:** Search traverses the immutable factual tree directly and therefore remains correct independently
of which GUI branches have been expanded.

**Promote when:** Measurements on representative large snapshots show unacceptable search or cross-view navigation
latency.

**Required design decisions:**

- Quantify index construction time and memory before replacing direct traversal.
- Build from authoritative native paths and preserve platform-specific comparison semantics.
- Publish the index atomically with the immutable snapshot or build it with cancellation and generation checks.
- Avoid delaying ordinary snapshot display merely to optimize a feature the user may not invoke.

## Large-tree presentation and recalculation performance

### Replace the lazy tree widget with a custom `QAbstractItemModel`

**Value:** Improve scaling, sorting/filter integration, or control over very large sibling collections.

**Promote when:** Profiling identifies `QTreeWidget` item creation or ownership as a material bottleneck that targeted
changes cannot solve.

**Required design decisions:**

- Provide stable parent/index relationships even though the factual snapshot does not store parent pointers.
- Preserve selection, keyboard navigation, accessibility, activation, expansion state, scrolling, and header sorting
  currently supplied by the widget.
- Preserve authoritative native paths and immutable snapshot lifetimes without copying the factual tree.
- Compare the measured gain against the substantial increase in model and lifecycle complexity.

### Pagination or top-N presentation for huge sibling sets

**Value:** Avoid creating thousands or millions of GUI items when a single expanded directory has an exceptional number
of immediate children.

**Promote when:** A measured real snapshot makes expansion latency or memory unacceptable.

**Required design decisions:**

- Preserve deterministic size/name ordering and provide an obvious route to every omitted child.
- Ensure search and cross-view selection can reveal an item outside the currently displayed page or top-N set.
- Keep unknown and overflow entries reachable rather than silently discarding them.
- Prefer pagination when complete browsing matters; consider top-N only if a clearly defined remainder view preserves
  access to the full data.

### Prepared or cached comparison accounting for threshold changes

**Value:** Make repeated threshold adjustments faster without rescanning.

**Promote when:** Measurements on the large-snapshot corpus show that current recalculation causes perceptible UI delay.

**Required design decisions:**

- Cache only threshold-independent accounting and preserve identical comparison semantics.
- Quantify retained memory against recomputation time.
- Invalidate the cache whenever either immutable snapshot changes.
- Keep incomplete coverage, hard-link correlation, reconciliation, and deterministic row ordering unchanged.

## Scanner performance

### Adaptive scan parallelism for cached metadata

**Value:** Reduce physical-disk seeking on cold metadata while still exploiting parallelism when filesystem metadata is
cached or the storage provides fast random access.

**Why storage type alone is insufficient:** A hard drive with cold metadata benefits from serialized traversal, while
the same drive with warm filesystem metadata can benefit from parallel traversal without generating physical disk
traffic. `storageSpeedForPath()` from cpputils can provide an initial classification, but it cannot describe current
cache residency.

**Promote when:** Measurements on cold and warm hard drives plus SSDs show that the fixed participant policy materially
hurts scan duration or responsiveness.

**Candidate policy to validate:**

- Start `FastRandomAccess` storage at the normal maximum participant count.
- Start `SlowOrUnknown` storage with one active participant while retaining the existing maximum-size worker pool.
- Measure only native `listDirectory()` and `getEntryMetadata()` latency, excluding tree construction and aggregation.
- Promote gradually (`1 -> 2 -> 4 -> N`) when a rolling sample consistently resembles cache hits, and reduce concurrency
  when seek-like latency returns.
- Do not persist a warm-cache decision across scans because cache residency can change with time and memory pressure.

**Required design decisions:**

- Derive sampling windows, thresholds, and hysteresis from measurements rather than intuition.
- Keep the policy behind the existing named participant-count decision point.
- Preserve identical factual and derived results at every concurrency level.
- Preserve cancellation responsiveness and bounded progress publication while participant count changes.

## Cleanup assistance and filesystem mutation

### Cleanup recommendations or junk labeling

**Value:** Help users move from identifying growth to deciding what may be safe or useful to remove.

**Promote when:** There is a trustworthy classification source and a well-defined user need beyond manual investigation.

**Required design decisions:**

- Never infer that an item is junk merely because it is large, new, or growing.
- Define where classification knowledge comes from, how current it is, and how uncertainty is displayed.
- Separate factual scan results from recommendations and make the evidence for each recommendation inspectable.
- Account for application data, caches with rebuild costs, shared files, hard links, permissions, and platform-specific
  locations.
- Begin as read-only guidance; deletion is a separate capability with a much higher safety bar.

### Deletion from inside SpaceGuard

**Value:** Complete a cleanup workflow without switching to a file manager.

**Promote when:** Read-only investigation and reveal actions are proven insufficient, and safe semantics can be specified
for every supported platform.

**Required design decisions:**

- Revalidate the live path and identity immediately before acting; never delete solely from historical snapshot facts.
- Define permanent deletion versus recycle-bin/trash behavior, confirmation, elevation, partial failure, cancellation,
  and recovery.
- Treat directory links, mount boundaries, hard links, permission changes, and replaced paths safely.
- Never recursively cross a reparse, symbolic-link, or mount boundary.
- Refresh or explicitly invalidate displayed results after mutation so historical values are not presented as current.
- Keep cleanup recommendations and deletion authorization separate; a recommendation must never imply consent.

## Promotion checklist

Before moving any item into active work:

1. Record the concrete user problem or measurement that justifies it.
2. Define behavior for exact, known-lower-bound, unknown, and overflow states where applicable.
3. Define native-path, hard-link, snapshot-lifetime, and stale-live-filesystem behavior where applicable.
4. Compare the simplest targeted implementation with the broader architectural alternative.
5. Specify automated coverage and a focused runtime inspection matrix.
6. Establish a self-contained review boundary before implementation begins.
