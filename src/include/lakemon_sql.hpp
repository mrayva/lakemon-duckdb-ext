#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

#include <string>

namespace lakemon {

inline std::string QuoteIdent(const std::string &ident) {
	std::string out = "\"";
	for (char c : ident) {
		if (c == '"') {
			out += "\"\"";
		} else {
			out += c;
		}
	}
	out += "\"";
	return out;
}

inline std::string QuoteString(const std::string &value) {
	std::string out = "'";
	for (char c : value) {
		if (c == '\'') {
			out += "''";
		} else {
			out += c;
		}
	}
	out += "'";
	return out;
}

inline std::string MetadataCatalog(const std::string &catalog) {
	return "__ducklake_metadata_" + catalog;
}

struct TableRef {
	std::string schema = "main";
	std::string table;
};

inline TableRef ParseTableRef(const std::string &raw) {
	TableRef ref;
	if (raw.empty()) {
		return ref;
	}
	const auto dot = raw.find('.');
	if (dot == std::string::npos) {
		ref.table = raw;
		return ref;
	}
	ref.schema = raw.substr(0, dot);
	ref.table = raw.substr(dot + 1);
	return ref;
}

inline duckdb::unique_ptr<duckdb::MaterializedQueryResult> RunSQL(duckdb::ClientContext &context,
                                                                    const std::string &sql) {
	duckdb::Connection con(*context.db);
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw duckdb::InvalidInputException("lakemon: %s", result->GetError());
	}
	return result;
}

} // namespace lakemon
