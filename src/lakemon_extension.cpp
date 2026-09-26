#define DUCKDB_EXTENSION_MAIN

#include "lakemon_extension.hpp"
#include "lakemon_catalog.hpp"
#include "lakemon_compat.hpp"
#include "lakemon_maintain.hpp"
#include "lakemon_policy.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include <exception>
#include <iomanip>
#include <sstream>
#include <vector>

namespace duckdb {

namespace {

struct MaintainBindData : public TableFunctionData {
	lakemon::MaintainOptions options;
};

struct StatsBindData : public TableFunctionData {
	string catalog;
	lakemon::TableRef table;
};

struct PolicyBindData : public TableFunctionData {};

struct RowState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t offset = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

static void EmitRows(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	if (!input.global_state) {
		lakemon::FinishChunk(output, 0);
		return;
	}
	auto &state = input.global_state->Cast<RowState>();
	idx_t count = 0;
	const idx_t out_cols = output.ColumnCount();
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.offset++];
		const idx_t row_cols = static_cast<idx_t>(row.size());
		const idx_t use_cols = row_cols < out_cols ? row_cols : out_cols;
		for (idx_t col = 0; col < use_cols; col++) {
			lakemon::WriteChunkValue(output, col, count, row[col]);
		}
		count++;
	}
	lakemon::FinishChunk(output, count);
}

static string RequireCatalog(const Value &value) {
	if (value.IsNull()) {
		throw InvalidInputException("lakemon: catalog name is required");
	}
	const string catalog = StringValue::Get(value);
	if (catalog.empty()) {
		throw InvalidInputException("lakemon: catalog name is required");
	}
	return catalog;
}

static void ParseNamedMaintain(TableFunctionBindInput &input, lakemon::MaintainOptions &options) {
	for (auto &entry : input.named_parameters) {
		if (entry.second.IsNull()) {
			throw InvalidInputException("lakemon: named parameter cannot be NULL");
		}
		if (entry.first == "dry_run") {
			options.dry_run = BooleanValue::Get(entry.second);
		} else if (entry.first == "skip_expire") {
			options.skip_expire = BooleanValue::Get(entry.second);
		} else if (entry.first == "skip_cleanup") {
			options.skip_cleanup = BooleanValue::Get(entry.second);
		} else if (entry.first == "expire_older_than") {
			options.expire_older_than = StringValue::Get(entry.second);
			options.expire_older_than_set = true;
		} else if (entry.first == "delete_older_than") {
			options.delete_older_than = StringValue::Get(entry.second);
			options.delete_older_than_set = true;
		} else if (entry.first == "max_compacted_files") {
			options.max_compacted_files = entry.second.GetValue<int64_t>();
		}
	}
}

static unique_ptr<FunctionData> MaintainBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, lakemon::ColumnNameList &names) {
	auto data = make_uniq<MaintainBindData>();
	data->options.catalog = RequireCatalog(input.inputs[0]);
	if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
		data->options.table = lakemon::ParseTableRef(StringValue::Get(input.inputs[1]));
	}
	try {
		ParseNamedMaintain(input, data->options);
	} catch (const InterruptException &) {
		throw;
	} catch (const Exception &) {
		throw;
	} catch (const std::exception &ex) {
		throw InvalidInputException("lakemon: %s", lakemon::SafeWhat(ex));
	}

	names = {"step", "schema_name", "table_name", "action", "status", "files_processed", "files_created", "details"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::VARCHAR};
	return std::move(data);
}

static unique_ptr<GlobalTableFunctionState> MaintainInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<MaintainBindData>();
	auto state = make_uniq<RowState>();
	std::vector<lakemon::MaintainRow> rows;
	try {
		rows = lakemon::RunMaintain(context, bind.options);
	} catch (const InterruptException &) {
		throw;
	} catch (const Exception &ex) {
		lakemon::MaintainRow row;
		row.step = "maintain";
		row.action = "orchestrate";
		row.status = "error";
		row.details = lakemon::SafeWhat(ex);
		rows.push_back(row);
	} catch (const std::exception &ex) {
		lakemon::MaintainRow row;
		row.step = "maintain";
		row.action = "orchestrate";
		row.status = "error";
		row.details = lakemon::SafeWhat(ex);
		rows.push_back(row);
	}
	for (auto &row : rows) {
		state->rows.push_back({Value(row.step), Value(row.schema_name), Value(row.table_name), Value(row.action),
		                       Value(row.status), Value::BIGINT(row.files_processed), Value::BIGINT(row.files_created),
		                       Value(row.details)});
	}
	return std::move(state);
}

static unique_ptr<FunctionData> StatsBind(ClientContext &, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, lakemon::ColumnNameList &names) {
	auto data = make_uniq<StatsBindData>();
	data->catalog = RequireCatalog(input.inputs[0]);
	if (input.inputs.size() >= 2 && !input.inputs[1].IsNull()) {
		data->table = lakemon::ParseTableRef(StringValue::Get(input.inputs[1]));
	}
	names = {"schema_name",
	         "table_name",
	         "file_count",
	         "file_size_bytes",
	         "delete_file_count",
	         "delete_file_size_bytes",
	         "delete_count",
	         "deleted_bytes_weighted",
	         "delete_ratio",
	         "rewrite_rung",
	         "rewrite_threshold",
	         "merge_tier_hint",
	         "target_file_size",
	         "auto_compact"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::DOUBLE,  LogicalType::VARCHAR, LogicalType::DOUBLE, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BOOLEAN};
	return std::move(data);
}

