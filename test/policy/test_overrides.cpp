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

static FileStat MakeDirty(uint64_t id, uint64_t deletes, uint64_t records, uint64_t bytes) {
	FileStat file;
	file.schema_name = "main";
	file.table_name = "events";
	file.data_file_id = id;
	file.delete_count = deletes;
	file.record_count = records;
	file.file_size_bytes = bytes;
	return file;
}

static void TestDefaultPolicyHasBuiltins() {
	const auto policy = DefaultPolicy();
	Expect(policy.rewrite.max_rewrite_steps == 3, "default max_rewrite_steps");
	Expect(policy.rewrite.byte_budget == 0, "default byte_budget unset");
	Expect(std::abs(policy.rewrite.min_delete_ratio - 0.01) < 1e-15, "default min_delete_ratio");
	Expect(policy.tiers.size() == 3, "default tier size");
	Expect(policy.overridden_keys.empty(), "defaults have no overrides");
	Expect(!PolicyKeyOverridden(policy, kKindRewriteLadder, kLadderName), "ladder not overridden");
}

static void TestPatchRewriteSteps() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_max_rewrite_steps = true;
	patch.max_rewrite_steps = 8;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "patch max_rewrite_steps");
	Expect(error.empty(), "no patch error");
	Expect(policy.rewrite.max_rewrite_steps == 8, "max_rewrite_steps overwritten");
	Expect(policy.rewrite.byte_budget == 0, "other knobs kept");
	Expect(std::abs(policy.rewrite.min_delete_ratio - 0.01) < 1e-15, "min_delete_ratio kept");
	Expect(PolicyKeyOverridden(policy, kKindRewriteLadder, kLadderName), "ladder marked override");
}

static void TestPatchRejectsUnknownAndRange() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_max_rewrite_steps = true;
	patch.max_rewrite_steps = 4;
	std::string error;
	Expect(!ApplyPolicyPatch(policy, "rewrite_ladder", "hot", patch, error), "unknown ladder name rejected");
	Expect(error.find("unknown rewrite ladder") != std::string::npos, "unknown name message");

	error.clear();
	Expect(!ApplyPolicyPatch(policy, "rewrite_rung", "default", patch, error), "old kind rejected");

	error.clear();
	Expect(!ApplyPolicyPatch(policy, "nope", "default", patch, error), "unknown kind rejected");

	error.clear();
	patch.max_rewrite_steps = 0;
	Expect(!ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "zero steps rejected");
	Expect(error.find("max_rewrite_steps") != std::string::npos, "range message");
	Expect(policy.rewrite.max_rewrite_steps == 3, "failed patch does not keep invalid value");

	error.clear();
	PolicyFieldPatch bad_ratio;
	bad_ratio.set_min_delete_ratio = true;
	bad_ratio.min_delete_ratio = 0.0;
	Expect(!ApplyPolicyPatch(policy, "rewrite_ladder", "default", bad_ratio, error), "min_delete_ratio 0 rejected");
	Expect(error.find("min_delete_ratio") != std::string::npos, "min_delete_ratio message");
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
	rewrite.set_max_rewrite_steps = true;
	rewrite.max_rewrite_steps = 4;
	Expect(!ApplyPolicyPatch(policy, "merge_tier", "micro", rewrite, error), "ladder field on tier rejected");
}

static void TestResetAndResetAllNamed() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_max_rewrite_steps = true;
	patch.max_rewrite_steps = 8;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "REWRITE_LADDER", "DEFAULT", patch, error), "kind/name are case-insensitive");
	Expect(policy.rewrite.max_rewrite_steps == 8, "patched");

	PolicyFieldPatch reset;
	reset.reset = true;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", reset, error), "reset named");
	Expect(policy.rewrite.max_rewrite_steps == 3, "max_rewrite_steps restored");
	Expect(!PolicyKeyOverridden(policy, kKindRewriteLadder, kLadderName), "override cleared");
}

