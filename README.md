# lakemon

DuckDB community extension that runs **smart DuckLake maintenance** from SQL.

`CHECKPOINT` on a DuckLake catalog already flushes, expires, merges, rewrites, and cleans up. `lakemon` sits on top of those same native `CALL`s and chooses *when* and *how* to invoke them:

- a **byte-weighted rewrite ladder** (delete *count* and deleted bytes, not equal-width ratio buckets)
- **merge size bands** (micro / small / medium) with per-tier `target_file_size`
- **catalog SELECT** inventory via DuckLake metadata (`__ducklake_metadata_<catalog>` or `ducklake_table_info` / `ducklake_list_files`)
- **execute-by-default** (`dry_run => true` to plan only)
- a row per step (`ok` / `skip` / `error` / `planned`) instead of an opaque checkpoint
- **native DuckLake options** (`rewrite_delete_threshold`, `target_file_size`, `expire_older_than`, `delete_older_than`, `auto_compact`) with table → schema → global precedence; `lakemon_maintain` named parameters override for that CALL

The policy is inspired by common DuckLake maintain patterns (rewrite heavily deleted files, then compact adjacent Parquet files in size tiers).

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

-- Inventory + recommended rewrite rung / merge tier
CALL lakemon_table_stats('lake');
CALL lakemon_table_stats('lake', 'myschema.t');

-- Execute maintain (default). Returns one row per step.
CALL lakemon_maintain('lake');
CALL lakemon_maintain('lake', 'myschema.t');

-- Plan only
CALL lakemon_maintain('lake', dry_run => true);

-- Inspect the built-in ladder and merge bands
CALL lakemon_policy();
SELECT lakemon_version();
```

### `CALL lakemon_maintain(catalog [, table])`

| Named parameter | Type | Default | Meaning |
| --- | --- | --- | --- |
| `dry_run` | `BOOLEAN` | `false` | Plan steps without calling DuckLake |
| `skip_expire` | `BOOLEAN` | `false` | Skip snapshot expiration |
| `skip_cleanup` | `BOOLEAN` | `false` | Skip old-file and orphan cleanup |
| `expire_older_than` | `VARCHAR` | unset | Interval literal, e.g. `'7 days'` (skipped if unset) |
| `delete_older_than` | `VARCHAR` | unset | Interval literal for cleanup; otherwise `cleanup_all => true` |
| `max_compacted_files` | `BIGINT` | tier default | Cap per merge-tier `CALL` |

`table` may be `table` (schema `main`) or `schema.table`.

Named parameters override native DuckLake catalog options for that invocation. When a named expire/cleanup interval is omitted, lakemon uses `expire_older_than` / `delete_older_than` from `ducklake_options` / `catalog.options()` (usually global). Catalog-wide maintain skips tables with `auto_compact = false`; an explicit table argument still maintains that table. `rewrite_delete_threshold` is the value passed to `ducklake_rewrite_data_files` when set; otherwise the built-in ladder. `target_file_size` informs the merge target and is not overwritten when already set on the catalog.

Execution order:

1. Catalog inventory
2. `ducklake_flush_inlined_data`
3. `ducklake_rewrite_data_files` with a **byte-weighted** threshold
4. `ducklake_merge_adjacent_files` once per size band that has at least two files
5. `ducklake_expire_snapshots` when `expire_older_than` is set
6. `ducklake_cleanup_old_files` + `ducklake_delete_orphaned_files`

Result columns: `step`, `schema_name`, `table_name`, `action`, `status`, `files_processed`, `files_created`, `details`.

Failed catalog inventory or a nested DuckLake `CALL` becomes `status = error` (`details` holds the message). The DuckDB session stays usable. Inventory failure stops the plan; later independent steps continue after a step error. Interrupt is not swallowed.

### `CALL lakemon_table_stats(catalog [, table])`

One row per table: file counts and bytes, delete counts, **deleted_bytes_weighted**, delete ratio, selected `rewrite_rung`, **effective** `rewrite_threshold`, `merge_tier_hint`, `target_file_size`, and `auto_compact`. `lakemon_policy()` lists built-in ladder/band defaults; stats show values after native DuckLake options. A missing catalog or unloaded `ducklake` raises a DuckDB error (session stays usable). Empty metadata returns no rows.

### Rewrite ladder (byte-weighted)

Files are **not** split into equal ratio buckets (0–25 / 25–50 / …). Each file is classified with delete **count** and byte-weighted deleted payload (`file_size * delete_count / record_count`). The table-level `delete_threshold` is the most aggressive rung that still holds a material share of deleted bytes.

| Rung | Qualifies when | `delete_threshold` |
| --- | --- | --- |
| `hot` | ≥ 10 000 deletes or ≥ 8 MiB deleted, and ratio ≥ 0.15 | `0.15` |
| `warm` | ≥ 1 000 deletes or ≥ 1 MiB deleted, and ratio ≥ 0.15 | `0.40` |
| `cool` | ≥ 100 deletes or ≥ 256 KiB deleted, and ratio ≥ 0.50 | `0.80` |
| `default` | ratio ≥ 0.95 | `0.95` |

### Merge size bands

| Tier | File size | `target_file_size` | Default `max_compacted_files` |
| --- | --- | --- | --- |
| `micro` | `[0, 1 MiB)` | `5MB` | 64 |
| `small` | `[1 MiB, 10 MiB)` | `32MB` | 32 |
| `medium` | `[10 MiB, 64 MiB)` | `128MB` | 16 |

Files ≥ 64 MiB are left alone.

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

- `make policy-test` — rewrite-ladder / merge-tier math and DuckLake option precedence (no DuckDB, seconds)
- `make test` — SQL smoke: `lakemon_version`, `lakemon_policy`, bind errors / maintain error rows when no catalog is attached
- CI builds the loadable extension for **DuckDB 1.5.5** and **DuckDB 2.x** (`v2.0-cyanoptera`) via `duckdb/extension-ci-tools`

End-to-end rewrite/merge against a live DuckLake catalog is left to your own lake (CI does not attach a lake). Manual check: `ATTACH` a lake, `CALL lake.set_option(...)` for the five honored keys, then `CALL lakemon_table_stats` / `lakemon_maintain(..., dry_run => true)` and confirm effective threshold, target, expire/cleanup intervals, and `auto_compact` skips.

## Community listing

See [community/README.md](community/README.md). A ready-to-copy `description.yml` lives in `community/description.yml`.

## License

MIT. See [LICENSE](LICENSE).
