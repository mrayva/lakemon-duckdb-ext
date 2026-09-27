#include "lakemon_catalog.hpp"

#include "duckdb/common/exception.hpp"

#include <exception>
#include <map>
#include <new>
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

static void AppendDiagnostic(std::vector<InventoryDiagnostic> *diagnostics, const std::string &schema,
                             const std::string &table, const std::string &source, const std::string &message) {
	if (!diagnostics) {
		return;
	}
	InventoryDiagnostic row;
	row.schema_name = schema;
	row.table_name = table;
	row.source = source;
	row.message = message.empty() ? "lakemon: unread DuckLake metadata" : message;
	diagnostics->push_back(std::move(row));
}

static std::vector<policy::FileStat> InventoryFromMetadata(duckdb::ClientContext &context, const std::string &catalog,
                                                           const TableRef &filter,
                                                           std::vector<InventoryDiagnostic> *diagnostics) {
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
		std::string schema_name;
		std::string table_name;
		try {
			schema_name = CellString(*result, 0, i);
			table_name = CellString(*result, 1, i);
			if (table_name.empty()) {
				continue;
			}
			policy::FileStat file;
			file.schema_name = schema_name;
			file.table_name = table_name;
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
		} catch (const std::bad_alloc &) {
			throw;
		} catch (const duckdb::Exception &ex) {
			AppendDiagnostic(diagnostics, schema_name, table_name, "metadata", SafeWhat(ex));
			continue;
		} catch (const std::exception &ex) {
			AppendDiagnostic(diagnostics, schema_name, table_name, "metadata", SafeWhat(ex));
			continue;
		}
	}
	return files;
}

static std::vector<policy::FileStat> InventoryFromListFiles(duckdb::ClientContext &context, const std::string &catalog,
                                                            const TableRef &filter,
                                                            std::vector<InventoryDiagnostic> *diagnostics) {
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
				AppendDiagnostic(diagnostics, filter.schema, table_name, "list_files",
				                 "lakemon: list_files returned no result");
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
		} catch (const std::bad_alloc &) {
			throw;
		} catch (const duckdb::Exception &ex) {
			AppendDiagnostic(diagnostics, filter.schema, table_name, "list_files", SafeWhat(ex));
			continue;
		} catch (const std::exception &ex) {
			AppendDiagnostic(diagnostics, filter.schema, table_name, "list_files", SafeWhat(ex));
			continue;
		}
	}
	return files;
}

std::vector<policy::FileStat> InventoryFiles(duckdb::ClientContext &context, const std::string &catalog,
                                             const TableRef &filter, std::vector<InventoryDiagnostic> *diagnostics) {
	if (catalog.empty()) {
		throw InvalidInputException("lakemon: catalog name is required");
	}
	try {
		return InventoryFromMetadata(context, catalog, filter, diagnostics);
	} catch (const duckdb::InterruptException &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw;
	} catch (const duckdb::Exception &) {
		try {
			return InventoryFromListFiles(context, catalog, filter, diagnostics);
		} catch (const duckdb::InterruptException &) {
			throw;
		} catch (const std::bad_alloc &) {
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

static bool TryParseOptionRows(QueryHandle &result, std::vector<policy::OptionBinding> &rows) {
	const auto n = result.RowCount();
	for (duckdb::idx_t i = 0; i < n; i++) {
		policy::OptionBinding row;
		row.name = CellString(result, 0, i);
		row.value = CellString(result, 1, i);
		row.scope = CellString(result, 2, i);
		row.scope_entry = CellString(result, 3, i);
		if (row.name.empty()) {
			continue;
		}
		rows.push_back(std::move(row));
	}
	return true;
}

static bool TryLoadOptionsQuery(duckdb::ClientContext &context, const std::string &sql,
                                std::vector<policy::OptionBinding> &rows, std::string &error) {
	try {
		auto result = RunSQL(context, sql);
		if (!result) {
			error = "lakemon: options query returned no result";
			return false;
		}
		rows.clear();
		TryParseOptionRows(*result, rows);
		error.clear();
		return true;
	} catch (const duckdb::InterruptException &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw;
	} catch (const duckdb::Exception &ex) {
		error = SafeWhat(ex);
		return false;
	} catch (const std::exception &ex) {
		error = SafeWhat(ex);
		return false;
	}
}

std::vector<policy::OptionBinding> LoadCatalogOptions(duckdb::ClientContext &context, const std::string &catalog,
                                                      std::string *error_out) {
	std::vector<policy::OptionBinding> rows;
	if (catalog.empty()) {
		if (error_out) {
			*error_out = "lakemon: catalog name is required";
		}
		return rows;
	}
	std::string error;
	const std::string ident = QuoteIdent(catalog);
	const std::string quoted = QuoteString(catalog);
	const std::string select =
	    "SELECT option_name, value, scope, scope_entry FROM ";
	if (TryLoadOptionsQuery(context, select + "ducklake_options(" + quoted + ")", rows, error) ||
	    TryLoadOptionsQuery(context, select + ident + ".options()", rows, error) ||
	    TryLoadOptionsQuery(context, select + ident + ".main.options()", rows, error)) {
		if (error_out) {
			error_out->clear();
		}
		return rows;
	}
	if (error_out) {
		*error_out = error.empty() ? "lakemon: could not read DuckLake options" : error;
	}
	return rows;
}

std::vector<policy::TableHint> InventoryTables(duckdb::ClientContext &context, const std::string &catalog,
                                               const TableRef &filter, std::vector<InventoryDiagnostic> *diagnostics,
                                               std::vector<policy::FileStat> *files_out) {
	std::vector<InventoryDiagnostic> local_diagnostics;
	std::vector<InventoryDiagnostic> *sink = diagnostics ? diagnostics : &local_diagnostics;
	auto files = InventoryFiles(context, catalog, filter, sink);
	if (files_out) {
		*files_out = files;
	}
	std::map<std::pair<std::string, std::string>, std::vector<policy::FileStat>> grouped;
	for (auto &file : files) {
		grouped[{file.schema_name, file.table_name}].push_back(std::move(file));
	}
	std::string options_error;
	const std::vector<policy::OptionBinding> options = LoadCatalogOptions(context, catalog, &options_error);
	std::vector<policy::TableHint> hints;
	for (auto &entry : grouped) {
		policy::TableHint hint = policy::SummarizeTable(entry.second);
		policy::ApplyNativeOptions(hint, options);
		hints.push_back(std::move(hint));
	}
	(void)options_error;
	// table_stats has no status column: total unread metadata must not look empty.
	if (!diagnostics && hints.empty() && !local_diagnostics.empty()) {
		throw InvalidInputException("lakemon: unread DuckLake metadata: %s", local_diagnostics.front().message.c_str());
	}
	return hints;
}

} // namespace lakemon
