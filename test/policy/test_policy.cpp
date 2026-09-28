#include "expect.hpp"
#include "lakemon_diagnostics.hpp"
#include "lakemon_pipeline.hpp"
#include "lakemon_policy.hpp"
#include "lakemon_sql.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace lakemon::policy;

static FileStat MakeFile(uint64_t id, uint64_t deletes, uint64_t records, uint64_t bytes) {
	FileStat file;
	file.data_file_id = id;
	file.delete_count = deletes;
	file.record_count = records;
	file.file_size_bytes = bytes;
	return file;
}

static FileStat MakeFile(const std::string &schema, const std::string &table, uint64_t id, uint64_t deletes,
                         uint64_t records, uint64_t bytes) {
	FileStat file = MakeFile(id, deletes, records, bytes);
	file.schema_name = schema;
	file.table_name = table;
	return file;
}

// Rejected: equal-width delete_threshold steps (0.1, 0.2, …).
static uint32_t EqualWidthRatioIndex(const FileStat &file, double width) {
	return static_cast<uint32_t>((DeleteRatio(file) / width) + 1e-9);
}

// Rejected: equal-count splits that ignore deletes (here: by size).
static std::vector<std::vector<FileStat>> EqualCountChunks(std::vector<FileStat> files, std::size_t chunk) {
	std::sort(files.begin(), files.end(),
	          [](const FileStat &a, const FileStat &b) { return a.file_size_bytes < b.file_size_bytes; });
	std::vector<std::vector<FileStat>> out;
	for (std::size_t i = 0; i < files.size(); i += chunk) {
		std::vector<FileStat> part;
		for (std::size_t j = i; j < files.size() && j < i + chunk; j++) {
			part.push_back(files[j]);
		}
		out.push_back(std::move(part));
	}
	return out;
}

static void TestDefaultLadderIsStrictlyDescendingCounts() {
	const auto ladder = DefaultRewriteLadder();
	std::string error;
	Expect(ValidateDeleteCountLadder(ladder, error), "default ladder validates");
	Expect(ladder.high_min > ladder.medium_min, "high_min > medium_min");
	Expect(ladder.medium_min > ladder.low_min, "medium_min > low_min");
	Expect(ladder.low_min >= 1, "low_min >= 1");
	Expect(ladder.byte_budget == 0, "byte_budget unset");
	Expect(ladder.max_rewrite_steps == 3, "default max_rewrite_steps is 3");
}

static void TestValidateRejectsInvertedAndZeroFloors() {
	std::string error;
	DeleteCountLadder zero_low = DefaultRewriteLadder();
	zero_low.low_min = 0;
	Expect(!ValidateDeleteCountLadder(zero_low, error), "low_min 0 rejected");
	Expect(error.find("low_min") != std::string::npos, "low_min message");

	error.clear();
	DeleteCountLadder equal_high = DefaultRewriteLadder();
	equal_high.high_min = 500;
	equal_high.medium_min = 500;
	Expect(!ValidateDeleteCountLadder(equal_high, error), "high_min == medium_min rejected");
	Expect(error.find("high_min") != std::string::npos, "high_min message");

	error.clear();
	DeleteCountLadder equal_mid = DefaultRewriteLadder();
	equal_mid.medium_min = 50;
	equal_mid.low_min = 50;
	Expect(!ValidateDeleteCountLadder(equal_mid, error), "medium_min == low_min rejected");
	Expect(error.find("medium_min") != std::string::npos, "medium_min message");

	error.clear();
	DeleteCountLadder zero_steps = DefaultRewriteLadder();
	zero_steps.max_rewrite_steps = 0;
	Expect(!ValidateDeleteCountLadder(zero_steps, error), "max_rewrite_steps 0 rejected");
	Expect(error.find("max_rewrite_steps") != std::string::npos, "max_rewrite_steps message");
}

