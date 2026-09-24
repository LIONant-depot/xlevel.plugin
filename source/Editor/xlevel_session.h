#ifndef XLEVEL_SESSION_H
#define XLEVEL_SESSION_H
#pragma once

// The Level editor as a real xeditor::resource_editor: the ECS world, Game.dll scripting, Play/Pause/Stop,
// the Level Tree / Entity Properties / System Registry panels and their dockspace - everything
// source/Editors/LevelEditor/LevelEditor_App.h used to own directly for the one "Level" tool. The shell now
// opens this exactly like it opens Texture: xeditor::open_resource_editors::Open(LevelTypeGuid, LibraryGuid).
//
// Level does not fit xeditor::document_editor<T_DOC> - a Level is not one file with a fixed guid for life; it is
// a container whose own document guid tracks WHICHEVER level is currently loaded (xlevel::LevelDocument::
// getGuid() already reads State.m_CurrentLevel, empty until something is opened) and its "save" persists a
// whole tree of Scenes, not one descriptor. So this implements xeditor::resource_editor directly, the same
// carve-out xeditor_resource_editor.h's own top comment documents for exactly this shape.
//
// STAGE (b) of the LevelEditor-ownership move (see the task's own staged plan): the shell now opens exactly one
// of these at startup via xeditor::open_resource_editors::Open({.m_Type=xecs::level::type_guid_v}, {}) - see
// LevelEditor_AppInit.h. It is a SINGLETON, unlike Texture: opened once, never destroyed for the life of the
// process (resource_editor::m_bOpen is never set false - m_State.m_bLevelEditorOpen, unchanged from the old
// app-owned model, is what actually shows/hides the peer tab). That's a deliberate, documented choice - Level
// has no natural per-Guid "which one" the way double-clicking a Texture does; command_set (still in the shell
// until stage (c)'s split) and WireAssetBrowser both hold a raw xlevel::session* obtained once at startup, which
// would dangle if this object could be destroyed while the tab is closed.
//
// The command/undo split (stage (c) - a fresh, framework-owned workspace xundo::system in the shell vs. this
// session's own, entity/scene-mutation-only one) is NOT done here: m_Undo below is a fresh xundo::system with no
// commands registered against it yet - LevelEditor_CommandSet.h's command_set still lives in the shell,
// constructed against this session's m_CmdContext/m_Undo exactly as it used to be constructed against
// LevelHostSession's. Wiring the ~40 Level-tagged commands onto m_Undo directly (removing the shell's
// dependency on command_set for them) is the explicit stage (c) TODO marked below.
// xscene_editor.h first - xlevel_context.h's level_context derives from xscene::scene_context, exactly the
// same order the shell's own LevelEditor_Kit.h already requires (xscene before xlevel).
#include "plugins/xscene.plugin/source/Editor/xscene_editor.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_editor.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_editor_tabs.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_demo_content.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GamePlugin.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_scene_sanity_scan.h"
#include "source/Tools/Editor/xeditor_resource_editor.h"
#include "dependencies/xeditor/include/xeditor/host.h"
#include "dependencies/toolbar.imgui/ximgui_toolbar.h"
#include "dependencies/xresource_pipeline_v2/source/editor/E10_AssetBrowser.h"

#include <memory>

namespace xlevel
{
    // This editor's world: a fresh one with its systems, project paths, System Registry order and inspector
    // wiring. Ported from source/Editors/LevelEditor/LevelEditor_AppWorld.h's app::CreateWorld - component types
    // are registered once for the whole process (RegisterHostComponents, below), not per world.
    struct session : xeditor::resource_editor
    {
        LevelDocument                              m_Document;
        xundo::system                              m_Undo;
        level_state                                m_State;
        std::unique_ptr<xecs::game_mgr::instance>  m_pGameMgr;
        std::wstring                               m_ProjectPath;
        xgpu::device*                              m_pDevice = nullptr;   // null in headless builds - see resource_editor's own comment on m_pDevice

        game_plugin_state                          m_GamePlugin;
        play_gate                                  m_PlayGate;

        level_context                              m_CmdContext{ m_State, m_pGameMgr, m_Undo };
        scene_sanity_scanner                       m_SceneScanner{ m_CmdContext };

        xproperty::inspector                       m_EntityInspector{ "Inspector" };
        xscene::entity_inspector_bridge            m_InspectorBridge;

