#include "lakemon_store.hpp"

#include "lakemon_compat.hpp"
#include "lakemon_sql.hpp"

#include "duckdb/common/exception.hpp"

#include <cstring>
#include <exception>
#include <new>
#include <sstream>

namespace lakemon {

using duckdb::InvalidInputException;

// Value columns after the catalog key in kTableSQL. SELECT and RowFromResult
// share this order; do not reorder without updating CREATE TABLE.
static const char *kPolicyValueCols[] = {"kind",
                                         "name",
                                         "min_file_size",
                                         "max_file_size",
                                         "target_file_size",
                                         "max_compacted_files",
                                         "high_min",
                                         "medium_min",
                                         "low_min",
                                         "byte_budget",
                                         "max_rewrite_steps"};

static const char *kSchemaSQL = "CREATE SCHEMA IF NOT EXISTS __lakemon";
static const char *kTableSQL =
    "CREATE TABLE IF NOT EXISTS __lakemon.policy ("
    "catalog VARCHAR NOT NULL, "
    "kind VARCHAR NOT NULL, "
    "name VARCHAR NOT NULL, "
    "min_file_size BIGINT, "
    "max_file_size BIGINT, "
    "target_file_size VARCHAR, "
    "max_compacted_files BIGINT, "
    "high_min BIGINT, "
    "medium_min BIGINT, "
    "low_min BIGINT, "
    "byte_budget BIGINT, "
    "max_rewrite_steps BIGINT, "
    "PRIMARY KEY (catalog, kind, name))";

static const char *kAlterSQL[] = {"ALTER TABLE __lakemon.policy ADD COLUMN IF NOT EXISTS high_min BIGINT",
                                  "ALTER TABLE __lakemon.policy ADD COLUMN IF NOT EXISTS medium_min BIGINT",
                                  "ALTER TABLE __lakemon.policy ADD COLUMN IF NOT EXISTS low_min BIGINT",
                                  "ALTER TABLE __lakemon.policy ADD COLUMN IF NOT EXISTS byte_budget BIGINT",
                                  "ALTER TABLE __lakemon.policy ADD COLUMN IF NOT EXISTS max_rewrite_steps BIGINT"};

static std::string PolicySelectList() {
	std::ostringstream sql;
	const size_t n = sizeof(kPolicyValueCols) / sizeof(kPolicyValueCols[0]);
	for (size_t i = 0; i < n; i++) {
		if (i > 0) {
			sql << ", ";
		}
		sql << kPolicyValueCols[i];
	}
	return sql.str();
}

static duckdb::idx_t PolicyCol(const char *name) {
	const size_t n = sizeof(kPolicyValueCols) / sizeof(kPolicyValueCols[0]);
	for (duckdb::idx_t i = 0; i < n; i++) {
		if (std::strcmp(kPolicyValueCols[i], name) == 0) {
			return i;
		}
	}
	return static_cast<duckdb::idx_t>(n);
}

static void TryMigratePolicyStore(duckdb::ClientContext &context) {
	const size_t n = sizeof(kAlterSQL) / sizeof(kAlterSQL[0]);
	for (size_t i = 0; i < n; i++) {
		try {
			RunSQL(context, kAlterSQL[i]);
		} catch (const duckdb::InterruptException &) {
			throw;
		} catch (const std::bad_alloc &) {
			throw;
		} catch (const duckdb::Exception &) {
			// Table may not exist yet (no overrides).
		} catch (const std::exception &) {
		}
	}
}

void EnsurePolicyStore(duckdb::ClientContext &context) {
	RunSQL(context, kSchemaSQL);
	RunSQL(context, kTableSQL);
	TryMigratePolicyStore(context);
}

static policy::StoredPolicyRow RowFromResult(QueryHandle &result, duckdb::idx_t i) {
	policy::StoredPolicyRow row;
	row.kind = CellString(result, PolicyCol("kind"), i);
	row.name = CellString(result, PolicyCol("name"), i);
	row.min_file_size = static_cast<uint64_t>(CellInt64(result, PolicyCol("min_file_size"), i));
	row.max_file_size = static_cast<uint64_t>(CellInt64(result, PolicyCol("max_file_size"), i));
	row.target_file_size = CellString(result, PolicyCol("target_file_size"), i);
	row.max_compacted_files = static_cast<uint64_t>(CellInt64(result, PolicyCol("max_compacted_files"), i));
	row.high_min = static_cast<uint64_t>(CellInt64(result, PolicyCol("high_min"), i));
	row.medium_min = static_cast<uint64_t>(CellInt64(result, PolicyCol("medium_min"), i));
	row.low_min = static_cast<uint64_t>(CellInt64(result, PolicyCol("low_min"), i));
	row.byte_budget = static_cast<uint64_t>(CellInt64(result, PolicyCol("byte_budget"), i));
	row.max_rewrite_steps = static_cast<uint64_t>(CellInt64(result, PolicyCol("max_rewrite_steps"), i));
	return row;
}

std::vector<policy::StoredPolicyRow> LoadStoredPolicyRows(duckdb::ClientContext &context,
                                                          const std::string &catalog, std::string *error_out) {
	std::vector<policy::StoredPolicyRow> rows;
	if (catalog.empty()) {
		if (error_out) {
			*error_out = "lakemon: catalog name is required";
		}
		return rows;
	}
	try {
		TryMigratePolicyStore(context);
		std::ostringstream sql;
		sql << "SELECT " << PolicySelectList() << " FROM __lakemon.policy WHERE catalog = "
		    << QuoteString(catalog);
		auto result = RunSQL(context, sql.str());
		if (!result) {
			return rows;
		}
		const auto n = result->RowCount();
		for (duckdb::idx_t i = 0; i < n; i++) {
			policy::StoredPolicyRow row = RowFromResult(*result, i);
			if (row.kind.empty() || row.name.empty()) {
				continue;
			}
			rows.push_back(std::move(row));
		}
		if (error_out) {
			error_out->clear();
		}
		return rows;
	} catch (const duckdb::InterruptException &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw;
	} catch (const duckdb::Exception &ex) {
		const std::string msg = SafeWhat(ex);
		// Missing schema/table means no overrides yet.
		if (msg.find("does not exist") != std::string::npos || msg.find("Catalog Error") != std::string::npos ||
		    msg.find("__lakemon") != std::string::npos) {
			if (error_out) {
				error_out->clear();
			}
			return rows;
		}
		if (error_out) {
			*error_out = msg;
		}
		return rows;
	} catch (const std::exception &ex) {
		if (error_out) {
			*error_out = SafeWhat(ex);
		}
		return rows;
	}
}

policy::ActivePolicy LoadActivePolicy(duckdb::ClientContext &context, const std::string &catalog,
                                      std::string *error_out) {
	std::string load_error;
	const std::vector<policy::StoredPolicyRow> rows = LoadStoredPolicyRows(context, catalog, &load_error);
	if (!load_error.empty()) {
		if (error_out) {
			*error_out = load_error;
		}
		return policy::DefaultPolicy();
	}
	std::string overlay_error;
	policy::ActivePolicy policy = policy::OverlayStoredRows(rows, &overlay_error);
	if (error_out) {
		*error_out = overlay_error;
	}
	return policy;
}

void UpsertStoredPolicyRow(duckdb::ClientContext &context, const std::string &catalog,
                           const policy::StoredPolicyRow &row) {
	if (catalog.empty() || row.kind.empty() || row.name.empty()) {
		throw InvalidInputException("lakemon: catalog, kind, and name are required");
	}
	EnsurePolicyStore(context);
	std::ostringstream sql;
	sql << "INSERT OR REPLACE INTO __lakemon.policy (catalog, " << PolicySelectList() << ") VALUES ("
	    << QuoteString(catalog) << ", " << QuoteString(row.kind) << ", " << QuoteString(row.name) << ", "
	    << row.min_file_size << ", " << row.max_file_size << ", " << QuoteString(row.target_file_size) << ", "
	    << row.max_compacted_files << ", " << row.high_min << ", " << row.medium_min << ", " << row.low_min << ", "
	    << row.byte_budget << ", " << row.max_rewrite_steps << ")";
	RunSQL(context, sql.str());
}

void DeleteStoredPolicyRow(duckdb::ClientContext &context, const std::string &catalog, const std::string &kind,
                           const std::string &name) {
	if (catalog.empty() || kind.empty() || name.empty()) {
		throw InvalidInputException("lakemon: catalog, kind, and name are required");
	}
	EnsurePolicyStore(context);
	std::ostringstream sql;
	sql << "DELETE FROM __lakemon.policy WHERE catalog = " << QuoteString(catalog) << " AND kind = "
	    << QuoteString(kind) << " AND name = " << QuoteString(name);
	RunSQL(context, sql.str());
}

void DeleteStoredPolicyCatalog(duckdb::ClientContext &context, const std::string &catalog) {
	if (catalog.empty()) {
		throw InvalidInputException("lakemon: catalog name is required");
	}
	EnsurePolicyStore(context);
	RunSQL(context, "DELETE FROM __lakemon.policy WHERE catalog = " + QuoteString(catalog));
}

} // namespace lakemon
