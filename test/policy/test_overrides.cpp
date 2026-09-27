#include "expect.hpp"
#include "lakemon_policy.hpp"

#include <cmath>
#include <iostream>
#include <string>

using namespace lakemon::policy;

static FileStat HighChurnFile() {
	FileStat file;
	file.schema_name = "main";
	file.table_name = "events";
	file.data_file_id = 1;
	file.file_size_bytes = 32 * kMiB;
	file.record_count = 40000;
	file.delete_count = 12000;
	return file;
}

static void TestDefaultPolicyHasBuiltins() {
	const auto policy = DefaultPolicy();
	Expect(policy.rewrite.high_min == 10000, "default high_min");
	Expect(policy.rewrite.medium_min == 1000, "default medium_min");
	Expect(policy.rewrite.low_min == 100, "default low_min");
	Expect(policy.rewrite.max_rewrite_steps == 3, "default max_rewrite_steps");
	Expect(policy.tiers.size() == 3, "default tier size");
	Expect(policy.overridden_keys.empty(), "defaults have no overrides");
	Expect(!PolicyKeyOverridden(policy, kKindRewriteLadder, kLadderName), "ladder not overridden");
}

static void TestPatchRewriteFloors() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_high_min = true;
	patch.high_min = 20000;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "patch high_min");
	Expect(error.empty(), "no patch error");
	Expect(policy.rewrite.high_min == 20000, "high_min overwritten");
	Expect(policy.rewrite.medium_min == 1000, "other floors kept");
	Expect(PolicyKeyOverridden(policy, kKindRewriteLadder, kLadderName), "ladder marked override");
}

static void TestPatchRejectsUnknownAndRange() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_high_min = true;
	patch.high_min = 20000;
	std::string error;
	Expect(!ApplyPolicyPatch(policy, "rewrite_ladder", "hot", patch, error), "unknown ladder name rejected");
	Expect(error.find("unknown rewrite ladder") != std::string::npos, "unknown name message");

	error.clear();
	Expect(!ApplyPolicyPatch(policy, "rewrite_rung", "default", patch, error), "old kind rejected");

	error.clear();
	Expect(!ApplyPolicyPatch(policy, "nope", "default", patch, error), "unknown kind rejected");

	error.clear();
	patch.high_min = 500;
	Expect(!ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "inverted floors rejected");
	Expect(error.find("high_min") != std::string::npos, "range message");
	Expect(policy.rewrite.high_min == 10000, "failed patch does not keep invalid value");
}

static void TestPatchWrongFieldKind() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_target_file_size = true;
	patch.target_file_size = "8MB";
	std::string error;
	Expect(!ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "merge field on ladder rejected");

	error.clear();
	PolicyFieldPatch rewrite;
	rewrite.set_high_min = true;
	rewrite.high_min = 20000;
	Expect(!ApplyPolicyPatch(policy, "merge_tier", "micro", rewrite, error), "ladder field on tier rejected");
}

static void TestResetAndResetAllNamed() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_low_min = true;
	patch.low_min = 50;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "REWRITE_LADDER", "DEFAULT", patch, error), "kind/name are case-insensitive");
	Expect(policy.rewrite.low_min == 50, "patched");

	PolicyFieldPatch reset;
	reset.reset = true;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", reset, error), "reset named");
	Expect(policy.rewrite.low_min == 100, "low_min restored");
	Expect(!PolicyKeyOverridden(policy, kKindRewriteLadder, kLadderName), "override cleared");
}

static void TestOverlayStoredRowsSkipsBad() {
	std::vector<StoredPolicyRow> rows;
	StoredPolicyRow ok = RowFromLadder(DefaultRewriteLadder());
	ok.high_min = 20000;
	ok.medium_min = 2000;
	ok.low_min = 200;
	rows.push_back(ok);

	StoredPolicyRow bad = ok;
	bad.name = "scorching";
	rows.push_back(bad);

	StoredPolicyRow invalid = ok;
	invalid.high_min = 1;
	invalid.medium_min = 1;
	invalid.low_min = 1;
	rows.push_back(invalid);

	std::string error;
	const ActivePolicy policy = OverlayStoredRows(rows, &error);
	Expect(policy.rewrite.high_min == 20000, "valid overlay applied");
	Expect(PolicyKeyOverridden(policy, kKindRewriteLadder, kLadderName), "ladder overlay flagged");
	Expect(policy.rewrite.medium_min == 2000, "valid floors kept after bad rows");
	Expect(!error.empty(), "first bad row recorded");
}