        // Scene tool state + the "Editor"/"Scene" toolbars (ported from LevelEditor_AppToolbars.h - these read
        // State/CmdContext/pGameMgr/GamePlugin directly, so they had to move here with the rest of the world/
        // document ownership rather than staying behind in the shell as originally sketched in stage (a)).
        int                                          m_SceneTool = 0; // Q=select, W=move, E=rotate, R=scale, F=frame
        bool                                         m_bPivotCenter = true;
        bool                                         m_bLocalSpace  = false;
        bool                                         m_bGridVisible = true;
        ximgui::toolbar::toolbar_host_state          m_EditorToolbarHost;
        static constexpr float                       kEditorToolbarWidth      = 570.0f;
        static constexpr float                       kSceneToolbarWidth       = 390.0f;
        static constexpr float                       kEditorToolbarHeight     = 20.0f;
        static constexpr float                       kEditorToolbarFontScale  = 1.0f;
        static constexpr float                       kEditorToolbarItemSpacing = 2.0f;

        std::vector<std::unique_ptr<xecs::scene::instance>> m_ReloadCapture;   // scenes held across a Game.dll reload

        // Registers this session's own demo content - kept as a plain static function (not inlined at each of
        // the two call sites below) so PollGameReload can re-run the exact same registration after a reload,
        // matching what construction does.
        static void RegisterHostComponents(xecs::game_mgr::instance& GameMgr) noexcept
        {
            GameMgr.RegisterComponents<xscene::name, xlevel::transform, xecs::editor::prefab_instance, xecs::component::entity_reference>();
        }

        static void RegisterHostSystems(xecs::game_mgr::instance& GameMgr) noexcept
        {
            GameMgr.RegisterSystems<xlevel::tick_logger_a, xlevel::tick_logger_b>();
        }