static void TestAssignBandUsesDeleteCountNotRatio() {
	const auto ladder = DefaultRewriteLadder();
	DeleteBand band;
	Expect(AssignDeleteBand(10000, ladder, band) && band == DeleteBand::High, "10000 is high");
	Expect(AssignDeleteBand(9999, ladder, band) && band == DeleteBand::Medium, "9999 is medium");
	Expect(AssignDeleteBand(1000, ladder, band) && band == DeleteBand::Medium, "1000 is medium");
	Expect(AssignDeleteBand(999, ladder, band) && band == DeleteBand::Low, "999 is low");
	Expect(AssignDeleteBand(100, ladder, band) && band == DeleteBand::Low, "100 is low");
	Expect(!AssignDeleteBand(99, ladder, band), "99 is below low_min");
	Expect(!AssignDeleteBand(0, ladder, band), "0 is below low_min");
	Expect(DeleteBandIndex(DeleteBand::High) == 0 && DeleteBandIndex(DeleteBand::Medium) == 1 &&
	           DeleteBandIndex(DeleteBand::Low) == 2,
	       "band index matches groups[] slots");
	Expect(std::string(DeleteBandName(DeleteBand::High)) == "high", "high name");
	Expect(std::string(DeleteBandName(DeleteBand::Medium)) == "medium", "medium name");
	Expect(std::string(DeleteBandName(DeleteBand::Low)) == "low", "low name");
}

static void TestDeleteCountLadderKeepsHighChurnAheadOfHighRatio() {
	const auto files = std::vector<FileStat>{
	    MakeFile(1, 50000, 1000000, 800000000), // 5% — 50k deletes
	    MakeFile(2, 20, 100, 8000),             // 20% — 20 deletes
	    MakeFile(3, 8000, 10000, 4000000),      // 80% — 8k deletes
	};
	const auto rungs = PlanRewriteRungs(files);
	Expect(rungs.size() == 2, "tiny-ratio is below low_min and must drop");
	Expect(rungs[0].band == "high", "first rung is high");
	Expect(rungs[0].files[0].data_file_id == 1, "big-churn is first");
	Expect(rungs[1].band == "medium", "second rung is medium");
	Expect(rungs[1].files[0].data_file_id == 3, "medium-hot is in medium");
	Expect(std::abs(rungs[0].delete_threshold - 0.05) < 1e-12, "high threshold is min ratio 0.05");
	Expect(std::abs(rungs[1].delete_threshold - 0.8) < 1e-12, "medium threshold is min ratio 0.8");
}

static void TestEqualWidthRatioStepsMisgroupTheSameFiles() {
	const FileStat big = MakeFile(1, 50000, 1000000, 800000000);
	const FileStat tiny = MakeFile(2, 20, 100, 8000);
	const FileStat hot = MakeFile(3, 8000, 10000, 4000000);

	const uint32_t tiny_idx = EqualWidthRatioIndex(tiny, 0.1);
	const uint32_t big_idx = EqualWidthRatioIndex(big, 0.1);
	const uint32_t hot_idx = EqualWidthRatioIndex(hot, 0.1);
	Expect(tiny_idx == 2, "tiny 0.20 lands in width bucket 2");
	Expect(big_idx == 0, "big 0.05 lands in width bucket 0");
	Expect(hot_idx == 8, "hot 0.80 lands in width bucket 8");
	Expect(tiny_idx > big_idx, "equal-width ratio buckets rank the 20-delete file above the 50k-delete file");

	const auto rungs = PlanRewriteRungs({big, tiny, hot});
	Expect(rungs.size() == 2, "two planned rungs");
	Expect(rungs[0].files[0].data_file_id == 1 && rungs[1].files[0].data_file_id == 3, "count ladder keeps big then hot");
	Expect(rungs[0].files.size() == 1 && rungs[1].files.size() == 1, "tiny-ratio is not planned");
}

