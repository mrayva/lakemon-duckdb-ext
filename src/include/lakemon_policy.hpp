// Standalone policy for DuckLake maintain orchestration.
// Header-only so the rewrite ladder and merge tiers can be tested without DuckDB.
#pragma once

#include "lakemon_options.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace lakemon {
namespace policy {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;

struct FileStat {
	std::string schema_name;
	std::string table_name;
	uint64_t data_file_id = 0;
	uint64_t file_size_bytes = 0;
	uint64_t record_count = 0;
	uint64_t delete_count = 0;
	uint64_t delete_file_size_bytes = 0;
};

struct RewriteRung {
	const char *name;
	// First matching rung wins (most aggressive first). Not equal-width ratio buckets.
	uint64_t min_delete_count;
	uint64_t min_deleted_bytes;
	double min_delete_ratio;
	double rewrite_threshold;
};

struct MergeTier {
	const char *name;
	uint64_t min_file_size;
	uint64_t max_file_size;
	const char *target_file_size;
	uint64_t max_compacted_files;
};

// Byte-weighted deleted payload: prefer actual delete_count * mean row size,
// fall back to delete-file bytes when row count is unknown.
inline uint64_t DeletedBytes(const FileStat &file) {
	if (file.record_count > 0 && file.delete_count > 0) {
		const double ratio =
		    std::min(1.0, static_cast<double>(file.delete_count) / static_cast<double>(file.record_count));
		return static_cast<uint64_t>(ratio * static_cast<double>(file.file_size_bytes));
	}
	return file.delete_file_size_bytes;
}

inline double DeleteRatio(const FileStat &file) {
	if (file.record_count == 0) {
		return file.delete_count > 0 || file.delete_file_size_bytes > 0 ? 1.0 : 0.0;
	}
	return std::min(1.0, static_cast<double>(file.delete_count) / static_cast<double>(file.record_count));
}

// Default rewrite ladder. Rungs consider delete *count* and byte-weighted mass
// instead of slicing files into equal ratio buckets (0-25 / 25-50 / ...).
inline const std::vector<RewriteRung> &DefaultRewriteLadder() {
	static const std::vector<RewriteRung> kLadder = {
	    {"hot", 10000, 8 * kMiB, 0.15, 0.15},
	    {"warm", 1000, 1 * kMiB, 0.15, 0.40},
	    {"cool", 100, 256 * 1024ULL, 0.50, 0.80},
	    {"default", 1, 1, 0.95, 0.95},
	};
	return kLadder;
}

// Merge size bands inspired by common DuckLake maintain patterns (streaming
// compaction: micro -> small -> medium).
inline const std::vector<MergeTier> &DefaultMergeTiers() {
	static const std::vector<MergeTier> kTiers = {
	    {"micro", 0, 1 * kMiB, "5MB", 64},
	    {"small", 1 * kMiB, 10 * kMiB, "32MB", 32},
	    {"medium", 10 * kMiB, 64 * kMiB, "128MB", 16},
	};
	return kTiers;
}

inline const RewriteRung *ClassifyRewrite(const FileStat &file) {
	const uint64_t deleted_bytes = DeletedBytes(file);
	const double ratio = DeleteRatio(file);
	for (const auto &rung : DefaultRewriteLadder()) {
		const bool count_hit = file.delete_count >= rung.min_delete_count && ratio >= rung.min_delete_ratio;
		const bool byte_hit = deleted_bytes >= rung.min_deleted_bytes && ratio >= rung.min_delete_ratio;
		if (count_hit || byte_hit) {
			return &rung;
		}
	}
	return nullptr;
}

// Choose one table-level rewrite threshold from byte-weighted mass, not from
// an equal-count vote across files.
inline double SelectRewriteThreshold(const std::vector<FileStat> &files) {
	uint64_t total_deleted = 0;
	for (const auto &file : files) {
		total_deleted += DeletedBytes(file);
	}
	if (total_deleted == 0) {
		return 0.95;
	}
	for (const auto &rung : DefaultRewriteLadder()) {
		uint64_t rung_mass = 0;
		for (const auto &file : files) {
			const auto *classified = ClassifyRewrite(file);
			if (classified && std::string(classified->name) == rung.name) {
				rung_mass += DeletedBytes(file);
			}
		}
		// Material rung: at least 25% of deleted bytes, or the rung's own floor.
		if (rung_mass >= rung.min_deleted_bytes && rung_mass * 4 >= total_deleted) {
			return rung.rewrite_threshold;
		}
	}
	return 0.95;
}

inline const MergeTier *ClassifyMerge(const FileStat &file) {
	for (const auto &tier : DefaultMergeTiers()) {
		if (file.file_size_bytes >= tier.min_file_size && file.file_size_bytes < tier.max_file_size) {
			return &tier;
		}
	}
	return nullptr;
}

inline bool FileQualifiesForMergeTier(const FileStat &file, const MergeTier &tier) {
	return file.file_size_bytes >= tier.min_file_size && file.file_size_bytes < tier.max_file_size;
}

struct TableHint {
	std::string schema_name;
	std::string table_name;
	uint64_t file_count = 0;
	uint64_t file_size_bytes = 0;
	uint64_t delete_file_count = 0;
	uint64_t delete_file_size_bytes = 0;
	uint64_t delete_count = 0;
	uint64_t deleted_bytes_weighted = 0;
	double delete_ratio = 0.0;
	double rewrite_threshold = 0.95;
	std::string rewrite_rung = "none";
	std::string merge_tier_hint = "none";
	std::string target_file_size;
	bool auto_compact = true;
};

// Overlay native DuckLake options (table → schema → global) on a table hint.
// Ladder rung is unchanged; rewrite_threshold becomes the effective CALL value.
inline void ApplyNativeOptions(TableHint &hint, const std::vector<OptionBinding> &options) {
	const ResolvedOption rewrite =
	    ResolveOption(options, "rewrite_delete_threshold", hint.schema_name, hint.table_name);
	hint.rewrite_threshold = EffectiveRewriteThreshold(hint.rewrite_threshold, rewrite);
	const ResolvedOption compact = ResolveOption(options, "auto_compact", hint.schema_name, hint.table_name);
	hint.auto_compact = EffectiveAutoCompact(compact);
	const ResolvedOption target = ResolveOption(options, "target_file_size", hint.schema_name, hint.table_name);
	if (target.found) {
		hint.target_file_size = target.value;
	}
}

inline TableHint SummarizeTable(const std::vector<FileStat> &files) {
	TableHint hint;
	if (files.empty()) {
		return hint;
	}
	hint.schema_name = files.front().schema_name;
	hint.table_name = files.front().table_name;
	uint64_t records = 0;
	uint64_t merge_micro = 0, merge_small = 0, merge_medium = 0;
	for (const auto &file : files) {
		hint.file_count++;
		hint.file_size_bytes += file.file_size_bytes;
		hint.delete_count += file.delete_count;
		hint.delete_file_size_bytes += file.delete_file_size_bytes;
		if (file.delete_count > 0 || file.delete_file_size_bytes > 0) {
			hint.delete_file_count++;
		}
		hint.deleted_bytes_weighted += DeletedBytes(file);
		records += file.record_count;
		if (FileQualifiesForMergeTier(file, DefaultMergeTiers()[0])) {
			merge_micro++;
		} else if (FileQualifiesForMergeTier(file, DefaultMergeTiers()[1])) {
			merge_small++;
		} else if (FileQualifiesForMergeTier(file, DefaultMergeTiers()[2])) {
			merge_medium++;
		}
	}
	hint.delete_ratio = records == 0 ? 0.0 : std::min(1.0, static_cast<double>(hint.delete_count) / static_cast<double>(records));
	hint.rewrite_threshold = SelectRewriteThreshold(files);
	for (const auto &rung : DefaultRewriteLadder()) {
		if (rung.rewrite_threshold == hint.rewrite_threshold) {
			hint.rewrite_rung = rung.name;
			break;
		}
	}
	if (merge_micro >= 2) {
		hint.merge_tier_hint = "micro";
	} else if (merge_small >= 2) {
		hint.merge_tier_hint = "small";
	} else if (merge_medium >= 2) {
		hint.merge_tier_hint = "medium";
	}
	return hint;
}

} // namespace policy
} // namespace lakemon
