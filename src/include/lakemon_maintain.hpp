#pragma once

#include "duckdb.hpp"
#include "lakemon_sql.hpp"

#include <string>
#include <vector>

namespace lakemon {

struct MaintainOptions {
	std::string catalog;
	TableRef table;
	bool dry_run = false;
	bool skip_expire = false;
	bool skip_cleanup = false;
	std::string expire_older_than;
	std::string delete_older_than;
	bool expire_older_than_set = false;
	bool delete_older_than_set = false;
	int64_t max_compacted_files = 0;
};

struct MaintainRow {
	std::string step;
	std::string schema_name;
	std::string table_name;
	std::string action;
	std::string status;
	int64_t files_processed = 0;
	int64_t files_created = 0;
	std::string details;
};

std::vector<MaintainRow> RunMaintain(duckdb::ClientContext &context, const MaintainOptions &options);

} // namespace lakemon
