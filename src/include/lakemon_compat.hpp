#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

// DuckDB's extension CMake defines DUCKDB_MAJOR_VERSION. Fall back to the
// identifier header that exists only on 2.x if the macro is missing.
#ifndef DUCKDB_MAJOR_VERSION
#if defined(__has_include)
#if __has_include("duckdb/common/identifier.hpp")
#define DUCKDB_MAJOR_VERSION 2
#else
#define DUCKDB_MAJOR_VERSION 1
#endif
#else
#define DUCKDB_MAJOR_VERSION 1
#endif
#endif

#if DUCKDB_MAJOR_VERSION >= 2
#define LAKEMON_DUCKDB_2 1
#include "duckdb/common/identifier.hpp"
#include "duckdb/main/query_result.hpp"
#else
#define LAKEMON_DUCKDB_2 0
#include "duckdb/main/materialized_query_result.hpp"
#endif

namespace lakemon {

#if LAKEMON_DUCKDB_2
typedef duckdb::QueryResult QueryHandle;
typedef duckdb::vector<duckdb::Identifier> ColumnNameList;
#else
typedef duckdb::MaterializedQueryResult QueryHandle;
typedef duckdb::vector<duckdb::string> ColumnNameList;
#endif

inline duckdb::unique_ptr<QueryHandle> RunSQL(duckdb::ClientContext &context, const std::string &sql) {
	duckdb::Connection con(*context.db);
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw duckdb::InvalidInputException("lakemon: %s", result->GetError());
	}
	return result;
}

inline void ReferenceScalar(duckdb::Vector &result, const duckdb::Value &value) {
#if LAKEMON_DUCKDB_2
	result.Reference(value, duckdb::count_t(1));
#else
	result.Reference(value);
#endif
}

inline void WriteChunkValue(duckdb::DataChunk &output, duckdb::idx_t col, duckdb::idx_t row,
                            const duckdb::Value &value) {
#if LAKEMON_DUCKDB_2
	output.data[col].SetValue(row, value);
#else
	output.SetValue(col, row, value);
#endif
}

inline void FinishChunk(duckdb::DataChunk &output, duckdb::idx_t count) {
#if LAKEMON_DUCKDB_2
	output.SetChildCardinality(count);
#else
	output.SetCardinality(count);
#endif
}

} // namespace lakemon
