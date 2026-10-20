#include "nekorox/tag_mutation.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace nekorox {
namespace {

std::string normalizedField(std::string value)
{
    for (auto& character : value) character = static_cast<char>(
        std::toupper(static_cast<unsigned char>(character)));
    return value;
}

bool validField(const std::string& field) noexcept
{
    if (field.empty() || field.size() > 128U) return false;
    return std::all_of(field.begin(), field.end(), [](unsigned char value) {
        return value >= 0x20U && value != 0x7fU;
    });
}

} // namespace

bool validateTagMutations(const std::vector<TagMutation>& mutations,
    std::string& error) noexcept
{
    error.clear();
    std::set<std::string> fields;
    for (const auto& mutation : mutations) {
        if (!validField(mutation.field)) {
            error = "tag field is empty or invalid";
            return false;
        }
        const auto field = normalizedField(mutation.field);
        if (!fields.insert(field).second) {
            error = "tag field is mutated more than once";
            return false;
        }
        if (mutation.mode == TagMutationMode::invalidPending) {
            error = mutation.error.empty()
                ? "tag draft is incomplete or invalid" : mutation.error;
            return false;
        }
        if (mutation.mode == TagMutationMode::keep &&
            !mutation.values.empty()) {
            error = "Keep cannot carry replacement values";
            return false;
        }
        if (mutation.mode == TagMutationMode::remove &&
            !mutation.values.empty()) {
            error = "Remove cannot carry replacement values";
            return false;
        }
        if (mutation.mode == TagMutationMode::set) {
            if (mutation.values.empty() ||
                std::any_of(mutation.values.begin(), mutation.values.end(),
                    [](const std::string& value) {
                        return value.empty() || value.size() > 64U * 1024U;
                    })) {
                error = "Set requires non-empty bounded values";
                return false;
            }
        }
    }
    return true;
}

bool applyTagMutations(TagValueMap& metadata,
    const std::vector<TagMutation>& mutations, std::string& error)
{
    if (!validateTagMutations(mutations, error)) return false;
    bool changed = false;
    for (const auto& mutation : mutations) {
        const auto field = normalizedField(mutation.field);
        if (mutation.mode == TagMutationMode::keep) continue;
        if (mutation.mode == TagMutationMode::remove) {
            changed = metadata.erase(field) != 0 || changed;
            continue;
        }
        const auto found = metadata.find(field);
        if (found == metadata.end() || found->second != mutation.values) {
            metadata[field] = mutation.values;
            changed = true;
        }
    }
    return changed;
}

void TagDocument::reset(std::vector<TagDocumentItem> items)
{
    loaded_ = std::move(items);
    selected_.clear();
    drafts_.clear();
    draftRevisions_.clear();
    nextDraftRevision_ = 0;
}

void TagDocument::select(std::set<std::string> itemIds)
{
    selected_.clear();
    for (const auto& item : loaded_) {
        if (itemIds.contains(item.itemId)) selected_.insert(item.itemId);
    }
}

const std::vector<TagDocumentItem>& TagDocument::loaded() const noexcept
{
    return loaded_;
}

bool TagDocument::refreshMetadata(std::size_t index,
    const std::string& itemId, TagValueMap metadata)
{
    if (index >= loaded_.size() || loaded_[index].itemId != itemId) return false;
    loaded_[index].metadata = std::move(metadata);
    return true;
}

const std::set<std::string>& TagDocument::selectedIds() const noexcept
{
    return selected_;
}

std::set<std::string> TagDocument::dirtyIds() const
{
    std::set<std::string> result;
    for (const auto& [itemId, mutations] : drafts_) {
        if (std::any_of(mutations.begin(), mutations.end(), [](const auto& item) {
                return item.mode != TagMutationMode::keep;
            })) result.insert(itemId);
    }
    return result;
}

const std::map<std::string, std::vector<TagMutation>>&
TagDocument::draftsByItem() const noexcept
{
    return drafts_;
}

bool TagDocument::hasInvalidDrafts() const noexcept
{
    for (const auto& [_, mutations] : drafts_) {
        if (std::any_of(mutations.begin(), mutations.end(), [](const auto& item) {
                return item.mode == TagMutationMode::invalidPending;
            })) return true;
    }
    return false;
}

