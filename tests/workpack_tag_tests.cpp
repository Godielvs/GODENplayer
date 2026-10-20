#include "nekorox/tag_mutation.h"

#include <stdexcept>

void runWorkpackTagTests()
{
    nekorox::TagValueMap metadata{
        {"ALBUM", {"Old"}},
        {"TITLE", {"Individual title"}},
        {"ARTIST", {"A", "B"}},
        {"REPLAYGAIN_TRACK_GAIN", {"-7.00 dB"}},
        {"UNKNOWN_PRIVATE_FIELD", {"preserve me"}},
    };
    std::string error;
    const std::vector<nekorox::TagMutation> edits{
        {"album", nekorox::TagMutationMode::set, {"New"}},
        {"artist", nekorox::TagMutationMode::remove, {}},
        {"title", nekorox::TagMutationMode::keep, {}},
    };
    if (!nekorox::applyTagMutations(metadata, edits, error) ||
        metadata.at("ALBUM") != std::vector<std::string>{"New"} ||
        metadata.contains("ARTIST") ||
        metadata.at("TITLE") != std::vector<std::string>{"Individual title"} ||
        !metadata.contains("REPLAYGAIN_TRACK_GAIN") ||
        !metadata.contains("UNKNOWN_PRIVATE_FIELD")) {
        throw std::runtime_error("WP10 mutation did not preserve untouched fields");
    }
    if (nekorox::applyTagMutations(metadata, edits, error)) {
        throw std::runtime_error("WP10 identical Apply was not a no-op");
    }
    const std::vector<nekorox::TagMutation> invalid{
        {"ALBUM", nekorox::TagMutationMode::set, {}}};
    if (nekorox::validateTagMutations(invalid, error)) {
        throw std::runtime_error("WP10 accepted ambiguous empty Set");
    }
}
