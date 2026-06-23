# Media Tag Index — Design Spec

**Date:** 2026-06-23
**Branch:** `feat/media-tag-index`
**Status:** Draft, pending user review

## 1. Goal

Turn the file explorer into a Hydrus-style, tag-driven media organizer. Files
(images + video) are indexed with their metadata and a BC1 thumbnail; the user
browses and filters by tags, ratings, and dates. Two front-ends produce the same
on-disk state: an in-app live path and a standalone batch indexer executable.

## 2. Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| Index store | **SQLite** (tag brain) + existing `thumbs.bc1blob` (pixel arena) | Tag queries are many-to-many inverted-index joins — relational, not KV. BC1 bytes stay in the flat arena for zero-copy `mmap → upload_bc1`. |
| Blob directory | Moves **into SQLite** (key → offset/length/dims) | Multi-process safe (CLI + app) via SQLite WAL; also removes the crash-loses-index fragility of the in-RAM rewrite-on-close map. |
| Tag source of truth | **Hybrid**: SQLite is live truth; the exiftool script becomes an *export* step writing tags back into files | Fast in-app tagging + portable files. |
| File identity | **Cheap content signature**: `size + xxh3(first 64 KB) + xxh3(last 64 KB)`; whole-file hash when `< 128 KB` | Rename-proof, near-instant, reuses the read already done for thumbnailing. Weaker dedup accepted. |
| v1 browse features | include/exclude tag filter (AND-include, NOT-exclude), autocomplete with counts, namespaced tag sidebar, sort by score/date/size | Covers the Hydrus core. |
| Deferred (not v1) | tag siblings/aliases, parent/implication tags, OR-groups, duplicate-resolution UI | YAGNI for v1. |
| Import source | **Embedded tags primary** (exiftool / EXIF-XMP-IPTC), **sidecar JSON fallback**, baked into the index | Files are the self-describing portable artifact; sidecar only for untagged files. |
| Overall architecture | Approach A + standalone indexer | Smallest delta; keeps every zero-copy property; one new "brain" + two front-ends. |

## 3. Component architecture

```text
libmediaindex  (NEW static lib — Vulkan-free, PCH-free; links into exe + app + tests)
├─ MediaIndexDb     SQLite owner: schema, files/tags/file_tags/FTS/blob_directory.
│                   WAL mode. All queries + upserts. Single C++ wrapper over the
│                   amalgamation; prepared statements cached per connection.
├─ BlobArena        thumbs.bc1blob append-only byte store. Each thumb's
│                   offset/length lives in the MediaIndexDb files row (§4);
│                   LRU + compaction coordinated via DB.
├─ ContentSignature size + xxh3(first 64 KB, last 64 KB) -> 128-bit file identity.
├─ MetadataExtractor embedded-first (exiftool -stay_open / EXIF-XMP-IPTC), sidecar
│                   JSON fallback; ffprobe for video -> MetaHeader + tag list.
└─ ThumbnailEncoder image_ops decode + encode_bc1 (CPU). App swaps in the GPU
                    vulkan_bc1_encoder; CLI uses CPU.

media-indexer  (NEW executable)
    Walks dirs (existing find extension list), fans out over ImageJobSystem,
    calls libmediaindex per file -> thumbnail + signature + metadata -> DB + arena.
    Pure batch. No Vulkan, no ImGui.

App (existing FileExplorer — refactored to consume libmediaindex)
    • file_browser_ui: tag-filter bar + namespace sidebar; grid driven by
      MediaIndexDb queries instead of a raw directory listing.
    • FileThumbnailCache / thumbnail_generator: thin consumers — read BC1 from
      BlobArena (mmap -> upload_bc1); on miss, GPU-encode + write through
      libmediaindex (identical writer to the CLI).
```

**Boundary contract:** `libmediaindex` is the only writer of DB + arena. The CLI
and the app are front-ends; both call the same `IndexWriter::ingest(path)` so the
on-disk state is identical regardless of producer.