std::optional<TagMutation> TagDocument::firstInvalidDraft() const
{
    for (const auto& [_, mutations] : drafts_) {
        const auto found = std::find_if(mutations.begin(), mutations.end(),
            [](const auto& item) {
                return item.mode == TagMutationMode::invalidPending;
            });
        if (found != mutations.end()) return *found;
    }
    return std::nullopt;
}

bool TagDocument::applyIntent(const TagMutation& input, std::string& error)
{
    TagMutation mutation = input;
    mutation.field = normalizedField(std::move(mutation.field));
    if (selected_.empty()) {
        error = "no tag items selected";
        return false;
    }
    if (mutation.mode == TagMutationMode::invalidPending) {
        if (!validField(mutation.field) || mutation.rawText.size() > 512U * 1024U) {
            error = "invalid pending tag draft is not bounded";
            return false;
        }
        mutation.values.clear();
        if (mutation.error.empty())
            mutation.error = "Set requires at least one non-empty value";
    } else if (!validateTagMutations(std::vector<TagMutation>{mutation}, error)) {
        return false;
    }
    for (const auto& itemId : selected_) {
        auto& mutations = drafts_[itemId];
        const auto found = std::find_if(mutations.begin(), mutations.end(),
            [&](const auto& item) {
                return normalizedField(item.field) == mutation.field;
            });
        bool changed = false;
        if (mutation.mode == TagMutationMode::keep) {
            if (found != mutations.end()) {
                mutations.erase(found);
                changed = true;
            }
        } else if (found == mutations.end()) {
            mutations.push_back(mutation);
            changed = true;
        } else {
            if (!(*found == mutation)) {
                *found = mutation;
                changed = true;
            }
        }
        if (mutations.empty()) {
            drafts_.erase(itemId);
            draftRevisions_.erase(itemId);
        } else if (changed) {
            draftRevisions_[itemId] = ++nextDraftRevision_;
        }
    }
    return true;
}

TagFieldAggregate TagDocument::aggregate(const std::string& requestedField) const
{
    TagFieldAggregate result;
    if (selected_.empty()) return result;
    result.hasSelection = true;
    const auto field = normalizedField(requestedField);
    bool first = true;
    for (const auto& item : loaded_) {
        if (!selected_.contains(item.itemId)) continue;
        TagMutationMode intent = TagMutationMode::keep;
        std::vector<std::string> values;
        std::string rawText;
        std::string intentError;
        if (const auto draft = drafts_.find(item.itemId); draft != drafts_.end()) {
            const auto found = std::find_if(draft->second.begin(), draft->second.end(),
                [&](const auto& mutation) {
                    return normalizedField(mutation.field) == field;
                });
            if (found != draft->second.end()) {
                intent = found->mode;
                values = found->values;
                rawText = found->rawText;
                intentError = found->error;
            }
        }
        if (intent == TagMutationMode::keep) {
            const auto original = item.metadata.find(field);
            if (original != item.metadata.end()) values = original->second;
        }
        if (first) {
            result.intent = intent;
            result.values = values;
            result.rawText = rawText;
            result.error = intentError;
            first = false;
        } else if (result.intent != intent || result.values != values ||
            result.rawText != rawText || result.error != intentError) {
            result.mixed = true;
        }
    }
    return result;
}

std::vector<TagItemMutationPlan> TagDocument::freezePlan() const
{
    std::vector<TagItemMutationPlan> result;
    for (const auto& item : loaded_) {
        const auto found = drafts_.find(item.itemId);
        if (found == drafts_.end() || found->second.empty()) continue;
        const auto revision = draftRevisions_.find(item.itemId);
        result.push_back({item.itemId, found->second,
            revision == draftRevisions_.end() ? 0U : revision->second});
    }
    return result;
}

void TagDocument::discardAppliedPlan(
    const std::vector<TagItemMutationPlan>& plan) noexcept
{
    for (const auto& item : plan) {
        const auto found = drafts_.find(item.itemId);
        const auto revision = draftRevisions_.find(item.itemId);
        if (found != drafts_.end() && found->second == item.mutations &&
            revision != draftRevisions_.end() &&
            revision->second == item.draftRevision) {
            drafts_.erase(found);
            draftRevisions_.erase(revision);
        }
    }
}

void TagDocument::revertAll() noexcept
{
    drafts_.clear();
    draftRevisions_.clear();
}

void TagDocument::revertSelection() noexcept
{
    for (const auto& itemId : selected_) {
        drafts_.erase(itemId);
        draftRevisions_.erase(itemId);
    }
}

} // namespace nekorox
