#pragma once

#include "presence_cover_policy.h"
#include "nlohmann-json/nlohmann/json.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace nekorox::component::public_covers {
using Json = nlohmann::json;

// A small bounded response must not become an unbounded/deep JSON allocation.
// Duplicate fields are rejected rather than interpreted differently by hosts.
inline Json parseJson(std::string_view body) {
    if (body.empty() || body.size() > 64U * 1024U) return Json();
    try {
        std::vector<std::set<std::string>> keys;
        return Json::parse(body.begin(), body.end(), [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 20) throw std::runtime_error("cover JSON depth");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key &&
                    (keys.empty() || !keys.back().insert(value.get<std::string>()).second))
                throw std::runtime_error("duplicate cover JSON key");
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
    } catch (...) { return Json(); }
}

inline std::string uploadedUrl(PresenceCoverProvider provider, std::string_view body) {
    try {
        const auto json = parseJson(body);
        if (!json.is_object() || !json.contains("success") || !json["success"].is_boolean() ||
                !json["success"].get<bool>()) return {};
        if (provider == PresenceCoverProvider::uguu) {
            if (!json.contains("files") || !json["files"].is_array() || json["files"].size() != 1U)
                return {};
            const auto& file = json["files"][0];
            // A deduplicated Uguu object keeps its original deletion time.
            // Acknowledgement alone cannot grant it another three hours.
            if (file.is_object() && file.contains("dupe") &&
                    (!file["dupe"].is_boolean() || file["dupe"].get<bool>())) return {};
            return file.is_object() && file.contains("url") && file["url"].is_string()
                ? file["url"].get<std::string>() : std::string{};
        }
        if (provider == PresenceCoverProvider::imgbb && json.contains("data") && json["data"].is_object()) {
            const auto& data = json["data"];
            return data.contains("url") && data["url"].is_string()
                ? data["url"].get<std::string>() : std::string{};
        }
    } catch (...) {}
    return {};
}

inline bool validMbid(std::string_view value) noexcept {
    if (value.size() != 36U) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') ||
                (value[i] >= 'A' && value[i] <= 'F'))) return false;
    }
    return true;
}

inline std::string folded(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

inline std::string matchingRelease(std::string_view body, const std::string& artist, const std::string& album) {
    try {
        const auto json = parseJson(body);
        if (!json.is_object() || !json.contains("releases") || !json["releases"].is_array()) return {};
        unsigned count = 0;
        for (const auto& release : json["releases"]) {
            if (++count > 3U) break;
            if (!release.is_object() || !release.contains("title") || !release["title"].is_string() ||
                    folded(release["title"].get<std::string>()) != folded(album) ||
                    !release.contains("artist-credit") || !release["artist-credit"].is_array()) continue;
            std::string credit;
            for (const auto& item : release["artist-credit"]) {
                if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {credit.clear();break;}
                credit += item["name"].get<std::string>();
                if (item.contains("joinphrase") && item["joinphrase"].is_string()) credit += item["joinphrase"].get<std::string>();
                if (credit.size() > 1024U) return {};
            }
            if (folded(credit) == folded(artist) && release.contains("id") && release["id"].is_string()) {
                auto id = release["id"].get<std::string>();
                if (validMbid(id)) return id;
            }
        }
    } catch (...) {}
    return {};
}
} // namespace nekorox::component::public_covers
