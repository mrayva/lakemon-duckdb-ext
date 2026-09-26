# Updating the DuckDB target

This repository follows the C++ [extension-template](https://github.com/duckdb/extension-template) update path.

1. Bump the `duckdb` submodule to the tagged release you want (for example `v1.5.5`).
2. Bump `extension-ci-tools` to the matching branch/tag (`v1.5.5`, `v1.5-variegata`, or `main` for 2.x).
3. Update `.github/workflows/MainDistributionPipeline.yml`:
   - `duckdb-stable-build` → latest 1.5.x tag
   - `duckdb-2-build` → `v2.0-cyanoptera` / `main` until 2.0.0 is tagged
4. Rebuild and run `make test` plus `make policy-test`.

The C++ extension API is **not** a stable ABI. A DuckDB bump may require source changes. Useful references:

- DuckDB [release notes](https://github.com/duckdb/duckdb/releases)
- [community-extensions UPDATING.md](https://github.com/duckdb/community-extensions/blob/main/UPDATING.md) (`ref` vs `ref_next`)
