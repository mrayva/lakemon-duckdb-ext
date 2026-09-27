// Inventory unread-metadata diagnostics. Header-only so the join used by
// table_stats can be tested without DuckDB.
#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace lakemon {

// Per-table / per-row unread metadata. Maintain emits status=error; the session
// stays usable. Interrupt and bad_alloc are never recorded here (they rethrow).
struct InventoryDiagnostic {
	std::string schema_name;
	std::string table_name;
	std::string source;
	std::string message;
};

// Join every unread-metadata row. Callers that collect diagnostics (maintain)
// emit one status=error row each; table_stats has no status column and raises
// this summary when the inventory is empty so nothing is masked.
inline std::string FormatInventoryDiagnostics(const std::vector<InventoryDiagnostic> &rows) {
	std::ostringstream out;
	for (size_t i = 0; i < rows.size(); i++) {
		if (i > 0) {
			out << "; ";
		}
		const auto &row = rows[i];
		if (!row.table_name.empty()) {
			if (!row.schema_name.empty()) {
				out << row.schema_name << ".";
			}
			out << row.table_name;
			if (!row.source.empty()) {
				out << " (" << row.source << ")";
			}
			out << ": ";
		} else if (!row.source.empty()) {
			out << row.source << ": ";
		}
		out << (row.message.empty() ? "lakemon: unread DuckLake metadata" : row.message);
	}
	return out.str();
}

} // namespace lakemon
