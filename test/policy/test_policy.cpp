#include "expect.hpp"
#include "lakemon_pipeline.hpp"
#include "lakemon_policy.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace lakemon::policy;

static void TestDeletedBytesUsesCountNotEqualBuckets() {
	FileStat heavy;
	heavy.file_size_bytes = 20 * kMiB;
	heavy.record_count = 20000;
	heavy.delete_count = 4000; // 20% ratio, but 4 MiB weighted — not enough for "hot" bytes
	Expect(DeletedBytes(heavy) == 4 * kMiB, "weighted bytes = size * delete_count/records");
	Expect(std::abs(DeleteRatio(heavy) - 0.20) < 1e-9, "ratio 0.20");

	const auto *rung = ClassifyRewrite(heavy);
	Expect(rung != nullptr, "20% + 4000 deletes classifies");
	Expect(std::string(rung->name) == "warm", "count+ratio lands on warm, not an equal 0-25% bucket");
}

static void TestHotRungByDeleteCount() {
	FileStat hot;
	hot.file_size_bytes = 64 * kMiB;
	hot.record_count = 80000;
	hot.delete_count = 16000; // 20% and 12.8 MiB weighted + 16k deletes
	const auto *rung = ClassifyRewrite(hot);
	Expect(rung != nullptr && std::string(rung->name) == "hot", "high delete_count + bytes -> hot");
}

static void TestDefaultRungTinyDeletes() {
	FileStat tiny;
	tiny.file_size_bytes = 1024;
	tiny.record_count = 100;
	tiny.delete_count = 96; // 96% but tiny payload
	const auto *rung = ClassifyRewrite(tiny);
	Expect(rung != nullptr && std::string(rung->name) == "default", "tiny high-ratio file uses default 0.95");
}

static void TestNoDeleteUnclassified() {
	FileStat clean;
	clean.file_size_bytes = 8 * kMiB;
	clean.record_count = 1000;
	clean.delete_count = 0;
	Expect(ClassifyRewrite(clean) == nullptr, "clean file has no rewrite rung");
	Expect(SelectRewriteThreshold({clean}) == 0.95, "no deleted mass keeps default threshold");
}

static void TestByteWeightedThresholdPicksHot() {
	std::vector<FileStat> files;
	FileStat hot;
	hot.schema_name = "main";
	hot.table_name = "events";
	hot.file_size_bytes = 32 * kMiB;
	hot.record_count = 40000;
	hot.delete_count = 12000;
	files.push_back(hot);
	// Many tiny high-ratio files must not outvote the hot mass via equal buckets.
	for (int i = 0; i < 20; i++) {
		FileStat crumb;
		crumb.schema_name = "main";
		crumb.table_name = "events";
		crumb.file_size_bytes = 4096;
		crumb.record_count = 50;
		crumb.delete_count = 48;
		files.push_back(crumb);
	}
	const double threshold = SelectRewriteThreshold(files);
	Expect(threshold == 0.15, "byte-weighted mass selects hot threshold, not equal-file vote");
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
}

static void TestOneFileDoesNotHintMerge() {
	FileStat f;
	f.schema_name = "s";
	f.table_name = "t";
	f.file_size_bytes = 100 * 1024;
	const auto hint = SummarizeTable(std::vector<FileStat>(1, f));
	Expect(hint.merge_tier_hint == "none", "one file stays below the 2ULL merge hint");
}

static void TestPipelineDependsOnFlush() {
	Expect(lakemon::StepDependsOnFlush("rewrite"), "rewrite depends on flush_inlined");
	Expect(lakemon::StepDependsOnFlush("merge"), "merge depends on flush_inlined");
	Expect(!lakemon::StepDependsOnFlush("expire_snapshots"), "expire is independent of flush");
	Expect(!lakemon::StepDependsOnFlush("cleanup_old_files"), "cleanup is independent of flush");
	Expect(!lakemon::StepDependsOnFlush("delete_orphaned_files"), "orphan cleanup is independent of flush");
	Expect(!lakemon::StepDependsOnFlush("inventory"), "inventory is not a flush dependent");
	Expect(std::string(lakemon::SkipReasonFlushFailed()) == "skipped: flush_inlined failed",
	       "skip reason is explicit");
}

int main() {
	TestDeletedBytesUsesCountNotEqualBuckets();
	TestHotRungByDeleteCount();
	TestDefaultRungTinyDeletes();
	TestNoDeleteUnclassified();
	TestByteWeightedThresholdPicksHot();
	TestMergeTiers();
	TestTableHintMerge();
	TestOneFileDoesNotHintMerge();
	TestPipelineDependsOnFlush();
	if (TestFailures()) {
		std::cerr << TestFailures() << " failure(s)" << std::endl;
		return 1;
	}
	std::cout << "policy tests ok" << std::endl;
	return 0;
}
