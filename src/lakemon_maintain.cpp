#include "lakemon_maintain.hpp"

#include "lakemon_catalog.hpp"
#include "lakemon_pipeline.hpp"
#include "lakemon_policy.hpp"
#include "lakemon_store.hpp"

#include "duckdb/common/exception.hpp"

#include <exception>
#include <new>
#include <sstream>

namespace lakemon {

using duckdb::Exception;

static MaintainRow MakeRow(const std::string &step, const std::string &schema, const std::string &table,
                           const std::string &action, const std::string &status, int64_t processed, int64_t created,
                           const std::string &details) {
	MaintainRow row;
	row.step = step;
	row.schema_name = schema;
	row.table_name = table;
	row.action = action;
	row.status = status;
	row.files_processed = processed;
	row.files_created = created;
	row.details = details;
	return row;
}

static int64_t CountResultRows(duckdb::ClientContext &context, const std::string &sql) {
	auto result = RunSQL(context, sql);
	if (!result) {
		return 0;
	}
	return static_cast<int64_t>(result->RowCount());
}

static void AppendCall(std::vector<MaintainRow> &rows, duckdb::ClientContext &context, const MaintainOptions &options,
                       const std::string &step, const std::string &action, const std::string &sql,
                       const std::string &schema, const std::string &table, const std::string &details) {
	if (options.dry_run) {
		rows.push_back(MakeRow(step, schema, table, action, "planned", 0, 0, details + " | " + sql));
		return;
	}
	try {
		const int64_t created = CountResultRows(context, sql);
		rows.push_back(MakeRow(step, schema, table, action, "ok", 0, created, details));
	} catch (const duckdb::InterruptException &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw;
	} catch (const Exception &ex) {
		rows.push_back(MakeRow(step, schema, table, action, "error", 0, 0, SafeWhat(ex)));
	} catch (const std::exception &ex) {
		rows.push_back(MakeRow(step, schema, table, action, "error", 0, 0, SafeWhat(ex)));
	}
}

static bool HasStepStatus(const std::vector<MaintainRow> &rows, const std::string &step, const std::string &status) {
	for (const auto &row : rows) {
		if (row.step == step && row.status == status) {
			return true;
		}
	}
	return false;
}

// Inventory failure: status=error row(s), then stop (do not CALL rewrite/merge
// against an unread catalog). Unread per-table metadata: status=error diagnostic
// rows; continue with tables that were read. flush_inlined failure: rewrite and
// merge are skipped (status=skip). expire/cleanup are independent and still run
// after a flush/rewrite/merge error. InterruptException and bad_alloc rethrow.
// The session stays usable.
std::vector<MaintainRow> RunMaintain(duckdb::ClientContext &context, const MaintainOptions &options) {
	std::vector<MaintainRow> rows;
	std::vector<policy::FileStat> files;
	std::vector<policy::TableHint> hints;
	std::vector<InventoryDiagnostic> diagnostics;
	std::string options_error;
	std::string policy_error;
	std::vector<policy::OptionBinding> catalog_options;
	policy::ActivePolicy active = policy::DefaultPolicy();
	try {
		hints = InventoryTables(context, options.catalog, options.table, &diagnostics, &files);
		catalog_options = LoadCatalogOptions(context, options.catalog, &options_error);
		active = LoadActivePolicy(context, options.catalog, &policy_error);
	} catch (const duckdb::InterruptException &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw;
	} catch (const Exception &ex) {
		rows.push_back(MakeRow("inventory", options.table.schema, options.table.table, "catalog_select", "error", 0, 0,
		                       SafeWhat(ex)));
		return rows;
	} catch (const std::exception &ex) {
		rows.push_back(MakeRow("inventory", options.table.schema, options.table.table, "catalog_select", "error", 0, 0,
		                       SafeWhat(ex)));
		return rows;
	}

	try {
	for (const auto &diag : diagnostics) {
		rows.push_back(MakeRow("inventory", diag.schema_name, diag.table_name, diag.source, "error", 0, 0, diag.message));
	}
	if (hints.empty()) {
		if (diagnostics.empty()) {
			rows.push_back(MakeRow("inventory", options.table.schema, options.table.table, "catalog_select", "skip", 0,
			                       0, "no DuckLake tables matched"));
		}
		return rows;
	}

	rows.push_back(MakeRow("inventory", "", "", "catalog_select", "ok", static_cast<int64_t>(files.size()),
	                       static_cast<int64_t>(hints.size()), "byte_weighted rewrite ladder + merge size bands"));
	if (!options_error.empty()) {
		rows.push_back(MakeRow("catalog_options", "", "", "ducklake_options", "error", 0, 0, options_error));
	}
	if (!policy_error.empty()) {
		rows.push_back(MakeRow("lakemon_policy", "", "", "persisted_overrides", "error", 0, 0, policy_error));
	}

	const bool catalog_wide = options.table.table.empty();
	const std::string expire_interval = policy::EffectiveInterval(
	    options.expire_older_than_set, options.expire_older_than,
	    policy::ResolveOption(catalog_options, "expire_older_than", "", ""));
	const std::string delete_interval = policy::EffectiveInterval(
	    options.delete_older_than_set, options.delete_older_than,
	    policy::ResolveOption(catalog_options, "delete_older_than", "", ""));

	AppendCall(rows, context, options, "flush_inlined", "ducklake_flush_inlined_data",
	           FlushInlinedDataCall(options.catalog, options.table), options.table.schema, options.table.table,
	           "flush inlined rows before rewrite/merge");
	const bool flush_failed = HasStepStatus(rows, "flush_inlined", "error");

	for (const auto &hint : hints) {
		TableRef ref;
		ref.schema = hint.schema_name;
		ref.table = hint.table_name;
		std::ostringstream details;
		details << "rung=" << hint.rewrite_rung << " threshold=" << hint.rewrite_threshold
		        << " deleted_bytes=" << hint.deleted_bytes_weighted;
		if (flush_failed && StepDependsOnFlush("rewrite")) {
			rows.push_back(MakeRow("rewrite", hint.schema_name, hint.table_name, "byte_weighted_ladder", "skip",
			                       static_cast<int64_t>(hint.file_count), 0, SkipReasonFlushFailed()));
			continue;
		}
		if (policy::SkipForAutoCompact(catalog_wide, hint.auto_compact)) {
			details << " auto_compact=false";
			rows.push_back(MakeRow("rewrite", hint.schema_name, hint.table_name, "byte_weighted_ladder", "skip",
			                       static_cast<int64_t>(hint.file_count), 0, details.str()));
			continue;
		}
		if (hint.rewrite_rung == "none" || hint.deleted_bytes_weighted == 0) {
			rows.push_back(MakeRow("rewrite", hint.schema_name, hint.table_name, "byte_weighted_ladder", "skip",
			                       static_cast<int64_t>(hint.file_count), 0, details.str()));
			continue;
		}
		std::ostringstream sql;
		sql << "CALL ducklake_rewrite_data_files(" << QuoteString(options.catalog) << TableArg(ref)
		    << SchemaNamed(ref) << ", delete_threshold => " << hint.rewrite_threshold << ")";
		AppendCall(rows, context, options, "rewrite", "byte_weighted_ladder", sql.str(), hint.schema_name,
		           hint.table_name, details.str());
	}

	for (const auto &hint : hints) {
		TableRef ref;
		ref.schema = hint.schema_name;
		ref.table = hint.table_name;
		std::vector<policy::FileStat> table_files;
		for (const auto &file : files) {
			if (file.schema_name == hint.schema_name && file.table_name == hint.table_name) {
				table_files.push_back(file);
			}
		}
		if (flush_failed && StepDependsOnFlush("merge")) {
			rows.push_back(MakeRow("merge", hint.schema_name, hint.table_name, "adjacent_files", "skip",
			                       static_cast<int64_t>(hint.file_count), 0, SkipReasonFlushFailed()));
			continue;
		}
		if (policy::SkipForAutoCompact(catalog_wide, hint.auto_compact)) {
			rows.push_back(MakeRow("merge", hint.schema_name, hint.table_name, "auto_compact", "skip",
			                       static_cast<int64_t>(hint.file_count), 0, "auto_compact=false"));
			continue;
		}
		for (const auto &tier : active.tiers) {
			uint64_t candidates = 0;
			for (const auto &file : table_files) {
				if (policy::FileQualifiesForMergeTier(file, tier)) {
					candidates++;
				}
			}
			if (candidates < 2) {
				rows.push_back(MakeRow("merge", hint.schema_name, hint.table_name, std::string("tier_") + tier.name,
				                       "skip", static_cast<int64_t>(candidates), 0, "need at least two files in band"));
				continue;
			}
			const int64_t cap =
			    options.max_compacted_files > 0 ? options.max_compacted_files : static_cast<int64_t>(tier.max_compacted_files);
			const std::string target = policy::EffectiveTargetFileSize(
			    policy::ResolveOption(catalog_options, "target_file_size", hint.schema_name, hint.table_name),
			    tier.target_file_size.c_str());
			const bool catalog_target = !hint.target_file_size.empty();
			if (!catalog_target) {
				std::ostringstream sql;
				sql << "CALL ducklake_set_option(" << QuoteString(options.catalog) << ", 'target_file_size', "
				    << QuoteString(target) << ")";
				if (options.dry_run) {
					rows.push_back(MakeRow("merge", hint.schema_name, hint.table_name,
					                       std::string("set_target_") + tier.name, "planned", 0, 0, sql.str()));
				} else {
					try {
						RunSQL(context, sql.str());
					} catch (const duckdb::InterruptException &) {
						throw;
					} catch (const std::bad_alloc &) {
						throw;
					} catch (const Exception &ex) {
						rows.push_back(MakeRow("merge", hint.schema_name, hint.table_name,
						                       std::string("set_target_") + tier.name, "error", 0, 0, SafeWhat(ex)));
						continue;
					} catch (const std::exception &ex) {
						rows.push_back(MakeRow("merge", hint.schema_name, hint.table_name,
						                       std::string("set_target_") + tier.name, "error", 0, 0, SafeWhat(ex)));
						continue;
					}
				}
			}
			std::ostringstream merge_sql;
			merge_sql << "CALL ducklake_merge_adjacent_files(" << QuoteString(options.catalog) << TableArg(ref)
			          << SchemaNamed(ref) << ", min_file_size => " << tier.min_file_size
			          << ", max_file_size => " << tier.max_file_size << ", max_compacted_files => " << cap << ")";
			AppendCall(rows, context, options, "merge", std::string("tier_") + tier.name, merge_sql.str(),
			           hint.schema_name, hint.table_name,
			           std::string("band ") + std::to_string(tier.min_file_size) + "-" +
			               std::to_string(tier.max_file_size) + " -> " + target);
		}
	}

	if (!options.skip_expire && !expire_interval.empty()) {
		std::ostringstream sql;
		sql << "CALL ducklake_expire_snapshots(" << QuoteString(options.catalog) << ", older_than => now() - INTERVAL "
		    << QuoteString(expire_interval) << ")";
		AppendCall(rows, context, options, "expire_snapshots", "expire", sql.str(), "", "",
		           "older_than=" + expire_interval);
	} else {
		rows.push_back(MakeRow("expire_snapshots", "", "", "expire", "skip", 0, 0,
		                       options.skip_expire ? "skip_expire" : "expire_older_than not set"));
	}

	if (!options.skip_cleanup) {
		std::ostringstream sql;
		sql << "CALL ducklake_cleanup_old_files(" << QuoteString(options.catalog);
		if (!delete_interval.empty()) {
			sql << ", older_than => now() - INTERVAL " << QuoteString(delete_interval);
		} else {
			sql << ", cleanup_all => true";
		}
		sql << ")";
		AppendCall(rows, context, options, "cleanup_old_files", "cleanup", sql.str(), "", "",
		           delete_interval.empty() ? "cleanup_all" : "older_than=" + delete_interval);

		std::ostringstream orphan;
		orphan << "CALL ducklake_delete_orphaned_files(" << QuoteString(options.catalog) << ")";
		AppendCall(rows, context, options, "delete_orphaned_files", "cleanup", orphan.str(), "", "",
		           "remove unreferenced files");
	} else {
		rows.push_back(MakeRow("cleanup_old_files", "", "", "cleanup", "skip", 0, 0, "skip_cleanup"));
	}

	return rows;
	} catch (const duckdb::InterruptException &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw;
	} catch (const Exception &ex) {
		rows.push_back(MakeRow("maintain", options.table.schema, options.table.table, "orchestrate", "error", 0, 0,
		                       SafeWhat(ex)));
		return rows;
	} catch (const std::exception &ex) {
		rows.push_back(MakeRow("maintain", options.table.schema, options.table.table, "orchestrate", "error", 0, 0,
		                       SafeWhat(ex)));
		return rows;
	}
}

} // namespace lakemon
