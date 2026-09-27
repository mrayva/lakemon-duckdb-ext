// Maintain pipeline rules. Header-only so the contract can be tested without DuckDB.
#pragma once

#include <string>

namespace lakemon {

// After inventory succeeds, each native CALL is best-effort and independent.
// A flush_inlined error is recorded as status=error; rewrite and merge still run.
// Only a hard inventory failure stops the pipeline before rewrite/merge.
inline bool InventoryFailureStopsPipeline() {
	return true;
}

inline bool FlushErrorSkipsStep(const std::string &step) {
	(void)step;
	return false;
}

} // namespace lakemon