static void TestOverlayStoredRowsSkipsBad() {
	std::vector<StoredPolicyRow> rows;
	StoredPolicyRow ok = RowFromLadder(DefaultRewriteLadder());
	ok.max_rewrite_steps = 8;
	ok.byte_budget = 1024;
	rows.push_back(ok);

	StoredPolicyRow bad = ok;
	bad.name = "scorching";
	rows.push_back(bad);

	StoredPolicyRow invalid = ok;
	invalid.max_rewrite_steps = 99;
	rows.push_back(invalid);

	std::string error;
	const ActivePolicy policy = OverlayStoredRows(rows, &error);
	Expect(policy.rewrite.max_rewrite_steps == 8, "valid overlay applied");
	Expect(policy.rewrite.byte_budget == 1024, "valid knobs kept after bad rows");
	Expect(PolicyKeyOverridden(policy, kKindRewriteLadder, kLadderName), "ladder overlay flagged");
	Expect(!error.empty(), "first bad row recorded");
}

static void TestLegacyCountFloorsAreIgnored() {
	StoredPolicyRow row = RowFromLadder(DefaultRewriteLadder());
	row.high_min = 50000;
	row.medium_min = 1;
	row.low_min = 1;
	row.max_rewrite_steps = 2;
	row.min_delete_ratio = 0.05;
	std::string error;
	const ActivePolicy policy = OverlayStoredRows({row}, &error);
	Expect(error.empty(), "legacy floors do not fail overlay");
	Expect(policy.rewrite.max_rewrite_steps == 2, "ladder size still overlays");
	Expect(std::abs(policy.rewrite.min_delete_ratio - 0.05) < 1e-15, "min_delete_ratio overlays");

	const auto files = std::vector<FileStat>{
	    MakeDirty(1, 90, 100, 100),
	    MakeDirty(2, 10, 100, 100),
	};
	const auto steps = PlanRewriteRungs(files, policy.rewrite);
	Expect(steps.size() == 2, "legacy floors are not bucket keys");
	Expect(steps[0].files[0].data_file_id == 1, "worst fraction still first");
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

	FileStat high = MakeDirty(1, 90, 100, 100);
	FileStat mid = MakeDirty(2, 50, 100, 100);
	FileStat low = MakeDirty(3, 10, 100, 100);
	FileStat clean = MakeDirty(4, 0, 100, 100);
	TableHint hint = SummarizeTable({high, mid, low, clean}, policy);
	Expect(hint.rewrite_steps.size() == 3, "adaptive plan has three rungs before overlay");
	Expect(hint.rewrite_steps[0].band == "rung_1", "first planned rung");

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
	Expect(hint.rewrite_plan.find("band=catalog") != std::string::npos, "plan shows catalog step");
	Expect(hint.rewrite_plan.find("delete_threshold=0.5") != std::string::npos, "catalog plan keeps the operator threshold");
	Expect(hint.rewrite_steps[0].files.size() == 2, "collapse keeps files with DeleteRatio >= catalog value");
	bool saw_clean = false;
	bool saw_low = false;
	bool saw_high = false;
	bool saw_mid = false;
	for (const auto &file : hint.rewrite_steps[0].files) {
		if (file.data_file_id == 4) {
			saw_clean = true;
		}
		if (file.data_file_id == 3) {
			saw_low = true;
		}
		if (file.data_file_id == 1) {
			saw_high = true;
		}
		if (file.data_file_id == 2) {
			saw_mid = true;
		}
	}
	Expect(!saw_clean, "files with no delete data stay skipped after catalog collapse");
	Expect(!saw_low, "files below catalog threshold are dropped");
	Expect(saw_high && saw_mid, "files at or above catalog threshold remain");
	Expect(hint.rewrite_steps[0].planned_bytes == 200, "planned_bytes matches filtered files");
}

static void TestCatalogCollapseEmptyWhenNoFileMeetsThreshold() {
	FileStat file = MakeDirty(1, 20, 100, 100); // 0.20
	TableHint hint = SummarizeTable({file});
	Expect(!hint.rewrite_steps.empty(), "0.20 meets default min_delete_ratio");

	std::vector<OptionBinding> options;
	OptionBinding catalog;
	catalog.name = "rewrite_delete_threshold";
	catalog.value = "0.50";
	catalog.scope = "GLOBAL";
	options.push_back(catalog);
	ApplyNativeOptions(hint, options);
	Expect(hint.rewrite_steps.empty(), "catalog collapse with no matching files skips rewrite");
	Expect(std::string(hint.rewrite_plan).find("no files meet catalog") != std::string::npos,
	       "skip reason names the catalog threshold miss");
}

