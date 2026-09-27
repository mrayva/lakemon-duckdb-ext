# Tests

## Policy (no DuckDB)

```shell
make policy-test
```

Compiles `test/policy/test_policy.cpp`, `test_options.cpp`, `test_overrides.cpp`, and `test_sql.cpp` against the header-only policy/options/diagnostics/SQL helpers (no DuckDB).

## Extension smoke

```shell
make test
```

SQL logic tests under `test/sql/`:

- `lakemon.test` — version + `lakemon_policy` rewrite ladder / merge tiers + `lakemon_set_policy` persist/read
- `lakemon_maintain.test` — bind errors when no DuckLake catalog is attached

These do not create a DuckLake catalog. Attach your own lake to exercise rewrite/merge end-to-end.
