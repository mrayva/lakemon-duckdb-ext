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

static constexpr const char *kKindRewriteRung = "rewrite_rung";
static constexpr const char *kKindMergeTier = "merge_tier";
static constexpr const char *kSourceDefault = "default";
static constexpr const char *kSourceOverride = "override";

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
	std::string name;
	// First matching rung wins (most aggressive first). Not equal-width ratio buckets.
	uint64_t min_delete_count;
	uint64_t min_deleted_bytes;
	double min_delete_ratio;
	double rewrite_threshold;
};

struct MergeTier {
	std::string name;
	uint64_t min_file_size;
	uint64_t max_file_size;
	std::string target_file_size;
	uint64_t max_compacted_files;
};

// One persisted override row. A write stores the full effective rung or tier.
struct StoredPolicyRow {
	std::string kind;
	std::string name;
	uint64_t min_delete_count = 0;
	uint64_t min_deleted_bytes = 0;
	double min_delete_ratio = 0.0;
	double rewrite_threshold = 0.0;
	uint64_t min_file_size = 0;
	uint64_t max_file_size = 0;
	std::string target_file_size;
	uint64_t max_compacted_files = 0;
};

// Sparse field patch from CALL lakemon_set_policy named parameters.
struct PolicyFieldPatch {
	bool set_min_delete_count = false;
	uint64_t min_delete_count = 0;
	bool set_min_deleted_bytes = false;
	uint64_t min_deleted_bytes = 0;
	bool set_min_delete_ratio = false;
	double min_delete_ratio = 0.0;
	bool set_rewrite_threshold = false;
	double rewrite_threshold = 0.0;
	bool set_min_file_size = false;
	uint64_t min_file_size = 0;
	bool set_max_file_size = false;
	uint64_t max_file_size = 0;
	bool set_target_file_size = false;
	std::string target_file_size;
	bool set_max_compacted_files = false;
	uint64_t max_compacted_files = 0;
	bool reset = false;
};