        session(xresource::full_guid /*Guid*/, e10::library::guid /*LibraryGuid*/, xgpu::device* pDevice) noexcept
            : m_pDevice(pDevice)
        {
            if (auto Err = m_Undo.Init({}, false); !Err.empty())
                xeditor::NotifyError(std::format("Level session xundo Init failed: {}", Err));
            m_Document.Bind(m_CmdContext);

            m_pGameMgr = std::make_unique<xecs::game_mgr::instance>();
            RegisterHostComponents(*m_pGameMgr);

            // Resolve the same project root every other editor example locates itself against (walking up from
            // the executable's own path to the first ancestor with a bootstrapped example.lionprj\Cache\Plugins -
            // see LevelEditor_AppInit.h's own comment for why this is structural, not name-based).
            TCHAR szModulePath[MAX_PATH];
            GetModuleFileName(NULL, szModulePath, MAX_PATH);
            std::filesystem::path RepoRoot;
            for (std::filesystem::path Dir = std::filesystem::path(szModulePath).parent_path(); ; )
            {
                std::error_code Ec;
                if (std::filesystem::exists(Dir / L"example.lionprj" / L"Cache" / L"Plugins", Ec) && !Ec) { RepoRoot = Dir; break; }
                const std::filesystem::path Parent = Dir.parent_path();
                if (Parent.empty() || Parent == Dir) break;
                Dir = Parent;
            }
            if (!RepoRoot.empty()) m_ProjectPath = e10::g_LibMgr.m_ProjectPath;   // the shell has already opened the project by the time Open() can run

#if defined(XECS_BUILD_SHARED)
            m_GamePlugin.m_Paths = xlevel::MakeScriptProjectPaths();
            if (!std::filesystem::exists(m_GamePlugin.m_Paths.m_CMakeLists))
                xlevel::RegenerateGameModuleSources(m_GamePlugin.m_Paths);
            xlevel::BuildGamePluginIfStale(m_GamePlugin, xlevel::GetLatestModuleSourceWriteTime(m_GamePlugin.m_Paths));
            xlevel::LoadGamePluginComponents(*m_pGameMgr, m_GamePlugin, /*Generation*/ 1);
#else
            xlevel::LogGamePlugin("Game.dll: this build was configured without XECS_BUILD_SHARED_LIBRARY - Game.dll support is disabled, Play just ticks the host's own systems.");
#endif
            RegisterHostSystems(*m_pGameMgr);
            xlevel::RegisterGamePluginSystems(*m_pGameMgr, m_GamePlugin);

            m_pGameMgr->m_SceneMgr.m_ProjectPath  = m_ProjectPath;
            m_pGameMgr->m_LevelMgr.m_ProjectPath  = m_ProjectPath;
            m_pGameMgr->m_PrefabMgr.m_ProjectPath = m_ProjectPath;
            m_pGameMgr->m_SystemMgr.m_ProjectPath = m_ProjectPath;
            if (auto Err = m_pGameMgr->m_SystemMgr.Load(); Err)
                xeditor::NotifyError(std::format("Failed to load System Registry order: {}", Err.getMessage()));

            m_EntityInspector.m_Settings.m_bRenderBackgroundDepth = false;
            m_EntityInspector.m_Settings.m_bRenderLeftBackground  = false;
            m_EntityInspector.m_Settings.m_bRenderRightBackground = false;
            m_EntityInspector.m_Settings.m_FramePadding      = ImVec2(4.0f, 3.0f);
            m_EntityInspector.m_Settings.m_ItemSpacing       = ImVec2(1.0f, 1.0f);
            m_EntityInspector.m_Settings.m_TableFramePadding = ImVec2(4.0f, 1.0f);
            e10::WireResourcePickerCallbacks(m_EntityInspector);
            m_InspectorBridge.RegisterCallbacks(m_EntityInspector, m_CmdContext);

#if defined(XECS_BUILD_SHARED)
            m_PlayGate.m_IsBuilding = [this]() noexcept { return m_GamePlugin.m_bBuilding; };
            m_PlayGate.m_StartBuild = [this]() noexcept { xlevel::StartGameReload(m_GamePlugin); };
#endif
            m_GamePlugin.m_Events.m_OnCollectRequiredComponents.Register<&session::CollectRequiredComponents>(*this);
            m_GamePlugin.m_Events.m_OnBeforeReload.Register<&session::BeforeReload>(*this);
            m_GamePlugin.m_Events.m_OnAfterReload.Register<&session::AfterReload>(*this);

            xlevel::g_pGamePlugin = &m_GamePlugin;

            // Service registration: lets xlevel::FindLevelContext()/TryGateLevelMutation (the write-lock gate
            // wired as EditorHost.m_OnBeforeEdit) and xscene:: code that takes a scene_context& reach this
            // session, the same way LevelEditor_AppInit.h used to provide its own CmdContext.
            if (auto* pHost = xeditor::host::current())
            {
                pHost->provide(m_CmdContext);
                pHost->provide<xscene::scene_context>(m_CmdContext);
#if defined(XECS_BUILD_SHARED)
                pHost->provide(m_PlayGate);
#endif
                pHost->m_IdleWork.m_OnRun.Register<&xlevel::scene_sanity_scanner::Run>(m_SceneScanner);
            }

            if (m_pDevice)   // non-headless: same one-time toolbar-position persistence AppInit.h used to do
            {
                m_EditorToolbarHost.m_Items.push_back
                ({ "Editor", ximgui::toolbar::toolbar_host_edge::Top, ximgui::toolbar::axis::Horizontal
                 , ImVec2(kEditorToolbarWidth, kEditorToolbarHeight), ImVec2(32.0f, 250.0f), ImVec2(24.0f, 24.0f) });
                m_EditorToolbarHost.m_Items.push_back
                ({ "Scene", ximgui::toolbar::toolbar_host_edge::Top, ximgui::toolbar::axis::Horizontal
                 , ImVec2(kSceneToolbarWidth, kEditorToolbarHeight), ImVec2(32.0f, 250.0f), ImVec2(24.0f, 72.0f) });
                ximgui::toolbar::RegisterSettingsHandler(m_EditorToolbarHost, "LevelEditorToolbar");
            }

            // TODO(stage c, command/undo split): register the Level-tagged half of
            // source/Editors/LevelEditor/commands/LevelEditor_CommandSet.h against m_Undo here (CmdSelect through
            // CmdDeleteFolder/CmdMakePrefabVariant - the entity/scene/play mutation commands; everything else -
            // asset CRUD, source control, compile, idle-work, Say/GetLog, OpenResourceEditor - stays registered
            // against the shell's own framework workspace xundo::system). Deferred so this checkpoint is a clean,
            // single, reviewable cut rather than two half-migrated command sets.
        }

