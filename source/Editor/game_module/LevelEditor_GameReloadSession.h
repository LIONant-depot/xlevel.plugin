#ifndef XLVL_NEW_LevelEditor_GAME_RELOAD_SESSION_H
#define XLVL_NEW_LevelEditor_GAME_RELOAD_SESSION_H
#pragma once
#include "dependencies/xeditor/include/xeditor/popup.h"

// What the editor does around a Game.dll reload: the raw snapshot bridge (Vn) that keeps gameplay continuous across the
// destroy/recreate, carrying the open scenes across it, the recompile-check Play and window focus start, the confirm
// modal for components the new module lacks, and PollGameReload, which finishes it once the build is done. Play itself
// (the transport and Stop) is in the Level editor.
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_ComponentCompatibility.h"
#include "dependencies/xLIONCore/src/game/xlioncore_editor.h"
#include <sstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <optional>

namespace xlevel
{
    //---------------------------------------------------------------------------
    // V1 vs Vn, direct user model: "when you hit play you snapshot the current state and save it
    // (V1); when you hit pause you may recompile etc - we call these ones Vn (n>1); when you hit stop
    // you only care about reloading V1, all other ones are 100% irrelevant, because the point is get
    // back to normal editing" - and, critically, "like Unity the tree represents the current truth
    // of the scenes": entities can die or get created (dumped into the default folder) while playing,
    // so Stop must put the Level tree back exactly as it was before Play, not just restore raw
    // component values. V1 is therefore the REAL Scene/Level/Prefab disk save (SaveEverything/
    // OpenLevel - already Scene-aware, already the proven mechanism that reconstructs the tree
    // correctly) taken once at Play-entry - not a raw binary dump. Vn does NOT need any of that
    // (direct user confirmation: "V1 is the only one that needs to serialize [the tree]... Vn does
    // not need that") - it stays the fast, ephemeral, scene-unaware xecs::game_mgr::instance::
    // SerializeGameState bridge below, purely to keep gameplay itself continuous across a mid-play
    // Game.dll reload, never touching the real saved assets and never read back by Stop.
    inline std::wstring GetReloadBridgeSnapshotPath( std::uint64_t EditorKey = 0 ) noexcept
    {
        // The process id is part of the name: two editors open on the same Level (a second launch, a test run beside the one being worked in) would otherwise write
        // and read ONE file in the temp folder, and one of them fails with "Permission denied" - which leaves its scenes without their entities after the reload.
        return (std::filesystem::temp_directory_path() / std::format(L"xGPU_LevelEditor_ReloadBridge_{}_{:016X}.bin", static_cast<unsigned long>(GetCurrentProcessId()), EditorKey)).wstring();
    }

    inline bool SaveSnapshot( xecs::game_mgr::instance& GameMgr, const std::wstring& Path ) noexcept
    {
        const std::string PathA{ std::filesystem::path(Path).string() };
        auto* pEcs = xlioncore::EditorOf(GameMgr);                      // the xECSEditor of the copy of the core this world belongs to
        if (!pEcs) { LogGamePlugin("Game.dll: snapshot save failed: the world has no xECSEditor"); return false; }
        if (auto Err = pEcs->SerializeGameState(PathA.c_str(), /*isRead*/false, /*isBinary*/true); Err)
        {
            LogGamePlugin(std::format("Game.dll: snapshot save failed: {}", Err.getMessage()));
            return false;
        }
        return true;
    }

    inline bool LoadSnapshot( xecs::game_mgr::instance& GameMgr, const std::wstring& Path, const game_plugin_state* pPlugin = nullptr ) noexcept
    {
        if (pPlugin && pPlugin->m_bSimulateSnapshotFailure)
        {
            LogGamePlugin("Game.dll: snapshot restore failed: simulated (SimulateSnapshotFailure)");
            return false;
        }
        const std::string PathA{ std::filesystem::path(Path).string() };
        auto* pEcs = xlioncore::EditorOf(GameMgr);
        if (!pEcs) { LogGamePlugin("Game.dll: snapshot restore failed: the world has no xECSEditor"); return false; }
        auto Err = pEcs->SerializeGameState(PathA.c_str(), /*isRead*/true, /*isBinary*/true);
        std::error_code Ec;
        std::filesystem::remove(Path, Ec);                      // a bridge is for one reload; it is not left behind in the temp folder
        if (Err)
        {
            LogGamePlugin(std::format("Game.dll: snapshot restore failed: {}", Err.getMessage()));
            return false;
        }
        return true;
    }

