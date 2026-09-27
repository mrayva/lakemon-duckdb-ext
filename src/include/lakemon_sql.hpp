#pragma once

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
	std::string schema;
	std::string table;

	TableRef() : schema("main") {
	}
};

inline TableRef ParseTableRef(const std::string &raw) {
	TableRef ref;
	if (raw.empty()) {
		return ref;
	}
	const std::string::size_type dot = raw.find('.');
	if (dot == std::string::npos) {
		ref.table = raw;
		return ref;
	}
	ref.schema = raw.substr(0, dot);
	ref.table = raw.substr(dot + 1);
	return ref;
}

// DuckLake CALL fragments. Flush takes named table_name / schema_name only.
// rewrite, merge, and list_files take a positional table and schema =>.
inline std::string TableArg(const TableRef &ref, const char *named = nullptr) {
	if (ref.table.empty()) {
		return "";
	}
	if (named && named[0] != '\0') {
		return std::string(", ") + named + " => " + QuoteString(ref.table);
	}
	return ", " + QuoteString(ref.table);
}

inline std::string SchemaNamed(const TableRef &ref, const char *param = "schema") {
	if (ref.table.empty() || ref.schema.empty() || ref.schema == "main") {
		return "";
	}
	return std::string(", ") + param + " => " + QuoteString(ref.schema);
}

inline std::string FlushInlinedDataCall(const std::string &catalog, const TableRef &ref) {
	return "CALL ducklake_flush_inlined_data(" + QuoteString(catalog) + TableArg(ref, "table_name") +
	       SchemaNamed(ref, "schema_name") + ")";
}

} // namespace lakemon