## 4. Data model (SQLite, WAL)

```sql
-- One row per indexed file. sig_* is the content signature (identity).
CREATE TABLE files (
  id            INTEGER PRIMARY KEY,         -- internal surrogate
  sig_size      INTEGER NOT NULL,
  sig_head      INTEGER NOT NULL,            -- xxh3 first 64 KB
  sig_tail      INTEGER NOT NULL,            -- xxh3 last  64 KB
  path          TEXT    NOT NULL,            -- last-known path (may move)
  kind          INTEGER NOT NULL,            -- 0=image 1=video
  -- promoted MetaHeader fields (sortable/filterable in the grid):
  src_w         INTEGER, src_h INTEGER,
  capture_unix  INTEGER,                     -- DateTimeOriginal / creation_time
  file_mtime    INTEGER,
  score         INTEGER,                     -- XMP:Rating / booru score
  duration_ms   INTEGER,                     -- 0 for stills
  -- (gps + codec are NOT promoted; they live in meta_full, details panel only)
  -- thumbnail blob reference:
  blob_offset   INTEGER, blob_length INTEGER, thumb_w INTEGER, thumb_h INTEGER,
  meta_full     BLOB,                        -- full exiftool/ffprobe JSON, zstd-compressed (details panel)
  UNIQUE(sig_size, sig_head, sig_tail)
);
CREATE INDEX files_score   ON files(score);
CREATE INDEX files_capture ON files(capture_unix);
CREATE INDEX files_size    ON files(sig_size);

-- Distinct tags, namespace split out for the sidebar ("creator:foo" -> ns/name).
CREATE TABLE tags (
  id    INTEGER PRIMARY KEY,
  ns    TEXT NOT NULL DEFAULT '',            -- "creator", "character", "" (unnamespaced)
  name  TEXT NOT NULL,
  UNIQUE(ns, name)
);
CREATE INDEX tags_name ON tags(name);

-- Many-to-many. Both index directions for fast include/exclude joins + counts.
CREATE TABLE file_tags (
  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  tag_id  INTEGER NOT NULL REFERENCES tags(id)  ON DELETE CASCADE,
  PRIMARY KEY (file_id, tag_id)
);
CREATE INDEX file_tags_tag ON file_tags(tag_id);

-- FTS over tag text for type-ahead autocomplete.
CREATE VIRTUAL TABLE tags_fts USING fts5(text, content='');
```

**MetaHeader (resolved):** the `files` columns above (`src_w/h`, `capture_unix`,
`file_mtime`, `score`, `duration_ms`) are the promoted, queryable fields. `gps`
and `codec` are intentionally NOT promoted — they live in `meta_full` (details
panel only). Adding a promoted column later = a re-index pass, so this list is
the load-bearing schema choice.

**Core query shapes:**

- *Include A AND B, exclude C:* intersect `file_tags` on the include tag-ids,
  `NOT IN` the exclude tag-ids; `ORDER BY score|capture_unix|sig_size`.
- *Autocomplete with counts:* `tags_fts MATCH 'foo*'` joined to a
  `COUNT(file_tags)` per tag.
- *Namespace sidebar:* `GROUP BY tags.ns` over the current result set.

## 5. Data flow

1. **Ingest (CLI batch or app on-miss)** — read file once →
   `ContentSignature` + `ThumbnailEncoder` (BC1) + `MetadataExtractor`
   (embedded tags, sidecar fallback). One SQLite write txn: upsert `files` row,
   allocate `blob_offset` (append to arena under the txn), upsert tags +
   `file_tags`. WAL lets the app keep reading during a CLI run.
2. **Query (app grid)** — tag-filter bar builds the include/exclude query;
   `MediaIndexDb` returns ordered file rows; grid reads each BC1 thumbnail from
   `BlobArena` via mmap → `upload_bc1` (zero copy).
3. **Edit (app)** — add/remove tag = `file_tags` insert/delete in a write txn.
   Instant; no file write.
