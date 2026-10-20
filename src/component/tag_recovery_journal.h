#pragma once

#include "util/json.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nekorox::component {

// Recovery v2 omits a physical-file link from item rows. Preserve those rows
// exactly instead of guessing which item belongs to a restored file. Patching
// string-value spans also preserves unknown fields and large integer literals
// that a parse/dump round trip through JSON's double representation could lose.
class TagRecoveryJournal final {
public:
    static constexpr std::size_t maxBytes = 4U * 1024U * 1024U;

    static std::optional<TagRecoveryJournal> parse(std::string text)
    {
        if (text.empty() || text.size() > maxBytes) return std::nullopt;
        try {
            const auto document = sonare::util::json::parse_strict(text);
            const auto* schema = document.find("schema");
            const auto* files = document.find("files");
            const auto* id = document.find("operationId");
            if (!schema || !schema->is_string() ||
                    schema->as_string() != "goden.tag-operation.v2" ||
                    !files || !files->is_array() || !id || !id->is_string()) return std::nullopt;
            TagRecoveryJournal result;
            result.text_ = std::move(text);
            result.operationId_ = id->as_string();
            const auto& raw = result.text_;
            auto root = std::size_t{0};
            if (raw.compare(0, 3, "\xef\xbb\xbf") == 0) root = 3;
            space(raw, root);
            const auto members = objectMembers(raw, {root, valueEnd(raw, root)});
            const auto array = std::find_if(members.begin(), members.end(),
                [](const auto& member) { return member.first == "files"; });
            if (array == members.end()) return std::nullopt;
            auto position = array->second.begin + 1;
            space(raw, position);
            for (const auto& value : files->as_array()) {
                const Span file{position, valueEnd(raw, position)};
                Entry entry;
                if (value.is_object()) {
                    entry.objectBegin = position;
                    for (const auto& member : objectMembers(raw, file)) {
                        if (member.first == "outcome") entry.outcome = member.second;
                        if (member.first == "detail") entry.detail = member.second;
                    }
                    const auto field = [&](const char* name) {
                        const auto* item = value.find(name);
                        return item && item->is_string() ? item->as_string() : std::string{};
                    };
                    entry.path = field("path");
                    entry.backup = field("backupPath");
                    entry.source = field("sourceSha256");
                    entry.post = field("postWriteSha256");
                    const auto outcome = field("outcome");
                    entry.eligible = outcome == "Committed" ||
                        outcome == "RecoverablePartial" || outcome == "Restored";
                }
                result.entries_.push_back(std::move(entry));
                position = file.end;
                space(raw, position);
                if (position < raw.size() && raw[position] == ',') ++position;
                space(raw, position);
            }
            return result;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool matchesOperation(std::string_view id) const noexcept { return operationId_ == id; }

    bool matchesFile(std::size_t index, std::string_view path,
        std::string_view backup, std::string_view source, std::string_view post) const noexcept
    {
        if (index >= entries_.size()) return false;
        const auto& entry = entries_[index];
        return entry.eligible && entry.outcome && source.size() == 64 && post.size() == 64 &&
            entry.path == path && entry.backup == backup && entry.source == source && entry.post == post;
    }

    bool markRestored(std::size_t index, std::string_view detail)
    {
        if (index >= entries_.size() || !entries_[index].eligible ||
                !entries_[index].outcome || detail.size() > 4096) return false;
        entries_[index].restoredDetail = std::string(detail);
        return true;
    }

    std::optional<std::string> render() const
    {
        struct Patch { Span span; std::string text; };
        std::vector<Patch> patches;
        for (const auto& entry : entries_) {
            if (!entry.restoredDetail) continue;
            patches.push_back({*entry.outcome, "\"Restored\""});
            auto detail = "\"" + sonare::util::json::escape_string(*entry.restoredDetail) + "\"";
            if (entry.detail) patches.push_back({*entry.detail, std::move(detail)});
            else patches.push_back({{entry.objectBegin + 1, entry.objectBegin + 1},
                "\"detail\":" + detail + ","});
        }
        std::sort(patches.begin(), patches.end(), [](const auto& a, const auto& b) {
            return a.span.begin < b.span.begin;
        });
        auto size = text_.size();
        std::size_t previous = 0;
        for (const auto& patch : patches) {
            if (patch.span.begin < previous || patch.span.end < patch.span.begin ||
                    patch.span.end > text_.size()) return std::nullopt;
            size -= patch.span.end - patch.span.begin;
            if (patch.text.size() > maxBytes - size) return std::nullopt;
            size += patch.text.size();
            previous = patch.span.end;
        }
        std::string output;
        output.reserve(size);
        previous = 0;
        for (const auto& patch : patches) {
            output.append(text_, previous, patch.span.begin - previous);
            output += patch.text;
            previous = patch.span.end;
        }
        output.append(text_, previous, text_.size() - previous);
        return output;
    }

private:
    struct Span { std::size_t begin = 0, end = 0; };
    struct Entry {
        std::size_t objectBegin = 0;
        std::optional<Span> outcome, detail;
        std::string path, backup, source, post;
        bool eligible = false;
        std::optional<std::string> restoredDetail;
    };

    static void space(const std::string& text, std::size_t& position) noexcept
    {
        while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
            text[position] == '\r' || text[position] == '\n')) ++position;
    }

    // Called only after strict, depth-bounded JSON validation. Walk lexical
    // spans without interpreting number literals or touching their spelling.
    static std::size_t valueEnd(const std::string& text, std::size_t begin)
    {
        auto position = begin;
        if (text.at(position) == '"') {
            for (++position; position < text.size(); ++position) {
                if (text[position] == '\\') ++position;
                else if (text[position] == '"') return position + 1;
            }
        } else if (text[position] == '{' || text[position] == '[') {
            std::size_t depth = 0;
            do {
                const auto c = text.at(position);
                if (c == '"') { position = valueEnd(text, position); continue; }
                if (c == '{' || c == '[') ++depth;
                if (c == '}' || c == ']') --depth;
                ++position;
            } while (depth);
            return position;
        } else {
            while (position < text.size() && text[position] != ',' && text[position] != '}' &&
                    text[position] != ']' && text[position] != ' ' && text[position] != '\r' &&
                    text[position] != '\n' && text[position] != '\t') ++position;
            return position;
        }
        throw std::runtime_error("Invalid validated JSON span");
    }

    static std::vector<std::pair<std::string, Span>> objectMembers(const std::string& text, Span object)
    {
        std::vector<std::pair<std::string, Span>> members;
        auto position = object.begin + 1;
        space(text, position);
        while (position < object.end - 1) {
            const auto keyEnd = valueEnd(text, position);
            auto key = sonare::util::json::parse_strict(text.substr(position, keyEnd - position)).as_string();
            position = keyEnd;
            space(text, position);
            ++position; // colon, already validated
            space(text, position);
            const Span value{position, valueEnd(text, position)};
            members.emplace_back(std::move(key), value);
            position = value.end;
            space(text, position);
            if (position < object.end - 1 && text[position] == ',') ++position;
            space(text, position);
        }
        return members;
    }

    std::string text_, operationId_;
    std::vector<Entry> entries_;
};

} // namespace nekorox::component
