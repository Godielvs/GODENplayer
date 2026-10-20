#include "nekorox/tag_mutation.h"
#include "physical_file_edit_plan.h"
#include "operation_sequence.h"

#include <stdexcept>

namespace {

void check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

} // namespace

void runR3TagIntegrityTests()
{
    using namespace nekorox;
    TagDocument document;
    document.reset({{"cue#1", {{"TITLE", {"Original"}}}}});
    document.select({"cue#1"});

    std::string error;
    TagMutation invalid{"TITLE", TagMutationMode::invalidPending, {}};
    invalid.rawText = "";
    invalid.error = "Set requires at least one non-empty line";
    check(document.applyIntent(invalid, error),
        "DATA03 invalid pending draft was not retained");
    check(document.hasInvalidDrafts(),
        "DATA03 invalid pending draft did not block Apply state");
    const auto aggregate = document.aggregate("TITLE");
    check(aggregate.intent == TagMutationMode::invalidPending &&
            aggregate.error == invalid.error,
        "DATA03 invalid pending draft became an invisible stale Set");

    auto metadata = TagValueMap{{"TITLE", {"Original"}}};
    const auto blocked = document.freezePlan();
    check(blocked.size() == 1U &&
            !applyTagMutations(metadata, blocked.front().mutations, error) &&
            metadata.at("TITLE") == std::vector<std::string>{"Original"},
        "DATA03 invalid pending draft reached a physical mutation");

    check(document.applyIntent(
            {"TITLE", TagMutationMode::set, {"Frozen"}}, error),
        "DATA05 valid replacement draft failed");
    const auto frozen = document.freezePlan();
    check(frozen.size() == 1U && frozen.front().draftRevision != 0U,
        "DATA05 frozen plan has no draft revision");
    check(document.applyIntent(
            {"TITLE", TagMutationMode::set, {"Newer"}}, error),
        "DATA05 newer draft failed");
    document.discardAppliedPlan(frozen);
    check(document.aggregate("TITLE").values ==
            std::vector<std::string>{"Newer"},
        "DATA05 stale acknowledgement erased a newer draft revision");

    TagDocument partial;
    partial.reset({
        {"cue#1", {{"TITLE", {"One"}}}},
        {"cue#2", {{"TITLE", {"Two"}}}},
    });
    partial.select({"cue#1", "cue#2"});
    check(partial.applyIntent(
            {"ALBUM", TagMutationMode::set, {"Frozen album"}}, error),
        "DATA05 multi-item draft could not be frozen");
    const auto partialFrozen = partial.freezePlan();
    check(partialFrozen.size() == 2U,
        "DATA05 multi-item freeze lost an item");
    partial.discardAppliedPlan({partialFrozen.front()});
    check(partial.dirtyIds() == std::set<std::string>{"cue#2"},
        "DATA05 a partial acknowledgement cleared or retained the wrong item");

    // Apply/Undo reread the file, then refresh the cached editor baseline.
    // This must not reset selection, pending work or its revision guard.
    check(partial.refreshMetadata(0, "cue#1",
            {{"TITLE", {"One"}}, {"ALBUM", {"Frozen album"}}}),
        "TAG baseline refresh rejected the committed row");
    partial.select({"cue#1"});
    check(partial.aggregate("ALBUM").intent == TagMutationMode::keep &&
            partial.aggregate("ALBUM").values ==
                std::vector<std::string>{"Frozen album"},
        "TAG completion still displays pre-Apply metadata");
    check(partial.loaded()[1].metadata.at("TITLE") ==
            std::vector<std::string>{"Two"} &&
            partial.dirtyIds() == std::set<std::string>{"cue#2"},
        "TAG refresh changed another item's baseline or draft");
    check(partial.refreshMetadata(0, "cue#1", {{"TITLE", {"One"}}}) &&
            partial.aggregate("ALBUM").values.empty(),
        "TAG Undo/removal did not remove the previous cached field");
    const auto selectedBefore = document.selectedIds();
    const auto pendingBefore = document.freezePlan();
    check(document.refreshMetadata(0, "cue#1", {{"TITLE", {"Frozen"}},
            {"GENRE", {"Jazz", "Ambient"}}, {"UNKNOWN", {"preserved"}}}),
        "TAG refresh of a row with a newer pending draft failed");
    check(document.selectedIds() == selectedBefore &&
            document.aggregate("TITLE").values == std::vector<std::string>{"Newer"} &&
            document.freezePlan()[0].draftRevision == pendingBefore[0].draftRevision &&
            document.freezePlan()[0].mutations == pendingBefore[0].mutations,
        "TAG refresh discarded selection or a newer pending draft");
    check(!document.refreshMetadata(0, "different-id", {}) &&
            !document.refreshMetadata(1, "cue#1", {}),
        "TAG delayed refresh accepted a replaced/out-of-range row");
    document.revertAll();
    check(document.aggregate("TITLE").values == std::vector<std::string>{"Frozen"} &&
            document.aggregate("GENRE").values ==
                std::vector<std::string>({"Jazz", "Ambient"}) &&
            document.aggregate("UNKNOWN").values == std::vector<std::string>{"preserved"},
        "TAG revert does not reveal the latest verified baseline");

    nekorox::component::PhysicalFileEditPlan physical;
    physical.path = L"multitrack.cue";
    physical.bySubsong.emplace(1U,
        nekorox::component::TagDelta{
            {"TITLE", TagMutationMode::set, {"One"}}});
    physical.bySubsong.emplace(2U,
        nekorox::component::TagDelta{
            {"TITLE", TagMutationMode::set, {"Two"}}});
    check(physical.bySubsong.size() == 2U &&
            physical.bySubsong.at(1U) != physical.bySubsong.at(2U),
        "DATA01 physical plan collapsed independent subsong deltas");

    const auto firstOperation =
        nekorox::component::nextOperationSequence();
    const auto secondOperation =
        nekorox::component::nextOperationSequence();
    check(secondOperation > firstOperation,
        "DATA06 reversible operations have no common chronological order");
}