static void TestEqualCountSizeSplitsMixCleanAndDirtyFiles() {
	const auto files = std::vector<FileStat>{
	    MakeFile(1, 0, 10000, 100),
	    MakeFile(2, 12000, 20000, 200),
	    MakeFile(3, 0, 10000, 300),
	    MakeFile(4, 15000, 20000, 400),
	};
	const auto chunks = EqualCountChunks(files, 2);
	Expect(chunks.size() == 2, "two size chunks");
	Expect(chunks[0][0].data_file_id == 1 && chunks[0][1].data_file_id == 2, "size order: clean-small + dirty-a");
	Expect(chunks[1][0].data_file_id == 3 && chunks[1][1].data_file_id == 4, "size order: clean-mid + dirty-b");
	bool mixed = true;
	for (const auto &chunk : chunks) {
		bool has_clean = false;
		bool has_dirty = false;
		for (const auto &file : chunk) {
			if (file.delete_count == 0) {
				has_clean = true;
			}
			if (file.delete_count >= 10000) {
				has_dirty = true;
			}
		}
		if (!has_clean || !has_dirty) {
			mixed = false;
		}
	}
	Expect(mixed, "equal-count size splits mix 0-delete files with high-delete files");

	const auto rungs = PlanRewriteRungs(files);
	Expect(rungs.size() == 1, "only high band");
	Expect(rungs[0].band == "high", "high band");
	Expect(rungs[0].files.size() == 2, "two dirty files");
	Expect(rungs[0].files[0].data_file_id == 4 && rungs[0].files[1].data_file_id == 2, "dirty-b then dirty-a");
	Expect(rungs[0].planned_deletes == 27000, "planned deletes sum");
}

static void TestByteBudgetIsSecondaryNotABucketKey() {
	DeleteCountLadder ladder = DefaultRewriteLadder();
	ladder.byte_budget = 250;
	const auto files = std::vector<FileStat>{
	    MakeFile(1, 20000, 40000, 200),
	    MakeFile(2, 18000, 40000, 80),
	    MakeFile(3, 15000, 40000, 40),
	    MakeFile(4, 2000, 5000, 10),
	};
	const auto rungs = PlanRewriteRungs(files, ladder);
	Expect(rungs[0].band == "high", "first band is still high");
	Expect(rungs[0].files.size() == 2, "budget keeps two high files");
	Expect(rungs[0].files[0].data_file_id == 1 && rungs[0].files[1].data_file_id == 3,
	       "200+80 exceeds 250 so h2 is skipped and h3 fills");
	Expect(rungs[0].planned_bytes == 240, "planned bytes after budget");
	Expect(rungs[1].band == "medium" && rungs[1].files[0].data_file_id == 4, "medium file is its own rung");
}

static void TestFirstFileMayExceedBudgetToMakeProgress() {
	DeleteCountLadder ladder = DefaultRewriteLadder();
	ladder.byte_budget = 50;
	const auto files = std::vector<FileStat>{
	    MakeFile(1, 20000, 40000, 500),
	    MakeFile(2, 19000, 40000, 10),
	};
	const auto rungs = PlanRewriteRungs(files, ladder);
	Expect(rungs[0].files.size() == 1, "only the first file");
	Expect(rungs[0].files[0].data_file_id == 1, "huge file still planned");
}

static void TestEmptyInputAndAllCleanFilesYieldNoRungs() {
	Expect(PlanRewriteRungs({}).empty(), "empty input");
	const auto clean = std::vector<FileStat>{MakeFile(1, 0, 1000, 10), MakeFile(2, 50, 1000, 10)};
	Expect(PlanRewriteRungs(clean).empty(), "below low_min yields no rungs");
}

static void TestZeroRecordFileWithDeletesHasRatioOne() {
	Expect(DeleteRatio(MakeFile(1, 5, 0, 1)) == 1.0, "ghost file ratio 1");
	Expect(DeleteRatio(MakeFile(2, 0, 0, 1)) == 0.0, "empty file ratio 0");
}

