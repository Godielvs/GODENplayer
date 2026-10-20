#pragma once

#include "sdk_compat.h"

#include <shellapi.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace nekorox::component {

// Copies the shell payload immediately, then resolves files and directories
// asynchronously through foobar2000's native incoming-item pipeline.
bool openDroppedFiles(HWND owner, HDROP drop);
bool openDroppedFilePath(HWND owner, std::wstring_view path);

// The open playlist is intentionally session-scoped. These calls remove stale
// state restored by foobar2000 and keep it out of the next saved session.
void initializeFileOpenService() noexcept;
void shutdownFileOpenService() noexcept;

// Called from the main playback observer. Playing from any other playlist
// dismisses the temporary album; advancing inside it keeps the album alive.
void notifyTemporaryAlbumPlaybackChanged() noexcept;

// Main-thread snapshot consumed by the Workspace. The items remain owned by
// foobar2000's session playlist and are never inserted into the media library.
bool temporaryAlbumItems(metadb_handle_list& output) noexcept;
std::uint64_t temporaryAlbumGeneration() noexcept;
std::optional<std::size_t> temporaryAlbumPlayingItem() noexcept;
bool playTemporaryAlbumItem(std::size_t itemIndex) noexcept;

// Compact state used by the isolated interaction harness. No paths or tags
// leave the process through this contract.
std::uint32_t fileOpenQaState() noexcept;

} // namespace nekorox::component
