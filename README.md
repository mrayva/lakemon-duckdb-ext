# lakemon

DuckDB community extension that runs **smart DuckLake maintenance** from SQL.

`CHECKPOINT` on a DuckLake catalog already flushes, expires, merges, rewrites, and cleans up. `lakemon` sits on top of those same native `CALL`s and chooses *when* and *how* to invoke them, split the same way a production maintain flow is split:

- **`CALL lakemon_maintain`** — table work: inventory, flush inlined data, adaptive rewrite, merge
- **`CALL lakemon_maintain_global`** — catalog-global retention: expire snapshots, old-file cleanup, orphan delete
- a **byte-weighted adaptive rewrite ladder** (files with deletes, worst delete-fraction first; up to `max_rewrite_steps` native `CALL`s whose `delete_threshold` values are cut when cumulative `file_size_bytes` crosses `total_bytes * i/N`)
- **merge size bands** (micro / small / medium) with per-tier `target_file_size`
- **catalog SELECT** inventory via DuckLake metadata (`__ducklake_metadata_<catalog>` or `ducklake_table_info` / `ducklake_list_files`)
- **execute-by-default** (`dry_run => true` to plan only)
- a row per step (`ok` / `skip` / `error` / `planned`) instead of an opaque checkpoint
- **native DuckLake options** (`rewrite_delete_threshold`, `target_file_size`, `expire_older_than`, `delete_older_than`, `auto_compact`) with table → schema → global precedence. `lakemon_maintain` named parameters override table work for that CALL; `lakemon_maintain_global` named parameters override expire/cleanup intervals
- **persisted lakemon policy** (`CALL lakemon_set_policy`) for the rewrite ladder and merge bands, stored in `__lakemon.policy` (not DuckLake `set_option` keys)

The policy is inspired by common DuckLake maintain patterns (rewrite heavily deleted files, then compact adjacent Parquet files in size tiers). **Breaking change:** `lakemon_maintain` no longer expires snapshots or cleans old/orphan files. Run table maintain, then global, for example `CALL lakemon_maintain('lake', 's.t'); CALL lakemon_maintain_global('lake');`.

## Requirements

- DuckDB **1.5.\*** (built and tested against `v1.5.5`) or DuckDB **2.x** (`v2.0-cyanoptera` / upcoming 2.0)
- The core **`ducklake`** extension loaded in the same session
- An attached DuckLake catalog (`ATTACH 'ducklake:...' AS lake`)

This extension does not replace `INSTALL ducklake`.

## Install / load