static unique_ptr<GlobalTableFunctionState> StatsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<StatsBindData>();
	auto state = make_uniq<RowState>();
	std::vector<lakemon::policy::TableHint> hints;
	try {
		hints = lakemon::InventoryTables(context, bind.catalog, bind.table);
	} catch (const InterruptException &) {
		throw;
	} catch (const Exception &) {
		throw;
	} catch (const std::exception &ex) {
		throw InvalidInputException("lakemon: %s", lakemon::SafeWhat(ex));
	}
	for (auto &hint : hints) {
		state->rows.push_back({Value(hint.schema_name), Value(hint.table_name), Value::BIGINT(static_cast<int64_t>(hint.file_count)),
		                       Value::BIGINT(static_cast<int64_t>(hint.file_size_bytes)),
		                       Value::BIGINT(static_cast<int64_t>(hint.delete_file_count)),
		                       Value::BIGINT(static_cast<int64_t>(hint.delete_file_size_bytes)),
		                       Value::BIGINT(static_cast<int64_t>(hint.delete_count)),
		                       Value::BIGINT(static_cast<int64_t>(hint.deleted_bytes_weighted)), Value(hint.delete_ratio),
		                       Value(hint.rewrite_rung), Value(hint.rewrite_threshold), Value(hint.merge_tier_hint),
		                       Value(hint.target_file_size), Value::BOOLEAN(hint.auto_compact)});
	}
	return std::move(state);
}

static unique_ptr<FunctionData> PolicyBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                           lakemon::ColumnNameList &names) {
	names = {"kind", "name", "min_value", "max_value", "threshold_or_target", "notes"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return make_uniq<PolicyBindData>();
}

static unique_ptr<GlobalTableFunctionState> PolicyInit(ClientContext &, TableFunctionInitInput &) {
	auto state = make_uniq<RowState>();
	for (const auto &rung : lakemon::policy::DefaultRewriteLadder()) {
		std::ostringstream threshold;
		threshold << std::fixed << std::setprecision(2) << rung.rewrite_threshold;
		state->rows.push_back({Value("rewrite_rung"), Value(rung.name),
		                       Value(std::to_string(rung.min_delete_count) + " deletes"),
		                       Value(std::to_string(rung.min_deleted_bytes) + " deleted_bytes"),
		                       Value(threshold.str()),
		                       Value("byte_weighted; not equal-width ratio buckets")});
	}
	for (const auto &tier : lakemon::policy::DefaultMergeTiers()) {
		state->rows.push_back({Value("merge_tier"), Value(tier.name), Value(std::to_string(tier.min_file_size)),
		                       Value(std::to_string(tier.max_file_size)), Value(tier.target_file_size),
		                       Value("max_compacted_files=" + std::to_string(tier.max_compacted_files))});
	}
	return std::move(state);
}

static void VersionFun(DataChunk &, ExpressionState &, Vector &result) {
	lakemon::ReferenceScalar(result, Value("0.1.0"));
}

static void AddNamedMaintainParams(TableFunction &function) {
	function.named_parameters["dry_run"] = LogicalType::BOOLEAN;
	function.named_parameters["skip_expire"] = LogicalType::BOOLEAN;
	function.named_parameters["skip_cleanup"] = LogicalType::BOOLEAN;
	function.named_parameters["expire_older_than"] = LogicalType::VARCHAR;
	function.named_parameters["delete_older_than"] = LogicalType::VARCHAR;
	function.named_parameters["max_compacted_files"] = LogicalType::BIGINT;
}

} // namespace

void LoadInternal(ExtensionLoader &loader) {
	auto version = ScalarFunction("lakemon_version", {}, LogicalType::VARCHAR, VersionFun);
	loader.RegisterFunction(version);

	TableFunctionSet maintain("lakemon_maintain");
	TableFunction maintain_one({LogicalType::VARCHAR}, EmitRows, MaintainBind, MaintainInit);
	AddNamedMaintainParams(maintain_one);
	maintain.AddFunction(maintain_one);
	TableFunction maintain_two({LogicalType::VARCHAR, LogicalType::VARCHAR}, EmitRows, MaintainBind, MaintainInit);
	AddNamedMaintainParams(maintain_two);
	maintain.AddFunction(maintain_two);
	loader.RegisterFunction(maintain);

	TableFunctionSet stats("lakemon_table_stats");
	stats.AddFunction(TableFunction({LogicalType::VARCHAR}, EmitRows, StatsBind, StatsInit));
	stats.AddFunction(TableFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, EmitRows, StatsBind, StatsInit));
	loader.RegisterFunction(stats);

	TableFunction policy("lakemon_policy", {}, EmitRows, PolicyBind, PolicyInit);
	loader.RegisterFunction(policy);
}

void LakemonExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string LakemonExtension::Name() {
	return "lakemon";
}

std::string LakemonExtension::Version() const {
#ifdef EXT_VERSION_LAKEMON
	return EXT_VERSION_LAKEMON;
#else
	return "0.1.0";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(lakemon, loader) {
	duckdb::LoadInternal(loader);
}
}
