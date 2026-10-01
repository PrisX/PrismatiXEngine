#include "Applications/Player/PlayerApp.h"
#include "Applications/Player/PlayerControls.h"
#include "Applications/Player/QaHooks.h"

#include "Engine/Graphics/Screenshot.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Diagnostics/FrameProfiler.h"
#include "Engine/IO/PackageVerifier.h"
#include "Engine/Progression/AwaitedWork.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Progression/UserDataLocation.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Script/ScriptFrameScheduler.h"
#include "Engine/Script/VnModelSnapshots.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/SDK/SourceMap.h"
#include "Engine/Support/Logger.h"
#include "Engine/Support/Utf8Path.h"
#include "Engine/UI/Layout.h"
#include "Engine/UI/Widgets.h"
#include "Engine/UI/Startup/SplashTypes.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

// Scripted QA journeys (PRISMATIX_E2E_JOURNEY and friends). They exist only
// in PrismatiXPlayerQA; the shipped Player gets the empty versions below, so
// none of this code or its project-specific route names ship with a game.
namespace px::player {

#if PRISMATIX_QA_HOOKS

namespace {
constexpr std::string_view kUIRouteE2ERoutes[] = {
    "backlog", "save", "load", "gallery", "settings",
    "settings-page1", "settings-page2", "settings-page3",
    "settings-page4", "settings-page5", "settings-page6",
    "settings-page7",
    "extra", "flowchart", "flowchart-common", "flowchart-yoshino",
    "flowchart-mako", "flowchart-murasame", "flowchart-lena",
    "flowchart-sub", "replay", "music",
};
constexpr std::string_view kUIActionsE2EReplayLabel =
    "sb.115-afterver1-01.yos-part-15";
}  // namespace

void PlayerApp::ConfigureE2EJourney() {
    const char* journey=PX_QA_ENV("PRISMATIX_E2E_JOURNEY");
    if(!journey)return;
    if(std::string_view(journey)=="video-return"){
        m_videoReturnE2E = true;
        m_videoReturnE2EStartedAt = SDL_GetTicks();
        const char* captureDir = PX_QA_ENV("PRISMATIX_E2E_CAPTURE_DIR");
        m_presentationE2ECaptureDir = captureDir ? captureDir : "e2e-video-return";
        PX_LOG_INFO("Player video return E2E armed captureDir={}",
                    m_presentationE2ECaptureDir);
        return;
    }
    if(std::string_view(journey)=="save-load"){
        m_saveLoadE2E=true;
        m_saveLoadE2EStartedAt=SDL_GetTicks();
        PX_LOG_INFO("Player save-load E2E armed slot=2");
        return;
    }
    if(std::string_view(journey)=="presentation"){
        m_presentationE2E=true;
        m_presentationE2EStage=1;
        m_presentationE2EStartedAt=SDL_GetTicks();
        const char* captureDir=PX_QA_ENV("PRISMATIX_E2E_CAPTURE_DIR");
        m_presentationE2ECaptureDir=captureDir?captureDir:"e2e-captures";
        const char* targetOperation=PX_QA_ENV("PRISMATIX_E2E_TARGET_OPERATION_ID");
        m_presentationE2ETargetOperationId=targetOperation?targetOperation:"";
        m_presentationE2ESplashCaptured.assign(
            m_splash?m_splash->Entries().size():0,false);
        PX_LOG_INFO("Player presentation E2E armed captureDir={}",
                    m_presentationE2ECaptureDir);
        return;
    }
    if(std::string_view(journey)=="ui-routes"){
        m_uiRoutesE2E=true;
        m_uiRoutesE2EStartedAt=SDL_GetTicks();
        const char* captureDir=PX_QA_ENV("PRISMATIX_E2E_CAPTURE_DIR");
        m_presentationE2ECaptureDir=captureDir?captureDir:"e2e-ui-routes";
        PX_LOG_INFO("Player UI routes E2E armed captureDir={} routes={}",
                    m_presentationE2ECaptureDir,
                    std::size(kUIRouteE2ERoutes));
        return;
    }
    if(std::string_view(journey)=="ui-actions"){
        m_uiActionsE2E=true;
        m_uiActionsE2EStartedAt=SDL_GetTicks();
        PX_LOG_INFO("Player UI actions E2E armed");
        return;
    }
    if(std::string_view(journey)=="settings-fullscreen"){
        m_settingsFullscreenE2E=true;
        m_settingsFullscreenE2EStartedAt=SDL_GetTicks();
        PX_LOG_INFO("Player settings fullscreen E2E armed");
        return;
    }
    if(std::string_view(journey)=="save-load-confirm"){
        m_saveLoadConfirmE2E = true;
        m_saveLoadConfirmE2EStartedAt = SDL_GetTicks();
        PX_LOG_INFO("Player save/load confirmation E2E armed");
        return;
    }
    if(std::string_view(journey)=="settings-sound"){
        m_settingsSoundE2E = true;
        m_settingsSoundE2EStartedAt = SDL_GetTicks();
        const char* captureDir = PX_QA_ENV("PRISMATIX_E2E_CAPTURE_DIR");
        m_presentationE2ECaptureDir = captureDir ? captureDir : "e2e-settings-sound";
        PX_LOG_INFO("Player settings sound E2E armed captureDir={}",
                    m_presentationE2ECaptureDir);
        return;
    }
    if(std::string_view(journey)=="save-load-incompatible"){
        m_incompatibleSaveE2E = true;
        m_incompatibleSaveE2EStartedAt = SDL_GetTicks();
        PX_LOG_INFO("Player incompatible save UI E2E armed");
        return;
    }
    if(std::string_view(journey)=="bench"){
        m_benchE2E = true;
        m_benchStageAt = SDL_GetTicks();
        PX_LOG_INFO("Player bench armed");
        return;
    }
    if(std::string_view(journey)!="catalog")return;
    m_e2eStage=E2EStage::AwaitTitle;
    m_e2eStartedAt=SDL_GetTicks();
    PX_LOG_INFO("Player E2E journey armed name=catalog");
}

bool PlayerApp::CapturePresentationE2EFrame(const std::string_view name){
    int width=0,height=0;
    m_runtime.Renderer().GetLogicalSize(width,height);
    auto png=graphics::CaptureThumbnailPng(
        m_runtime.Renderer().Handle(),width,height);
    if(png.empty())return false;
    std::error_code error;
    const std::filesystem::path directory=m_presentationE2ECaptureDir;
    std::filesystem::create_directories(directory,error);
    if(error)return false;
    const auto path=directory/(std::string(name)+".png");
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
    const bool saved=stream.good();
    PX_LOG_INFO("Player presentation E2E capture name={} path={} saved={}",
                name,path.string(),saved);
    return saved;
}

void PlayerApp::BenchCapture(std::string name) {
    if (!PX_QA_ENV("PRISMATIX_E2E_CAPTURE_DIR")) return;
    m_presentationE2ECaptureDir = PX_QA_ENV("PRISMATIX_E2E_CAPTURE_DIR");
    m_runtime.OnNextComposedFrame([this, name = std::move(name)] {
        (void)CapturePresentationE2EFrame(name);
    });
}

void PlayerApp::DriveBenchJourney() {
    // Reading (auto), skipping, then opening each menu route. Timings come
    // from the frame profiler (PRISMATIX_PERF) plus the summaries logged here.
    static constexpr std::array<std::string_view, 5> kRoutes{
        "save", "load", "settings", "backlog", "gallery"};
    const std::uint64_t now = SDL_GetTicks();
    const auto lines = [this] {
        return m_session->Backlog().LastSequence() - m_benchSequence;
    };
    switch (m_benchStage) {
        case 0:
            if (m_appState == AppState::Title && m_ui.CurrentRoute() == "title" &&
                !m_routePreloadBatch && ++m_benchSettledFrames >= 60) {
                m_benchSettledFrames = 0;
                BenchCapture("title");
                StartGame();
                m_benchStage = 1;
            }
            break;
        case 1:
            if (m_appState == AppState::Game && !m_gameStartTransitionActive) {
                m_settings.skipReadOnly = false;
                m_benchSequence = m_session->Backlog().LastSequence();
                m_benchStageAt = now;
                m_benchStage = 2;
            }
            break;
        case 2:
            if (m_session->VM().State() == vn::VMState::WaitingChoice) {
                if (!m_benchChoiceCaptured) {
                    // Hold the prompt a few frames so the capture shows it.
                    if (++m_benchSettledFrames < 20) break;
                    m_benchSettledFrames = 0;
                    m_benchChoiceCaptured = true;
                    BenchCapture("choice");
                    break;
                }
                m_session->SelectChoice(0);
            }
            if (!m_benchDialogueCaptured &&
                m_session->VM().State() == vn::VMState::WaitingClick) {
                m_benchDialogueCaptured = true;
                BenchCapture("dialogue");
            }
            m_skipMode = true;
            if (now - m_benchStageAt >= 15'000) {
                PX_LOG_INFO("Player bench skip lines={} seconds=15 linesPerSecond={:.1f}",
                            lines(), static_cast<double>(lines()) / 15.0);
                m_skipMode = false;
                m_autoMode = true;
                m_settings.autoWaitMs = 300;
                m_benchSequence = m_session->Backlog().LastSequence();
                m_benchStageAt = now;
                m_benchStage = 3;
            }
            break;
        case 3:
            if (m_session->VM().State() == vn::VMState::WaitingChoice)
                m_session->SelectChoice(0);
            m_autoMode = true;
            // Screenshots of ordinary reading and of any screen transition.
            if (const auto effect = m_runtime.Renderer().ActiveScreenEffect();
                effect && effect->elapsedSeconds >= effect->durationSeconds * 0.5f &&
                m_benchCapturedEffect != effect->handle) {
                m_benchCapturedEffect = effect->handle;
                BenchCapture("transition-" + effect->effect + "-" + std::to_string(effect->handle));
            }
            if ((now - m_benchStageAt) / 4000 != m_benchReadingShot) {
                m_benchReadingShot = (now - m_benchStageAt) / 4000;
                BenchCapture("reading-" + std::to_string(m_benchReadingShot));
            }
            if (now - m_benchStageAt >= 20'000) {
                PX_LOG_INFO("Player bench reading lines={} seconds=20", lines());
                m_autoMode = false;
                m_benchStage = 4;
            }
            break;
        case 4:
            if (m_benchRouteIndex >= kRoutes.size()) {
                PX_LOG_INFO("Player bench complete");
                m_benchE2E = false;
                m_quitRequested = true;
                break;
            }
            OpenScreen(std::string(kRoutes[m_benchRouteIndex]));
            m_benchStageAt = now;
            m_benchSettledFrames = 0;
            m_benchStage = 5;
            break;
        case 5:
            if (m_routePreloadBatch ||
                m_runtime.Renderer().PresentationHoldActive()) break;
            if (++m_benchSettledFrames == 1)
                PX_LOG_INFO("Player bench route={} readyMs={}",
                            kRoutes[m_benchRouteIndex], now - m_benchStageAt);
            if (m_benchSettledFrames == 30 || m_benchSettledFrames == 85)
                BenchCapture("route-" + std::string(kRoutes[m_benchRouteIndex]) + "-" +
                             std::to_string(m_benchSettledFrames));
            if (m_benchSettledFrames >= 90) {
                HandleUIAction({"overlay.close", {}});
                ++m_benchRouteIndex;
                m_benchStage = 4;
            }
            break;
        default:
            break;
    }
}

void PlayerApp::DriveE2EJourney() {
    if (m_videoReturnE2E) {
        const auto now = SDL_GetTicks();
        if (now - m_videoReturnE2EStartedAt > 300'000) {
            PX_LOG_ERROR("Player video return E2E timed out stage={} route={}",
                         m_videoReturnE2EStage, m_ui.CurrentRoute());
            m_e2eFailed = true;
            m_videoReturnE2E = false;
            m_quitRequested = true;
            return;
        }
        switch (m_videoReturnE2EStage) {
            case 0:
                if (m_appState == AppState::Title && m_ui.CurrentRoute() == "title" &&
                    !m_routePreloadBatch && ++m_videoReturnE2ESettledFrames >= 60) {
                    StartGame();
                    m_videoReturnE2EStage = 1;
                }
                break;
            case 1:
                if (m_appState == AppState::Game && !m_gameStartTransitionActive) {
                    m_settings.skipReadOnly = false;
                    m_skipMode = true;
                    if (m_session->VM().State() == vn::VMState::WaitingVideo &&
                        m_ui.CurrentScreen() == ui::GalgameUI::Screen::Video) {
                        m_videoReturnE2EVideoAt = now;
                        m_videoReturnE2EStage = 2;
                        PX_LOG_INFO("Player video return E2E movie started textSkip=true");
                    }
                }
                break;
            case 2:
                // Keep text skip enabled throughout playback. No advance/cancel
                // input is sent, so this exercises natural movie completion.
                if (m_session->VM().State() != vn::VMState::WaitingVideo) {
                    m_skipMode = false;
                    const bool restored =
                        m_ui.CurrentScreen() == ui::GalgameUI::Screen::HUD &&
                        m_ui.HasControlNamed("MessageWindow") &&
                        m_ui.HasControlNamed("Dialogue");
                    PX_LOG_INFO("Player video return E2E movie ended elapsedMs={} route={} restored={}",
                                now - m_videoReturnE2EVideoAt, m_ui.CurrentRoute(), restored);
                    if (!restored) {
                        m_e2eFailed = true;
                        m_videoReturnE2E = false;
                        m_quitRequested = true;
                    } else {
                        m_videoReturnE2EStage = 3;
                    }
                }
                break;
            case 3:
                if (!m_routePreloadBatch && m_storyHudVisible && !m_hudHidden &&
                    m_hudFade >= 0.99f &&
                    m_session->VM().State() == vn::VMState::WaitingClick &&
                    m_session->Dialogue().Finished() &&
                    !m_session->Dialogue().State().displayText.empty()) {
                    m_videoReturnE2EStage = 4;
                    m_runtime.OnNextComposedFrame([this] {
                        const bool restored =
                            m_ui.CurrentScreen() == ui::GalgameUI::Screen::HUD &&
                            m_ui.HasControlNamed("MessageWindow") &&
                            m_ui.HasControlNamed("Dialogue");
                        const bool captured = CapturePresentationE2EFrame("video-return-dialogue");
                        PX_LOG_INFO("Player video return E2E complete restored={} captured={} route={}",
                                    restored, captured, m_ui.CurrentRoute());
                        m_e2eFailed = !restored || !captured;
                        m_videoReturnE2E = false;
                        m_quitRequested = true;
                    });
                }
                break;
            default:
                break;
        }
        return;
    }
    if (m_benchE2E) {
        DriveBenchJourney();
        return;
    }
    if (m_incompatibleSaveE2E) {
        if (SDL_GetTicks() - m_incompatibleSaveE2EStartedAt > 30'000) {
            PX_LOG_ERROR("Player incompatible save UI E2E timed out stage={}",
                         m_incompatibleSaveE2EStage);
            m_e2eFailed = true;
            m_incompatibleSaveE2E = false;
            m_quitRequested = true;
            return;
        }
        switch (m_incompatibleSaveE2EStage) {
            case 0:
                if (m_appState == AppState::Title && m_ui.CurrentRoute() == "title") {
                    OpenScreen("load");
                    m_incompatibleSaveE2EStage = 1;
                }
                break;
            case 1:
                if (m_ui.CurrentRoute() == "load") {
                    HandleUIAction({"load.slot", "1"});
                    m_incompatibleSaveE2EStage = 2;
                }
                break;
            case 2: {
                HandleUIAction({"saveLoad.confirm.accept", {}});
                auto* error = FindUiControlNamed<ui::Control>(m_ui.Root(), "SaveLoadError");
                const bool passed = error && error->IsVisibleInTree() &&
                    m_ui.CurrentRoute() == "load" && m_appState == AppState::Title;
                PX_LOG_INFO("Player incompatible save UI E2E {}", passed ? "passed" : "failed");
                m_e2eFailed = !passed;
                m_incompatibleSaveE2E = false;
                m_quitRequested = true;
                break;
            }
            default: break;
        }
        return;
    }
    if (m_settingsSoundE2E) {
        if (SDL_GetTicks() - m_settingsSoundE2EStartedAt > 30'000) {
            PX_LOG_ERROR("Player settings sound E2E timed out stage={}",
                         m_settingsSoundE2EStage);
            m_e2eFailed = true;
            m_settingsSoundE2E = false;
            m_quitRequested = true;
            return;
        }
        if (m_settingsSoundE2EStage == 0 && m_appState == AppState::Title &&
            m_ui.CurrentRoute() == "title") {
            OpenScreen("settings");
            m_settingsSoundE2EStage = 1;
        } else if (m_settingsSoundE2EStage == 1 &&
                   m_ui.CurrentRoute() == "settings") {
            m_settingsPage = 3;
            for (int page = 1; page <= 4; ++page)
                (void)m_ui.UpdateUiData("settings.page" + std::to_string(page),
                                        Variant(page == 3));
            m_settingsSoundE2EStage = 2;
        } else if (m_settingsSoundE2EStage == 2 &&
                   !m_routePreloadBatch &&
                   SDL_GetTicks() - m_settingsSoundE2EStartedAt > 2'000) {
            m_settingsSoundE2EStage = 3;
        }
        return;
    }
    if (m_saveLoadConfirmE2E) {
        const auto finish = [this](bool passed) {
            m_e2eFailed = !passed;
            PX_LOG_INFO("Player save/load confirmation E2E {}", passed ? "passed" : "failed");
            m_saveLoadConfirmE2E = false;
            m_quitRequested = true;
        };
        if (SDL_GetTicks() - m_saveLoadConfirmE2EStartedAt > 90'000) {
            PX_LOG_ERROR("Player save/load confirmation E2E timed out stage={}",
                         m_saveLoadConfirmE2EStage);
            finish(false);
            return;
        }
        switch (m_saveLoadConfirmE2EStage) {
            case 0:
                if (m_appState == AppState::Title && m_ui.CurrentRoute() == "title") {
                    StartGame();
                    m_saveLoadConfirmE2EStage = 1;
                }
                break;
            case 1:
                if (m_appState == AppState::Game && !m_gameStartTransitionActive &&
                    m_session->Dialogue().Active()) {
                    if (!SaveSlot(1, {})) { finish(false); break; }
                    OpenScreen("load");
                    m_saveLoadConfirmE2EStage = 2;
                }
                break;
            case 2:
                if (m_ui.CurrentRoute() == "load") {
                    HandleUIAction({"load.slot", "1"});
                    m_saveLoadConfirmE2EStage = 3;
                }
                break;
            case 3:
                HandleUIAction({"saveLoad.confirm.accept", {}});
                if (m_appState == AppState::Game && m_ui.CurrentRoute() != "load")
                    finish(true);
                else
                    finish(false);
                break;
            default: break;
        }
        return;
    }
    if(m_settingsFullscreenE2E){
        if(SDL_GetTicks()-m_settingsFullscreenE2EStartedAt>30'000){
            PX_LOG_ERROR("Player settings fullscreen E2E timed out stage={}",
                         m_settingsFullscreenE2EStage);
            m_e2eFailed=true;m_settingsFullscreenE2E=false;m_quitRequested=true;
            return;
        }
        const auto activate=[this](const char* name){
            auto* button=FindUiControlNamed<ui::Button>(m_ui.Root(),name);
            if(button)button->Activate();
            return button!=nullptr;
        };
        switch(m_settingsFullscreenE2EStage){
            case 0:
                if(m_appState==AppState::Title&&m_ui.CurrentRoute()=="title"){
                    OpenScreen("settings");
                    m_settingsFullscreenE2EStage=1;
                }
                break;
            case 1:
                if(m_ui.CurrentRoute()=="settings"){
                    if(!activate("FullscreenModeLabelHit")){
                        PX_LOG_ERROR("Player settings fullscreen label button missing");
                        m_e2eFailed=true;m_settingsFullscreenE2E=false;m_quitRequested=true;
                    }else m_settingsFullscreenE2EStage=2;
                }
                break;
            case 2:
                if(m_settings.fullscreen&&
                   (SDL_GetWindowFlags(m_runtime.GetWindow().Handle())&
                    SDL_WINDOW_FULLSCREEN)){
                    if(!activate("WindowModeLabelHit")){
                        PX_LOG_ERROR("Player settings windowed label button missing");
                        m_e2eFailed=true;m_settingsFullscreenE2E=false;m_quitRequested=true;
                    }else m_settingsFullscreenE2EStage=3;
                }
                break;
            case 3:
                if(!m_settings.fullscreen&&
                   !(SDL_GetWindowFlags(m_runtime.GetWindow().Handle())&
                     SDL_WINDOW_FULLSCREEN)){
                    int width = 0, height = 0;
                    SDL_GetWindowSize(m_runtime.GetWindow().Handle(), &width, &height);
                    if (width != m_settings.windowWidth ||
                        height != m_settings.windowHeight) {
                        PX_LOG_ERROR("Player windowed E2E size mismatch actual={}x{} expected={}x{}",
                                     width, height, m_settings.windowWidth, m_settings.windowHeight);
                        m_e2eFailed = true;
                    } else {
                        PX_LOG_INFO("Player settings fullscreen E2E complete windowed={}x{}",
                                    width, height);
                    }
                    m_settingsFullscreenE2E=false;m_quitRequested=true;
                }
                break;
            default:break;
        }
        return;
    }
    if(m_saveLoadE2E){
        if(SDL_GetTicks()-m_saveLoadE2EStartedAt>120'000){
            PX_LOG_ERROR("Player save-load E2E timed out before title");
            m_e2eFailed=true;
            m_saveLoadE2E=false;
            m_quitRequested=true;
            return;
        }
        if(m_appState!=AppState::Title)return;
        const bool loaded=LoadSlot(2);
        m_e2eFailed=!loaded||m_appState!=AppState::Game;
        if(m_e2eFailed)
            PX_LOG_ERROR("Player save-load E2E failed slot=2");
        else
            PX_LOG_INFO("Player save-load E2E passed slot=2");
        m_saveLoadE2E=false;
        m_quitRequested=true;
        return;
    }
    if(m_presentationE2E){
        if(SDL_GetTicks()-m_presentationE2EStartedAt>90'000){
            PX_LOG_ERROR("Player presentation E2E timed out stage={}",
                         m_presentationE2EStage);
            m_quitRequested=true;
            m_presentationE2E=false;
            return;
        }
        if(m_presentationE2EStage==3&&m_appState==AppState::Game&&
           !m_gameStartTransitionActive){
            if(!m_presentationE2ETargetOperationId.empty()&&
               m_session->VM().CurrentOperationId()==m_presentationE2ETargetOperationId){
                m_session->Dialogue().ShowAll();
                return;
            }
            const auto actors=m_session->Stage().Snapshot();
            const auto& dialogue=m_session->Dialogue().State();
            const bool waitingForPresentation=!actors.empty()&&
                !dialogue.fullText.empty()&&
                std::ranges::any_of(actors,[](const auto& actor){
                    return !actor.exiting&&actor.targetAlpha>=245.0f&&
                           actor.alpha<245.0f;
                });
            if(waitingForPresentation)return;
            if(m_session->VM().State()==vn::VMState::WaitingChoice)
                m_session->SelectChoice(0);
            else if(m_session->VM().State()==vn::VMState::WaitingClick||
                    m_session->VM().State()==vn::VMState::WaitingTimer){
                m_session->Dialogue().ShowAll();
                m_session->Advance();
            }
        }
        return;
    }
    if(m_uiRoutesE2E){
        const std::uint64_t now=SDL_GetTicks();
        if(now-m_uiRoutesE2EStartedAt>120'000){
            PX_LOG_ERROR("Player UI routes E2E timed out index={} route={}",
                         m_uiRoutesE2EIndex,
                         m_uiRoutesE2EIndex<std::size(kUIRouteE2ERoutes)
                             ? kUIRouteE2ERoutes[m_uiRoutesE2EIndex]
                             : std::string_view("complete"));
            m_e2eFailed=true;
            m_uiRoutesE2E=false;
            m_quitRequested=true;
            return;
        }
        if(m_uiRoutesE2ESaveConfirmStage==1){
            auto* dialog=FindUiControlNamed<ui::Control>(
                m_ui.Root(),"SaveLoadConfirm");
            if(m_pendingSaveLoadSlot==0&&dialog&&dialog->IsVisibleInTree()){
                m_uiRoutesE2ESaveConfirmStage=2;
                m_uiRoutesE2EAwaitingCapture=true;
                m_uiRoutesE2ECaptureAt=now+350;
            }
            return;
        }
        if(m_uiRoutesE2ESaveConfirmStage==3){
            auto* dialog=FindUiControlNamed<ui::Control>(
                m_ui.Root(),"SaveLoadConfirm");
            if(m_pendingSaveLoadSlot||(dialog&&dialog->IsVisibleInTree()))return;
            const Status closed=m_session->Routes().CloseModal();
            if(!closed){
                for(const auto& diagnostic:closed.Diagnostics())diag::Emit(diagnostic);
                m_e2eFailed=true;m_uiRoutesE2E=false;m_quitRequested=true;
                return;
            }
            m_uiRoutesE2ESaveConfirmStage=0;
            ++m_uiRoutesE2EIndex;
            return;
        }
        if(m_uiRoutesE2EAwaitingCapture)return;
        if(m_uiRoutesE2EIndex>=std::size(kUIRouteE2ERoutes)){
            PX_LOG_INFO("Player UI routes E2E complete routes={}",
                        std::size(kUIRouteE2ERoutes));
            m_uiRoutesE2E=false;
            m_quitRequested=true;
            return;
        }
        if(!m_uiRoutesE2EReadinessLogged&&
           now-m_uiRoutesE2EStartedAt>15'000){
            PX_LOG_INFO(
                "Player UI routes E2E readiness appState={} base={} modal={} ui={}",
                static_cast<int>(m_appState),
                m_session->Routes().CurrentRoute(),
                m_session->Routes().CurrentModalRoute(),m_ui.CurrentRoute());
            m_uiRoutesE2EReadinessLogged=true;
        }
        if(m_appState!=AppState::Title||
           !m_session->Routes().CurrentModalRoute().empty())
            return;
        const std::string target(kUIRouteE2ERoutes[m_uiRoutesE2EIndex]);
        const Status shown=m_session->Routes().ShowModal(target);
        if(!shown){
            for(const auto& diagnostic:shown.Diagnostics())diag::Emit(diagnostic);
            PX_LOG_ERROR("Player UI routes E2E could not open route={}",target);
            m_e2eFailed=true;
            m_uiRoutesE2E=false;
            m_quitRequested=true;
            return;
        }
        m_uiRoutesE2EAwaitingCapture=true;
        m_uiRoutesE2ECaptureAt=now+350;
        PX_LOG_INFO("Player UI routes E2E opened index={} route={}",
                    m_uiRoutesE2EIndex,target);
        return;
    }
    if(m_uiActionsE2E){
        if(SDL_GetTicks()-m_uiActionsE2EStartedAt>90'000){
            PX_LOG_ERROR("Player UI actions E2E timed out stage={}",
                         m_uiActionsE2EStage);
            m_e2eFailed=true;
            m_uiActionsE2E=false;
            m_quitRequested=true;
            return;
        }
        switch(m_uiActionsE2EStage){
            case 0:
                if(m_appState==AppState::Title&&m_ui.CurrentRoute()=="title"){
                    OpenScreen("flowchart");
                    m_uiActionsE2EStage=1;
                }
                break;
            case 1:
                if(m_session->Routes().CurrentModalRoute()=="flowchart"&&
                   m_ui.CurrentRoute()=="flowchart"){
                    auto* filter=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"btyosButton");
                    if(!filter){
                        PX_LOG_ERROR("Player UI actions E2E flowchart filter missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    filter->Activate();
                    m_uiActionsE2EStage=2;
                }
                break;
            case 2:
                if(m_session->Routes().CurrentModalRoute()=="flowchart-yoshino"&&
                   m_ui.CurrentRoute()=="flowchart-yoshino"){
                    PX_LOG_INFO("Player UI actions E2E flowchart filter dispatched route={}",
                                m_ui.CurrentRoute());
                    auto* item=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"flow_yoshino_1Button");
                    if(!item){
                        PX_LOG_ERROR("Player UI actions E2E flowchart item missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    item->Activate();
                    m_uiActionsE2EStage=7;
                }
                break;
            case 7:
                if(m_selectedFlowEntry=="sb.100-ver1-01.yos-part-0"&&
                   m_appState==AppState::Title&&
                   m_ui.CurrentRoute()=="flowchart-yoshino"){
                    auto* heading=FindUiControlNamed<ui::Label>(
                        m_ui.Root(),"FlowSelectedHeading");
                    auto* jump=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"scenejumpButton");
                    if(!heading||heading->Text().empty()||!jump||!jump->Enabled()){
                        PX_LOG_ERROR("Player UI actions E2E flowchart selection detail missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    PX_LOG_INFO("Player UI actions E2E flowchart selection visible heading={}",
                                heading->Text());
                    HandleUIAction({"overlay.close",{}});
                    OpenScreen("music");
                    m_uiActionsE2EStage=3;
                }
                break;
            case 3:
                if(m_session->Routes().CurrentModalRoute()=="music"&&
                   m_ui.CurrentRoute()=="music"){
                    auto* track=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"music_SongOpButton");
                    if(!track){
                        PX_LOG_ERROR("Player UI actions E2E music control missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    track->Activate();
                    m_uiActionsE2EStage=4;
                }
                break;
            case 4:
                if(m_musicTrackSelected&&!m_musicTracks.empty()){
                    const std::string activePath =
                        m_runtime.Audio().CaptureState().music.path;
                    if(activePath!=m_musicTracks.front()){
                        PX_LOG_ERROR(
                            "Player UI actions E2E music path mismatch expected={} actual={}",
                            m_musicTracks.front(),activePath);
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    PX_LOG_INFO("Player UI actions E2E music argument dispatched path={}",
                                m_musicTracks.front());
                    HandleUIAction({"overlay.close",{}});
                    OpenScreen("replay");
                    m_uiActionsE2EStage=5;
                }
                break;
            case 5:
                if(m_session->Routes().CurrentModalRoute()=="replay"&&
                   m_ui.CurrentRoute()=="replay"){
                    auto* replay=FindUiControlNamed<ui::Button>(
                        m_ui.Root(),"yoshinoButton");
                    if(!replay){
                        PX_LOG_ERROR("Player UI actions E2E replay control missing");
                        m_e2eFailed=true;m_uiActionsE2E=false;m_quitRequested=true;
                        break;
                    }
                    replay->Activate();
                    m_uiActionsE2EStage=6;
                }
                break;
            case 6:
                if(m_appState==AppState::Game&&!m_gameStartTransitionActive){
                    const int target=m_session->VM().CurrentProgram().LabelIndex(
                        std::string(kUIActionsE2EReplayLabel));
                    if(target<0||m_session->VM().ProgramCounter()<=target){
                        PX_LOG_ERROR(
                            "Player UI actions E2E replay did not restart at label target={} pc={}",
                            target,m_session->VM().ProgramCounter());
                        m_e2eFailed=true;
                    }else{
                        PX_LOG_INFO(
                            "Player UI actions E2E complete replayLabel={} pc={}",
                            kUIActionsE2EReplayLabel,m_session->VM().ProgramCounter());
                    }
                    m_uiActionsE2E=false;
                    m_quitRequested=true;
                }
                break;
            default:break;
        }
        return;
    }
    if(m_e2eStage==E2EStage::Disabled||m_e2eStage==E2EStage::Complete)
        return;
    if(SDL_GetTicks()-m_e2eStartedAt>30'000){
        PX_LOG_ERROR("Player E2E journey timed out stage={} modal={} route={} ui={}",
                     static_cast<int>(m_e2eStage),
                     m_session->Routes().CurrentModalRoute(),
                     m_session->Routes().CurrentRoute(), m_ui.CurrentRoute());
        m_e2eFailed=true;
        m_e2eStage=E2EStage::Complete;
        m_quitRequested=true;
        return;
    }
    auto& input=m_runtime.GetInput();
    switch(m_e2eStage){
        case E2EStage::AwaitTitle: {
            if(m_appState==AppState::Title){
                const auto titleState=m_ui.CaptureRuntimeState();
                if(!titleState.animation||titleState.animation->position<=0.0f)
                    break;
                PX_LOG_INFO("Player Phase 2 authored title animation advanced");
                ui::ActionInvocation invocation;
                invocation.action="acceptance.phase2-title";
                invocation.context.sourceScene=std::string(m_ui.CurrentRoute());
                const auto started=m_ui.Actions().Start(std::move(invocation));
                if(!started){
                    PX_LOG_ERROR("Player Phase 2 title Action failed to start");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                m_e2eStage=E2EStage::AwaitTitleAsync;
            }
            break;
        }
        case E2EStage::AwaitTitleAsync:
            if(m_session->Routes().CurrentRoute()=="phone"){
                const bool actualScene=
                    m_ui.CurrentRoute()=="phone"&&
                    m_ui.CurrentScreen()==ui::GalgameUI::Screen::Custom&&
                    m_ui.Root()&&m_ui.Root()->Name()=="PhoneRoot";
                if(!actualScene){
                    PX_LOG_ERROR("Player arbitrary phone route did not install its authored scene");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                m_e2eCustomTitleRouteSeen=true;
            }
            if(m_e2eCustomTitleRouteSeen&&m_scriptHost&&
               !m_scriptHost->HasPendingAction()&&
               m_session->Routes().CurrentRoute()=="title"){
                PX_LOG_INFO("Player Phase 2 title async and custom phone route complete");
                input.InjectAction(InputAction::FocusNext);
                input.InjectAction(InputAction::Accept);
                m_e2eStage=E2EStage::AwaitGameReady;
            }
            break;
        case E2EStage::AwaitGameReady:
            if(m_appState==AppState::Game&&!m_gameStartTransitionActive&&
               m_profile.CGUnlocked("ending-rin")&&
               m_scriptHost&&!m_scriptHost->HasPendingCommand()&&
               !m_scriptHost->HasPendingAction()&&
               m_session->Dialogue().Active()){
                ui::ActionInvocation invocation;
                invocation.action="acceptance.phase2-menu";
                invocation.context.sourceScene=std::string(m_ui.CurrentRoute());
                const auto started=m_ui.Actions().Start(std::move(invocation));
                if(!started){
                    PX_LOG_ERROR("Player Phase 2 menu Action failed to start");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                m_e2eStage=E2EStage::AwaitMenuAsync;
            }
            break;
        case E2EStage::AwaitMenuAsync:
            if(m_session->Routes().CurrentModalRoute()=="replay"){
                const bool actualScene=
                    m_ui.CurrentRoute()=="replay"&&
                    m_ui.CurrentScreen()==ui::GalgameUI::Screen::Custom&&
                    m_ui.Root()&&m_ui.Root()->Name()=="ReplayRoot";
                if(!actualScene){
                    PX_LOG_ERROR("Player arbitrary replay route did not install its authored scene");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                m_e2eCustomMenuRouteSeen=true;
            }
            if(m_e2eCustomMenuRouteSeen&&m_scriptHost&&
               !m_scriptHost->HasPendingAction()&&
               m_session->Routes().CurrentModalRoute().empty()){
                if (PX_QA_ENV("PRISMATIX_E2E_LOAD_PREVIOUS_SAVE")) {
                    if (!LoadSlot(2)) {
                        PX_LOG_ERROR("Player E2E previous-version save could not load");
                        m_e2eFailed = true;
                        m_e2eStage = E2EStage::Complete;
                        m_quitRequested = true;
                        break;
                    }
                    PX_LOG_INFO("Player E2E previous-version save loaded");
                }
                bool consistent =
                    m_session->Routes().ShowModal("replay").IsOk()&&
                    SaveSlot(1,{})&&
                    m_session->Routes().CloseModal().IsOk()&&
                    m_session->Routes().Push("phone").IsOk()&&LoadSlot(1);
                const auto restored=m_session->Routes().CaptureState();
                consistent=consistent&&
                    restored.stack==std::vector<std::string>{"hud"}&&
                    restored.modals==std::vector<std::string>{"replay"}&&
                    m_ui.CurrentRoute()=="replay"&&m_ui.Root()&&
                    m_ui.Root()->Name()=="ReplayRoot";
                if(!consistent){
                    PX_LOG_ERROR("Player save/load did not restore the route and UI stack");
                    m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                    m_quitRequested=true;break;
                }
                (void)m_session->Routes().CloseModal();
                PX_LOG_INFO("Player Phase 2 menu async and save/load stack complete");
                HandleUIAction({"save.open", {}});
                m_e2eStage=E2EStage::AwaitSaveScreen;
            }
            break;
        case E2EStage::AwaitSaveScreen:
            if (m_pendingSaveScreen) break;
            if (m_ui.CurrentScreen() != ui::GalgameUI::Screen::Save) {
                PX_LOG_ERROR("Player save menu was replaced before the next frame");
                m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;break;
            }
            PX_LOG_INFO("Player save menu survived a rendered frame");
            if (!SaveSlot(2, {}) || !LoadSlot(2) || m_ui.CurrentRoute() != "hud" ||
                !m_session->Routes().CurrentModalRoute().empty()) {
                PX_LOG_ERROR("Player save picker leaked into the loaded gameplay checkpoint");
                m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;break;
            }
            PX_LOG_INFO("Player save picker checkpoint restored gameplay HUD");
            m_e2eStage=E2EStage::AwaitPostMenuGameReady;
            break;
        case E2EStage::AwaitPostMenuGameReady:
            if(m_appState==AppState::Game&&m_profile.CGUnlocked("ending-rin")&&
               m_scriptHost&&!m_scriptHost->HasPendingCommand()&&
               !m_scriptHost->HasPendingAction()&&
               m_session->Dialogue().Active()){
                if(!m_e2eLocaleSwitched&&m_supportedLocales.size()>1){
                    const std::string target=m_supportedLocales[1];
                    if(!LoadLocale(target,true)){
                        PX_LOG_ERROR("Player E2E locale switch failed locale={}",target);
                        m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                        m_quitRequested=true;break;
                    }
                    m_e2eLocaleSwitched=true;
                    PX_LOG_INFO("Player E2E locale switched locale={} text={}",
                                target,m_session->Dialogue().State().fullText);
                    const auto localizedAudio = m_session->Audio().CaptureState();
                    std::string localizedVoice = localizedAudio.voice.path;
                    if (localizedVoice.empty()) {
                        const auto& backlog = m_session->Backlog().Entries();
                        const auto voice = std::ranges::find_if(
                            backlog.rbegin(), backlog.rend(),
                            [](const vn::BacklogEntry& entry) {
                                return !entry.voice.empty();
                            });
                        if (voice != backlog.rend())
                            localizedVoice = voice->voice;
                    }
                    PX_LOG_INFO(
                        "Player E2E localized media background={} voice={}",
                        m_session->Stage().BackgroundPath(),
                        localizedVoice);
                    break;
                }
                m_e2eClearCountBeforeF2=m_profile.ClearCount();
                if(const auto* value=m_session->Variables().GetValue("routeName"))
                    m_e2eSessionValueBeforeF2=value->Clone();
                input.InjectKeyPress(SDL_SCANCODE_F2);
                m_e2eStage=E2EStage::VerifyReleaseF2;
            }
            break;
        case E2EStage::VerifyReleaseF2: {
            const auto* value=m_session->Variables().GetValue("routeName");
            const bool sessionUnchanged=
                (!m_e2eSessionValueBeforeF2&&value==nullptr)||
                (m_e2eSessionValueBeforeF2&&value&&
                 *m_e2eSessionValueBeforeF2==*value);
            if(m_profile.ClearCount()!=m_e2eClearCountBeforeF2||
               !sessionUnchanged){
                PX_LOG_ERROR("Player release F2 policy changed persistent or session state");
                m_e2eFailed=true;m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;break;
            }
            PX_LOG_INFO("Player release F2 policy ignored demo reset shortcut");
            if (input.KeyBindings("screen.open.gallery") !=
                std::vector<int>{SDL_SCANCODE_F8}) {
                PX_LOG_ERROR("Player E2E saved Gallery shortcut was not restored");
                m_e2eFailed = true;
                m_e2eStage = E2EStage::Complete;
                m_quitRequested = true;
                break;
            }
            input.InjectKeyPress(SDL_SCANCODE_F8);
            m_e2eStage=E2EStage::AwaitGallery;
            break;
        }
        case E2EStage::AwaitGallery:
            if(m_session->Routes().CurrentModalRoute()=="gallery"&&
               m_ui.CurrentRoute()=="gallery")
                m_e2eStage=E2EStage::CaptureGallery;
            break;
        case E2EStage::CloseGallery:
            if(m_routePreloadBatch)break;
            if (input.HasAction("acceptance.close-gallery"))
                input.InjectKeyPress(SDL_SCANCODE_F6);
            else {
                input.InjectAction(InputAction::Cancel);
                input.InjectKeyPress(SDL_SCANCODE_ESCAPE);
            }
            m_e2eStage=E2EStage::AwaitOverlayClosed;
            break;
        case E2EStage::AwaitOverlayClosed:
            if(m_session->Routes().CurrentModalRoute().empty()){
                PX_LOG_INFO("Player E2E journey complete name=catalog");
                m_e2eStage=E2EStage::Complete;
                m_quitRequested=true;
            }
            break;
        default:break;
    }
}

void PlayerApp::CaptureUIRoutesE2EFrame() {
    if(!m_uiRoutesE2E||!m_uiRoutesE2EAwaitingCapture||
       m_uiRoutesE2EIndex>=std::size(kUIRouteE2ERoutes)||
       SDL_GetTicks()<m_uiRoutesE2ECaptureAt)
        return;
    // The capture must show the incoming route, not the held outgoing frame
    // or a mid-transition blend after a slow first-time asset preload.
    if (m_routePreloadBatch) return;
    if (const auto& transition = m_session->Routes().ActiveTransition();
        transition &&
        m_runtime.Renderer().ScreenEffectPlaying(transition->handle))
        return;
    const std::string_view route=kUIRouteE2ERoutes[m_uiRoutesE2EIndex];
    if(m_session->Routes().CurrentModalRoute()!=route||
       m_ui.CurrentRoute()!=route)
        return;
    const auto summary=graphics::CaptureFrameSummary(
        m_runtime.Renderer().Handle());
    const bool confirmCapture=route=="save"&&m_uiRoutesE2ESaveConfirmStage==2;
    const std::string captureName=
        "route-"+std::to_string(m_uiRoutesE2EIndex+1)+"-"+
        std::string(route)+(confirmCapture?"-confirm":"");
    const bool saved=CapturePresentationE2EFrame(captureName);
    PX_LOG_INFO(
        "Player UI routes E2E frame index={} route={} width={} height={} colors={} hash={} saved={}",
        m_uiRoutesE2EIndex,route,summary.width,summary.height,
        summary.sampledColors,summary.hash,saved);
    if(!saved||!summary.Valid()||summary.width<640||summary.height<360||
       summary.sampledColors<8){
        PX_LOG_ERROR("Player UI routes E2E frame is blank or undersized route={}",
                     route);
        m_e2eFailed=true;
        m_uiRoutesE2E=false;
        m_quitRequested=true;
        return;
    }
    if(route=="save"&&m_uiRoutesE2ESaveConfirmStage==0){
        auto* slot=FindUiControlNamed<ui::Button>(m_ui.Root(),"Slot0Button");
        // TSX-authored slot grids reuse one keyed template rather than
        // materializing a separately named button for every save slot.
        if(!slot)slot=FindUiControlNamed<ui::Button>(m_ui.Root(),"SlotButton");
        if(!slot){
            PX_LOG_ERROR("Player UI routes E2E authored save slot missing");
            m_e2eFailed=true;m_uiRoutesE2E=false;m_quitRequested=true;
            return;
        }
        slot->Activate();
        m_uiRoutesE2ESaveConfirmStage=1;
        m_uiRoutesE2EAwaitingCapture=false;
        return;
    }
    if(confirmCapture){
        auto* cancel=FindUiControlNamed<ui::Button>(
            m_ui.Root(),"ConfirmNoButton");
        if(!cancel){
            PX_LOG_ERROR("Player UI routes E2E authored save confirmation missing");
            m_e2eFailed=true;m_uiRoutesE2E=false;m_quitRequested=true;
            return;
        }
        cancel->Activate();
        m_uiRoutesE2ESaveConfirmStage=3;
        m_uiRoutesE2EAwaitingCapture=false;
        return;
    }
    const Status closed=m_session->Routes().CloseModal();
    if(!closed){
        for(const auto& diagnostic:closed.Diagnostics())diag::Emit(diagnostic);
        m_e2eFailed=true;
        m_uiRoutesE2E=false;
        m_quitRequested=true;
        return;
    }
    ++m_uiRoutesE2EIndex;
    m_uiRoutesE2EAwaitingCapture=false;
}

void PlayerApp::CaptureE2EGalleryFrame() {
    if(m_e2eStage!=E2EStage::CaptureGallery)return;
    if(m_routePreloadBatch)return;
    const auto capture=graphics::CaptureFrameSummary(
        m_runtime.Renderer().Handle());
    PX_LOG_INFO("Player E2E gallery frame width={} height={} colors={} hash={}",
                capture.width,capture.height,capture.sampledColors,capture.hash);
    if(!capture.Valid()||capture.width<640||capture.height<360||
       capture.sampledColors<8){
        PX_LOG_ERROR("Player E2E gallery frame is blank or undersized");
        m_e2eFailed=true;
        m_e2eStage=E2EStage::Complete;
        m_quitRequested=true;
        return;
    }
    m_e2eStage=E2EStage::CloseGallery;
}

#else

void PlayerApp::ConfigureE2EJourney() {}
void PlayerApp::DriveE2EJourney() {}
void PlayerApp::DriveBenchJourney() {}
void PlayerApp::BenchCapture(std::string) {}
void PlayerApp::CaptureE2EGalleryFrame() {}
void PlayerApp::CaptureUIRoutesE2EFrame() {}
bool PlayerApp::CapturePresentationE2EFrame(std::string_view) { return false; }

#endif

}  // namespace px::player
