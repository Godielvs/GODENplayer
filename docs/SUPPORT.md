# Support and diagnostics
See the [compatibility matrix](COMPATIBILITY.md) for supported targets and
untested environments. The installer is unsigned; verify its matching hashes
and report exact security detections rather than disabling antivirus. Normal
playback needs no separate VC++ package, Python, Node or ffmpeg: preserve the
complete official host runtime when repairing an installation.

Include candidate version/hash, Windows build, GPU/driver, display scale/refresh, audio format and steps to reproduce. Separate audio interruption, UI unresponsiveness and a frozen visual. State whether Library/Album was visible, and whether pause/resize/track switching was involved.

Use a generated or authorized sample. Do not send complete profiles or music. Inspect logs for paths, titles, Discord/session URLs and credentials before sharing. A failure of a cover service should leave text/progress and playback usable. Report the provider and status, not private bearer tokens.

Keep app.previous until the replacement is accepted. Recover app.rollback.pending manually only after closing the player; do not delete the sole retained profile. Installer transaction logs identify the committed and recovery directories.

Report reproducible problems in [GitHub Issues](https://github.com/Godielvs/GODENplayer/issues), including the information above.

## Discord cover recovery

In **Options → Discord**, choose a provider, read its disclosure, allow its use and click **Apply / Reconnect**. Provider changes and Automatic checkboxes are not saved by simply closing Options. Unapplied edits remain available when switching sections; closing Options warns before discarding them. **Advanced** contains the Automatic service list, Application ID and cover diagnostics. A blank ImgBB/relay secret field keeps the saved credential.

- **Try again now** restarts recovery using saved settings and existing cache URLs. It does not discard a valid acknowledged upload or enable another host.
- **Test cover** also rechecks cached images. A ready status means native origin/delivery checks passed, not independent confirmation inside Discord.
- **Automatic** is useful when a host is unavailable. Enable Uguu as an independent temporary uploader if desired; only checked services participate. ImgBB requires your own key. Catalogue is optional and can show another edition's cover.
- Temporary links renew while covers are enabled. Expired links fall back to text if renewal fails. Repeated Next/Previous is not a remedy for an upstream outage.

Report the selected provider, enabled Automatic services and the visible status. Do not include API keys, relay credentials, complete profiles or private session URLs. Test with an image you are entitled to share. No firewall/antivirus weakening is required.
