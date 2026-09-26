#pragma once

#include "duckdb.hpp"
#include "lakemon_policy.hpp"
#include "lakemon_sql.hpp"

#include <string>
#include <vector>

namespace lakemon {

std::vector<policy::FileStat> InventoryFiles(duckdb::ClientContext &context, const std::string &catalog,
                                             const TableRef &filter);

std::vector<policy::TableHint> InventoryTables(duckdb::ClientContext &context, const std::string &catalog,
                                               const TableRef &filter);

// Native DuckLake options via ducklake_options / catalog.options().
// On unread options, rows stay empty and error_out (if set) receives the message.
std::vector<policy::OptionBinding> LoadCatalogOptions(duckdb::ClientContext &context, const std::string &catalog,
                                                      std::string *error_out);

} // namespace lakemon
