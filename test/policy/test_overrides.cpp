#include "expect.hpp"
#include "lakemon_policy.hpp"

#include <cmath>
#include <iostream>
#include <string>

using namespace lakemon::policy;

static FileStat HotishFile() {
	FileStat file;
	file.schema_name = "main";
	file.table_name = "events";
	file.file_size_bytes = 32 * kMiB;
	file.record_count = 40000;
	file.delete_count = 12000;
	return file;
}

static void TestDefaultPolicyHasBuiltins() {
	const auto policy = DefaultPolicy();
	Expect(policy.ladder.size() == 4, "default ladder size");
	Expect(policy.tiers.size() == 3, "default tier size");
	Expect(policy.overridden_keys.empty(), "defaults have no overrides");
	Expect(!PolicyKeyOverridden(policy, kKindRewriteRung, "hot"), "hot not overridden");
}

static void TestPatchRewriteThreshold() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_rewrite_threshold = true;
	patch.rewrite_threshold = 0.20;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_rung", "hot", patch, error), "patch hot threshold");
	Expect(error.empty(), "no patch error");
	const auto *hot = FindRewriteRung(policy, "hot");
	Expect(hot && std::abs(hot->rewrite_threshold - 0.20) < 1e-9, "hot threshold overwritten");
	Expect(hot && hot->min_delete_count == 10000, "other hot fields kept");
	Expect(PolicyKeyOverridden(policy, kKindRewriteRung, "hot"), "hot marked override");
	Expect(!PolicyKeyOverridden(policy, kKindRewriteRung, "warm"), "warm still default");
}

static void TestPatchRejectsUnknownAndRange() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_rewrite_threshold = true;
	patch.rewrite_threshold = 0.20;
	std::string error;
	Expect(!ApplyPolicyPatch(policy, "rewrite_rung", "scorching", patch, error), "unknown rung rejected");
	Expect(error.find("unknown rewrite rung") != std::string::npos, "unknown rung message");

	error.clear();
	Expect(!ApplyPolicyPatch(policy, "nope", "hot", patch, error), "unknown kind rejected");

	error.clear();
	patch.rewrite_threshold = 1.5;
	Expect(!ApplyPolicyPatch(policy, "rewrite_rung", "hot", patch, error), "out of range rejected");
	Expect(error.find("rewrite_threshold") != std::string::npos, "range message");
	Expect(FindRewriteRung(policy, "hot")->rewrite_threshold == 0.15, "failed patch does not keep invalid value");
}

static void TestPatchWrongFieldKind() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_target_file_size = true;
	patch.target_file_size = "8MB";
	std::string error;
	Expect(!ApplyPolicyPatch(policy, "rewrite_rung", "hot", patch, error), "merge field on rung rejected");

	error.clear();
	PolicyFieldPatch rewrite;
	rewrite.set_rewrite_threshold = true;
	rewrite.rewrite_threshold = 0.10;
	Expect(!ApplyPolicyPatch(policy, "merge_tier", "micro", rewrite, error), "rung field on tier rejected");
}

static void TestResetAndResetAllNamed() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_rewrite_threshold = true;
	patch.rewrite_threshold = 0.20;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "REWRITE_RUNG", "HOT", patch, error), "kind/name are case-insensitive");
	Expect(FindRewriteRung(policy, "hot")->rewrite_threshold == 0.20, "patched");

	PolicyFieldPatch reset;
	reset.reset = true;
	Expect(ApplyPolicyPatch(policy, "rewrite_rung", "hot", reset, error), "reset named");
	Expect(FindRewriteRung(policy, "hot")->rewrite_threshold == 0.15, "hot restored");
	Expect(!PolicyKeyOverridden(policy, kKindRewriteRung, "hot"), "override cleared");
}

static void TestOverlayStoredRowsSkipsBad() {
	std::vector<StoredPolicyRow> rows;
	StoredPolicyRow hot = RowFromRung(*FindRewriteRung(DefaultPolicy(), "hot"));
	hot.rewrite_threshold = 0.10;
	hot.min_delete_ratio = 0.10;
	rows.push_back(hot);

	StoredPolicyRow bad = hot;
	bad.name = "scorching";
	rows.push_back(bad);

	StoredPolicyRow invalid = hot;
	invalid.name = "warm";
	invalid.rewrite_threshold = 2.0;
	rows.push_back(invalid);

	std::string error;
	const ActivePolicy policy = OverlayStoredRows(rows, &error);
	Expect(std::abs(FindRewriteRung(policy, "hot")->rewrite_threshold - 0.10) < 1e-9, "valid overlay applied");
	Expect(PolicyKeyOverridden(policy, kKindRewriteRung, "hot"), "hot overlay flagged");
	Expect(FindRewriteRung(policy, "warm")->rewrite_threshold == 0.40, "invalid row skipped");
	Expect(!error.empty(), "first bad row recorded");
}

static void TestOverriddenLadderClassifies() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_min_delete_count = true;
	patch.min_delete_count = 50000;
	patch.set_min_deleted_bytes = true;
	patch.min_deleted_bytes = 32 * kMiB;
	patch.set_rewrite_threshold = true;
	patch.rewrite_threshold = 0.25;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_rung", "hot", patch, error), "raise hot count and byte floors");

	FileStat file = HotishFile();
	const auto *rung = ClassifyRewrite(file, policy.ladder);
	Expect(rung && rung->name == "warm", "file no longer meets raised hot floor");

	const auto hint = SummarizeTable(std::vector<FileStat>(1, file), policy);
	Expect(hint.rewrite_rung == "warm", "summarize uses persisted ladder");
	Expect(std::abs(hint.rewrite_threshold - 0.40) < 1e-9, "warm threshold after overlay");
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

static void TestNativeOptionsStillOverlayLadder() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	patch.set_rewrite_threshold = true;
	patch.rewrite_threshold = 0.20;
	std::string error;
	Expect(ApplyPolicyPatch(policy, "rewrite_rung", "hot", patch, error), "persist hot 0.20");

	FileStat file = HotishFile();
	TableHint hint = SummarizeTable(std::vector<FileStat>(1, file), policy);
	Expect(std::abs(hint.rewrite_threshold - 0.20) < 1e-9, "persisted ladder threshold");
	Expect(hint.rewrite_rung == "hot", "rung from persisted ladder");

	std::vector<OptionBinding> options;
	OptionBinding catalog;
	catalog.name = "rewrite_delete_threshold";
	catalog.value = "0.50";
	catalog.scope = "GLOBAL";
	options.push_back(catalog);
	ApplyNativeOptions(hint, options);
	Expect(std::abs(hint.rewrite_threshold - 0.50) < 1e-9, "DuckLake option still wins CALL threshold");
	Expect(hint.rewrite_rung == "hot", "rung stays ladder-classified");
}

static void TestEmptyPatchRejected() {
	ActivePolicy policy = DefaultPolicy();
	PolicyFieldPatch patch;
	std::string error;
	Expect(!ApplyPolicyPatch(policy, "rewrite_rung", "hot", patch, error), "empty patch rejected");
}

int main() {
	TestDefaultPolicyHasBuiltins();
	TestPatchRewriteThreshold();
	TestPatchRejectsUnknownAndRange();
	TestPatchWrongFieldKind();
	TestResetAndResetAllNamed();
	TestOverlayStoredRowsSkipsBad();
	TestOverriddenLadderClassifies();
	TestOverriddenMergeBand();
	TestNativeOptionsStillOverlayLadder();
	TestEmptyPatchRejected();
	if (TestFailures()) {
		std::cerr << TestFailures() << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "override tests ok" << std::endl;
	return 0;
}
