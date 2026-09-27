# Tests

## Policy (no DuckDB)

```shell
make policy-test
```

Compiles `test/policy/test_policy.cpp`, `test_options.cpp`, and `test_overrides.cpp` against the header-only policy/options/diagnostics code (no DuckDB).

## Extension smoke

```shell
make test
```

SQL logic tests under `test/sql/`:

- `lakemon.test` — version + `lakemon_policy` ladder/tiers + `lakemon_set_policy` persist/read
- `lakemon_maintain.test` — bind errors when no DuckLake catalog is attached

These do not create a DuckLake catalog. Attach your own lake to exercise rewrite/merge end-to-end.
