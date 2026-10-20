#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace nekorox {

enum class TagMutationMode : unsigned char { keep, set, remove, invalidPending };

struct TagMutation {
    std::string field;
    TagMutationMode mode = TagMutationMode::keep;
    std::vector<std::string> values;
    std::string rawText;
    std::string error;
};

[[nodiscard]] inline bool operator==(const TagMutation& left,
    const TagMutation& right) noexcept
{
    return left.field == right.field && left.mode == right.mode &&
        left.values == right.values && left.rawText == right.rawText &&
        left.error == right.error;
}

using TagValueMap = std::map<std::string, std::vector<std::string>>;

struct TagDocumentItem {
    std::string itemId;
    TagValueMap metadata;
};

struct TagItemMutationPlan {
    std::string itemId;
    std::vector<TagMutation> mutations;
    std::uint64_t draftRevision = 0;
};

struct TagFieldAggregate {
    bool hasSelection = false;
    bool mixed = false;
    TagMutationMode intent = TagMutationMode::keep;
    std::vector<std::string> values;
    std::string rawText;
    std::string error;
};

// Pure edit-session model. Loaded, selected and dirty items deliberately remain
// independent so a UI selection change cannot discard or broaden pending work.
class TagDocument final {
public:
    void reset(std::vector<TagDocumentItem> items);
    void select(std::set<std::string> itemIds);
    [[nodiscard]] const std::vector<TagDocumentItem>& loaded() const noexcept;
    // Refresh a known row from verified metadata without resetting drafts or
    // selection. The ID check prevents a delayed batch updating another row.
    [[nodiscard]] bool refreshMetadata(std::size_t index,
        const std::string& itemId, TagValueMap metadata);
    [[nodiscard]] const std::set<std::string>& selectedIds() const noexcept;
    [[nodiscard]] std::set<std::string> dirtyIds() const;
    [[nodiscard]] const std::map<std::string, std::vector<TagMutation>>&
        draftsByItem() const noexcept;
    [[nodiscard]] bool hasInvalidDrafts() const noexcept;
    [[nodiscard]] std::optional<TagMutation> firstInvalidDraft() const;
    [[nodiscard]] bool applyIntent(const TagMutation& mutation,
        std::string& error);
    [[nodiscard]] TagFieldAggregate aggregate(const std::string& field) const;
    [[nodiscard]] std::vector<TagItemMutationPlan> freezePlan() const;
    void discardAppliedPlan(const std::vector<TagItemMutationPlan>& plan) noexcept;
    void revertAll() noexcept;
    void revertSelection() noexcept;

private:
    std::vector<TagDocumentItem> loaded_;
    std::set<std::string> selected_;
    std::map<std::string, std::vector<TagMutation>> drafts_;
    std::map<std::string, std::uint64_t> draftRevisions_;
    std::uint64_t nextDraftRevision_ = 0;
};

[[nodiscard]] bool validateTagMutations(
    const std::vector<TagMutation>& mutations,
    std::string& error) noexcept;

// Applies only named user fields. Unknown/technical fields already in the map
// survive untouched; multi-values remain independent values.
[[nodiscard]] bool applyTagMutations(
    TagValueMap& metadata,
    const std::vector<TagMutation>& mutations,
    std::string& error);

} // namespace nekorox
