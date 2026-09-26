# Listing lakemon on DuckDB community extensions

Community extensions are built, signed, and served by the
[duckdb/community-extensions](https://github.com/duckdb/community-extensions) repository.
After a listing is merged, users install with:

```sql
INSTALL lakemon FROM community;
LOAD lakemon;
```

Official documentation: [Submit a Community Extension](https://duckdb.org/community_extensions/documentation).

## Preconditions (from the official docs)

1. This repository must be **public**, open source, and hosted on GitHub.
2. The extension must build with the C++ [extension-template](https://github.com/duckdb/extension-template) layout (`cmake` + the default distribution workflow).
3. Pin a **commit hash** (not a moving branch) as `repo.ref`. The community CI builds exactly that revision.

This repo already matches the template layout. `community/description.yml` is the descriptor to copy.

## Remaining steps

1. Make `github.com/paulosuzart/lakemon-duckdb-ext` public when you are ready to list it.
2. Confirm CI is green on the commit you want to publish (DuckDB 1.5 and 2.x jobs in `.github/workflows/MainDistributionPipeline.yml`).
3. Copy `community/description.yml` into a fork of [duckdb/community-extensions](https://github.com/duckdb/community-extensions) at:

   ```
   extensions/lakemon/description.yml
   ```

4. Set `repo.ref` to the **full commit SHA** of this repository that you want built (and optionally `repo.ref_next` to a SHA compatible with DuckDB’s upcoming release, currently `v2.0-cyanoptera`). Official guidance: provide the hash of the latest commit targeting stable as `ref`.
5. Open a pull request against `duckdb/community-extensions` that adds **only** that `description.yml` (plus docs fields you want on the generated page).
6. Community CI builds and tests the extension. After maintainer approval and a successful deploy, the listing appears at
   `https://duckdb.org/community_extensions/extensions/lakemon`.
7. Later updates: bump `repo.ref` (and versions) in a follow-up PR. See
   [UPDATING.md](https://github.com/duckdb/community-extensions/blob/main/UPDATING.md)
   in the community-extensions repository for the `ref` / `ref_next` release cycle.

Do not invent extra process: the public docs currently describe a single-file PR to `extensions/<name>/description.yml`. Questions belong in the DuckDB Discord `#extensions` channel.
