#include "nekorox/album_session.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <map>
#include <numeric>
#include <random>
#include <tuple>

namespace nekorox {
namespace {

std::wstring folded(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return value;
}

bool sameText(const std::wstring& left, const std::wstring& right)
{
    return folded(left) == folded(right);
}

bool discFolder(const std::wstring& name)
{
    const auto value = folded(name);
    std::size_t prefix = std::wstring::npos;
    if (value.starts_with(L"cd")) prefix = 2U;
    else if (value.starts_with(L"disc")) prefix = 4U;
    else if (value.starts_with(L"disk")) prefix = 4U;
    if (prefix == std::wstring::npos) return false;
    while (prefix < value.size() &&
            (value[prefix] == L' ' || value[prefix] == L'-' ||
                value[prefix] == L'_')) {
        ++prefix;
    }
    if (prefix == value.size()) return false;
    return std::all_of(value.begin() + prefix, value.end(),
        [](wchar_t character) { return std::iswdigit(character) != 0; });
}

int sortableNumber(int value) noexcept
{
    return value > 0 ? value : 0x3fffffff;
}

} // namespace

std::wstring albumEditionRoot(const std::wstring& path)
{
    if (path.empty()) return {};
    std::filesystem::path value(path);
    auto folder = value.parent_path();
    if (discFolder(folder.filename().wstring())) folder = folder.parent_path();
    return folded(folder.lexically_normal().wstring());
}

std::vector<std::size_t> resolveAlbumSession(
    const std::vector<AlbumSessionCandidate>& candidates,
    std::size_t anchorIndex)
{
    if (anchorIndex >= candidates.size()) return {};
    const auto& anchor = candidates[anchorIndex];
    const auto edition = albumEditionRoot(anchor.path);
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        const auto releaseMatch = !anchor.releaseId.empty()
            ? sameText(candidate.releaseId, anchor.releaseId)
            : candidate.releaseId.empty() &&
                sameText(candidate.albumArtist, anchor.albumArtist) &&
                sameText(candidate.album, anchor.album) &&
                // Per-track release/remaster dates can differ within the
                // same album. Folder/release identity still separates editions.
                albumEditionRoot(candidate.path) == edition;
        if (releaseMatch) result.push_back(index);
    }
    std::stable_sort(result.begin(), result.end(), [&](auto left, auto right) {
        const auto& a = candidates[left];
        const auto& b = candidates[right];
        return std::tuple(sortableNumber(a.disc), sortableNumber(a.track),
                   a.subsong, folded(a.path), a.sourceIndex) <
            std::tuple(sortableNumber(b.disc), sortableNumber(b.track),
                b.subsong, folded(b.path), b.sourceIndex);
    });
    return result;
}

std::wstring albumSessionYear(
    const std::vector<AlbumSessionCandidate>& candidates,
    const std::vector<std::size_t>& selected)
{
    std::map<std::wstring, std::size_t> counts;
    std::wstring representative;
    std::size_t most = 0;
    for (const auto index : selected) {
        if (index >= candidates.size() || candidates[index].year.empty()) continue;
        const auto count = ++counts[candidates[index].year];
        if (count > most) {
            representative = candidates[index].year;
            most = count;
        }
    }
    return representative;
}

std::vector<std::size_t> makeAlbumPermutation(
    std::size_t count,
    std::size_t current,
    AlbumSequencePolicy policy)
{
    std::vector<std::size_t> result(count);
    std::iota(result.begin(), result.end(), 0U);
    if (!policy.shuffle || count < 2U) return result;
    std::mt19937_64 random(policy.seed);
    std::shuffle(result.begin(), result.end(), random);
    const auto found = std::find(result.begin(), result.end(), current);
    if (found != result.end()) std::rotate(result.begin(), found, result.end());
    return result;
}

std::optional<std::size_t> resolveAlbumPolicyIndex(
    const std::vector<std::size_t>& permutation,
    std::size_t current,
    int direction,
    AlbumRepeatMode repeat) noexcept
{
    if (permutation.empty() || direction == 0) return std::nullopt;
    if (repeat == AlbumRepeatMode::track) return current;
    const auto found = std::find(permutation.begin(), permutation.end(), current);
    if (found == permutation.end()) return std::nullopt;
    if (direction > 0) {
        const auto next = std::next(found);
        if (next != permutation.end()) return *next;
        return repeat == AlbumRepeatMode::album
            ? std::optional<std::size_t>(permutation.front()) : std::nullopt;
    }
    if (found != permutation.begin()) return *std::prev(found);
    return repeat == AlbumRepeatMode::album
        ? std::optional<std::size_t>(permutation.back()) : std::nullopt;
}

} // namespace nekorox