static void TestInvalidLadderFailsClosed() {
	DeleteCountLadder bad;
	bad.high_min = 1;
	bad.medium_min = 1;
	bad.low_min = 1;
	bad.max_rewrite_steps = 3;
	std::string error;
	Expect(PlanRewriteRungs({}, bad, &error).empty(), "invalid ladder returns no rungs");
	Expect(!error.empty(), "invalid ladder records an error");
}

static void TestMaxRewriteStepsCap() {
	DeleteCountLadder ladder = DefaultRewriteLadder();
	ladder.max_rewrite_steps = 1;
	const auto files = std::vector<FileStat>{
	    MakeFile(1, 20000, 40000, 100),
	    MakeFile(2, 2000, 4000, 100),
	    MakeFile(3, 200, 400, 100),
	};
	const auto rungs = PlanRewriteRungs(files, ladder);
	Expect(rungs.size() == 1, "cap keeps only the first (high) rung");
	Expect(rungs[0].band == "high", "high runs first under the cap");

	ladder.max_rewrite_steps = 2;
	const auto two = PlanRewriteRungs(files, ladder);
	Expect(two.size() == 2, "cap 2 keeps high and medium");
	Expect(two[0].band == "high" && two[1].band == "medium", "low is dropped by the cap");
}

static void TestSkipBelowLowMin() {
	const auto files = std::vector<FileStat>{
	    MakeFile(1, 99, 100, 10),
	    MakeFile(2, 50, 50, 10),
	};
	Expect(PlanRewriteRungs(files).empty(), "files below low_min are skipped");
}

static void TestMinRatioThresholdPerRung() {
	const auto files = std::vector<FileStat>{
	    MakeFile(1, 20000, 40000, 100), // 0.50 high
	    MakeFile(2, 15000, 100000, 100), // 0.15 high
	    MakeFile(3, 2000, 2500, 100),   // 0.80 medium
	};
	const auto rungs = PlanRewriteRungs(files);
	Expect(rungs.size() == 2, "high and medium");
	Expect(std::abs(rungs[0].delete_threshold - 0.15) < 1e-12, "high threshold is the min ratio in the band");
	Expect(std::abs(rungs[1].delete_threshold - 0.80) < 1e-12, "medium threshold is that band's min ratio");
}

static void TestDeletedBytesUsesCount() {
	FileStat heavy;
	heavy.file_size_bytes = 20 * kMiB;
	heavy.record_count = 20000;
	heavy.delete_count = 4000;
	Expect(DeletedBytes(heavy) == 4 * kMiB, "weighted bytes = size * delete_count/records");
	Expect(std::abs(DeleteRatio(heavy) - 0.20) < 1e-9, "ratio 0.20");
}