4. **Export (on demand)** — the exiftool script logic, invoked as a step, writes
   the DB's tags back into the files (`-Keywords`/`-XMP:Subject`/`-Rating`/…) so
   files stay portable.
5. **Details panel** — on open, parse `meta_full` for the file (the long tail).

## 6. Concurrency model

- **SQLite WAL**: many concurrent readers + one writer across processes. The app
  reads freely while `media-indexer` writes.
- **Blob arena append**: offsets are allocated *inside* the SQLite write txn that
  inserts the row, so two writers never collide; arena writes are append-only.
- **Compaction** (reclaim dead blobs): exclusive — runs only when it can take the
  SQLite write lock and no live readers hold pinned offsets; gated behind an
  app-idle / CLI `--compact` trigger, never mid-browse.
- **exiftool -stay_open**: one long-lived subprocess owned by a `ManagedThread`
  (reap + kill-on-teardown) per the project's thread convention.

## 7. Error handling

- **Missing/corrupt sidecar JSON** → fall back to embedded tags; log and continue
  (never abort a batch run for one bad file).
- **exiftool/ffprobe failure** → index the file with no tags / minimal header,
  mark `meta_full = NULL`, continue; re-attempted on next scan.
- **Arena/DB divergence** → DB is authoritative for offsets; a row pointing past
  arena EOF is treated as a miss and re-generated. Orphan arena bytes (no row)
  are reclaimed by compaction.
- **DB loss** → the embedded-tag layer is rebuildable by re-scanning files;
  DB-only edits not yet exported are the precious part (documented backup point).
- **Map/arena growth** → arena is sized lazily; SQLite autovacuum off, manual
  `--compact` reclaims.

## 8. Testing

- `ContentSignature`: small (<128 KB whole-file) vs large (head/tail) paths;
  rename stability; collision sanity.
- `MetadataExtractor`: fixtures with (a) embedded EXIF/XMP tags, (b) sidecar
  JSON only, (c) both → embedded wins; image (exiftool) + video (ffprobe).
- `MediaIndexDb`: include/exclude queries, autocomplete counts, namespace
  grouping, sort orders; upsert idempotency on re-ingest.
- `BlobArena`: offset allocation, LRU eviction, compaction correctness,
  two-process append safety (WAL).
- End-to-end: `media-indexer` over a fixtures dir, asserting DB rows, arena
  bytes, and a sample query result. Standalone target (no Vulkan/PCH), like
  `image_tests` / `thumbnail_blob_tests`.

## 9. Build / dependencies

- New static lib target **`libmediaindex`** (PCH-free, std-only — mirrors the
  image core so it links into standalone test + CLI targets).
- New executable target **`media-indexer`**.
- **SQLite**: via **vcpkg** — `sqlite3[fts5]` (FTS5 feature is required for
  autocomplete), consumed as `find_package(unofficial-sqlite3 CONFIG REQUIRED)`
  → `unofficial::sqlite3::sqlite3`. vcpkg is manifest-off here, so install into
  the global tree: `vcpkg install sqlite3[fts5]`.
- **xxhash**: via **vcpkg** — `find_package(xxHash CONFIG REQUIRED)` →
  `xxHash::xxhash`, for the content signature.
- **zstd**: already linked via the media stack — reused to compress `meta_full`.
- exiftool: system binary, or the repo's `external/exiftool` fallback;
  ffprobe: the existing static FFmpeg.

## 10. Resolved decisions (from review)

1. **MetaHeader promoted fields** — `src_w/h`, `capture_unix`, `file_mtime`,
   `score`, `duration_ms`. `gps` and `codec` dropped to `meta_full`.
2. **SQLite + xxhash sourcing** — via vcpkg (§9), not hand-vendored.
3. **`meta_full` compression** — zstd-compressed JSON.
4. **`bc1_encode.comp`** — removed from `.gitignore`; tracked for clean-clone
   GPU builds.
