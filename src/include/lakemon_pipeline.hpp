// Maintain step dependency after a failed predecessor.
// Header-only so the rule can be tested without DuckDB.
#pragma once

#include <string>

namespace lakemon {

// rewrite and merge read flushed data files, so they depend on flush_inlined.
// expire_snapshots and cleanup are retention and stay independent.
inline bool StepDependsOnFlush(const std::string &step) {
	return step == "rewrite" || step == "merge";
}

inline const char *SkipReasonFlushFailed() {
	return "skipped: flush_inlined failed";
}

} // namespace lakemon