static void TestOverriddenLadderClassifies() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_high_min = true;
	patch.high_min = 50000;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "raise high floor");

	FileStat file = HighChurnFile();
	const auto steps = PlanRewriteRungs(std::vector<FileStat>(1, file), policy.rewrite);
	Expect(steps.size() == 1 && steps[0].band == "medium", "file no longer meets raised high floor");

	const auto hint = SummarizeTable(std::vector<FileStat>(1, file), policy);
	Expect(hint.rewrite_steps.size() == 1 && hint.rewrite_steps[0].band == "medium", "summarize uses persisted floors");
	Expect(std::abs(hint.rewrite_threshold - DeleteRatio(file)) < 1e-9, "threshold is the file's ratio");
}

static void TestOverriddenMergeBand() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_max_file_size = true;
	patch.max_file_size = 2 * kMiB;
	patch.set_target_file_size = true;
	patch.target_file_size = "8MB";
	std::string error;
	Expect(ApplyPolicyPatch(policy, "merge_tier", "micro", patch, error), "widen micro band");

	FileStat file;
	file.schema_name = "s";
	file.table_name = "t";
	file.file_size_bytes = 1500 * 1024; // 1.5 MiB — default small, override micro
	Expect(ClassifyMerge(file) && ClassifyMerge(file)->name == "small", "default band is small");
	Expect(ClassifyMerge(file, policy.tiers) && ClassifyMerge(file, policy.tiers)->name == "micro",
	       "override band is micro");

	std::vector<FileStat> files(2, file);
	const auto hint = SummarizeTable(files, policy);
	Expect(hint.merge_tier_hint == "micro", "summarize merge hint follows override");
	Expect(FindMergeTier(policy, "micro")->target_file_size == "8MB", "target persisted on tier");
}

static void TestNativeOptionsCollapseAdaptiveRungs() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_max_rewrite_steps = true;
	patch.max_rewrite_steps = 3;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "persist max_rewrite_steps");

	FileStat high = HighChurnFile();
	FileStat medium;
	medium.schema_name = "main";
	medium.table_name = "events";
	medium.data_file_id = 2;
	medium.file_size_bytes = 4 * kMiB;
	medium.record_count = 10000;
	medium.delete_count = 8000;
	TableHint hint = SummarizeTable({high, medium}, policy);
	Expect(hint.rewrite_steps.size() == 2, "adaptive plan has two rungs before overlay");
	Expect(hint.rewrite_steps[0].band == "high", "first planned band is high");

	std::vector<OptionBinding> options;
	OptionBinding catalog;
	catalog.name = "rewrite_delete_threshold";
	catalog.value = "0.50";
	catalog.scope = "GLOBAL";
	options.push_back(catalog);
	ApplyNativeOptions(hint, options);
	Expect(hint.rewrite_steps.size() == 1, "catalog option collapses to one CALL");
	Expect(hint.rewrite_steps[0].band == "catalog", "collapsed step is catalog");
	Expect(std::abs(hint.rewrite_threshold - 0.50) < 1e-9, "DuckLake option is the CALL threshold");
	Expect(hint.rewrite_plan.find("catalog threshold=") != std::string::npos, "plan shows catalog step");
}

static void TestEmptyPatchRejected() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	std::string error;
	Expect(!ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "empty patch rejected");
}

static void TestMaxRewriteStepsPatch() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_max_rewrite_steps = true;
	patch.max_rewrite_steps = 1;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "cap rewrite steps");
	Expect(policy.rewrite.max_rewrite_steps == 1, "cap stored");

	FileStat high = HighChurnFile();
	FileStat medium = high;
	medium.data_file_id = 2;
	medium.delete_count = 2000;
	medium.record_count = 4000;
	const auto hint = SummarizeTable({high, medium}, policy);
	Expect(hint.rewrite_steps.size() == 1, "persisted cap is applied by summarize");
	Expect(hint.rewrite_steps[0].band == "high", "only high survives the cap");
}

int main() {
	TestDefaultPolicyHasBuiltins();
	TestPatchRewriteFloors();
	TestPatchRejectsUnknownAndRange();
	TestPatchWrongFieldKind();
	TestResetAndResetAllNamed();
	TestOverlayStoredRowsSkipsBad();
	TestOverriddenLadderClassifies();
	TestOverriddenMergeBand();
	TestNativeOptionsCollapseAdaptiveRungs();
	TestEmptyPatchRejected();
	TestMaxRewriteStepsPatch();
	if (TestFailures()) {
		std::cerr << TestFailures() << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "override tests ok" << std::endl;
	return 0;
}
