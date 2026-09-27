#pragma once

#include "duckdb.hpp"
#include "lakemon_policy.hpp"
#include "lakemon_sql.hpp"

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

std::vector<policy::FileStat> InventoryFiles(duckdb::ClientContext &context, const std::string &catalog,
                                             const TableRef &filter,
                                             std::vector<InventoryDiagnostic> *diagnostics = nullptr);

std::vector<policy::TableHint> InventoryTables(duckdb::ClientContext &context, const std::string &catalog,
                                               const TableRef &filter,
                                               std::vector<InventoryDiagnostic> *diagnostics = nullptr,
                                               std::vector<policy::FileStat> *files_out = nullptr);

// Native DuckLake options via ducklake_options / catalog.options().
// On unread options, rows stay empty and error_out (if set) receives the message.
std::vector<policy::OptionBinding> LoadCatalogOptions(duckdb::ClientContext &context, const std::string &catalog,
                                                      std::string *error_out);

} // namespace lakemon
