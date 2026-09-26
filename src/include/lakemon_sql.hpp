#pragma once

#include "lakemon_compat.hpp"

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

} // namespace lakemon
