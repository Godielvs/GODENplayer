#pragma once

namespace nekorox::component {

void notifyWorkspaceModelChanged();
void notifyWorkspacePlaybackChanged(bool positionOnly = false);
void notifyWorkspacePlaybackSettled();
void notifyWorkspaceArtworkChanged();
bool revealNowPlayingInWorkspace();
void toggleWorkspaceQueue();
void notifyWorkspaceQueueChanged();
void notifyWorkspaceHistoryChanged();
void showWorkspaceHistory();
void openWorkspaceCommandPalette();
void notifyNativePanelsPlaybackChanged(bool artworkChanged = false);

} // namespace nekorox::component