struct ActivePolicy {
	std::vector<RewriteRung> ladder;
	std::vector<MergeTier> tiers;
	std::vector<std::string> overridden_keys;
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

inline ActivePolicy DefaultPolicy() {
	ActivePolicy policy;
	policy.ladder = DefaultRewriteLadder();
	policy.tiers = DefaultMergeTiers();
	return policy;
}

inline std::string PolicyOverrideKey(const std::string &kind, const std::string &name) {
	return kind + ":" + name;
}

inline bool PolicyKeyOverridden(const ActivePolicy &policy, const std::string &kind, const std::string &name) {
	const std::string key = PolicyOverrideKey(kind, name);
	for (const auto &entry : policy.overridden_keys) {
		if (entry == key) {
			return true;
		}
	}
	return false;
}

inline void MarkOverridden(ActivePolicy &policy, const std::string &kind, const std::string &name) {
	if (PolicyKeyOverridden(policy, kind, name)) {
		return;
	}
	policy.overridden_keys.push_back(PolicyOverrideKey(kind, name));
}

inline void ClearOverridden(ActivePolicy &policy, const std::string &kind, const std::string &name) {
	const std::string key = PolicyOverrideKey(kind, name);
	std::vector<std::string> kept;
	for (const auto &entry : policy.overridden_keys) {
		if (entry != key) {
			kept.push_back(entry);
		}
	}
	policy.overridden_keys.swap(kept);
}

inline bool InUnitInterval(double value) noexcept {
	return value >= 0.0 && value <= 1.0;
}

inline bool ValidateRewriteRung(const RewriteRung &rung, std::string &error) {
	if (rung.name.empty()) {
		error = "lakemon: rewrite rung name is required";
		return false;
	}
	if (!InUnitInterval(rung.min_delete_ratio)) {
		error = "lakemon: min_delete_ratio must be between 0 and 1";
		return false;
	}
	if (!InUnitInterval(rung.rewrite_threshold)) {
		error = "lakemon: rewrite_threshold must be between 0 and 1";
		return false;
	}
	return true;
}

inline bool ValidateMergeTier(const MergeTier &tier, std::string &error) {
	if (tier.name.empty()) {
		error = "lakemon: merge tier name is required";
		return false;
	}
	if (tier.min_file_size >= tier.max_file_size) {
		error = "lakemon: min_file_size must be < max_file_size";
		return false;
	}
	if (tier.target_file_size.empty()) {
		error = "lakemon: target_file_size is required";
		return false;
	}
	if (tier.max_compacted_files < 1) {
		error = "lakemon: max_compacted_files must be >= 1";
		return false;
	}
	return true;
}

inline RewriteRung *FindRewriteRung(ActivePolicy &policy, const std::string &name) {
	for (auto &rung : policy.ladder) {
		if (EqualsCI(rung.name, name)) {
			return &rung;
		}
	}
	return nullptr;
}

inline const RewriteRung *FindRewriteRung(const ActivePolicy &policy, const std::string &name) {
	for (const auto &rung : policy.ladder) {
		if (EqualsCI(rung.name, name)) {
			return &rung;
		}
	}
	return nullptr;
}

inline MergeTier *FindMergeTier(ActivePolicy &policy, const std::string &name) {
	for (auto &tier : policy.tiers) {
		if (EqualsCI(tier.name, name)) {
			return &tier;
		}
	}
	return nullptr;
}

inline const MergeTier *FindMergeTier(const ActivePolicy &policy, const std::string &name) {
	for (const auto &tier : policy.tiers) {
		if (EqualsCI(tier.name, name)) {
			return &tier;
		}
	}
	return nullptr;
}

inline bool KnownPolicyKind(const std::string &kind) noexcept {
	return EqualsCI(kind, kKindRewriteRung) || EqualsCI(kind, kKindMergeTier);
}

inline bool NormalizePolicyKind(const std::string &kind, std::string &canonical, std::string &error) {
	if (EqualsCI(kind, kKindRewriteRung)) {
		canonical = kKindRewriteRung;
		return true;
	}
	if (EqualsCI(kind, kKindMergeTier)) {
		canonical = kKindMergeTier;
		return true;
	}
	error = "lakemon: kind must be rewrite_rung or merge_tier";
	return false;
}

inline bool PatchHasField(const PolicyFieldPatch &patch) noexcept {
	return patch.set_min_delete_count || patch.set_min_deleted_bytes || patch.set_min_delete_ratio ||
	       patch.set_rewrite_threshold || patch.set_min_file_size || patch.set_max_file_size ||
	       patch.set_target_file_size || patch.set_max_compacted_files;
}

inline bool ResetNamedPolicy(ActivePolicy &policy, const std::string &kind, const std::string &name,
                             std::string &error) {
	std::string canonical;
	if (!NormalizePolicyKind(kind, canonical, error)) {
		return false;
	}
	if (canonical == kKindRewriteRung) {
		const RewriteRung *def = nullptr;
		for (const auto &rung : DefaultRewriteLadder()) {
			if (EqualsCI(rung.name, name)) {
				def = &rung;
				break;
			}
		}
		RewriteRung *cur = FindRewriteRung(policy, name);
		if (!def || !cur) {
			error = "lakemon: unknown rewrite rung '" + name + "'";
			return false;
		}
		*cur = *def;
		ClearOverridden(policy, canonical, cur->name);
		return true;
	}
	const MergeTier *def = nullptr;
	for (const auto &tier : DefaultMergeTiers()) {
		if (EqualsCI(tier.name, name)) {
			def = &tier;
			break;
		}
	}
	MergeTier *cur = FindMergeTier(policy, name);
	if (!def || !cur) {
		error = "lakemon: unknown merge tier '" + name + "'";
		return false;
	}
	*cur = *def;
	ClearOverridden(policy, canonical, cur->name);
	return true;
}

// Apply a sparse CALL patch onto the named built-in rung or tier. Unknown names
// are rejected so classification order stays hot/warm/cool/default and
// micro/small/medium.
inline bool ApplyPolicyPatch(ActivePolicy &policy, const std::string &kind, const std::string &name,
                             const PolicyFieldPatch &patch, std::string &error) {
	std::string canonical;
	if (!NormalizePolicyKind(kind, canonical, error)) {
		return false;
	}
	if (name.empty()) {
		error = "lakemon: policy name is required";
		return false;
	}
	if (patch.reset) {
		return ResetNamedPolicy(policy, canonical, name, error);
	}
	if (!PatchHasField(patch)) {
		error = "lakemon: set_policy requires at least one field or reset => true";
		return false;
	}
	if (canonical == kKindRewriteRung) {
		RewriteRung *rung = FindRewriteRung(policy, name);
		if (!rung) {
			error = "lakemon: unknown rewrite rung '" + name + "'";
			return false;
		}
		if (patch.set_min_file_size || patch.set_max_file_size || patch.set_target_file_size ||
		    patch.set_max_compacted_files) {
			error = "lakemon: merge-tier fields are not valid for rewrite_rung";
			return false;
		}
		RewriteRung next = *rung;
		if (patch.set_min_delete_count) {
			next.min_delete_count = patch.min_delete_count;
		}
		if (patch.set_min_deleted_bytes) {
			next.min_deleted_bytes = patch.min_deleted_bytes;
		}
		if (patch.set_min_delete_ratio) {
			next.min_delete_ratio = patch.min_delete_ratio;
		}
		if (patch.set_rewrite_threshold) {
			next.rewrite_threshold = patch.rewrite_threshold;
		}
		if (!ValidateRewriteRung(next, error)) {
			return false;
		}
		*rung = next;
		MarkOverridden(policy, canonical, rung->name);
		return true;
	}
	MergeTier *tier = FindMergeTier(policy, name);
	if (!tier) {
		error = "lakemon: unknown merge tier '" + name + "'";
		return false;
	}
	if (patch.set_min_delete_count || patch.set_min_deleted_bytes || patch.set_min_delete_ratio ||
	    patch.set_rewrite_threshold) {
		error = "lakemon: rewrite-rung fields are not valid for merge_tier";
		return false;
	}
	MergeTier next = *tier;
	if (patch.set_min_file_size) {
		next.min_file_size = patch.min_file_size;
	}
	if (patch.set_max_file_size) {
		next.max_file_size = patch.max_file_size;
	}
	if (patch.set_target_file_size) {
		next.target_file_size = patch.target_file_size;
	}
	if (patch.set_max_compacted_files) {
		next.max_compacted_files = patch.max_compacted_files;
	}
	if (!ValidateMergeTier(next, error)) {
		return false;
	}
	*tier = next;
	MarkOverridden(policy, canonical, tier->name);
	return true;
}

inline StoredPolicyRow RowFromRung(const RewriteRung &rung) {
	StoredPolicyRow row;
	row.kind = kKindRewriteRung;
	row.name = rung.name;
	row.min_delete_count = rung.min_delete_count;
	row.min_deleted_bytes = rung.min_deleted_bytes;
	row.min_delete_ratio = rung.min_delete_ratio;
	row.rewrite_threshold = rung.rewrite_threshold;
	return row;
}

inline StoredPolicyRow RowFromTier(const MergeTier &tier) {
	StoredPolicyRow row;
	row.kind = kKindMergeTier;
	row.name = tier.name;
	row.min_file_size = tier.min_file_size;
	row.max_file_size = tier.max_file_size;
	row.target_file_size = tier.target_file_size;
	row.max_compacted_files = tier.max_compacted_files;
	return row;
}

inline bool OverlayStoredRow(ActivePolicy &policy, const StoredPolicyRow &row, std::string &error) {
	std::string canonical;
	if (!NormalizePolicyKind(row.kind, canonical, error)) {
		return false;
	}
	if (canonical == kKindRewriteRung) {
		RewriteRung *rung = FindRewriteRung(policy, row.name);
		if (!rung) {
			error = "lakemon: unknown rewrite rung '" + row.name + "'";
			return false;
		}
		RewriteRung next = *rung;
		next.min_delete_count = row.min_delete_count;
		next.min_deleted_bytes = row.min_deleted_bytes;
		next.min_delete_ratio = row.min_delete_ratio;
		next.rewrite_threshold = row.rewrite_threshold;
		if (!ValidateRewriteRung(next, error)) {
			return false;
		}
		*rung = next;
		MarkOverridden(policy, canonical, rung->name);
		return true;
	}
	MergeTier *tier = FindMergeTier(policy, row.name);
	if (!tier) {
		error = "lakemon: unknown merge tier '" + row.name + "'";
		return false;
	}
	MergeTier next = *tier;
	next.min_file_size = row.min_file_size;
	next.max_file_size = row.max_file_size;
	next.target_file_size = row.target_file_size;
	next.max_compacted_files = row.max_compacted_files;
	if (!ValidateMergeTier(next, error)) {
		return false;
	}
	*tier = next;
	MarkOverridden(policy, canonical, tier->name);
	return true;
}

// Built-in defaults overlaid with persisted rows. Invalid rows are skipped
// (first error is recorded) so a bad store cannot abort classification.
inline ActivePolicy OverlayStoredRows(const std::vector<StoredPolicyRow> &rows, std::string *error_out = nullptr) {
	ActivePolicy policy = DefaultPolicy();
	for (const auto &row : rows) {
		std::string error;
		if (!OverlayStoredRow(policy, row, error)) {
			if (error_out && error_out->empty()) {
				*error_out = error;
			}
		}
	}
	return policy;
}

inline double FallbackRewriteThreshold(const std::vector<RewriteRung> &ladder) {
	if (ladder.empty()) {
		return 0.95;
	}
	return ladder.back().rewrite_threshold;
}

inline const RewriteRung *ClassifyRewrite(const FileStat &file, const std::vector<RewriteRung> &ladder) {
	const uint64_t deleted_bytes = DeletedBytes(file);
	const double ratio = DeleteRatio(file);
	for (const auto &rung : ladder) {
		const bool count_hit = file.delete_count >= rung.min_delete_count && ratio >= rung.min_delete_ratio;
		const bool byte_hit = deleted_bytes >= rung.min_deleted_bytes && ratio >= rung.min_delete_ratio;
		if (count_hit || byte_hit) {
			return &rung;
		}
	}
	return nullptr;
}

inline const RewriteRung *ClassifyRewrite(const FileStat &file) {
	return ClassifyRewrite(file, DefaultRewriteLadder());
}

// Choose one table-level rewrite threshold from byte-weighted mass, not from
// an equal-count vote across files.
inline double SelectRewriteThreshold(const std::vector<FileStat> &files, const std::vector<RewriteRung> &ladder) {
	uint64_t total_deleted = 0;
	for (const auto &file : files) {
		total_deleted += DeletedBytes(file);
	}
	if (total_deleted == 0) {
		return FallbackRewriteThreshold(ladder);
	}
	for (const auto &rung : ladder) {
		uint64_t rung_mass = 0;
		for (const auto &file : files) {
			const auto *classified = ClassifyRewrite(file, ladder);
			if (classified && classified->name == rung.name) {
				rung_mass += DeletedBytes(file);
			}
		}
		// Material rung: at least 25% of deleted bytes, or the rung's own floor.
		if (rung_mass >= rung.min_deleted_bytes && rung_mass * 4 >= total_deleted) {
			return rung.rewrite_threshold;
		}
	}
	return FallbackRewriteThreshold(ladder);
}

inline double SelectRewriteThreshold(const std::vector<FileStat> &files) {
	return SelectRewriteThreshold(files, DefaultRewriteLadder());
}

inline const MergeTier *ClassifyMerge(const FileStat &file, const std::vector<MergeTier> &tiers) {
	for (const auto &tier : tiers) {
		if (file.file_size_bytes >= tier.min_file_size && file.file_size_bytes < tier.max_file_size) {
			return &tier;
		}
	}
	return nullptr;
}

inline const MergeTier *ClassifyMerge(const FileStat &file) {
	return ClassifyMerge(file, DefaultMergeTiers());
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

inline TableHint SummarizeTable(const std::vector<FileStat> &files, const ActivePolicy &policy) {
	TableHint hint;
	if (files.empty()) {
		return hint;
	}
	hint.schema_name = files.front().schema_name;
	hint.table_name = files.front().table_name;
	uint64_t records = 0;
	std::vector<uint64_t> merge_counts(policy.tiers.size(), 0);
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
		for (std::vector<MergeTier>::size_type i = 0; i < policy.tiers.size(); i++) {
			if (FileQualifiesForMergeTier(file, policy.tiers[i])) {
				merge_counts[i]++;
				break;
			}
		}
	}
	hint.delete_ratio = records == 0 ? 0.0 : std::min(1.0, static_cast<double>(hint.delete_count) / static_cast<double>(records));
	hint.rewrite_threshold = SelectRewriteThreshold(files, policy.ladder);
	for (const auto &rung : policy.ladder) {
		if (rung.rewrite_threshold == hint.rewrite_threshold) {
			hint.rewrite_rung = rung.name;
			break;
		}
	}
	for (std::vector<MergeTier>::size_type i = 0; i < policy.tiers.size(); i++) {
		if (merge_counts[i] >= 2ULL) {
			hint.merge_tier_hint = policy.tiers[i].name;
			break;
		}
	}
	return hint;
}

inline TableHint SummarizeTable(const std::vector<FileStat> &files) {
	return SummarizeTable(files, DefaultPolicy());
}

} // namespace policy
} // namespace lakemon
