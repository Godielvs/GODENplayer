#include "chrome.h"
#include "build_identity.h"
#include "album_session_service.h"
#include "analysis_controller.h"
#include "database_service.h"
#include "daw_window.h"
#include "discord_presence_service.h"
#include "display_environment.h"
#include "file_open_service.h"
#include "library_model.h"
#include "legacy_import_service.h"
#include "lyrics_service.h"
#include "keep_playing_service.h"
#include "listen_window.h"
#include "master_dsp_controller.h"
#include "master_dsp_window.h"
#include "media_file_operation_service.h"
#include "note_editor.h"
#include "options_window.h"
#include "packet_cost_controller.h"
#include "performance_telemetry.h"
#include "persona_editor.h"
#include "runtime.h"
#include "scrub_smoother_dsp.h"
#include "scrub_preview_service.h"
#include "sdk_compat.h"
#include "tag_edit_service.h"
#include "tag_editor_window.h"
#include "text_util.h"
#include "ui_preferences.h"
#include "worker_reaper.h"

#include <filesystem>
#include <fstream>
#include <shobjidl.h>
#include <stdexcept>

DECLARE_COMPONENT_VERSION(
    "GODENplayer Core",
    nekorox::component::build_identity::kVersion,
    "Native GODENplayer x64 core and local listening lab.");

VALIDATE_COMPONENT_FILENAME("foo_nekorox.dll");
FOOBAR2000_IMPLEMENT_CFG_VAR_DOWNGRADE;

namespace nekorox::component {
namespace {

std::filesystem::path startupGuardPath()
{
    const auto native = filesystem::g_get_native_path(core_api::get_profile_path());
    return std::filesystem::path(utf8ToWide(native.c_str())) /
        L"nekorox" / L"startup.incomplete";
}

bool armStartupGuard(const std::filesystem::path& path)
{
    std::error_code error;
    const auto previousIncomplete = std::filesystem::exists(path, error);
    error.clear();
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) throw std::filesystem::filesystem_error(
        "could not create NekoRoX profile directory",
        path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("could not arm NekoRoX startup guard");
    stream << "nekorox-startup-v1\n";
    stream.flush();
    if (!stream) throw std::runtime_error("could not persist NekoRoX startup guard");
    return previousIncomplete;
}

void clearStartupGuard(const std::filesystem::path& path) noexcept
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

class Lifecycle : public initquit {
public:
    void on_init() override
    {
        SetCurrentProcessExplicitAppUserModelID(L"Goden.GODENplayer.x64");
        initializeHeaderWordmarkPreference();
        initializeFallbackAccentPreference();
        try {
            guardPath_ = startupGuardPath();
            safeMode_ = armStartupGuard(guardPath_);
            guardArmed_ = true;
        } catch (const std::exception& exception) {
            safeMode_ = true;
            console::printf(
                "GODENplayer Core could not persist its startup guard: %s. "
                "This launch will use recovery mode.",
                exception.what());
        }
        try {
            if (!WorkerReaper::instance().start()) {
                throw std::runtime_error("could not start worker reaper");
            }
            databaseStarted_ = DatabaseService::instance().start();
            if (!databaseStarted_) {
                throw std::runtime_error("could not start database service");
            }
            MediaFileOperationService::instance().beginBindingRecovery();
            DisplayEnvironmentService::instance().start();
            displayEnvironmentStarted_ = true;
            initializeFileOpenService();
            if (!safeMode_) {
                LegacyImportService::instance().start();
                legacyImportStarted_ = true;
            }
            Runtime::instance().start();
            runtimeStarted_ = true;
            if (!LyricsService::instance().start()) {
                throw std::runtime_error("could not start lyrics service");
            }
            lyricsStarted_ = true;
            MasterDspController::instance().start();
            masterDspStarted_ = true;
            (void)PerformanceTelemetry::instance().startUiHeartbeat();
            DiscordPresenceService::instance().start();
            presenceStarted_ = true;
            ensureScrubSmootherDspEnabled(!safeMode_);
            LibraryModel::instance().start();
            libraryStarted_ = true;
            AlbumSessionService::instance().start();
            albumSessionStarted_ = true;
            if (!safeMode_) {
                ChromeController::instance().start();
                chromeStarted_ = true;
            }
            if (guardArmed_) clearStartupGuard(guardPath_);
            startupComplete_ = true;
            if (safeMode_) {
                console::print(
                    "GODENplayer Core started in one-shot recovery mode; "
                    "legacy import and custom chrome are disabled until restart.");
            } else {
                console::print("GODENplayer Core started");
            }
        } catch (const std::exception& exception) {
            console::printf(
                "GODENplayer Core startup was incomplete: %s. "
                "The next launch will use recovery mode.",
                exception.what());
        } catch (...) {
            console::print(
                "GODENplayer Core startup was incomplete. "
                "The next launch will use recovery mode.");
        }
    }

    void on_quit() override
    {
        TagEditorWindow::instance().shutdownAndDrain();
        TagEditService::instance().shutdownAndDrain();
        MediaFileOperationService::instance().shutdownAndDrain();
        PerformanceTelemetry::instance().stopUiHeartbeat();
        ListenWindow::instance().close();
        NoteEditorWindow::instance().close();
        OptionsWindow::instance().close();
        MasterDspWindow::instance().close();
        DawWindow::instance().shutdown();
        ScrubPreviewService::instance().shutdown();
        restoreScrubSmootherDspChain();
        PersonaEditorWindow::instance().close();
        shutdownFileOpenService();
        PacketCostController::instance().shutdownAndDrain();
        if (presenceStarted_) DiscordPresenceService::instance().stop();
        if (lyricsStarted_) LyricsService::instance().shutdownAndDrain();
        KeepPlayingService::instance().stop();
        if (albumSessionStarted_) AlbumSessionService::instance().stop();
        AnalysisController::instance().shutdownAndDrain();
        WorkerReaper::instance().shutdownAndDrain();
        if (legacyImportStarted_) LegacyImportService::instance().stop();
        if (chromeStarted_) ChromeController::instance().stop();
        // start() can fail after creating its worker. The model owns whether
        // there is a thread to cancel/join, independently of bootstrap flags.
        LibraryModel::instance().stop();
        if (runtimeStarted_) Runtime::instance().stop();
        if (masterDspStarted_) MasterDspController::instance().stop();
        // A retry (or an exception after thread creation) can change service
        // ownership after bootstrap. Its idempotent stop owns the join.
        DatabaseService::instance().stop();
        if (displayEnvironmentStarted_)
            DisplayEnvironmentService::instance().stop();
        if (startupComplete_ && guardArmed_) clearStartupGuard(guardPath_);
    }

private:
    std::filesystem::path guardPath_;
    bool safeMode_ = false;
    bool guardArmed_ = false;
    bool startupComplete_ = false;
    bool databaseStarted_ = false;
    bool displayEnvironmentStarted_ = false;
    bool runtimeStarted_ = false;
    bool lyricsStarted_ = false;
    bool masterDspStarted_ = false;
    bool presenceStarted_ = false;
    bool libraryStarted_ = false;
    bool albumSessionStarted_ = false;
    bool legacyImportStarted_ = false;
    bool chromeStarted_ = false;
};

FB2K_SERVICE_FACTORY(Lifecycle);
FB2K_ON_INIT_STAGE(
    initializeAlbumAmbientPreference, init_stages::after_config_read);
FB2K_ON_INIT_STAGE(
    initializeAlbumPanelPreferences, init_stages::after_config_read);
FB2K_ON_INIT_STAGE(
    initializeMotionPreferences, init_stages::after_config_read);

} // namespace
} // namespace nekorox::component