        ~session() noexcept override
        {
            if (auto* pHost = xeditor::host::current())
            {
                pHost->m_IdleWork.m_OnRun.RemoveDelegates(&m_SceneScanner);
#if defined(XECS_BUILD_SHARED)
                pHost->withdraw<play_gate>();
#endif
                pHost->withdraw<xscene::scene_context>();
                pHost->withdraw<level_context>();
            }
            m_pGameMgr.reset();
            xlevel::UnloadGamePlugin(m_GamePlugin);
        }

        xeditor::IDocument& getDocument() noexcept override { return m_Document; }
        xundo::system&      getUndo()     noexcept override { return m_Undo; }
        bool                isLoaded()  const noexcept override { return true; }   // a Level tool is always "loaded" - it may simply have nothing open yet

        // ---- World lifecycle (ported from LevelEditor_AppWorld.h's app:: methods) ----
        void CreateWorld() noexcept
        {
            m_pGameMgr = std::make_unique<xecs::game_mgr::instance>();
            RegisterHostSystems(*m_pGameMgr);
            xlevel::RegisterGamePluginSystems(*m_pGameMgr, m_GamePlugin);

            m_pGameMgr->m_SceneMgr.m_ProjectPath  = m_ProjectPath;
            m_pGameMgr->m_LevelMgr.m_ProjectPath  = m_ProjectPath;
            m_pGameMgr->m_PrefabMgr.m_ProjectPath = m_ProjectPath;
            m_pGameMgr->m_SystemMgr.m_ProjectPath = m_ProjectPath;
            if (auto Err = m_pGameMgr->m_SystemMgr.Load(); Err)
                xeditor::NotifyError(std::format("Failed to load System Registry order: {}", Err.getMessage()));
        }

        void RestoreWorld(xlevel::persist_mode PersistMode) noexcept
        {
            m_State.m_SelectedEntity        = {};
            m_State.m_bEntityInspectorDirty = true;

            if (PersistMode == xlevel::persist_mode::RawSnapshotBridge)
            {
                xlevel::LoadSnapshot(*m_pGameMgr, xlevel::GetReloadBridgeSnapshotPath());
                xlevel::ReattachOpenScenes(*m_pGameMgr, std::move(m_ReloadCapture));
                if (!m_State.m_CurrentLevel.empty())
                    m_pGameMgr->m_LevelMgr.Load(m_State.m_CurrentLevel);
            }
            else if (!m_State.m_CurrentLevel.empty())
            {
                xlevel::OpenLevel(*m_pGameMgr, m_State, xresource::full_guid{ m_State.m_CurrentLevel.m_Instance, m_State.m_CurrentLevel.m_Type });
            }

            xlevel::LogWorldEntityCount(*m_pGameMgr, PersistMode == xlevel::persist_mode::RawSnapshotBridge ? "Vn restore" : "V1/disk restore");

            if (m_State.m_SelectedEntityId != xecs::scene::invalid_permanent_id_v)
            {
                if (auto* pScene = m_pGameMgr->m_SceneMgr.Find(m_State.m_SelectedEntityScene))
                {
                    if (auto It = pScene->m_LocalToRuntime.find(m_State.m_SelectedEntityId); It != pScene->m_LocalToRuntime.end())
                        m_State.m_SelectedEntity = It->second;
                    else
                        m_State.m_SelectedEntityId = xecs::scene::invalid_permanent_id_v;
                }
                else
                {
                    m_State.m_SelectedEntityId = xecs::scene::invalid_permanent_id_v;
                }
            }
        }

        void CollectRequiredComponents(std::vector<xecs::scene::component_dependency>& Out) noexcept
        {
            for (auto& SceneGuid : m_State.m_OpenScenes)
                for (auto& Dep : xecs::scene::LoadSceneComponentDependencies(m_ProjectPath, SceneGuid))
                    Out.push_back(Dep);
        }

        void BeforeReload() noexcept
        {
            m_ReloadCapture = xlevel::CaptureOpenScenes(*m_pGameMgr, m_State);
            xlevel::SaveSnapshot(*m_pGameMgr, xlevel::GetReloadBridgeSnapshotPath());
            m_pGameMgr.reset();
        }