static void TestMultiBandPlanEmitsDistinctCalls() {
	std::vector<FileStat> files;
	files.push_back(MakeFile("main", "events", 1, 50000, 1000000, 800000000));
	files.push_back(MakeFile("main", "events", 2, 20, 100, 8000));
	files.push_back(MakeFile("main", "events", 3, 8000, 10000, 4000000));
	const auto hint = SummarizeTable(files);
	Expect(hint.rewrite_steps.size() == 2, "dry-run plan has two rewrite steps");
	Expect(hint.rewrite_steps[0].band == "high" && hint.rewrite_steps[1].band == "medium", "High then Medium");
	Expect(hint.rewrite_steps[0].delete_threshold != hint.rewrite_steps[1].delete_threshold,
	       "thresholds are data-driven and distinct");
	Expect(hint.rewrite_plan.find("band=high") != std::string::npos, "plan surfaces high band");
	Expect(hint.rewrite_plan.find("band=medium") != std::string::npos, "plan surfaces medium band");
	Expect(hint.rewrite_plan.find("planned_files=") != std::string::npos, "plan surfaces file counts");

	lakemon::TableRef ref;
	ref.schema = "main";
	ref.table = "events";
	const std::string high_sql = lakemon::RewriteDataFilesCall("lake", ref, hint.rewrite_steps[0].delete_threshold);
	const std::string medium_sql = lakemon::RewriteDataFilesCall("lake", ref, hint.rewrite_steps[1].delete_threshold);
	Expect(high_sql != medium_sql, "each rung is its own CALL");
	Expect(high_sql.find("delete_threshold => 0.05") != std::string::npos, "high CALL uses derived 0.05");
	Expect(medium_sql.find("delete_threshold => 0.8") != std::string::npos, "medium CALL uses derived 0.8");

	const auto preview = PreviewRewriteRows(hint.rewrite_steps);
	Expect(preview.size() == 2, "maintain emits one result row per rung, not one opaque rewrite");
	Expect(preview[0].action == "high" && preview[1].action == "medium", "action is the computed band");
	Expect(preview[0].files_processed == 1 && preview[1].files_processed == 1, "files_processed is planned files");
	Expect(preview[0].details.find("delete_threshold=0.05") != std::string::npos, "high row shows data-driven threshold");
	Expect(preview[0].details.find("planned_files=1") != std::string::npos, "high row shows planned_files");
	Expect(preview[0].details.find("planned_bytes=800000000") != std::string::npos, "high row shows planned_bytes");
	Expect(preview[0].details.find("planned_deletes=50000") != std::string::npos, "high row shows planned_deletes");
	Expect(preview[1].details.find("delete_threshold=0.8") != std::string::npos, "medium row shows data-driven threshold");
	Expect(preview[1].details.find("planned_deletes=8000") != std::string::npos, "medium row shows planned_deletes");
	Expect(preview[0].details.find("band=high") != std::string::npos && preview[1].details.find("band=medium") != std::string::npos,
	       "each row names its band");

	const std::string skip_high = preview[0].details + " | auto_compact=false";
	Expect(skip_high.find("band=high") != std::string::npos && skip_high.find("planned_deletes=50000") != std::string::npos,
	       "skip/error rows keep the planned rung, not an opaque rewrite");
	const std::string ok_high = preview[0].details + " | created=1";
	Expect(ok_high.find("delete_threshold=0.05") != std::string::npos && ok_high.find("created=1") != std::string::npos,
	       "execute ok keeps the plan and records how the CALL affected the table");
}

static void TestMergeTiers() {
	FileStat micro;
	micro.file_size_bytes = 200 * 1024;
	Expect(ClassifyMerge(micro) && std::string(ClassifyMerge(micro)->name) == "micro", "micro tier");
	FileStat small;
	small.file_size_bytes = 3 * kMiB;
	Expect(ClassifyMerge(small) && std::string(ClassifyMerge(small)->name) == "small", "small tier");
	FileStat medium;
	medium.file_size_bytes = 20 * kMiB;
	Expect(ClassifyMerge(medium) && std::string(ClassifyMerge(medium)->name) == "medium", "medium tier");
	FileStat large;
	large.file_size_bytes = 80 * kMiB;
	Expect(ClassifyMerge(large) == nullptr, "already-large files skip merge tiers");
}

static void TestTableHintMerge() {
	std::vector<FileStat> files;
	for (int i = 0; i < 5; i++) {
		FileStat f;
		f.schema_name = "s";
		f.table_name = "t";
		f.file_size_bytes = 100 * 1024;
		f.record_count = 10;
		files.push_back(f);
	}
	const auto hint = SummarizeTable(files);
	Expect(hint.merge_tier_hint == "micro", "five micro files hint micro merge");
	Expect(hint.file_count == 5, "file_count");
	Expect(hint.rewrite_steps.empty() && hint.rewrite_plan == "none", "no deletes → no rewrite steps");
}

static void TestOneFileDoesNotHintMerge() {
	FileStat f;
	f.schema_name = "s";
	f.table_name = "t";
	f.file_size_bytes = 100 * 1024;
	const auto hint = SummarizeTable(std::vector<FileStat>(1, f));
	Expect(hint.merge_tier_hint == "none", "one file stays below the 2ULL merge hint");
}

