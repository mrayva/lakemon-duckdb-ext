// Standalone policy for DuckLake maintain orchestration.
// Header-only so the adaptive rewrite ladder and merge tiers can be tested
// without DuckDB.
#pragma once

#include "lakemon_options.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace lakemon {
namespace policy {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;

static constexpr const char *kKindRewriteLadder = "rewrite_ladder";
static constexpr const char *kKindMergeTier = "merge_tier";
static constexpr const char *kLadderName = "default";
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

// Absolute delete-count floors for High → Medium → Low rungs. Thresholds are
// derived per maintain pass from the files in each band, not stored here.
struct DeleteCountLadder {
	uint64_t high_min = 10000;
	uint64_t medium_min = 1000;
	uint64_t low_min = 100;
	// 0 = unset. Size is never the bucket key; budget only trims a band.
	uint64_t byte_budget = 0;
	uint64_t max_rewrite_steps = 3;
};

enum class DeleteBand { High = 0, Medium = 1, Low = 2 };

static_assert(static_cast<int>(DeleteBand::High) == 0, "groups[0] is High");
static_assert(static_cast<int>(DeleteBand::Medium) == 1, "groups[1] is Medium");
static_assert(static_cast<int>(DeleteBand::Low) == 2, "groups[2] is Low");

inline int DeleteBandIndex(DeleteBand band) {
	return static_cast<int>(band);
}

struct MergeTier {
	std::string name;
	uint64_t min_file_size;
	uint64_t max_file_size;
	std::string target_file_size;
	uint64_t max_compacted_files;
};

// One persisted override row. A write stores the full effective ladder or tier.
struct StoredPolicyRow {
	std::string kind;
	std::string name;
	uint64_t min_file_size = 0;
	uint64_t max_file_size = 0;
	std::string target_file_size;
	uint64_t max_compacted_files = 0;
	uint64_t high_min = 0;
	uint64_t medium_min = 0;
	uint64_t low_min = 0;
	uint64_t byte_budget = 0;
	uint64_t max_rewrite_steps = 0;
};

// Sparse field patch from CALL lakemon_set_policy named parameters.
struct PolicyFieldPatch {
	bool set_high_min = false;
	uint64_t high_min = 0;
	bool set_medium_min = false;
	uint64_t medium_min = 0;
	bool set_low_min = false;
	uint64_t low_min = 0;
	bool set_byte_budget = false;
	uint64_t byte_budget = 0;
	bool set_max_rewrite_steps = false;
	uint64_t max_rewrite_steps = 0;
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
	DeleteCountLadder rewrite;
	std::vector<MergeTier> tiers;
	std::vector<std::string> overridden_keys;
};

// One rewrite CALL: files that share a delete-count band.
struct PlannedRewriteStep {
	std::string band;
	std::vector<FileStat> files;
	double delete_threshold = 0.0;
	uint64_t planned_bytes = 0;
	uint64_t planned_deletes = 0;
};

// Byte-weighted deleted payload for table stats (not a bucket key).
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

inline const char *DeleteBandName(DeleteBand band) {
	switch (band) {
	case DeleteBand::High:
		return "high";
	case DeleteBand::Medium:
		return "medium";
	case DeleteBand::Low:
		return "low";
	}
	return "none";
}

inline bool ValidateDeleteCountLadder(const DeleteCountLadder &ladder, std::string &error) {
	if (ladder.low_min < 1) {
		error = "lakemon: rewrite ladder low_min must be >= 1 (zero would include clean files)";
		return false;
	}
	if (ladder.high_min <= ladder.medium_min) {
		error = "lakemon: rewrite ladder high_min must be > medium_min";
		return false;
	}
	if (ladder.medium_min <= ladder.low_min) {
		error = "lakemon: rewrite ladder medium_min must be > low_min";
		return false;
	}
	if (ladder.max_rewrite_steps < 1) {
		error = "lakemon: max_rewrite_steps must be >= 1";
		return false;
	}
	return true;
}

inline DeleteCountLadder DefaultRewriteLadder() {
	return DeleteCountLadder();
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
	policy.rewrite = DefaultRewriteLadder();
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

// Native ducklake_rewrite_data_files delete_threshold must be in (0, 1].
// 0.0 matches every file (including those with no deletes).
inline bool PositiveRewriteThreshold(double value) noexcept {
	return value > 0.0 && value <= 1.0;
}

static constexpr const char *kSkipNonPositiveRewriteThreshold =
    "rewrite_delete_threshold<=0; use native CALL ducklake_rewrite_data_files(..., delete_threshold => 0)";

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
	return EqualsCI(kind, kKindRewriteLadder) || EqualsCI(kind, kKindMergeTier);
}

inline bool NormalizePolicyKind(const std::string &kind, std::string &canonical, std::string &error) {
	if (EqualsCI(kind, kKindRewriteLadder)) {
		canonical = kKindRewriteLadder;
		return true;
	}
	if (EqualsCI(kind, kKindMergeTier)) {
		canonical = kKindMergeTier;
		return true;
	}
	error = "lakemon: kind must be rewrite_ladder or merge_tier";
	return false;
}

inline bool IsRewriteLadderName(const std::string &name) noexcept {
	return EqualsCI(name, kLadderName);
}

inline bool PatchHasField(const PolicyFieldPatch &patch) noexcept {
	return patch.set_high_min || patch.set_medium_min || patch.set_low_min || patch.set_byte_budget ||
	       patch.set_max_rewrite_steps || patch.set_min_file_size || patch.set_max_file_size ||
	       patch.set_target_file_size || patch.set_max_compacted_files;
}

inline bool PatchHasRewriteField(const PolicyFieldPatch &patch) noexcept {
	return patch.set_high_min || patch.set_medium_min || patch.set_low_min || patch.set_byte_budget ||
	       patch.set_max_rewrite_steps;
}

inline bool PatchHasMergeField(const PolicyFieldPatch &patch) noexcept {
	return patch.set_min_file_size || patch.set_max_file_size || patch.set_target_file_size ||
	       patch.set_max_compacted_files;
}

inline bool ResetNamedPolicy(ActivePolicy &policy, const std::string &kind, const std::string &name,
                             std::string &error) {
	std::string canonical;
	if (!NormalizePolicyKind(kind, canonical, error)) {
		return false;
	}
	if (canonical == kKindRewriteLadder) {
		if (!IsRewriteLadderName(name)) {
			error = "lakemon: unknown rewrite ladder '" + name + "'";
			return false;
		}
		policy.rewrite = DefaultRewriteLadder();
		ClearOverridden(policy, canonical, kLadderName);
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

// Apply a sparse CALL patch onto the rewrite ladder or a named merge tier.
// Rewrite name must be "default". Merge names stay micro/small/medium.
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
	if (canonical == kKindRewriteLadder) {
		if (!IsRewriteLadderName(name)) {
			error = "lakemon: unknown rewrite ladder '" + name + "'";
			return false;
		}
		if (PatchHasMergeField(patch)) {
			error = "lakemon: merge-tier fields are not valid for rewrite_ladder";
			return false;
		}
		DeleteCountLadder next = policy.rewrite;
		if (patch.set_high_min) {
			next.high_min = patch.high_min;
		}
		if (patch.set_medium_min) {
			next.medium_min = patch.medium_min;
		}
		if (patch.set_low_min) {
			next.low_min = patch.low_min;
		}
		if (patch.set_byte_budget) {
			next.byte_budget = patch.byte_budget;
		}
		if (patch.set_max_rewrite_steps) {
			next.max_rewrite_steps = patch.max_rewrite_steps;
		}
		if (!ValidateDeleteCountLadder(next, error)) {
			return false;
		}
		policy.rewrite = next;
		MarkOverridden(policy, canonical, kLadderName);
		return true;
	}
	MergeTier *tier = FindMergeTier(policy, name);
	if (!tier) {
		error = "lakemon: unknown merge tier '" + name + "'";
		return false;
	}
	if (PatchHasRewriteField(patch)) {
		error = "lakemon: rewrite-ladder fields are not valid for merge_tier";
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

inline StoredPolicyRow RowFromLadder(const DeleteCountLadder &ladder) {
	StoredPolicyRow row;
	row.kind = kKindRewriteLadder;
	row.name = kLadderName;
	row.high_min = ladder.high_min;
	row.medium_min = ladder.medium_min;
	row.low_min = ladder.low_min;
	row.byte_budget = ladder.byte_budget;
	row.max_rewrite_steps = ladder.max_rewrite_steps;
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
	if (canonical == kKindRewriteLadder) {
		if (!IsRewriteLadderName(row.name)) {
			error = "lakemon: unknown rewrite ladder '" + row.name + "'";
			return false;
		}
		DeleteCountLadder next;
		next.high_min = row.high_min;
		next.medium_min = row.medium_min;
		next.low_min = row.low_min;
		next.byte_budget = row.byte_budget;
		next.max_rewrite_steps = row.max_rewrite_steps == 0 ? 3 : row.max_rewrite_steps;
		if (!ValidateDeleteCountLadder(next, error)) {
			return false;
		}
		policy.rewrite = next;
		MarkOverridden(policy, canonical, kLadderName);
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

inline bool AssignDeleteBand(uint64_t delete_count, const DeleteCountLadder &ladder, DeleteBand &out) {
	if (delete_count >= ladder.high_min) {
		out = DeleteBand::High;
		return true;
	}
	if (delete_count >= ladder.medium_min) {
		out = DeleteBand::Medium;
		return true;
	}
	if (delete_count >= ladder.low_min) {
		out = DeleteBand::Low;
		return true;
	}
	return false;
}

inline std::vector<FileStat> TakeWithinBudget(std::vector<FileStat> files, uint64_t byte_budget) {
	if (byte_budget == 0) {
		return files;
	}
	std::vector<FileStat> chosen;
	uint64_t used = 0;
	for (auto &file : files) {
		uint64_t next = used;
		if (file.file_size_bytes > ~static_cast<uint64_t>(0) - used) {
			next = ~static_cast<uint64_t>(0);
		} else {
			next = used + file.file_size_bytes;
		}
		if (chosen.empty() || next <= byte_budget) {
			used = next;
			chosen.push_back(std::move(file));
		}
	}
	return chosen;
}

inline bool FinishRewriteRung(DeleteBand band, std::vector<FileStat> files, uint64_t byte_budget,
                              PlannedRewriteStep &out) {
	if (files.empty()) {
		return false;
	}
	std::sort(files.begin(), files.end(), [](const FileStat &a, const FileStat &b) {
		if (a.delete_count != b.delete_count) {
			return a.delete_count > b.delete_count;
		}
		if (a.file_size_bytes != b.file_size_bytes) {
			return a.file_size_bytes > b.file_size_bytes;
		}
		return a.data_file_id < b.data_file_id;
	});
	files = TakeWithinBudget(std::move(files), byte_budget);
	std::vector<FileStat> positive;
	positive.reserve(files.size());
	for (auto &file : files) {
		if (DeleteRatio(file) > 0.0) {
			positive.push_back(std::move(file));
		}
	}
	files = std::move(positive);
	if (files.empty()) {
		return false;
	}
	double threshold = 1.0;
	uint64_t planned_bytes = 0;
	uint64_t planned_deletes = 0;
	for (const auto &file : files) {
		threshold = std::min(threshold, DeleteRatio(file));
		planned_bytes += file.file_size_bytes;
		planned_deletes += file.delete_count;
	}
	if (!PositiveRewriteThreshold(threshold)) {
		return false;
	}
	out.band = DeleteBandName(band);
	out.files = std::move(files);
	out.delete_threshold = threshold;
	out.planned_bytes = planned_bytes;
	out.planned_deletes = planned_deletes;
	return true;
}

// Bucket files by absolute delete_count (High → Medium → Low). Rank inside a
// band by delete_count desc, then file_size_bytes desc. Bytes never form
// buckets. Each rung's delete_threshold is the minimum delete_ratio so the
// native CALL can reach those files. Invalid ladders fail closed (no rungs).
inline std::vector<PlannedRewriteStep> PlanRewriteRungs(const std::vector<FileStat> &files,
                                                        const DeleteCountLadder &ladder,
                                                        std::string *error_out = nullptr) {
	std::string error;
	if (!ValidateDeleteCountLadder(ladder, error)) {
		if (error_out) {
			*error_out = error;
		}
		return {};
	}
	std::vector<FileStat> groups[3];
	for (const auto &file : files) {
		DeleteBand band;
		if (AssignDeleteBand(file.delete_count, ladder, band)) {
			groups[DeleteBandIndex(band)].push_back(file);
		}
	}
	const DeleteBand order[3] = {DeleteBand::High, DeleteBand::Medium, DeleteBand::Low};
	std::vector<PlannedRewriteStep> rungs;
	for (int i = 0; i < 3; i++) {
		PlannedRewriteStep step;
		if (FinishRewriteRung(order[i], std::move(groups[i]), ladder.byte_budget, step)) {
			rungs.push_back(std::move(step));
		}
	}
	if (rungs.size() > ladder.max_rewrite_steps) {
		rungs.resize(static_cast<std::vector<PlannedRewriteStep>::size_type>(ladder.max_rewrite_steps));
	}
	return rungs;
}

inline std::vector<PlannedRewriteStep> PlanRewriteRungs(const std::vector<FileStat> &files) {
	return PlanRewriteRungs(files, DefaultRewriteLadder());
}

inline std::string FormatRewriteStepDetails(const PlannedRewriteStep &step) {
	std::ostringstream out;
	out << "band=" << step.band << " delete_threshold=" << step.delete_threshold
	    << " planned_files=" << step.files.size() << " planned_bytes=" << step.planned_bytes
	    << " planned_deletes=" << step.planned_deletes;
	return out.str();
}

inline std::string FormatRewritePlan(const std::vector<PlannedRewriteStep> &steps) {
	if (steps.empty()) {
		return "none";
	}
	std::ostringstream out;
	for (std::vector<PlannedRewriteStep>::size_type i = 0; i < steps.size(); i++) {
		if (i > 0) {
			out << "; ";
		}
		out << FormatRewriteStepDetails(steps[i]);
	}
	return out.str();
}

// One maintain result row per planned rewrite CALL. Tests use this so dry_run
// / execute rows stay self-explanatory without a live DuckLake session.
struct RewriteRowPreview {
	std::string action;
	int64_t files_processed = 0;
	std::string details;
};

inline std::vector<RewriteRowPreview> PreviewRewriteRows(const std::vector<PlannedRewriteStep> &steps) {
	std::vector<RewriteRowPreview> rows;
	for (const auto &step : steps) {
		RewriteRowPreview row;
		row.action = step.band;
		row.files_processed = static_cast<int64_t>(step.files.size());
		row.details = FormatRewriteStepDetails(step);
		rows.push_back(std::move(row));
	}
	return rows;
}

inline std::string FormatRewriteLadderNotes(const DeleteCountLadder &ladder) {
	std::ostringstream out;
	out << "medium_min=" << ladder.medium_min << "; low_min=" << ladder.low_min << "; byte_budget=";
	if (ladder.byte_budget == 0) {
		out << "none";
	} else {
		out << ladder.byte_budget;
	}
	out << "; max_rewrite_steps=" << ladder.max_rewrite_steps;
	return out.str();
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
	std::vector<PlannedRewriteStep> rewrite_steps;
	std::string rewrite_plan = "none";
	double rewrite_threshold = 0.0;
	std::string merge_tier_hint = "none";
	std::string target_file_size;
	bool auto_compact = true;
};

inline void RefreshRewritePlan(TableHint &hint) {
	hint.rewrite_plan = FormatRewritePlan(hint.rewrite_steps);
	hint.rewrite_threshold = hint.rewrite_steps.empty() ? 0.0 : hint.rewrite_steps.front().delete_threshold;
}

// Overlay native DuckLake options (table → schema → global) on a table hint.
// rewrite_delete_threshold, when set in (0, 1], is a FULL override: one
// band=catalog CALL with that threshold. A catalog value <= 0 skips rewrite
// (lakemon never emits delete_threshold 0.0). max_rewrite_steps and per-rung
// byte_budget do not apply after collapse (they already shaped the planned
// rungs; collapse does not re-budget or emit extra CALLs). Files below low_min
// were filtered before collapse and stay out. auto_compact is honored by maintain.
inline void ApplyNativeOptions(TableHint &hint, const std::vector<OptionBinding> &options) {
	const ResolvedOption rewrite =
	    ResolveOption(options, "rewrite_delete_threshold", hint.schema_name, hint.table_name);
	double catalog_value = 0;
	bool skip_non_positive = false;
	if (rewrite.found && TryParseDouble(rewrite.value, catalog_value)) {
		if (catalog_value <= 0.0) {
			hint.rewrite_steps.clear();
			skip_non_positive = true;
		} else if (PositiveRewriteThreshold(catalog_value) && !hint.rewrite_steps.empty()) {
			PlannedRewriteStep step;
			step.band = "catalog";
			step.delete_threshold = catalog_value;
			for (const auto &existing : hint.rewrite_steps) {
				for (const auto &file : existing.files) {
					step.files.push_back(file);
					step.planned_bytes += file.file_size_bytes;
					step.planned_deletes += file.delete_count;
				}
			}
			hint.rewrite_steps.clear();
			hint.rewrite_steps.push_back(std::move(step));
		}
	}
	if (skip_non_positive) {
		hint.rewrite_plan = kSkipNonPositiveRewriteThreshold;
		hint.rewrite_threshold = 0.0;
	} else {
		RefreshRewritePlan(hint);
	}
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
	hint.rewrite_steps = PlanRewriteRungs(files, policy.rewrite);
	RefreshRewritePlan(hint);
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