        void AfterReload() noexcept
        {
            CreateWorld();
            RestoreWorld(xlevel::persist_mode::RawSnapshotBridge);
        }

        // The Stop button's own handler - see xlevel_play_session.h's own comment on V1/Vn for why Stop always
        // reloads from the last real disk save (V1), never a mid-play Vn bridge snapshot.
        void StopPlay(const std::vector<std::string>& KeepCommands) noexcept
        {
            m_Undo.JumpTo(m_State.m_PlayHistoryBoundary);

            m_pGameMgr->Stop();
            m_pGameMgr.reset();
            CreateWorld();
            RestoreWorld(xlevel::persist_mode::RestoreFromV1);

            m_Undo.TruncateRedoBranch();
            if (const auto Surviving = xlevel::FilterSurvivingTargets(*m_pGameMgr, KeepCommands); !Surviving.empty())
            {
                [[maybe_unused]] const bool bAllApplied = xeditor::RunGroup(m_Undo, "Keep Play Mode Changes", Surviving);
            }

            m_State.m_PlayState = xlevel::level_state::play_state::Stopped;
            if (auto* pHost = xeditor::host::current()) pHost->end_play(&m_State);
        }

        // ---- Menu bar + the "Editor"/"Scene" toolbars (ported from LevelEditor_AppToolbars.h) ----
        void RenderParentEditorToolbar() noexcept
        {
            if (!ImGui::BeginMenuBar()) return;

            if (ImGui::BeginMenu("File"))
            {
                if (auto* pHost = xeditor::host::current())
                    if (auto* pBrowser = pHost->find<e10::assert_browser>())
                        if (ImGui::MenuItem("Asset Browser...")) pBrowser->Show(true);

                ImGui::Separator();

                const bool bCanSave = !m_State.isPlaying()
                    && (!m_State.m_CurrentLevel.empty() || !m_State.m_OpenScenes.empty())
                    && xlevel::HasUnsavedDocumentChanges(m_State, m_Undo);
                ImGui::BeginDisabled(!bCanSave);
                if (ImGui::MenuItem("Save", "Ctrl+S")) { xlevel::SaveEverything(*m_pGameMgr, m_State); xlevel::MarkDocumentClean(m_State, m_Undo); }
                ImGui::EndDisabled();

                const bool bCanClose = !m_State.isPlaying() && (!m_State.m_CurrentLevel.empty() || !m_State.m_OpenScenes.empty());
                ImGui::BeginDisabled(!bCanClose);
                if (ImGui::MenuItem("Close")) xlevel::RequestCloseLevel(*m_pGameMgr, m_State, m_Undo);
                ImGui::EndDisabled();

                ImGui::EndMenu();
            }

            if (m_GamePlugin.m_bBuilding)
            {
                ImGui::SameLine(ImGui::GetWindowWidth() - 250.0f);
                ImGui::TextDisabled("Game.dll: building...");
            }

            xlevel::RenderPlayTransport(m_CmdContext, { ImVec2(30.0f, 0.0f), true, true });
            ImGui::EndMenuBar();
        }

