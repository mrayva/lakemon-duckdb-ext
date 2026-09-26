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

} // namespace lakemon