static void TestCatalogCollapseHonorsPriorStepCap() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_max_rewrite_steps = true;
	patch.max_rewrite_steps = 1;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "cap to one planned rung");

	FileStat high = HighChurnFile();
	FileStat medium = high;
	medium.data_file_id = 2;
	medium.delete_count = 2000;
	medium.record_count = 4000;
	medium.file_size_bytes = 4 * kMiB;
	TableHint hint = SummarizeTable({high, medium}, policy);
	Expect(hint.rewrite_steps.size() == 1, "N=1 is a single floor cut");
	Expect(std::abs(hint.rewrite_steps[0].delete_threshold -
	                std::min(DeleteRatio(high), DeleteRatio(medium))) < 1e-12,
	       "single cut uses the positive floor");

	std::vector<OptionBinding> options;
	OptionBinding catalog;
	catalog.name = "rewrite_delete_threshold";
	catalog.value = "0.25";
	catalog.scope = "GLOBAL";
	options.push_back(catalog);
	ApplyNativeOptions(hint, options);
	Expect(hint.rewrite_steps.size() == 1, "catalog is a full CALL-count override");
	Expect(hint.rewrite_steps[0].files.size() == 2, "N=1 already planned every dirty file");
	Expect(std::abs(hint.rewrite_threshold - 0.25) < 1e-9, "catalog threshold replaces the derived one");
}

static void TestEmptyPatchRejected() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	std::string error;
	Expect(!ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "empty patch rejected");
}

static void TestPatchMinDeleteRatio() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_min_delete_ratio = true;
	patch.min_delete_ratio = 0.2;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "patch min_delete_ratio");
	Expect(std::abs(policy.rewrite.min_delete_ratio - 0.2) < 1e-15, "min_delete_ratio stored");

	const auto files = std::vector<FileStat>{
	    MakeDirty(1, 90, 100, 100),
	    MakeDirty(2, 10, 100, 100), // 0.10 below 0.2
	};
	const auto hint = SummarizeTable(files, policy);
	Expect(hint.rewrite_steps.size() == 1, "file below min_delete_ratio is excluded");
	Expect(hint.rewrite_steps[0].files.size() == 1 && hint.rewrite_steps[0].files[0].data_file_id == 1,
	       "only the 0.90 file remains");
}

static void TestMaxRewriteStepsPatch() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_max_rewrite_steps = true;
	patch.max_rewrite_steps = 1;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_ladder", "default", patch, error), "cap rewrite steps");
	Expect(policy.rewrite.max_rewrite_steps == 1, "cap stored");

	const auto files = std::vector<FileStat>{
	    MakeDirty(1, 90, 100, 100),
	    MakeDirty(2, 10, 100, 100),
	};
	const auto hint = SummarizeTable(files, policy);
	Expect(hint.rewrite_steps.size() == 1, "persisted cap is applied by summarize");
	Expect(std::abs(hint.rewrite_steps[0].delete_threshold - 0.10) < 1e-12, "N=1 ends at the floor");
}

int main() {
	TestDefaultPolicyHasBuiltins();
	TestPatchRewriteSteps();
	TestPatchRejectsUnknownAndRange();
	TestPatchWrongFieldKind();
	TestResetAndResetAllNamed();
	TestOverlayStoredRowsSkipsBad();
	TestLegacyCountFloorsAreIgnored();
	TestOverriddenMergeBand();
	TestNativeOptionsCollapseAdaptiveRungs();
	TestCatalogCollapseEmptyWhenNoFileMeetsThreshold();
	TestCatalogCollapseHonorsPriorStepCap();
	TestEmptyPatchRejected();
	TestPatchMinDeleteRatio();
	TestMaxRewriteStepsPatch();
	if (TestFailures()) {
		std::cerr << TestFailures() << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "override tests ok" << std::endl;
	return 0;
}