        void RenderEditorToolbar(const char* Name, ximgui::toolbar::axis Axis) noexcept
        {
            const bool bHorizontal = Axis == ximgui::toolbar::axis::Horizontal;
            const float ButtonHeight = bHorizontal ? kEditorToolbarHeight - 4.0f : 28.0f;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kEditorToolbarItemSpacing, ImGui::GetStyle().ItemSpacing.y));
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kEditorToolbarFontScale);
            bool bFirstButton = true;

            auto ToolbarButton = [&](const char* LongLabel, const char* ShortLabel, bool bActive, bool bDisabled, auto&& OnClick)
            {
                if (bHorizontal && !bFirstButton) ImGui::SameLine();
                bFirstButton = false;
                if (bActive) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Header]);
                ImGui::BeginDisabled(bDisabled);
                if (ImGui::Button(bHorizontal ? LongLabel : ShortLabel, bHorizontal ? ImVec2(52.0f, ButtonHeight) : ImVec2(32.0f, ButtonHeight))) OnClick();
                ImGui::EndDisabled();
                if (bActive) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::TextUnformatted(LongLabel); ImGui::EndTooltip(); }
            };
            auto ToolbarSeparator = [&]()
            {
                if (bHorizontal) { ImGui::SameLine(); ImGui::TextDisabled("|"); }
                else ImGui::Separator();
            };

            if (std::strcmp(Name, "Editor") == 0)
            {
                const bool bCanSave = !m_State.isPlaying()
                    && (!m_State.m_CurrentLevel.empty() || !m_State.m_OpenScenes.empty())
                    && xlevel::HasUnsavedDocumentChanges(m_State, m_Undo);
                ToolbarButton("Save", "S", false, !bCanSave, [&]() { xlevel::SaveEverything(*m_pGameMgr, m_State); xlevel::MarkDocumentClean(m_State, m_Undo); });
                ToolbarButton("Undo", "U", false, m_State.isPlaying(), [&]() { m_Undo.Undo(); });
                ToolbarButton("Redo", "R", false, m_State.isPlaying(), [&]() { m_Undo.Redo(); });
                ToolbarButton("Assets", "A", false, false, [&]() { if (auto* pHost = xeditor::host::current()) pHost->open_drawer_tab(ImGui::GetMainViewport(), 1); });
                ToolbarSeparator();

                xlevel::RenderPlayTransport(m_CmdContext, { ImVec2(52.0f, ButtonHeight), bHorizontal, false });
                bFirstButton = false;

                ToolbarSeparator();
                ToolbarButton("Hierarchy", "H", false, false, [&]() { ImGui::SetWindowFocus(xlevel::editor_tabs::kLevelTreeWindow); });
                ToolbarButton("Inspector", "I", false, false, [&]() { ImGui::SetWindowFocus(xlevel::editor_tabs::kInspectorWindow); });
                ToolbarButton("Systems", "Y", false, false, [&]() { ImGui::SetWindowFocus(xlevel::editor_tabs::kSystemRegistryWindow); });
            }
            else
            {
                auto SceneButton = [&](const char* Label, int ToolIndex, const char* Tooltip)
                {
                    if (bHorizontal && !bFirstButton) ImGui::SameLine();
                    bFirstButton = false;
                    if (m_SceneTool == ToolIndex) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Header]);
                    if (ImGui::Button(Label, ImVec2(32.0f, ButtonHeight))) m_SceneTool = ToolIndex;
                    if (m_SceneTool == ToolIndex) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::TextUnformatted(Tooltip); ImGui::EndTooltip(); }
                };
                SceneButton("Q", 0, "Select tool");
                SceneButton("W", 1, "Move tool");
                SceneButton("E", 2, "Rotate tool");
                SceneButton("R", 3, "Scale tool");
                SceneButton("F", 4, "Frame selected");
                ToolbarSeparator();

                auto SceneToggle = [&](const char* LongLabel, const char* ShortLabel, bool& bValue)
                {
                    if (bHorizontal) ImGui::SameLine();
                    if (ImGui::Button(bHorizontal ? LongLabel : ShortLabel, bHorizontal ? ImVec2(58.0f, ButtonHeight) : ImVec2(32.0f, ButtonHeight))) bValue = !bValue;
                    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::TextUnformatted(LongLabel); ImGui::EndTooltip(); }
                };
                SceneToggle("Pivot", "P", m_bPivotCenter);
                SceneToggle("Local", "L", m_bLocalSpace);
                SceneToggle("Grid", "G", m_bGridVisible);
            }

            ImGui::PopFont();
            ImGui::PopStyleVar();
        }

        // Recompile-check completion + the deferred "Stop" click. MUST run at a clean top-of-frame point, never
        // nested inside an active ImGui frame (a Game.dll reload/Stop tears the whole world down and rebuilds
        // it - confirmed empirically in the original port that doing this from inside an active ImGui frame,
        // e.g. a menu-bar scope, corrupts ImGui's window-stack bookkeeping). Render() below always runs AFTER
        // xgpu::tools::imgui::BeginRendering() has already started the frame (it is called from
        // open_resource_editors::RenderAll(), itself called from mid-frame), so these two cannot live there -
        // the shell calls this once, before BeginRendering, exactly where LevelEditor_AppFrame.h's own
        // PollGameReload/deferred-Stop calls always ran.
        void PumpBeforeFrame() noexcept
        {
            xlevel::PollGameReload(m_CmdContext, m_GamePlugin, &session::RegisterHostComponents);

            if (m_State.m_bStopRequested)
            {
                m_State.m_bStopRequested = false;
                StopPlay(m_State.m_PendingKeepTweaksCommands);
                m_State.m_PendingKeepTweaksCommands.clear();
            }
        }

        // ---- The whole tool window: dockspace, panels, modals, the Play tick gate. ----
        // A peer root like Texture's own document_editor::Render(), but hand-implemented (not
        // xeditor::document_editor<T_DOC>) because a Level's own dockspace/panel shape (Level Tree + Inspector +
        // System Registry inside a further-nested dockspace, not a flat panel list) and always-open,
        // never-per-Guid-instanced lifecycle don't fit that template - see this file's own top comment.
        void Render() noexcept override
        {
            auto* pHost = xeditor::host::current();

            std::string LevelTabName;
            if (!m_State.m_CurrentLevel.empty())
                e10::RemapGUIDToString(LevelTabName, xresource::full_guid{ m_State.m_CurrentLevel.m_Instance, m_State.m_CurrentLevel.m_Type });
            else
                LevelTabName = "Level";

            xresource::full_guid LevelDockGuid{};
            if (!m_State.m_CurrentLevel.empty())
            {
                LevelDockGuid.m_Instance = m_State.m_CurrentLevel.m_Instance;
                LevelDockGuid.m_Type     = xecs::level::type_guid_v;
            }

            if (m_State.m_bAwaitingSaveBeforeClose) m_State.m_bLevelEditorOpen = true;

            // Level is a permanent, singleton tool (opened once at process startup, never destroyed like a
            // per-Guid Texture tab - see this file's own top comment) - resource_editor::m_bOpen therefore stays
            // true for the object's whole life; m_State.m_bLevelEditorOpen (unchanged from the old app-owned
            // model) is what actually drives whether the peer tab/dockspace is shown.
            const bool bSkipLevelPeer = !m_State.m_bLevelEditorOpen && m_State.m_CurrentLevel.empty()
                && m_State.m_OpenScenes.empty() && !m_State.m_bAwaitingSaveBeforeClose;

            bool bParentEditorVisible = false;
            if (!bSkipLevelPeer)
            {
                bool bTabOpen = m_State.m_bLevelEditorOpen;
                bParentEditorVisible = xlevel::editor_tabs::RenderLevelEditorDockspace(
                    [this]() { RenderParentEditorToolbar(); },
                    LevelTabName.c_str(), m_pDevice, xecs::level::type_guid_v, LevelDockGuid, &bTabOpen);
                if (!bTabOpen)
                {
                    xlevel::RequestCloseLevel(*m_pGameMgr, m_State, m_Undo);
                    m_State.m_bLevelEditorOpen = m_State.m_bAwaitingSaveBeforeClose || !m_State.m_CurrentLevel.empty() || !m_State.m_OpenScenes.empty();
                }
                else
                {
                    m_State.m_bLevelEditorOpen = true;
                }
            }

            if (bParentEditorVisible)
            {
                if (pHost) pHost->m_Notifier.render();
                xlevel::RenderKeepTweaksModal(m_CmdContext);
                // NOTE: the original (LevelEditor_AppFrame.h) passed the shell's separate workspace xundo::system
                // (LevelEditorUndo) here, not the Level session's own - a pre-existing mismatch against how
                // CmdRemoveSceneDependency is actually registered (see LevelEditor_CommandSet.h: Level, not
                // Workspace). Passing m_Undo is what stage (c)'s command split makes correct; not yet exercised
                // since nothing opens this session before stage (b).
                xlevel::RenderRemoveDependencyConfirmModal(m_Undo);
                xlevel::RenderSaveBeforeCloseModal(*m_pGameMgr, m_State, m_Undo);

                if (m_State.m_bPendingStartGameReloadAfterOpen)
                {
                    m_State.m_bPendingStartGameReloadAfterOpen = false;
#if defined(XECS_BUILD_SHARED)
                    xlevel::StartGameReload(m_GamePlugin);
#endif
                }

                if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !m_State.isPlaying()
                    && (!m_State.m_CurrentLevel.empty() || !m_State.m_OpenScenes.empty())
                    && xlevel::HasUnsavedDocumentChanges(m_State, m_Undo))
                {
                    xlevel::SaveEverything(*m_pGameMgr, m_State);
                    xlevel::MarkDocumentClean(m_State, m_Undo);
                }

                if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt && !m_State.isPlaying())
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_Z) && !ImGui::GetIO().KeyShift) m_Undo.Undo();
                    else if (ImGui::IsKeyPressed(ImGuiKey_Y) || (ImGui::IsKeyPressed(ImGuiKey_Z) && ImGui::GetIO().KeyShift)) m_Undo.Redo();
                }
            }

            if (m_State.m_PlayState == xlevel::level_state::play_state::Playing)
            {
                m_pGameMgr->Run();
                if (m_State.m_bStepOneFrame)
                {
                    m_State.m_bStepOneFrame = false;
                    m_State.m_PlayState = xlevel::level_state::play_state::Paused;
                }
            }
            else if (m_State.m_PlayState == xlevel::level_state::play_state::Paused && m_State.m_bStepOneFrame)
            {
                m_State.m_bStepOneFrame = false;
                m_pGameMgr->Run();
            }

            // Asset-open routing (a Level or Scene double-clicked/dropped from the browser) stays wired by the
            // shell's WireAssetBrowser (source/Editors/LevelEditor/extensions/asset_browser/), which reaches this
            // singleton session's m_pGameMgr/m_State/m_Undo directly the same way it always reached app::'s own -
            // see that file's own comment. A cleaner plugin-registered callback (per the "What moves where"
            // table) is left for a later pass; not required for this stage's compile-and-behave-identically bar.

            if (bParentEditorVisible)
            {
                if (m_State.m_bPlayBusyPopup)
                {
                    ImGui::OpenPopup("##PlayBusy");
                    m_State.m_bPlayBusyPopup = false;
                }
                if (ImGui::BeginPopupModal("##PlayBusy", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::Text("Play is already active in another Level editor.");
                    ImGui::Text("Stop that Play first, then try again.");
                    if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                xeditor::session* pMySession = nullptr;
                if (pHost) for (auto& S : pHost->m_Sessions) if (S && &S->undo() == &m_Undo) { pMySession = S.get(); break; }
                const bool bLevelWritable = xlevel::IsLevelWritable(pHost, pMySession, m_State);
                if (!bLevelWritable)
                {
                    xlevel::editor_tabs::SetNextLevelEditorToolClass();
                    if (ImGui::Begin("##LevelReadOnlyBanner", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize))
                        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Read-only: this Level/scene is being edited in another session.");
                    ImGui::End();
                }

                xlevel::editor_tabs::SetNextLevelEditorToolClass();
                xlevel::RenderLevelTreePanel(m_CmdContext, xlevel::editor_tabs::kLevelTreeWindow, m_Undo, !bLevelWritable);

                if (xlevel::FlushPendingOpenLevelFromTree(*m_pGameMgr, m_State, m_Undo))
                    xlevel::StartGameReload(m_GamePlugin);

                xlevel::editor_tabs::SetNextLevelEditorToolClass();
                xscene::RenderEntityPropertiesPanel(m_CmdContext, xlevel::editor_tabs::kInspectorWindow, m_EntityInspector, m_InspectorBridge, !bLevelWritable);

                xlevel::editor_tabs::SetNextLevelEditorToolClass();
                xlevel::RenderSystemRegistryPanel(*m_pGameMgr, m_State);

                ImGui::SetNextWindowPos(ImVec2(250.0f, 90.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(1050.0f, 480.0f), ImGuiCond_FirstUseEver);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                xlevel::editor_tabs::SetNextLevelEditorToolClass();
                if (ImGui::Begin(xlevel::editor_tabs::kEditorWindow))
                {
                    ximgui::toolbar::RenderToolbarHost(m_EditorToolbarHost, ImGui::GetContentRegionAvail(),
                        [this](const char* Name, ximgui::toolbar::axis Axis) { RenderEditorToolbar(Name, Axis); },
                        [&]() { ImGui::TextDisabled("Editor"); });
                }
                ImGui::End();
                ImGui::PopStyleVar();

                xlevel::RenderReloadCompatibilityModal(m_CmdContext);
            }
        }
    };

    // Same shape as xmaterial_editor.h's own g_Registration.
    inline const xeditor::auto_register_resource_editor g_Registration
    { xecs::level::type_guid_v
    , [](xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };
}

#endif // XLEVEL_SESSION_H