    //---------------------------------------------------------------------------
    // Diagnostic only - the Level tree can't show anything meaningful right after a raw snapshot
    // restore (no Scene ever gets reopened - see persist_mode's own comment), so this is the one way
    // to actually confirm real entity/component data survived the round trip rather than just
    // guessing from an empty-looking tree. Left in permanently (not added-then-reverted) per this
    // project's own persistent-diagnostic-logging convention - logged to the Game.dll Log panel,
    // which is already visible, after every single reload regardless of which persist_mode ran.
    //---------------------------------------------------------------------------
    inline void LogWorldEntityCount( xecs::game_mgr::instance& GameMgr, const char* pLabel ) noexcept
    {
        int nArchetypes = 0;
        int nEntities    = 0;
        for (auto& pArchetype : GameMgr.m_ArchetypeMgr.m_lArchetype)
        {
            ++nArchetypes;
            for (auto pF = pArchetype->getFamilyHead(); pF; pF = pF->m_Next.get())
                for (auto pP = &pF->m_DefaultPool; pP; pP = pP->m_Next.get())
                    nEntities += pP->Size();
        }
        LogGamePlugin(std::format("Game.dll: [{}] world now has {} archetype(s), {} live entit(y/ies)", pLabel, nArchetypes, nEntities));
    }