Until the extension is listed on the [DuckDB community extensions](https://duckdb.org/community_extensions) site, load a locally built binary with unsigned extensions enabled:

```sql
INSTALL lakemon FROM community; -- after the community listing is merged
LOAD lakemon;
```

Local build:

```shell
duckdb -unsigned
```

```sql
LOAD 'build/release/extension/lakemon/lakemon.duckdb_extension';
```

See [community/README.md](community/README.md) for the remaining steps to list the extension.

## Usage

```sql
INSTALL ducklake;
LOAD ducklake;
LOAD lakemon;

ATTACH 'ducklake:metadata.ducklake' AS lake (DATA_PATH 'data_files');

-- Inventory + planned rewrite steps / merge tier
CALL lakemon_table_stats('lake');
CALL lakemon_table_stats('lake', 'myschema.t');

-- Table work only (inventory, flush, rewrite, merge). Returns one row per step.
CALL lakemon_maintain('lake');
CALL lakemon_maintain('lake', 'myschema.t');
CALL lakemon_maintain('lake', dry_run => true);

-- Catalog-global retention (expire snapshots, old-file + orphan cleanup)
CALL lakemon_maintain_global('lake');
CALL lakemon_maintain_global('lake', dry_run => true);
CALL lakemon_maintain('lake', 'myschema.t'); CALL lakemon_maintain_global('lake');

-- Inspect the built-in ladder and merge bands
CALL lakemon_policy();
-- Effective policy for a catalog (defaults + persisted overrides)
CALL lakemon_policy('lake');
-- Persist ladder / band overrides (current DuckDB database, keyed by catalog)
CALL lakemon_set_policy('lake', 'rewrite_ladder', 'default', max_rewrite_steps => 4);
CALL lakemon_set_policy('lake', 'merge_tier', 'micro', target_file_size => '8MB');
SELECT lakemon_version();
```

### `CALL lakemon_maintain(catalog [, table])`

Table work only. Does **not** run `ducklake_expire_snapshots`, `ducklake_cleanup_old_files`, or `ducklake_delete_orphaned_files`. Retention named parameters (`skip_expire`, `skip_cleanup`, `expire_older_than`, `delete_older_than`) belong on `lakemon_maintain_global`.

| Named parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `dry_run` | `BOOLEAN` | `false` | Plan steps without calling DuckLake |
| `max_compacted_files` | `BIGINT` | tier default | Cap per merge-tier `CALL` |

`table` may be `table` (schema `main`) or `schema.table`. Catalog-wide `CALL lakemon_maintain('lake')` still loops tables for flush / rewrite / merge only.

Named parameters override table-scoped work for that invocation. Catalog-wide maintain skips tables with `auto_compact = false`; an explicit table argument still maintains that table. When DuckLake `rewrite_delete_threshold` is set in **(0, 1]** (table → schema → global), lakemon emits **one** rewrite `CALL` with that value (full override of per-rung thresholds and CALL count) and keeps only files whose delete fraction is **>=** that catalog value. If none remain, rewrite is skipped. If the catalog value is **<= 0**, rewrite is skipped for that table this cycle (flush and merge still run); lakemon never emits `delete_threshold` 0.0. `max_rewrite_steps` / `byte_budget` / `min_delete_ratio` have already been applied. Otherwise each planned byte-weighted rung is its own `ducklake_rewrite_data_files` `CALL`. `target_file_size` informs the merge target and is not overwritten when already set on the catalog.

Execution order:

1. Catalog inventory
2. `ducklake_flush_inlined_data`
3. `ducklake_rewrite_data_files` once per byte-weighted ladder rung (worst delete-fraction first), each with a data-driven `delete_threshold`
4. `ducklake_merge_adjacent_files` once per size band that has at least two files

Result columns: `step`, `schema_name`, `table_name`, `action`, `status`, `files_processed`, `files_created`, `details`.

Rewrite emits **one result row per planned rung** (`action` is `rung_1` / `rung_2` / …, or `catalog` when DuckLake `rewrite_delete_threshold` is set). `details` always includes `band`, `delete_threshold`, `planned_files`, `planned_bytes`, and `planned_deletes`. `files_processed` is that rung's planned file count. `dry_run => true` sets `status=planned` and appends the generated `CALL`. After execute, `status` is `ok` / `skip` / `error`: `ok` appends `created=` (DuckLake result rows), `error` keeps the planned counts and appends the message, and `auto_compact` skips still list each computed rung.

Failed catalog inventory or a nested DuckLake `CALL` becomes `status = error` (`details` holds the message). Unread per-table metadata is an inventory `error` row (not a silent drop); tables that were read still proceed. The DuckDB session stays usable. A total inventory failure stops the plan.

After inventory succeeds, flush, rewrite, and merge are **independent** (best-effort). A `flush_inlined` error is recorded as `status = error` and rewrite/merge still run. Interrupt and out-of-memory are not swallowed.

### `CALL lakemon_maintain_global(catalog)`

Catalog-global retention only. Does **not** flush, rewrite, or merge tables.

| Named parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `dry_run` | `BOOLEAN` | `false` | Plan steps without calling DuckLake |
| `skip_expire` | `BOOLEAN` | `false` | Skip snapshot expiration |
| `skip_cleanup` | `BOOLEAN` | `false` | Skip old-file and orphan cleanup |
| `expire_older_than` | `VARCHAR` | unset | Interval literal, e.g. `'7 days'` (skipped if unset) |
| `delete_older_than` | `VARCHAR` | unset | Interval literal for cleanup; otherwise `cleanup_all => true` |

Named parameters override native DuckLake catalog options for that invocation. When a named expire/cleanup interval is omitted, lakemon uses `expire_older_than` / `delete_older_than` from `ducklake_options` / `catalog.options()` (usually global). Expire is skipped when the interval is unset (same as before). Cleanup still runs with `cleanup_all => true` unless `skip_cleanup`.

Execution order:

1. `ducklake_expire_snapshots` when `expire_older_than` is set
2. `ducklake_cleanup_old_files` + `ducklake_delete_orphaned_files`

Same result columns as `lakemon_maintain`. Global steps leave `schema_name` / `table_name` empty. Expire and cleanup are independent of each other and of table maintain. A nested DuckLake `CALL` becomes `status = error`; the session stays usable.

### `CALL lakemon_table_stats(catalog [, table])`

One row per table: file counts and bytes, delete counts, **deleted_bytes_weighted**, delete ratio, **rewrite_plan** (each planned byte-weighted rung with derived threshold plus file/byte/delete counts), first-step `rewrite_threshold`, `merge_tier_hint`, `target_file_size`, and `auto_compact`. `lakemon_policy()` lists built-in ladder/band defaults; `lakemon_policy(catalog)` lists the effective policy after persisted overrides; stats then apply native DuckLake options. A missing catalog or unloaded `ducklake` raises a DuckDB error (session stays usable). Empty metadata returns no rows. Total unread metadata (no table could be read) also raises; the error includes every diagnostic. Maintain instead emits one inventory `error` row per diagnostic.

### `CALL lakemon_policy([catalog])`

Zero-argument form returns the built-in ladder and merge bands. With a catalog name, rows are the **effective** policy for that catalog (built-in defaults overlaid with rows in `__lakemon.policy`).

| Column | Meaning |
| --- | --- |
| `kind` | `rewrite_ladder` or `merge_tier` |
| `name` | `default` (rewrite ladder), or `micro` / `small` / `medium` |
| `min_value` / `max_value` | rewrite ladder rung range (`1` / `max_rewrite_steps`), or merge file-size band |
| `threshold_or_target` | `data-driven` (rewrite) or merge `target_file_size` |
| `notes` | `max_rewrite_steps` / `byte_budget` / `min_delete_ratio`, or `max_compacted_files` |
| `source` | `default` or `override` |

### `CALL lakemon_set_policy(catalog, kind, name, …)`

Writes the rewrite ladder or one merge band into `__lakemon.policy` in the **current DuckDB database** (creates schema `__lakemon` on first write). The catalog argument is a key only — the DuckLake catalog does not need to be attached. Rewrite name must be `default`. Merge names stay `micro` / `small` / `medium`.

| Named parameter | Type | Applies to |
| --- | --- | --- |
| `max_rewrite_steps` | `BIGINT` | `rewrite_ladder` (ladder size `N`; `1`–`16`; default `3`) |
| `min_delete_ratio` | `DOUBLE` | `rewrite_ladder` (pool floor in `(0, 1]`; default `0.01`) |
| `byte_budget` | `BIGINT` | `rewrite_ladder` (`0` = unset; optional per-rung size cap) |
| `min_file_size` / `max_file_size` | `BIGINT` | `merge_tier` (`min < max`) |
| `target_file_size` | `VARCHAR` | `merge_tier` |
| `max_compacted_files` | `BIGINT` | `merge_tier` (`>= 1`) |
| `reset` | `BOOLEAN` | delete that override |
| `reset_all` | `BOOLEAN` | `CALL lakemon_set_policy(catalog, reset_all => true)` |

Omitted fields keep the current effective value (previous override or built-in). Result columns: `kind`, `name`, `status`, `details` (`ok` / `error`). Bind errors and `status=error` leave the DuckDB session usable.

Precedence for maintain/stats: **built-in defaults → persisted lakemon policy → native DuckLake options → CALL named parameters** (`lakemon_maintain` for table knobs, `lakemon_maintain_global` for expire/cleanup intervals).

### Rewrite ladder (byte-weighted adaptive)

Files are **not** split into equal-width `delete_threshold` steps (`0.1`, `0.2`, …), equal-count chunks, or absolute `delete_count` high/medium/low bands.

Instead, lakemon considers only files whose delete fraction is **at least `min_delete_ratio`** (default `0.01`). Files below that floor are excluded from the byte pool and from rung membership so a huge near-clean file cannot collapse the ladder. Qualifying files are ordered by delete fraction descending (worst first). The planner walks cumulative `file_size_bytes` and cuts a ladder of `N = max_rewrite_steps` thresholds when the running total crosses `total_bytes * i/N`. Each cut becomes one `ducklake_rewrite_data_files` `CALL` with that data-driven `delete_threshold`. Duplicate cuts (same threshold) merge their file slices. Optional `byte_budget` can trim a rung after the cuts are chosen.

The ladder **never** ends at `0.0`. The last (most inclusive) threshold is the **smallest observed delete fraction among files that meet `min_delete_ratio`**. A table with no qualifying delete data skips rewrite (flush and merge still run). A rung whose threshold would be **<= 0** is skipped.

lakemon **never** uses `delete_threshold` 0.0 — that native threshold matches every file in the table, including ones with no deletes. If you still want a zero threshold, call DuckLake directly: `CALL ducklake_rewrite_data_files('lake', 't', delete_threshold => 0)`.

| Knob | Default | Meaning |
| --- | --- | --- |
| `max_rewrite_steps` | `3` (`1`–`16`) | How many byte-weighted cuts to emit |
| `min_delete_ratio` | `0.01` (`(0, 1]`) | Exclude files below this delete fraction from the ladder |
| `byte_budget` | unset | Optional per-rung size cap |

Adaptive rewrite uses `delete_count` / `record_count` from DuckLake metadata. The `ducklake_list_files` fallback does not expose counts; it synthesizes a conservative fraction from `delete_file_size_bytes / file_size_bytes` (clamped to `(0, 1]`). Metadata inventory is still required for accurate count-based ladders.

A single near-zero threshold would rewrite every dirty file in one `CALL` (time / memory / CPU). Byte-weighted rungs slice that work: an early high-fraction cut rewrites the worst files first; later cuts lower the threshold as more bytes accumulate.

### Merge size bands

| Tier | File size | `target_file_size` | Default `max_compacted_files` |
| --- | --- | --- | --- |
| `micro` | `[0, 1 MiB)` | `5MB` | 64 |
| `small` | `[1 MiB, 10 MiB)` | `32MB` | 32 |
| `medium` | `[10 MiB, 64 MiB)` | `128MB` | 16 |

Files ≥ 64 MiB are left alone.

## Tips

- **Partial maintain.** For a single table step, call the native DuckLake functions directly (`ducklake_flush_inlined_data`, `ducklake_rewrite_data_files`, `ducklake_merge_adjacent_files`). For expire + old-file / orphan cleanup, use `CALL lakemon_maintain_global('lake')` or the native `ducklake_expire_snapshots` / `ducklake_cleanup_old_files` / `ducklake_delete_orphaned_files` CALLs. `lakemon_maintain` is the orchestrated table pass (ladder + merge bands). Plan with `lakemon_table_stats` and `dry_run => true`. If `flush_inlined` fails, rewrite and merge still run; each step records its own `ok` / `skip` / `error`.
- **Zero rewrite threshold.** lakemon never plans or executes `ducklake_rewrite_data_files` with `delete_threshold` 0.0 (or <= 0). Use the native `CALL ducklake_rewrite_data_files(..., delete_threshold => 0)` if you need that.
- **Large backlog.** No wall-clock budget in lakemon today. Prefer one table (`CALL lakemon_maintain('lake', 'schema.t')`) then `CALL lakemon_maintain_global('lake')`, tighter policy via `lakemon_set_policy` / DuckLake `set_option`, or native `CALL`s with file caps (`max_compacted_files`). An unbounded full-catalog table pass can run a long time; escape hatch is per-table / native steps. Expire and orphan cleanup are catalog-global and do not need a per-table loop.

## Building

This repository follows the [DuckDB C++ extension template](https://github.com/duckdb/extension-template). Clone with submodules:

```shell
git clone --recurse-submodules https://github.com/paulosuzart/lakemon-duckdb-ext.git
cd lakemon-duckdb-ext
```

Optional: `ccache` and `ninja` (`GEN=ninja make`) for faster rebuilds. No vcpkg packages are required.

```shell
make                 # builds DuckDB + the extension
make test            # SQL tests (extension smoke)
make policy-test     # header-only policy tests (no DuckDB)
```

Binaries:

```
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/lakemon/lakemon.duckdb_extension
```

The in-tree shell already loads `lakemon`.

## Tests

- `make policy-test` — rewrite-ladder / merge-tier math, DuckLake option precedence, persisted policy overlays, and generated DuckLake CALL SQL (no DuckDB, seconds)
- `make test` — SQL smoke: `lakemon_version`, `lakemon_policy` / `lakemon_set_policy`, bind errors / maintain error rows when no catalog is attached, `lakemon_maintain_global` registration and retention skip/plan rows
- CI: `make policy-test` on every push/PR. Full DuckDB **1.5.5** and **2.x** (`v2.0-cyanoptera`) distribution builds run on `main`, `v*` tags, or **Actions → Main Extension Distribution Pipeline → Run workflow** (use that before a community listing submit).

End-to-end rewrite/merge against a live DuckLake catalog is left to your own lake (CI does not attach a lake). Manual check: `ATTACH` a lake, `CALL lakemon_set_policy(...)` for ladder size / merge bands, `CALL lake.set_option(...)` for the five honored DuckLake keys, then `CALL lakemon_table_stats` / `lakemon_maintain(..., dry_run => true)` and confirm planned rewrite steps (rung + byte-derived threshold, never `0.0`), merge target, and `auto_compact` skips. Confirm expire/cleanup intervals with `CALL lakemon_maintain_global(..., dry_run => true)`.

## Community listing

See [community/README.md](community/README.md). A ready-to-copy `description.yml` lives in `community/description.yml`.

## License

MIT. See [LICENSE](LICENSE).
