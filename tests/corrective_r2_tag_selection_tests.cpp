#include "nekorox/tag_mutation.h"

#include <stdexcept>

void runCorrectiveR2TagSelectionTests()
{
    using namespace nekorox;
    TagDocument document;
    document.reset({
        {"one", {{"TITLE", {"One"}}, {"GENRE", {"Rock", "Math"}}}},
        {"two", {{"TITLE", {"Two"}}, {"GENRE", {"Rock"}}}},
        {"three", {{"TITLE", {"Three"}}, {"COMMENT", {"untouched"}}}},
    });
    document.select({"two"});
    std::string error;
    if (!document.applyIntent({"TITLE", TagMutationMode::set, {"Changed"}}, error))
        throw std::runtime_error("C05 single-item draft failed: " + error);
    document.select({"one", "three"});
    if (!document.applyIntent({"GENRE", TagMutationMode::set,
            {"Electronic", "Breaks"}}, error))
        throw std::runtime_error("C05 multi-item draft failed: " + error);
    if (document.dirtyIds() != std::set<std::string>({"one", "two", "three"}))
        throw std::runtime_error("C05 loaded/selected/dirty collections collapsed");
    document.select({"two"});
    const auto title = document.aggregate("title");
    if (title.mixed || title.intent != TagMutationMode::set ||
        title.values != std::vector<std::string>({"Changed"}))
        throw std::runtime_error("C05 selection change lost a per-item draft");
    document.select({"one", "three"});
    const auto genre = document.aggregate("GENRE");
    if (genre.mixed || genre.intent != TagMutationMode::set ||
        genre.values != std::vector<std::string>({"Electronic", "Breaks"}))
        throw std::runtime_error("C05 multi-value aggregate was not retained");
    const auto frozen = document.freezePlan();
    if (frozen.size() != 3 || frozen[0].itemId != "one" ||
        frozen[1].itemId != "two" || frozen[2].itemId != "three")
        throw std::runtime_error("C05 immutable plan did not target exact dirty items");
    document.select({"two"});
    if (!document.applyIntent({"TITLE", TagMutationMode::set, {"Newer"}}, error))
        throw std::runtime_error("C05 post-request draft failed");
    document.discardAppliedPlan(frozen);
    if (document.aggregate("TITLE").values != std::vector<std::string>({"Newer"}))
        throw std::runtime_error("C05 delayed completion erased a newer draft");
    document.select({"one", "three"});
    document.revertSelection();
    if (document.dirtyIds() != std::set<std::string>({"two"}))
        throw std::runtime_error("C05 selection-only revert touched another draft");
    document.revertAll();
    if (!document.freezePlan().empty())
        throw std::runtime_error("C05 revert-all did not clear pending edits");
}