    //---------------------------------------------------------------------------
    // The Vn (RawSnapshotBridge) tree-preservation trick - direct user insight: "the raw
    // serialization just needs to make sure entities are restored with the same exact ID" (confirmed
    // empirically: xecs::component::entity's own m_Value round-trips bit-for-bit identical through
    // SerializeGameState - the write path's own "GlobalEntities" record restores each entity's
    // Validation flag at its EXACT original global-info slot index, not a freshly reallocated one -
    // see xecs_game_mgr.cpp's own comment on that record). Since the entity VALUES a Scene's
    // m_LocalToRuntime/m_RuntimeToLocal maps reference never change, NO translation is needed at all
    // - moving the whole xecs::scene::instance object out before the destroy and back in after the
    // restore is sufficient; its maps are still valid, unmodified, pointing at the exact same entity
    // values that come back. Folders/parent-scene edges/pending-changes/everything else about the
    // scene comes along for free in the same move, for the same reason RestoreFromV1 doesn't need
    // any of this at all (it goes through OpenLevel instead).
    //---------------------------------------------------------------------------
    inline std::vector<std::unique_ptr<xecs::scene::instance>> CaptureOpenScenes
    ( xecs::game_mgr::instance& GameMgr
    , const xlevel::level_state&       State
    ) noexcept
    {
        std::vector<std::unique_ptr<xecs::scene::instance>> Captured;
        for (auto& SceneGuid : State.m_OpenScenes)
        {
            if (auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid))
            {
                LogGamePlugin(std::format("Game.dll: [Vn capture] scene {:016X} - {} entit(y/ies), {} folder(s)",
                    SceneGuid.m_Instance.m_Value, pScene->m_LocalToRuntime.size(), pScene->m_Folders.size()));
                Captured.push_back(std::make_unique<xecs::scene::instance>(std::move(*pScene)));
            }
            else
            {
                LogGamePlugin(std::format("Game.dll: [Vn capture] scene {:016X} NOT FOUND in SceneMgr", SceneGuid.m_Instance.m_Value));
            }
        }
        LogGamePlugin(std::format("Game.dll: [Vn capture] {} of {} open scene(s) captured", Captured.size(), State.m_OpenScenes.size()));
        return Captured;
    }

    inline void ReattachOpenScenes
    ( xecs::game_mgr::instance&                              GameMgr
    , std::vector<std::unique_ptr<xecs::scene::instance>>&&  Captured
    ) noexcept
    {
        for (auto& pScene : Captured)
        {
            const auto SceneGuid = pScene->m_Guid;
            auto& NewScene = xlioncore::Ecs(GameMgr).FindOrCreateScene(SceneGuid);
            NewScene = std::move(*pScene);
            LogGamePlugin(std::format("Game.dll: [Vn reattach] scene {:016X} - {} entit(y/ies), {} folder(s), state={}",
                SceneGuid.m_Instance.m_Value, NewScene.m_LocalToRuntime.size(), NewScene.m_Folders.size(), (int)NewScene.m_State));
        }
    }

    //---------------------------------------------------------------------------
    // Re-registers an ALREADY-loaded plugin module's components against a freshly reset registry,
    // without touching the DLL itself at all (no FreeLibrary/LoadLibrary, no new shadow copy, same
    // xecs::plugin::token/generation as before) - the world still has to be destroyed and recreated
    // (the component registry is reset process-wide the moment ANY plugin generation changes owner,
    // and a fresh xecs::game_mgr::instance needs everything re-registered into it from scratch), but
    // the CODE didn't change, so there's no reason to pay for a fresh compile-output copy or a
    // FreeLibrary/LoadLibrary cycle. Used by StopPlaySession, where "as fast as possible" applies just
    // as much as it does to the play-session snapshot above - Stop is not a recompile, it's "throw
    // away the play session's world and rebuild a clean one".
    //---------------------------------------------------------------------------
    inline void ReregisterAlreadyLoadedPlugin( xecs::game_mgr::instance& GameMgr, game_plugin_state& Plugin ) noexcept
    {
        if (!Plugin.isLoaded() || Plugin.m_bCrashed) return;

        if (auto* pRegisterComponents = reinterpret_cast<xecs_plugin_pfn_register_components*>(GetProcAddress(Plugin.m_hModule, XECS_PLUGIN_REGISTER_COMPONENTS_NAME)))
        {
            struct call { xecs_plugin_pfn_register_components* m_pFn; xecs::game_mgr::instance* m_pGameMgr; xecs::plugin::token m_Token; } Call{ pRegisterComponents, &GameMgr, Plugin.m_Token };
            GuardedModuleCall(Plugin, "its components", [](void* p) noexcept { auto& C = *static_cast<call*>(p); C.m_pFn(*C.m_pGameMgr, C.m_Token); }, &Call);
        }
    }

    //---------------------------------------------------------------------------
    // Step 1 of 2 - a recompile-CHECK, not a user-facing "reload" action anymore (there is no more
    // manual "Reload Game" button - matches Unity's own model: recompiling is something the editor
    // just does for you). Called automatically from two places only, per direct user direction: once
    // on the frame the app window regains OS focus (xgpu::tools::imgui::ConsumeWindowFocusGained -
    // "the user tabbed back in after editing code"), and once when the Play button is pressed
    // (Stopped -> Playing only - see level_state::m_bPlayRequested). Kicks off
    // BuildGamePluginIfStale on a background thread and returns immediately; does NOT touch
    // pGameMgr/the world/the currently loaded generation AT ALL - that's the whole point (see
    // game_plugin_state's own comment). A no-op if a build is already in flight.
    //---------------------------------------------------------------------------
    inline void StartGameReload( game_plugin_state& Plugin ) noexcept
    {
        if (Plugin.m_bBuilding) return;

        // A project with no script modules has nothing to put in Game.dll: no cmake, no build, no DLL - the check is answered
        // right away (the editors waiting on it, e.g. for Play, see "up to date" at their next frame).
        // The Game this plugin state (a Level) builds and loads. Each Game has its own game project, DLL and build markers (script_project_paths::ForGame).
        const auto Game = Plugin.m_Game;
        Plugin.m_Paths = ForGame(Plugin.m_Paths, Game);

        if ((!Game || ReadGame(Game).m_Modules.empty()) && !Plugin.isLoaded())
        {
            Plugin.m_LastStatus = "Game.dll: the project has no script modules - nothing to build";
            Plugin.m_LastResult = build_result::UpToDate;
            ++Plugin.m_ResultSeq;
            return;
        }

        Plugin.m_bBuilding  = true;
        Plugin.BuildJob();                                   // made here, on the thread that starts the build: CancelBuild reads it from another one
        Plugin.m_GameInputs = CaptureGameInputs(Game);      // what the build waits for the resource pipeline to have made: read here for the same reason
        // A Game whose compile is current but whose game project is not there (a cache that was cleared, or a project made before each Game had its own folder): the pipeline would
        // not run it again, so ask for it.
        if (Plugin.m_GameInputs.m_bHasGame && !std::filesystem::exists(Plugin.m_GameInputs.m_CMakeLists))
            xresource_editor::g_LibMgr.RecompileResource(xresource_editor::g_LibMgr.m_ProjectGUID, xresource::full_guid{ xresource::instance_guid{ Game }, xgame::type_guid_v });
        // Computed HERE, on the main thread, and captured by value - NOT re-computed inside the
        // background task. See BuildGamePluginIfStale's own comment on ModuleSourceTime for why: it
        // reads xlevel::g_ScriptConfig/the Game resource, neither safe to touch from the background thread
        // this function's lambda runs on.
        const auto ModuleSourceTime = GetLatestModuleSourceWriteTime(Plugin.m_Paths);
        Plugin.m_BuildFuture = std::async(std::launch::async, [&Plugin, ModuleSourceTime]() noexcept
        {
            return BuildGamePluginIfStale(Plugin, ModuleSourceTime);
        });
    }

    // How a world is persisted across the destroy/recreate it always does - the ONE thing
    // that genuinely differs between "a normal reload" and "Stop", beyond just which DLL-swap
    // strategy applies. Direct user model: Play writes ONE snapshot ("V1", the REAL Scene/Level/
    // Prefab disk save - see GetReloadBridgeSnapshotPath's own comment for why this must be disk, not
    // the fast binary dump) the moment it starts; every mid-play/paused reload afterward writes its
    // own throwaway "Vn" (n>1, the fast binary bridge) purely to keep gameplay continuous across that
    // one reload - Stop only ever cares about V1, every Vn is 100% irrelevant to it, because the
    // whole point of Stop is getting back to normal editing - Level tree included - exactly as it was
    // before Play, matching Unity's own Play/Stop semantics.
    //
    //   RawSnapshotBridge - EVERY code-triggered reload (Playing, Paused, AND plain edit-mode) lands
    //                       here now - the world must be destroyed anyway (a Game.dll swap), so
    //                       write/read this reload's own throwaway "Vn" (GetReloadBridgeSnapshotPath -
    //                       overwritten every cycle) so whatever the user currently has - gameplay
    //                       state while Playing, or just unsaved edits while Stopped - survives the
    //                       destroy/recreate intact, entirely in memory, without touching the real
    //                       saved project on disk. Scene-unaware, and deliberately so (Vn never needs
    //                       the tree, only V1 does) - CaptureOpenScenes/ReattachOpenScenes carry the
    //                       tree across separately, snapshot-independent.
    //   RestoreFromV1     - Stop. Never saves anything - there's nothing worth saving; whatever the
    //                       play session's raw Vn bridging left the world in is 100% discarded.
    //                       Reloads via OpenLevel - correct precisely because V1 was itself a real
    //                       disk save (written explicitly by Play, or by PollGameReload's own
    //                       UpToDate/Rebuilt branches right before flipping to Playing - never as a
    //                       silent side effect of an edit-mode reload), so "reload from disk" already
    //                       means "reload V1", nothing more needs building.
    //
    // There used to be a third mode, DiskSaveAndReload, used for every edit-mode (not-playing)
    // reload - it saved the real Scene/Level/Prefab assets to disk unconditionally as part of the
    // reload. Removed per direct user request after an external review correctly flagged it: tabbing
    // back into the editor after an unrelated code edit would silently commit whatever was in the
    // scene to disk, with no explicit Save action from the user - surprising, and unlike Unity/Unreal,
    // neither of which persists anything to the real project on a domain reload / Live Coding patch.
    // RawSnapshotBridge already does everything DiskSaveAndReload needed (preserve current state
    // across the destroy/recreate) without the disk write, so switching every reload to it was a
    // straight subtraction, not a new code path - see PollGameReload's own comment for the one place
    // that used to get V1 "for free" as DiskSaveAndReload's side effect and now writes it explicitly.
    enum class persist_mode : std::uint8_t { RawSnapshotBridge, RestoreFromV1 };



    //---------------------------------------------------------------------------
    // Walks every currently open scene's own live entities, finds every one that actually HAS one of
    // MissingGuids (via its archetype's own component bits, resolved through the registry the OLD
    // generation - still fully loaded and running at the point this is called, from the confirm
    // modal's own "Strip and Continue" button - is still registered against), and removes it via the
    // normal, undo-tracked command bus (so the user can Undo this later if it turns out to be wrong).
    // Best-effort: a guid the OLD registry itself doesn't resolve either (shouldn't happen - it came
    // straight out of this session's own live world moments ago) is silently skipped.
    //---------------------------------------------------------------------------
    // Declared (not defined - see scene/commands/LevelEditor_Commands_MakePrefab.h for the inline definition this
    // refers to) here too since this file's own place in the umbrella include order is earlier than
    // that one - inline variables have external linkage, so a plain extern declaration anywhere in
    // the same program is enough to use it, no redefinition risk.
    inline void StripMissingComponentsFromOpenScenes( xlevel::level_context& Ed, const std::vector<xecs::scene::component_dependency>& MissingDeps ) noexcept
    {
        auto*          pWorld   = &Ed.World();
        auto*          pState   = &Ed.State();
        xundo::system* pDocUndo = &Ed.m_Undo;

        for (auto& SceneGuid : pState->m_OpenScenes)
        {
            auto* pScene = pWorld->m_SceneMgr.Find(SceneGuid);
            if (!pScene) continue;

            for (auto& Pair : pScene->m_LocalToRuntime)
            {
                const auto Id     = Pair.first;
                auto&      Entity = Pair.second;

                auto& Ecs = xlioncore::Ecs(*pWorld);
                if (!Ecs.IsAlive(Entity)) continue;

                for (auto& Dep : MissingDeps)
                {
                    if (!Ecs.HasComponent(Entity, Dep.m_Guid)) continue;

                    xeditor::Run(*pDocUndo, std::format("RemoveComponent -Scene {} -Id {} -Component {:016X}"
                        , xscene::commands::FormatSceneGuid(SceneGuid)
                        , xscene::commands::FormatEntityId(Id)
                        , Dep.m_Guid.m_Value
                        ));
                }
            }
        }
    }

    //---------------------------------------------------------------------------
    // Called once per frame by the Level's editor. Reads/writes the pending state of the Level's game module and strips the components from the Level's own scenes.
    //---------------------------------------------------------------------------
    inline void RenderReloadCompatibilityModal(xlevel::level_context& Ed, const ImVec2* pCenter = nullptr) noexcept
    {
        auto* pPlugin = Ed.m_pGamePlugin;
        if (!pPlugin) return;
        if (pPlugin->m_bPendingMissing)
            ImGui::OpenPopup("Game.dll Reload - Missing Components");

        if (xeditor::BeginModal("Game.dll Reload - Missing Components", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings, pCenter))
        {
            if (pPlugin->m_bPendingMissing)
            {
                ImGui::Text("The new Game.dll build no longer has %zu component type(s)\nthat currently-open scenes use:", pPlugin->m_PendingMissing.size());
                for (auto& Dep : pPlugin->m_PendingMissing)
                    ImGui::BulletText("%s", Dep.m_Name.c_str());
                ImGui::Separator();
                ImGui::TextWrapped(
                    "Strip and Continue: removes these components from the affected entities\n"
                    "(undoable) and reloads.\n"
                    "Cancel: keeps the current generation running - fix your script and try again."
                );
                ImGui::Separator();

                if (ImGui::Button("Strip and Continue", ImVec2(160, 0)))
                {
                    StripMissingComponentsFromOpenScenes(Ed, pPlugin->m_PendingMissing);
                    pPlugin->m_bPendingMissing = false;
                    pPlugin->m_PendingMissing.clear();
                    ImGui::CloseCurrentPopup();
                    StartGameReload(*pPlugin);
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    pPlugin->m_bPendingMissing = false;
                    pPlugin->m_PendingMissing.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    //---------------------------------------------------------------------------
    // Step 2 of 2 - call once per frame, at a clean frame boundary (BEFORE BeginRendering, never
    // mid-frame - confirmed empirically that running the heavy world-rebuild synchronously inside an
    // active ImGui frame corrupts its window-stack bookkeeping). A no-op unless a build is both
    // in-flight AND finished (checked via a non-blocking wait_for), so safe to call unconditionally
    // every frame regardless of Plugin.m_bBuilding's current value.
    //
    // On a FAILED build: stops here, and cancels any pending Play request (m_bPlayRequested) rather
    // than starting a play session against a known-broken build. The currently loaded generation (if
    // any) was never unloaded, never touched - it just keeps running exactly as it was.
    //
    // On UpToDate (checked but nothing needed rebuilding - the common case once this runs on every
    // focus-regain, not just an explicit click): no world-touching reload at all. If a Play was
    // requested, it can proceed directly - SaveEverything below gives Stop a fresh, correct revert
    // point, and Play just keeps ticking the SAME live world (no reason to tear anything down over a
    // check that found nothing to do).
    //
    // On Rebuilt: runs the full destroy/recreate/DLL-swap sequence via ReloadGameModule, always via the
    // raw in-memory snapshot bridge (persist_mode::RawSnapshotBridge) regardless of Play state - see
    // persist_mode's own comment for why this reload never touches disk on its own anymore. If a Play
    // was ALSO requested (the user pressed Play while a rebuild happened to be needed), enters play
    // directly afterward - but MUST write V1 explicitly here (see below), since the reload itself no
    // longer does that as a side effect the way the old DiskSaveAndReload mode used to.
    //---------------------------------------------------------------------------
    template< typename T_REGISTER_HOST_COMPONENTS_FN >
    bool PollGameReload( xlevel::level_context& Ed, game_plugin_state& Plugin, T_REGISTER_HOST_COMPONENTS_FN&& RegisterHostComponents, std::uint32_t& SeenResultSeq ) noexcept
    {
        auto& State = Ed.State();

        // Game.dll is shared by every open Level: whichever editor notices the build finished first takes the result and, if
        // a new module was built, does the one reload (which has every editor snapshot and rebuild its own world).
        if (Plugin.m_bBuilding && Plugin.m_BuildFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            Plugin.m_LastResult = Plugin.m_BuildFuture.get();
            Plugin.m_bBuilding  = false;
            ++Plugin.m_ResultSeq;
            if (Plugin.m_LastResult == build_result::Rebuilt)
                ReloadGameModule(Plugin, RegisterHostComponents);
        }

        if (SeenResultSeq == Plugin.m_ResultSeq) return false;
        SeenResultSeq = Plugin.m_ResultSeq;

        // The part that belongs to this editor alone: a Play it was waiting on.
        if (Plugin.m_LastResult == build_result::Failed)
        {
            xlevel::CancelPlayRequest(State);
            return false;
        }

        if (State.m_bPlayRequested)
        {
            State.m_bPlayRequested = false;
            xlevel::EnterPlaying(Ed);
        }
        return Plugin.m_LastResult == build_result::Rebuilt;
    }
}

#endif // XLVL_NEW_LevelEditor_GAME_RELOAD_SESSION_H
