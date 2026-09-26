#include "lakemon_catalog.hpp"

#include "duckdb/common/exception.hpp"

#include <map>
#include <sstream>

namespace lakemon {

using duckdb::InvalidInputException;
using duckdb::Value;

static bool MatchesFilter(const policy::FileStat &file, const TableRef &filter) {
	if (filter.table.empty()) {
		return true;
	}
	if (file.table_name != filter.table) {
		return false;
	}
	return filter.schema.empty() || file.schema_name == filter.schema;
}

static std::vector<policy::FileStat> InventoryFromMetadata(duckdb::ClientContext &context, const std::string &catalog,
                                                           const TableRef &filter) {
	const std::string meta = QuoteIdent(MetadataCatalog(catalog));
	std::ostringstream sql;
	sql << "SELECT s.schema_name, t.table_name, d.data_file_id, d.file_size_bytes, d.record_count, "
	       "COALESCE(del.delete_count, 0) AS delete_count, "
	       "COALESCE(del.file_size_bytes, 0) AS delete_file_size_bytes "
	       "FROM "
	    << meta << ".ducklake_schema s "
	    << "JOIN " << meta << ".ducklake_table t "
	    << "ON t.schema_id = s.schema_id AND t.end_snapshot IS NULL AND s.end_snapshot IS NULL "
	    << "JOIN " << meta << ".ducklake_data_file d "
	    << "ON d.table_id = t.table_id AND d.end_snapshot IS NULL "
	    << "LEFT JOIN " << meta << ".ducklake_delete_file del "
	    << "ON del.data_file_id = d.data_file_id AND del.end_snapshot IS NULL";
	if (!filter.table.empty()) {
		sql << " WHERE t.table_name = " << QuoteString(filter.table);
		if (!filter.schema.empty()) {
			sql << " AND s.schema_name = " << QuoteString(filter.schema);
		}
	}

	auto result = RunSQL(context, sql.str());
	std::vector<policy::FileStat> files;
	if (!result) {
		return files;
	}
	const auto n = result->RowCount();
	for (duckdb::idx_t i = 0; i < n; i++) {
		try {
			policy::FileStat file;
			file.schema_name = CellString(*result, 0, i);
			file.table_name = CellString(*result, 1, i);
			if (file.table_name.empty()) {
				continue;
			}
			file.data_file_id = static_cast<uint64_t>(CellInt64(*result, 2, i));
			file.file_size_bytes = static_cast<uint64_t>(CellInt64(*result, 3, i));
			file.record_count = static_cast<uint64_t>(CellInt64(*result, 4, i));
			file.delete_count = static_cast<uint64_t>(CellInt64(*result, 5, i));
			file.delete_file_size_bytes = static_cast<uint64_t>(CellInt64(*result, 6, i));
			if (MatchesFilter(file, filter)) {
				files.push_back(std::move(file));
			}
		} catch (const duckdb::InterruptException &) {
			throw;
		} catch (const duckdb::Exception &) {
			continue;
		} catch (const std::exception &) {
			continue;
		}
	}
	return files;
}

static std::vector<policy::FileStat> InventoryFromListFiles(duckdb::ClientContext &context, const std::string &catalog,
                                                            const TableRef &filter) {
	auto tables = RunSQL(context, "SELECT table_name, schema_id, table_id FROM ducklake_table_info(" +
	                                  QuoteString(catalog) + ")");
	std::vector<policy::FileStat> files;
	if (!tables) {
		return files;
	}
	const auto table_n = tables->RowCount();
	for (duckdb::idx_t t = 0; t < table_n; t++) {
		const auto table_name = CellString(*tables, 0, t);
		if (table_name.empty() || (!filter.table.empty() && table_name != filter.table)) {
			continue;
		}
		std::ostringstream list_sql;
		list_sql << "SELECT data_file, data_file_size_bytes, delete_file, delete_file_size_bytes "
		            "FROM ducklake_list_files("
		         << QuoteString(catalog) << ", " << QuoteString(table_name);
		if (!filter.schema.empty() && filter.schema != "main") {
			list_sql << ", schema => " << QuoteString(filter.schema);
		}
		list_sql << ")";
		try {
			auto listed = RunSQL(context, list_sql.str());
			if (!listed) {
				continue;
			}
			const auto file_n = listed->RowCount();
			for (duckdb::idx_t i = 0; i < file_n; i++) {
				policy::FileStat file;
				file.schema_name = filter.schema.empty() ? "main" : filter.schema;
				file.table_name = table_name;
				file.file_size_bytes = static_cast<uint64_t>(CellInt64(*listed, 1, i));
				file.delete_file_size_bytes = static_cast<uint64_t>(CellInt64(*listed, 3, i));
				// list_files does not expose delete_count; treat any delete file as a rewrite candidate via bytes.
				file.delete_count = file.delete_file_size_bytes > 0 ? 1 : 0;
				file.record_count = 0;
				files.push_back(std::move(file));
			}
		} catch (const duckdb::InterruptException &) {
			throw;
		} catch (const duckdb::Exception &) {
			continue;
		} catch (const std::exception &) {
			continue;
		}
	}
	return files;
}

std::vector<policy::FileStat> InventoryFiles(duckdb::ClientContext &context, const std::string &catalog,
                                             const TableRef &filter) {
	if (catalog.empty()) {
		throw InvalidInputException("lakemon: catalog name is required");
	}
	try {
		return InventoryFromMetadata(context, catalog, filter);
	} catch (const duckdb::InterruptException &) {
		throw;
	} catch (const duckdb::Exception &) {
		try {
			return InventoryFromListFiles(context, catalog, filter);
		} catch (const duckdb::InterruptException &) {
			throw;
		} catch (const duckdb::Exception &second) {
			throw InvalidInputException(
			    "lakemon: could not read DuckLake catalog %s (is ducklake loaded and the catalog attached?): %s",
			    catalog, SafeWhat(second));
		} catch (const std::exception &second) {
			throw InvalidInputException(
			    "lakemon: could not read DuckLake catalog %s (is ducklake loaded and the catalog attached?): %s",
			    catalog, SafeWhat(second));
		}
	} catch (const std::exception &first) {
		throw InvalidInputException(
		    "lakemon: could not read DuckLake catalog %s (is ducklake loaded and the catalog attached?): %s", catalog,
		    SafeWhat(first));
	}
}

std::vector<policy::TableHint> InventoryTables(duckdb::ClientContext &context, const std::string &catalog,
                                               const TableRef &filter) {
	auto files = InventoryFiles(context, catalog, filter);
	std::map<std::pair<std::string, std::string>, std::vector<policy::FileStat>> grouped;
	for (auto &file : files) {
		grouped[{file.schema_name, file.table_name}].push_back(std::move(file));
	}
	std::vector<policy::TableHint> hints;
	for (auto &entry : grouped) {
		hints.push_back(policy::SummarizeTable(entry.second));
	}
	return hints;
}

} // namespace lakemon
