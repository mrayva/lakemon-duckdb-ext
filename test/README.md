# Tests

## Policy (no DuckDB)

```shell
make policy-test
```

Compiles `test/policy/test_policy.cpp` against `src/include/lakemon_policy.hpp` only.

## Extension smoke

```shell
make test
```

SQL logic tests under `test/sql/`:

- `lakemon.test` — version + `lakemon_policy` ladder/tiers
- `lakemon_maintain.test` — bind errors when no DuckLake catalog is attached

These do not create a DuckLake catalog. Attach your own lake to exercise rewrite/merge end-to-end.