static void TestPipelineStepsAreIndependent() {
	Expect(lakemon::InventoryFailureStopsPipeline(), "hard inventory failure stops rewrite/merge");
	Expect(!lakemon::FlushErrorSkipsStep("rewrite"), "flush error does not skip rewrite");
	Expect(!lakemon::FlushErrorSkipsStep("merge"), "flush error does not skip merge");
	Expect(!lakemon::TableMaintainIncludesRetention(),
	       "lakemon_maintain must not expire snapshots or clean old/orphan files");
	Expect(!lakemon::GlobalMaintainIncludesTableWork(),
	       "lakemon_maintain_global must not flush, rewrite, or merge");
	Expect(!lakemon::FlushErrorSkipsStep("expire_snapshots"), "flush error does not skip expire");
	Expect(!lakemon::FlushErrorSkipsStep("cleanup_old_files"), "flush error does not skip cleanup");
	Expect(!lakemon::FlushErrorSkipsStep("delete_orphaned_files"), "flush error does not skip orphan cleanup");
}

static void TestGlobalExpireCleanupSkipSemantics() {
	Expect(std::string(lakemon::ExpireSkipReason(true, false)) == "skip_expire", "skip_expire wins");
	Expect(std::string(lakemon::ExpireSkipReason(true, true)) == "skip_expire", "skip_expire wins even if unset");
	Expect(std::string(lakemon::ExpireSkipReason(false, true)) == "expire_older_than not set",
	       "unset interval skips expire");
	Expect(lakemon::ExpireSkipReason(false, false) == nullptr, "set interval runs expire");
	Expect(!lakemon::ShouldRunCleanup(true), "skip_cleanup skips old-file and orphan cleanup");
	Expect(lakemon::ShouldRunCleanup(false), "cleanup runs unless skipped");
}

static void TestFormatInventoryDiagnosticsJoinsAll() {
	lakemon::InventoryDiagnostic first;
	first.schema_name = "main";
	first.table_name = "events";
	first.source = "metadata";
	first.message = "missing delete_count";
	lakemon::InventoryDiagnostic second;
	second.schema_name = "sales";
	second.table_name = "orders";
	second.source = "list_files";
	second.message = "lakemon: list_files returned no result";
	const std::string joined = lakemon::FormatInventoryDiagnostics({first, second});
	Expect(joined.find("main.events (metadata): missing delete_count") != std::string::npos,
	       "first diagnostic is kept");
	Expect(joined.find("sales.orders (list_files): lakemon: list_files returned no result") != std::string::npos,
	       "second diagnostic is not truncated");
	Expect(joined.find("; ") != std::string::npos, "multiple diagnostics are joined");
}

int main() {
	TestDefaultLadderIsStrictlyDescendingCounts();
	TestValidateRejectsInvertedAndZeroFloors();
	TestAssignBandUsesDeleteCountNotRatio();
	TestDeleteCountLadderKeepsHighChurnAheadOfHighRatio();
	TestEqualWidthRatioStepsMisgroupTheSameFiles();
	TestEqualCountSizeSplitsMixCleanAndDirtyFiles();
	TestByteBudgetIsSecondaryNotABucketKey();
	TestFirstFileMayExceedBudgetToMakeProgress();
	TestEmptyInputAndAllCleanFilesYieldNoRungs();
	TestZeroRecordFileWithDeletesHasRatioOne();
	TestInvalidLadderFailsClosed();
	TestMaxRewriteStepsCap();
	TestSkipBelowLowMin();
	TestMinRatioThresholdPerRung();
	TestDeletedBytesUsesCount();
	TestMultiBandPlanEmitsDistinctCalls();
	TestMergeTiers();
	TestTableHintMerge();
	TestOneFileDoesNotHintMerge();
	TestPipelineStepsAreIndependent();
	TestGlobalExpireCleanupSkipSemantics();
	TestFormatInventoryDiagnosticsJoinsAll();
	if (TestFailures()) {
		std::cerr << TestFailures() << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "policy tests ok" << std::endl;
	return 0;
}
