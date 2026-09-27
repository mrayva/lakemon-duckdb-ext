#include "expect.hpp"
#include "lakemon_options.hpp"
#include "lakemon_policy.hpp"

#include <cmath>
#include <iostream>
#include <vector>

using namespace lakemon::policy;

static std::vector<OptionBinding> SampleOptions() {
	std::vector<OptionBinding> rows;
	OptionBinding global_rewrite;
	global_rewrite.name = "rewrite_delete_threshold";
	global_rewrite.value = "0.50";
	global_rewrite.scope = "GLOBAL";
	rows.push_back(global_rewrite);

	OptionBinding schema_rewrite;
	schema_rewrite.name = "rewrite_delete_threshold";
	schema_rewrite.value = "0.40";
	schema_rewrite.scope = "SCHEMA";
	schema_rewrite.scope_entry = "analytics";
	rows.push_back(schema_rewrite);

	OptionBinding table_rewrite;
	table_rewrite.name = "rewrite_delete_threshold";
	table_rewrite.value = "0.15";
	table_rewrite.scope = "TABLE";
	table_rewrite.scope_entry = "analytics.events";
	rows.push_back(table_rewrite);

	OptionBinding global_expire;
	global_expire.name = "expire_older_than";
	global_expire.value = "30 days";
	global_expire.scope = "GLOBAL";
	rows.push_back(global_expire);

	OptionBinding global_delete;
	global_delete.name = "delete_older_than";
	global_delete.value = "7 days";
	global_delete.scope = "GLOBAL";
	rows.push_back(global_delete);

	OptionBinding table_compact;
	table_compact.name = "auto_compact";
	table_compact.value = "false";
	table_compact.scope = "TABLE";
	table_compact.scope_entry = "main.skip_me";
	rows.push_back(table_compact);

	OptionBinding schema_target;
	schema_target.name = "target_file_size";
	schema_target.value = "32MB";
	schema_target.scope = "SCHEMA";
	schema_target.scope_entry = "analytics";
	rows.push_back(schema_target);
	return rows;
}

static void TestPrecedence() {
	const auto rows = SampleOptions();
	const ResolvedOption table = ResolveOption(rows, "rewrite_delete_threshold", "analytics", "events");
	Expect(table.found && table.value == "0.15" && table.scope == "TABLE", "table wins");

	const ResolvedOption schema = ResolveOption(rows, "rewrite_delete_threshold", "analytics", "other");
	Expect(schema.found && schema.value == "0.40" && schema.scope == "SCHEMA", "schema then global");

	const ResolvedOption global = ResolveOption(rows, "rewrite_delete_threshold", "main", "t");
	Expect(global.found && global.value == "0.50" && global.scope == "GLOBAL", "global fallback");

	const ResolvedOption missing = ResolveOption(rows, "parquet_compression", "main", "t");
	Expect(!missing.found, "unknown option is unset");
}

static void TestRewriteHonor() {
	ResolvedOption unset;
	Expect(EffectiveRewriteThreshold(0.15, unset) == 0.15, "unset catalog keeps ladder");

	ResolvedOption catalog;
	catalog.found = true;
	catalog.value = "0.50";
	Expect(std::abs(EffectiveRewriteThreshold(0.15, catalog) - 0.50) < 1e-9, "catalog option is the CALL threshold");

	catalog.value = "1.5";
	Expect(EffectiveRewriteThreshold(0.15, catalog) == 0.15, "out-of-range catalog value ignored");
}

static void TestIntervalsAndOverride() {
	const auto rows = SampleOptions();
	const ResolvedOption expire = ResolveOption(rows, "expire_older_than", "", "");
	Expect(EffectiveInterval(false, "", expire) == "30 days", "catalog expire when CALL unset");
	Expect(EffectiveInterval(true, "2 days", expire) == "2 days", "CALL expire overrides catalog");
	Expect(EffectiveInterval(true, "", expire).empty(), "explicit empty CALL does not fall back");

	const ResolvedOption del = ResolveOption(rows, "delete_older_than", "", "");
	Expect(EffectiveInterval(false, "", del) == "7 days", "catalog delete_older_than");
}

static void TestAutoCompact() {
	const auto rows = SampleOptions();
	const ResolvedOption skip = ResolveOption(rows, "auto_compact", "main", "skip_me");
	Expect(!EffectiveAutoCompact(skip), "table auto_compact=false");
	Expect(SkipForAutoCompact(true, false), "catalog-wide skips false");
	Expect(!SkipForAutoCompact(false, false), "explicit table still maintained");
	Expect(!SkipForAutoCompact(true, true), "true stays in the plan");

	ResolvedOption unset;
	Expect(EffectiveAutoCompact(unset), "default auto_compact is true");
}

static void TestTargetAndHintOverlay() {
	const auto rows = SampleOptions();
	const ResolvedOption target = ResolveOption(rows, "target_file_size", "analytics", "events");
	Expect(EffectiveTargetFileSize(target, "5MB") == "32MB", "schema target informs band default");
	ResolvedOption unset;
	Expect(EffectiveTargetFileSize(unset, "5MB") == "5MB", "band default when catalog unset");

	FileStat file;
	file.schema_name = "analytics";
	file.table_name = "events";
	file.data_file_id = 1;
	file.file_size_bytes = 32 * kMiB;
	file.record_count = 40000;
	file.delete_count = 12000;
	TableHint hint = SummarizeTable(std::vector<FileStat>(1, file));
	Expect(hint.rewrite_steps.size() == 1 && hint.rewrite_steps[0].band == "high", "12000 deletes → high band");
	Expect(std::abs(hint.rewrite_threshold - 0.30) < 1e-9, "derived threshold is the file ratio");
	ApplyNativeOptions(hint, rows);
	Expect(hint.rewrite_steps.size() == 1 && hint.rewrite_steps[0].band == "catalog",
	       "table catalog threshold collapses the plan");
	Expect(std::abs(hint.rewrite_threshold - 0.15) < 1e-9, "table catalog threshold 0.15");
	Expect(hint.target_file_size == "32MB", "schema target on hint");
	Expect(hint.auto_compact, "analytics.events has no auto_compact=false");
}

int main() {
	TestPrecedence();
	TestRewriteHonor();
	TestIntervalsAndOverride();
	TestAutoCompact();
	TestTargetAndHintOverlay();
	if (TestFailures()) {
		std::cerr << TestFailures() << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "option tests ok" << std::endl;
	return 0;
}
