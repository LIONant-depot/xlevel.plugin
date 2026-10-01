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
// One of these per open Level, exactly like Texture: open_resource_editors::Open(LevelGuid, LibraryGuid) creates it
// (a double-click in the Assets panel, a drop, the OpenLevel command), it loads that Level, and closing the Level
// closes it. Several can be open at once; each has its own world, undo stack, panels and window ids (m_Names). What
// they share is the game module (Game.dll - the scripts belong to no Level), see level_services. One extra,
// never-shown editor without a Level exists so the commands that name no Level have something to act on when none is
// open.
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
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_editor.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_editor_tabs.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_demo_content.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GamePlugin.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_scene_sanity_scan.h"
#include "source/Tools/Editor/xeditor_resource_editor.h"
#include "source/Tools/Editor/xeditor_inspector.h"
#include "source/Tools/Editor/xeditor_toolbar.h"
#include "dependencies/xeditor/include/xeditor/host.h"
#include "dependencies/toolbar.imgui/ximgui_toolbar.h"
#include "dependencies/actions.imgui/ximgui_actions.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_asset_browser.h"
#include "dependencies/xeditor_tools/src/xeditor_tools_camera.h"
#include "dependencies/xeditor_tools/src/xeditor_tools_grid.h"
#include "source/tools/xgpu_imgui_breach.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_plugin_dlls.h"
#include "dependencies/xLIONRender/src/xlionrender_api.h"
#include "dependencies/xLIONCore/src/tags/xlioncore_tags.h"
#include "dependencies/xLIONCore/src/transform/xlioncore_transform.h"
#include "dependencies/ImGuizmo/src/ImGuizmo.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_viewport_tools.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_tool_collider_box.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_tool_collider_shapes.h"

#include <memory>
#include <limits>
#include <cmath>

namespace xlevel
{
    // What every open Level shares: the game module (Game.dll - the scripts are a resource of their own, not part of any
    // Level) and the gate Play waits on for its build. Created by the first editor, unloaded by the shell at shutdown.
    struct level_services
    {
        game_plugin_state Plugin;
        play_gate         Gate;
        bool              bReady = false;

        // True while the editor started without a usable Game.dll and is building its first one in the background. The
        // editor is up and responsive meanwhile, but opening a Level (and the command pipe) waits for it: a Level opened
        // without its script components would silently drop them from every entity that has one.
        bool              bInitialBuild = false;
    };

    inline level_services& Services() noexcept { static level_services s_Services; return s_Services; }

    // Called once by the shell, after every editor is gone and before the process ends.
    inline void ShutdownLevelServices() noexcept
    {
        auto& Svc = Services();
        if (!Svc.bReady) return;
#if defined(XECS_BUILD_SHARED)
        if (auto* pHost = xeditor::host::current()) pHost->withdraw<play_gate>();

        // A build still running is stopped rather than waited for.
        if (Svc.Plugin.m_bBuilding)
        {
            xlevel::CancelGameBuild();
            if (Svc.Plugin.m_BuildFuture.valid()) Svc.Plugin.m_BuildFuture.wait();
            Svc.Plugin.m_bBuilding = false;
        }
#endif
        xlevel::UnloadGamePlugin(Svc.Plugin);
        Svc.bReady = false;
    }

    struct session;
    // The shell hooks this to give every new Level editor its commands (the ones that are addressed to it by name).
    inline std::function<void(session&)> g_OnSessionCreated;

    // The Level editor's ACTIONS: what its keys, menus, toolbar buttons, the command palette and the command pipe reach.
    // An action is an xproperty function member; its identity is its path (Level/Save, Level/Viewport/ToolMove). Keys are
    // member_keys tags (defaults - the user's keymap replaces them), the "why not" is member_dynamic_reason. See
    // dependencies/actions.imgui. Each does exactly what the hand-written key check or toolbar lambda used to do.
    // What the mouse does in the Level editor's surfaces (listed on the mouse in the F1 view, and in the status line under the cursor).
    // Descriptions only: the viewport and the tree handle the mouse themselves, as before.
    namespace gestures
    {
        using namespace ximgui::actions;
        inline constexpr gesture viewport[] =
        { { 0,             mouse_input::Left,   mouse_kind::Click,  "Select",      "Selects the entity under the mouse; clicking empty ground clears the selection." }
        , { ImGuiMod_Ctrl, mouse_input::Left,   mouse_kind::Click,  "Add / remove","Adds the entity under the mouse to the selection, or takes it out." }
        , { 0,             mouse_input::Left,   mouse_kind::Drag,   "Gizmo",       "Drag a handle of the gizmo to move, rotate or scale the selected entity (Q W E R choose the tool)." }
        , { 0,             mouse_input::Right,  mouse_kind::Drag,   "Look around", "Turns the camera where it is." }
        , { 0,             mouse_input::Right,  mouse_kind::Drag,   "Fly",         "Keep it held and press W A S D to move, Q down, E up.", "W A S D Q E" }
        , { 0,             mouse_input::Middle, mouse_kind::Drag,   "Pan",         "Slides the camera sideways and up / down." }
        , { 0,             mouse_input::Wheel,  mouse_kind::Scroll, "Zoom",        "Moves the camera toward or away from what it looks at." } };

        inline constexpr gesture tree[] =
        { { 0,             mouse_input::Left,   mouse_kind::Click,  "Select",      "Selects the entity (when the button is released, so a drag can start from the row)." }
        , { ImGuiMod_Ctrl, mouse_input::Left,   mouse_kind::Click,  "Add / remove","Adds the entity to the selection, or takes it out." }
        , { 0,             mouse_input::Left,   mouse_kind::Click,  "Rename",      "Clicking the one selected entity again starts renaming it." }
        , { 0,             mouse_input::Left,   mouse_kind::Drag,   "Move",        "Drag an entity onto a folder, or onto the Level row to take it out of its folder." }
        , { 0,             mouse_input::Right,  mouse_kind::Click,  "Menu",        "Opens the menu of the row." } };
    }

    struct session_actions
    {
        session* m_pS = nullptr;        // xproperty creates objects by default construction, so the owner is a pointer set by the session
        session_actions() noexcept = default;
        explicit session_actions(session& S) noexcept : m_pS(&S) {}
        session& S() const noexcept { return *m_pS; }

        void        Save()       noexcept;      const char* WhyNoSave() const noexcept;
        void        Undo()       noexcept;      const char* WhyNoUndo() const noexcept;
        void        Redo()       noexcept;      const char* WhyNoRedo() const noexcept;
        void        Delete()     noexcept;      const char* WhyNoDelete() const noexcept;
        void        Rename()     noexcept;      const char* WhyNoRename() const noexcept;
        void        Play()       noexcept;      const char* WhyNoPlay()   const noexcept;
        void        Stop()       noexcept;      const char* WhyNoStop()   const noexcept;
        void        ToolSelect() noexcept;
        void        ToolMove()   noexcept;
        void        ToolRotate() noexcept;
        void        ToolScale()  noexcept;
        const char* WhyNoTool()  const noexcept;

        XPROPERTY_DEF
        ( "Level", session_actions
        , obj_action<"Save", &session_actions::Save
            , member_help<"Saves the Level and the Scenes it may write">
            , ximgui::actions::member_keys<"Ctrl+S", true>
            , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoSave(); }> >
        , obj_action<"Undo", &session_actions::Undo
            , member_help<"Undoes the last change to this Level">
            , ximgui::actions::member_keys<"Ctrl+Z">
            , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoUndo(); }> >
        , obj_action<"Redo", &session_actions::Redo
            , member_help<"Redoes the change that was just undone">
            , ximgui::actions::member_keys<"Ctrl+Y,Ctrl+Shift+Z">
            , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoRedo(); }> >
        , obj_action<"Play", &session_actions::Play
            , member_help<"Starts playing this Level (or resumes it)">
            , ximgui::actions::member_keys<"F5">
            , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoPlay(); }> >
        , obj_action<"Stop", &session_actions::Stop
            , member_help<"Stops playing and puts the Level back as it was">
            , ximgui::actions::member_keys<"Shift+F5">
            , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoStop(); }> >
        , obj_scope<"Entity"
            , obj_action<"Delete", &session_actions::Delete
                , member_help<"Deletes the selected entity and everything under it (undoable)">
                , ximgui::actions::member_keys<"Delete">
                , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoDelete(); }> >
            , obj_action<"Rename", &session_actions::Rename
                , member_help<"Renames the selected entity in the Level tree">
                , ximgui::actions::member_keys<"F2">
                , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoRename(); }> >
            >
        , obj_scope<"Viewport"
            , obj_action<"ToolSelect", &session_actions::ToolSelect
                , member_help<"Select tool (while a viewport tool is editing: finishes it)">
                , ximgui::actions::member_keys<"Q">
                , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoTool(); }> >
            , obj_action<"ToolMove", &session_actions::ToolMove
                , member_help<"Move tool (while a viewport tool is editing: Move mode)">
                , ximgui::actions::member_keys<"W">
                , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoTool(); }> >
            , obj_action<"ToolRotate", &session_actions::ToolRotate
                , member_help<"Rotate tool (while a viewport tool is editing: Rotate mode)">
                , ximgui::actions::member_keys<"E">
                , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoTool(); }> >
            , obj_action<"ToolScale", &session_actions::ToolScale
                , member_help<"Scale tool (while a viewport tool is editing: Resize mode)">
                , ximgui::actions::member_keys<"R">
                , member_dynamic_reason<+[](const session_actions& A) noexcept -> const char* { return A.WhyNoTool(); }> >
            >
        )
    };
    XPROPERTY_REG(session_actions)
    XIMGUI_ACTIONS_OWNER(session_actions)

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

        game_plugin_state&                         m_GamePlugin = Services().Plugin;   // shared by every open Level
        xresource::full_guid                       m_LevelGuid;                        // the Level this editor is for (empty: the stand-in editor that is used when no Level is open)
        editor_tabs::window_names                  m_Names;                            // this editor's own window ids
        std::uint32_t                              m_SeenBuildSeq = 0;                 // the last finished Game.dll build this editor reacted to
        bool                                       m_bOpenRequested = false;           // a Level was asked for (closing it closes this editor)

        level_context                              m_CmdContext{ m_State, m_pGameMgr, m_Undo };
        scene_sanity_scanner                       m_SceneScanner{ m_CmdContext };

        xproperty::inspector                       m_EntityInspector{ "Inspector" };
        xscene::entity_inspector_bridge            m_InspectorBridge;

        // Scene tool state + the "Editor"/"Scene" toolbars (ported from LevelEditor_AppToolbars.h - these read
        // State/CmdContext/pGameMgr/GamePlugin directly, so they had to move here with the rest of the world/
        // document ownership rather than staying behind in the shell as originally sketched in stage (a)).
        int                                          m_SceneTool = 0; // Q=select, W=move, E=rotate, R=scale, F=frame
        session_actions                              m_Actions{ *this };      // the keys/menu/toolbar actions (see session_actions)
        bool                                         m_bPivotCenter = true;
        bool                                         m_bLocalSpace  = false;
        ximgui::toolbar::toolbar_host_state          m_EditorToolbarHost{ {}, false, [](const char* pText) noexcept { xeditor::hint::Text("%s", pText); } };      // its tooltip is the editors' hint window
        char                                         m_ToolbarHandlerName[48] = {};
        bool                                         m_bToolbarHandler = false;

        // Gizmo drag state (Move/Rotate/Scale tools) - persists across frames while ImGuizmo::IsUsing()
        // is true, so the Translate/Rotate/Scale command is Execute()'d exactly once on mouse-release
        // (Before = value captured when the drag started), not once per frame.
        bool                                         m_bGizmoWasUsing     = false;
        xmath::fvec3                                 m_GizmoBeforePosition{};
        xmath::fquat                                 m_GizmoBeforeRotation{};
        xmath::fvec3                                 m_GizmoBeforeScale{};

        // Per-component viewport tools ("Edit Collider", ...) - see xlevel_viewport_tools.h. While one is
        // active it owns the W/E/R keys and the gizmo; the Transform gizmo steps aside.
        viewport_tools::editor                       m_ToolEditor;
        bool                                         m_bLevelWritable = true;     // last frame's, for the Inspector toggle
        std::function<void(xproperty::inspector&, const xproperty::type::object&, void*, std::string_view, int)> m_OnArrayElementRender;
        std::function<void(xproperty::inspector&, const xproperty::type::object&, void*, std::string_view)> m_OnScaleRow;

        // The "Editor" viewport's own camera + ground grid - every other 3D editor already shares these
        // via xeditor_tools; the Level Editor never had a camera or a grid at all before this.
        xeditor_tools::camera                       m_Camera;
        xecs::scene::permanent_id                   m_CameraAimedAt = xecs::scene::invalid_permanent_id_v;     // the selection the camera last turned to look at

        // The camera turns to look at a new selection without moving: the eye stays where it is, the angles (and the distance to the orbit
        // point) glide from what they are to what looks at the entity.
        struct camera_glide
        {
            xmath::fvec3 m_Eye   = xmath::fvec3::fromZero();
            float        m_Pitch = 0, m_Yaw = 0, m_Distance = 0;       // where the glide started
            float        m_dPitch = 0, m_dYaw = 0, m_dDistance = 0;    // how far it goes
            float        m_T = 1.0f;                                   // how far along it is (1 = arrived)
        }                                           m_CameraGlide;
        xeditor_tools::grid                          m_Grid;
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
            GameMgr.RegisterComponents<xecs::editor::prefab_instance, xecs::component::entity_reference>();
            RegisterEngineDLLComponents(GameMgr, L"LIONCore.dll");
            RegisterEngineDLLComponents(GameMgr, L"LIONRender.dll");
        }

        // Not static (unlike RegisterHostComponents, passed around as a bare function pointer by
        // PollGameReload) - needs m_pDevice to know whether to register LIONRender's own system.
        void RegisterHostSystems(xecs::game_mgr::instance& GameMgr) noexcept
        {
            GameMgr.RegisterSystems<xlevel::tick_logger_a, xlevel::tick_logger_b>();
            RegisterEngineDLLSystems(GameMgr, L"LIONCore.dll");

            // LIONRender's component (Primitive) is registered unconditionally above
            // (RegisterHostComponents) so headless scenes still carry the data - but headless has no
            // device/window to draw with, so its render SYSTEM never runs there.
            if (m_pDevice) RegisterEngineDLLSystems(GameMgr, L"LIONRender.dll");

            // xscene.plugin/xlevel.plugin (compiled directly into THIS binary, xLION.exe) name
            // xlioncore::static_tag/transform directly for the static-demotion-while-Playing feature -
            // real types, actually registered by xLIONCore.dll's own XecsPlugin_RegisterSystems just
            // above (which already syncs ITS OWN local info_v copies via the same SyncLocalBitIDs<>()).
            // That sync only fixes xLIONCore.dll's copies, though - info_v<T> is a per-BINARY
            // singleton (see info::m_BitID's own comment in xecs_component_type.h), so xLION.exe gets
            // its own separate, otherwise-never-synced copies of these same two types. This is the
            // exact scenario SyncLocalBitIDs<>() itself documents ("types some OTHER binary
            // registered that this one queries/creates", e.g. LIONRender using LIONCore's rigid_body) -
            // mirroring xLIONCore.dll's own call above, just for this binary's copies instead. Without
            // this, any exe-side code reading xlioncore::static_tag/transform's raw .m_BitID directly
            // gets a garbage/default value (confirmed live: asserted "Bit >= 0 && Bit < max" the one
            // time this was missed) - must run after Lock (already done, inside LIONCore.dll's own
            // RegisterSystems above) and before any host-compiled system/command touches these types.
            xecs::component::mgr::SyncLocalBitIDs<xlioncore::static_tag, xlioncore::transform>();
        }

        session(xresource::full_guid Guid, xresource_editor::library::guid /*LibraryGuid*/, xgpu::device* pDevice) noexcept
            : m_pDevice(pDevice)
        {
            m_LevelGuid = Guid;
            m_Names.Init(Guid.m_Instance.m_Value);

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
            if (!RepoRoot.empty()) m_ProjectPath = xresource_editor::g_LibMgr.m_ProjectPath;   // the shell has already opened the project by the time Open() can run

#if defined(XECS_BUILD_SHARED)
            // The first editor brings the game module up; every other one just uses it (its components are already in the
            // process-wide registry, each editor only registers the systems into its own world).
            if (auto& Svc = Services(); !Svc.bReady)
            {
                // Never blocks startup on a build:
                //  - no script modules in the project: there is nothing to build or load at all;
                //  - a usable Game.dll (built by this configuration) is loaded right away, even if its sources changed -
                //    the rebuild then runs in the background and swaps it in like any other reload;
                //  - no usable Game.dll: the first one is built in the background (bInitialBuild) and Levels open when it is.
                m_GamePlugin.m_Paths = xlevel::MakeScriptProjectPaths();
                if (auto Err = xlevel::LoadScriptConfig(m_GamePlugin.m_Paths.m_Project.wstring(), xlevel::g_ScriptConfig); Err)
                    xlevel::LogGamePlugin(std::format("Game.dll: failed to read Script.config.txt: {}", Err.getMessage()));
                xlevel::RegenerateGameModuleSources(m_GamePlugin.m_Paths);   // rewrites only what changed

                if (xlevel::g_ScriptConfig.m_ModuleRefs.empty())
                {
                    m_GamePlugin.m_LastStatus = "Game.dll: the project has no script modules - nothing to build or load";
                    xlevel::LogGamePlugin(m_GamePlugin.m_LastStatus);
                }
                else
                {
                    if (xlevel::IsGamePluginUsable(m_GamePlugin.m_Paths))
                        xlevel::LoadGamePluginComponents(*m_pGameMgr, m_GamePlugin, /*Generation*/ 1);

                    if (xlevel::IsGamePluginStale(m_GamePlugin.m_Paths, xlevel::GetLatestModuleSourceWriteTime(m_GamePlugin.m_Paths)))
                    {
                        xlevel::LogGamePlugin(m_GamePlugin.isLoaded()
                            ? "Game.dll: sources changed - rebuilding in the background, the current build stays loaded until then"
                            : "Game.dll: no usable build - building in the background, Levels open when it is ready");
                        xlevel::StartGameReload(m_GamePlugin);
                        Svc.bInitialBuild = !m_GamePlugin.isLoaded();
                    }
                }

                Svc.Gate.m_IsBuilding = []() noexcept { return Services().Plugin.m_bBuilding; };
                Svc.Gate.m_StartBuild = []() noexcept { xlevel::StartGameReload(Services().Plugin); };
                if (auto* pHost = xeditor::host::current()) pHost->provide(Svc.Gate);
                Svc.bReady = true;
            }
            m_SeenBuildSeq = m_GamePlugin.m_ResultSeq;
#else
            if (auto& Svc = Services(); !Svc.bReady)
            {
                xlevel::LogGamePlugin("Game.dll: this build was configured without XECS_BUILD_SHARED_LIBRARY - Game.dll support is disabled, Play just ticks the host's own systems.");
                Svc.bReady = true;
            }
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
            xeditor::BindInspectorHints(m_EntityInspector);        // property help is the editors' hint window
            xresource_editor::WireResourcePickerCallbacks(m_EntityInspector);
            m_InspectorBridge.RegisterCallbacks(m_EntityInspector, m_CmdContext);

            // Per-element viewport-tool toggles ("Edit Collider" on each PhysicsColliderBox box row).
            // m_ComponentMap maps the live instance back to its component type (the bridge keeps it current).
            m_OnArrayElementRender = [this](xproperty::inspector&, const xproperty::type::object&, void* pInstance, std::string_view ArrayPath, int Index)
            {
                auto It = m_InspectorBridge.m_ComponentMap.find(pInstance);
                if (It == m_InspectorBridge.m_ComponentMap.end()) return;
                m_ToolEditor.RenderInspectorToggle(m_State, *It->second, ArrayPath, Index, !m_bLevelWritable || m_State.isPlaying());
            };
            m_EntityInspector.m_OnArrayElementRender.Register(m_OnArrayElementRender);

            // Transform/Scale header row: after its label, a right-aligned sticky "Lock" toggle bound to
            // transform::m_EditorLockScale (see transform::setScaleAxis). A plain field write - not undoable.
            m_OnScaleRow = [this](xproperty::inspector&, const xproperty::type::object&, void* pInstance, std::string_view Path)
            {
                if (Path != "Transform/Scale") return;
                auto It = m_InspectorBridge.m_ComponentMap.find(pInstance);
                if (It == m_InspectorBridge.m_ComponentMap.end() || It->second->m_Guid.m_Value != xlioncore::transform::typedef_v.m_Guid.m_Value) return;

                auto& T = *static_cast<xlioncore::transform*>(pInstance);
                ImGui::SameLine();
                const char* pIcon = xlevel::DependenciesIcon();      // the "Link" glyph, same as the Level Tree Dependencies row
                const float W     = ImGui::CalcTextSize(pIcon).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - W));
                const bool bOn = T.m_EditorLockScale;
                // Frameless: transparent button, the glyph colour carries the state (accent when locked, dim when not).
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, bOn ? ImVec4(0.35f, 0.65f, 1.0f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::BeginDisabled(!m_bLevelWritable || m_State.isPlaying());
                if (ImGui::SmallButton(pIcon)) T.m_EditorLockScale = !bOn;
                ImGui::EndDisabled();
                ImGui::PopStyleColor(4);
                xproperty::inspector::Tooltip("Lock scale: editing one axis scales all three proportionally");
            };
            m_EntityInspector.m_OnLeftColumnAppend.Register(m_OnScaleRow);

            m_GamePlugin.m_Events.m_OnCollectRequiredComponents.Register<&session::CollectRequiredComponents>(*this);
            m_GamePlugin.m_Events.m_OnBeforeReload.Register<&session::BeforeReload>(*this);
            m_GamePlugin.m_Events.m_OnAfterReload.Register<&session::AfterReload>(*this);

            xlevel::g_pGamePlugin = &m_GamePlugin;

            // Registration: lets xlevel::TryGateLevelMutation (the write-lock gate wired as EditorHost.m_OnBeforeEdit) and the
            // commands that act on "the Level the user is working on" reach this editor. The newest real Level becomes the
            // active one; the stand-in editor (no Level) only serves until there is one.
            m_CmdContext.m_pToolEditor = &m_ToolEditor;
            g_LevelContexts.push_back(&m_CmdContext);
            if (g_pActiveLevelContext == nullptr || !m_LevelGuid.m_Instance.empty())
                g_pActiveLevelContext = &m_CmdContext;
            if (auto* pHost = xeditor::host::current())
                pHost->m_IdleWork.m_OnRun.Register<&xlevel::scene_sanity_scanner::Run>(m_SceneScanner);

            if (m_pDevice)   // non-headless: same one-time toolbar-position persistence AppInit.h used to do
            {
                m_EditorToolbarHost.m_Items.push_back
                ({ "Scene", ximgui::toolbar::toolbar_host_edge::Top, ximgui::toolbar::axis::Horizontal
                 , ImVec2(kSceneToolbarWidth, kEditorToolbarHeight), ImVec2(32.0f, 250.0f), ImVec2(24.0f, 72.0f) });
                // One handler per editor, under its own name, and removed again in the destructor (it points at this editor).
                std::snprintf(m_ToolbarHandlerName, sizeof(m_ToolbarHandlerName), "LevelEditorToolbar%016llX", static_cast<unsigned long long>(m_LevelGuid.m_Instance.m_Value));
                ximgui::toolbar::RegisterSettingsHandler(m_EditorToolbarHost, m_ToolbarHandlerName);
                m_bToolbarHandler = true;
            }

            // A Level editor is for one Level: load it (and every Scene it owns) right away, and have the game module's
            // build check run once it is up, like the old double-click did.
            if (!m_LevelGuid.m_Instance.empty())
            {
                m_bOpenRequested = true;
                xlevel::OpenLevel(*m_pGameMgr, m_State, m_LevelGuid);
                xlevel::MarkDocumentClean(m_State, m_Undo);
                m_State.m_bPendingStartGameReloadAfterOpen = true;
            }

            // The shell builds the commands that are addressed to this editor by name (Name\Command) and keeps them in
            // m_ShellCommands, which goes away before anything they refer to.
            if (g_OnSessionCreated) g_OnSessionCreated(*this);
        }

        ~session() noexcept override
        {
            m_ShellCommands.reset();   // the commands registered on m_Undo go first

            // A closing editor gives back whatever it held: the Play slot and its Scene/Level write locks.
            if (auto* pHost = xeditor::host::current())
            {
                pHost->m_IdleWork.m_OnRun.RemoveDelegates(&m_SceneScanner);
                pHost->end_play(&m_State);
                if (auto* pMe = xlevel::FindHostSession(m_Undo))
                    std::erase_if(pHost->m_WriteLocks, [&](const xeditor::host::write_lock& L) noexcept { return L.pWriter == pMe; });
            }

            m_GamePlugin.m_Events.m_OnCollectRequiredComponents.RemoveDelegates(this);
            m_GamePlugin.m_Events.m_OnBeforeReload.RemoveDelegates(this);
            m_GamePlugin.m_Events.m_OnAfterReload.RemoveDelegates(this);

            std::erase(g_LevelContexts, &m_CmdContext);
            if (g_pActiveLevelContext == &m_CmdContext)
            {
                g_pActiveLevelContext = nullptr;
                for (auto* pCtx : g_LevelContexts) g_pActiveLevelContext = pCtx;   // the newest one left (the stand-in if that is all there is)
            }

            if (m_bToolbarHandler && ImGui::GetCurrentContext()) ImGui::RemoveSettingsHandler(m_ToolbarHandlerName);

            m_pGameMgr.reset();
            m_Grid.Release();
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
                xlevel::LoadSnapshot(*m_pGameMgr, xlevel::GetReloadBridgeSnapshotPath(m_LevelGuid.m_Instance.m_Value));
                xlevel::ReattachOpenScenes(*m_pGameMgr, std::move(m_ReloadCapture));
                if (!m_State.m_CurrentLevel.empty())
                    m_pGameMgr->m_LevelMgr.Load(m_State.m_CurrentLevel);
            }
            else if (!m_State.m_CurrentLevel.empty())
            {
                xlevel::OpenLevel(*m_pGameMgr, m_State, xresource::full_guid{ m_State.m_CurrentLevel.m_Instance, m_State.m_CurrentLevel.m_Type });
            }

            if (!m_LevelGuid.m_Instance.empty())   // the stand-in editor has no world worth reporting
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
            xlevel::SaveSnapshot(*m_pGameMgr, xlevel::GetReloadBridgeSnapshotPath(m_LevelGuid.m_Instance.m_Value));
            m_pGameMgr.reset();
        }

        void AfterReload() noexcept
        {
            CreateWorld();
            m_pGameMgr->EnableBuilders(m_State.isPlaying());
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

        // The host's action context (keys, hints), if it provides one.
        ximgui::actions::context* ActionContext() const noexcept
        {
            auto* pHost = xeditor::host::current();
            return pHost ? pHost->find<ximgui::actions::context>() : nullptr;
        }

        // Q/W/E/R and their toolbar buttons: while a viewport tool is editing they drive ITS mode (Move/Rotate/Resize, Q ends
        // it); otherwise they pick the scene tool.
        void SetSceneTool(int ToolIndex) noexcept
        {
            if (m_ToolEditor.isActive() && ToolIndex <= 3) m_ToolEditor.SetToolIndex(ToolIndex);
            else                                           m_SceneTool = ToolIndex;
        }

        // After a toolbar button: the action's hint (name, live key, why not) in place of a hand-written tooltip.
        void ActionHint(std::string_view SubPath, const char* pFallback) noexcept
        {
            auto* pCtx = ActionContext();
            const auto* pA = pCtx ? pCtx->Find(*xproperty::getObject(m_Actions), SubPath) : nullptr;
            if (pA) pCtx->Hint(*pA, &m_Actions);
            else if (ImGui::IsItemHovered()) { xeditor::hint::Text("%s", pFallback); }
        }

        // ---- Menu bar + the "Editor"/"Scene" toolbars (ported from LevelEditor_AppToolbars.h) ----
        // The top bar of the window: the same one every other editor has (xeditor::RenderEditorToolbar): Undo / Redo / Save on the left, and in the
        // middle - where the others have Compile + Feedback - Play and Step. No File menu: Save is here and on Ctrl+S, the Asset Browser is the drawer
        // (Space), closing the Level is the tab's close button.
        static void CenterPlay(void* pUser) noexcept
        {
            auto& Self = *static_cast<session*>(pUser);
            if (Self.m_GamePlugin.m_bBuilding)
            {
                ImGui::SameLine(ImGui::GetWindowWidth() - 250.0f);
                ImGui::TextDisabled("Game.dll: building...");
            }
            xlevel::RenderPlayTransport(Self.m_CmdContext, { ImVec2(30.0f, 0.0f), true, true });
        }

        void RenderParentEditorToolbar() noexcept
        {
            xeditor::toolbar_model Bar{};
            Bar.m_pUndo             = &m_Undo;
            Bar.m_bUndoRedoEnabled  = !m_State.isPlaying();
            Bar.m_bDirty            = m_Actions.WhyNoSave() == nullptr;
            Bar.m_bCanCompile       = false;
            Bar.m_pUser             = this;
            Bar.m_OnSave            = [](void* p) noexcept { static_cast<session*>(p)->m_Actions.Save(); };
            Bar.m_OnUndo            = [](void* p) noexcept { static_cast<session*>(p)->m_Actions.Undo(); };
            Bar.m_OnRedo            = [](void* p) noexcept { static_cast<session*>(p)->m_Actions.Redo(); };
            Bar.m_OnHint            = [](void* p, const char* pAction) noexcept { static_cast<session*>(p)->ActionHint(pAction, pAction); };
            Bar.m_OnCenter          = &session::CenterPlay;
            xeditor::RenderEditorToolbar(Bar);
        }

        void RenderEditorToolbar(const char* Name, ximgui::toolbar::axis Axis) noexcept
        {
            const bool bHorizontal = Axis == ximgui::toolbar::axis::Horizontal;
            const float ButtonHeight = bHorizontal ? kEditorToolbarHeight - 4.0f : 28.0f;
            if (auto* pCtx = ActionContext(); pCtx && pCtx->DrawToolbar(Name, !bHorizontal)) return;     // the user's own layout of this toolbar (keymap file)
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kEditorToolbarItemSpacing, ImGui::GetStyle().ItemSpacing.y));
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kEditorToolbarFontScale);
            bool bFirstButton = true;

            auto ToolbarButton = [&](const char* LongLabel, const char* ShortLabel, bool bActive, bool bDisabled, auto&& OnClick, const char* pAction = nullptr)
            {
                if (bHorizontal && !bFirstButton) ImGui::SameLine();
                bFirstButton = false;
                if (bActive) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Header]);
                ImGui::BeginDisabled(bDisabled);
                if (ImGui::Button(bHorizontal ? LongLabel : ShortLabel, bHorizontal ? ImVec2(52.0f, ButtonHeight) : ImVec2(32.0f, ButtonHeight))) OnClick();
                ImGui::EndDisabled();
                if (bActive) ImGui::PopStyleColor();
                if (pAction) ActionHint(pAction, LongLabel);
                else if (ImGui::IsItemHovered()) { xeditor::hint::Text("%s", LongLabel); }
            };
            auto ToolbarSeparator = [&]()
            {
                if (bHorizontal) { ImGui::SameLine(); ImGui::TextDisabled("|"); }
                else ImGui::Separator();
            };

            // (The old "Editor" toolbar row - Save / Undo / Redo / Assets / Play / Step / Hierarchy / Inspector / Systems - is gone: the
            // top bar is the same one every other editor has, see RenderParentEditorToolbar.)
            {
                auto SceneButton = [&](const char* Label, int ToolIndex, const char* Tooltip, const char* pAction = nullptr)
                {
                    if (bHorizontal && !bFirstButton) ImGui::SameLine();
                    bFirstButton = false;
                    // While a viewport tool is editing, W/E/R drive ITS mode (Move/Rotate/Resize) and Q ends it.
                    const bool bToolKey = m_ToolEditor.isActive() && ToolIndex <= 3;
                    const bool bActive  = bToolKey ? ToolIndex >= 1 && static_cast<int>(m_ToolEditor.m_Mode) == ToolIndex - 1
                                                   : m_SceneTool == ToolIndex;
                    if (bActive) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Header]);
                    if (ImGui::Button(Label, ImVec2(32.0f, ButtonHeight))) SetSceneTool(ToolIndex);
                    if (bActive) ImGui::PopStyleColor();
                    if (pAction) ActionHint(pAction, Tooltip);
                    else if (ImGui::IsItemHovered()) { xeditor::hint::Text("%s", Tooltip); }
                };
                SceneButton("Q", 0, "Select tool", "Viewport/ToolSelect");
                SceneButton("W", 1, "Move tool",   "Viewport/ToolMove");
                SceneButton("E", 2, "Rotate tool", "Viewport/ToolRotate");
                SceneButton("R", 3, "Scale tool",  "Viewport/ToolScale");
                SceneButton("F", 4, "Frame selected");
                ToolbarSeparator();

                auto SceneToggle = [&](const char* LongLabel, const char* ShortLabel, bool& bValue)
                {
                    if (bHorizontal) ImGui::SameLine();
                    if (ImGui::Button(bHorizontal ? LongLabel : ShortLabel, bHorizontal ? ImVec2(58.0f, ButtonHeight) : ImVec2(32.0f, ButtonHeight))) bValue = !bValue;
                    if (ImGui::IsItemHovered()) { xeditor::hint::Text("%s", LongLabel); }
                };
                SceneToggle("Pivot", "P", m_bPivotCenter);
                SceneToggle("Local", "L", m_bLocalSpace);
            }

            ImGui::PopFont();
            ImGui::PopStyleVar();
        }

        // The "Editor" window's 3D viewport body: camera + ground grid, shared with every other editor via
        // xeditor_tools. Runs inside RenderToolbarHost's own child window (the toolbar host's own comment
        // documents that its RenderBody callback already gets a dedicated child region), so
        // GetContentRegionAvail/GetCursorScreenPos here are exactly the viewport rect. No shadow caster
        // exists for Level geometry yet, so the grid is always lit (a zero shadow matrix - the same "no
        // shadow" convention xeditor_tools::grid::Draw already documents).
        void RenderViewport() noexcept
        {
            if (!m_pDevice) { ImGui::TextDisabled("Editor"); return; }
            const bool bFirstInit = !m_Grid.m_bReady;
            if (!m_Grid.Init(*m_pDevice, false)) { ImGui::TextDisabled("Editor"); return; }
            if (bFirstInit)
            {
                // A zero near/far (xgpu::tools::view's own default) clips the 100-unit grid entirely -
                // every other editor's own camera setup already sets this before first use.
                m_Camera.m_View.setFov(60_xdeg);
                m_Camera.m_View.setNearZ(0.01f);
                m_Camera.m_View.setFarZ(10000.0f);

                // Every other editor's camera auto-fits around real geometry (m_bReframe fits m_Radius/
                // m_Center); Level starts with no geometry to fit to, so a zero-pitch default look would
                // stare straight along the horizon - the flat ground plane edge-on, invisible. Start at a
                // fixed 3/4-overhead framing instead, the same kind of default every other editor's own
                // camera ends up at once it has fit around something.
                m_Camera.m_bReframe = false;
                m_Camera.m_Distance = 15.0f;
                m_Camera.m_Angles   = xmath::radian3(-30_xdeg, 45_xdeg, 0_xdeg);
                m_Camera.m_Target   = { 0, 0, 0 };

                xlionrender::Init(*m_pDevice);
            }

            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const ImVec2 Min   = ImGui::GetCursorScreenPos();
            if (Avail.x <= 1.0f || Avail.y <= 1.0f) return;

            // View matrices must be current before the gizmo below, which in turn must run before the
            // InvisibleButton (see the gizmo block's comment) - so this is one frame stale relative to
            // this frame's HandleInput() orbit adjustment. Only visible while actively orbiting, and
            // orbiting (right/middle) and dragging the gizmo (left) are mutually exclusive anyway.
            // A new selection (from the viewport, the tree, undo...) becomes the point the camera orbits around and looks at.
            // Only when the selection changes, so orbiting and panning afterwards stay free.
            if (m_State.m_SelectedEntityId != m_CameraAimedAt)
            {
                m_CameraAimedAt = m_State.m_SelectedEntityId;
                if (m_CameraAimedAt != xecs::scene::invalid_permanent_id_v)
                    if (auto* pXform = xscene::commands::ResolveTransform(m_CmdContext, m_State.m_SelectedEntityScene, m_State.m_SelectedEntityId))
                    {
                        // The orbit that puts the entity at the target from where the eye is now: eye = target + RotateY(yaw)(RotateX(pitch)((0,0,distance))).
                        const auto  Eye = m_Camera.m_View.getPosition();
                        const auto  Off = Eye - pXform->m_Position;
                        const float Len = std::sqrt(Off.m_X * Off.m_X + Off.m_Y * Off.m_Y + Off.m_Z * Off.m_Z);
                        if (Len > 0.05f)           // the eye is on top of it: no direction to turn to
                        {
                            constexpr float Pi = 3.14159265f;
                            auto& G     = m_CameraGlide;
                            G.m_Eye     = Eye;
                            G.m_Pitch   = m_Camera.m_Angles.m_Pitch.m_Value;
                            G.m_Yaw     = m_Camera.m_Angles.m_Yaw.m_Value;
                            G.m_Distance= m_Camera.m_Distance;
                            const float Limit = 89.0f * Pi / 180.0f;
                            G.m_dPitch  = std::clamp(-std::asin(std::clamp(Off.m_Y / Len, -1.0f, 1.0f)), -Limit, Limit) - G.m_Pitch;
                            float dYaw  = std::atan2(Off.m_X, Off.m_Z) - G.m_Yaw;
                            G.m_dYaw    = dYaw - 2.0f * Pi * std::round(dYaw / (2.0f * Pi));      // the short way round
                            G.m_dDistance = Len - G.m_Distance;
                            G.m_T       = 0.0f;
                        }
                    }
            }

            // Glide, easing in and out. Grabbing the camera (orbit / pan / fly) ends the glide.
            if (m_CameraGlide.m_T < 1.0f)
            {
                constexpr float Seconds = 0.3f;
                auto& G = m_CameraGlide;
                if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) G.m_T = 1.0f;
                else
                {
                    G.m_T = std::min(1.0f, G.m_T + ImGui::GetIO().DeltaTime / Seconds);
                    const float u = G.m_T;
                    const float t = u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f);                    // smootherstep: slow start, fast middle, slow stop

                    m_Camera.m_Angles.m_Pitch.m_Value = G.m_Pitch + G.m_dPitch * t;
                    m_Camera.m_Angles.m_Yaw.m_Value   = G.m_Yaw   + G.m_dYaw   * t;
                    m_Camera.m_Distance               = G.m_Distance + G.m_dDistance * t;

                    // Keep the eye where it was: the target is the eye minus the orbit offset of these angles.
                    xmath::fvec3 Orbit(0, 0, m_Camera.m_Distance);
                    Orbit.RotateX(m_Camera.m_Angles.m_Pitch);
                    Orbit.RotateY(m_Camera.m_Angles.m_Yaw);
                    m_Camera.m_Target = G.m_Eye - Orbit;
                }
            }

            m_Camera.UpdateView(Min, Avail.x, Avail.y);

            // The 3D scene is queued FIRST, before the gizmo: AddCustomRenderCallback just inserts a
            // callback into this window's draw list, and draw-list order IS paint order. The gizmo's
            // own vertices (below) must come after these or the opaque scene paints over it (direct user
            // report: "you are rendering the gizmo using the ZBuffer"). Queueing early changes nothing
            // about WHAT these draw - the lambdas run at render time and read live state (Transform,
            // camera, SetSelectedEntity - which is still set further down, after click-to-pick).
            //
            // Grid: color-only editor helper - never submitted to xlionrender's entity draw/pick list.
            // Always drawn; grid does not participate in selection (CPU pick + ground MaxT only).
            xgpu::tools::imgui::AddCustomRenderCallback([this](xgpu::cmd_buffer& CmdBuffer, const ImVec2&, const ImVec2&)
            {
                m_Grid.Draw(CmdBuffer, m_Camera.m_View.getW2C(), m_Camera.m_View.getPosition(), xmath::fmat4::fromZero());
            });

            // Structural changes (new entities, AddComponent archetype migrations) stay in a pending
            // list until flushed - normally only done once per frame inside GameMgr.Run(), which only
            // runs while Playing. The editor needs entities visible to Search/Foreach (render, gizmos,
            // ...) as soon as they're created, not just after the first Play - flush unconditionally,
            // every frame, here. Cheap no-op when the pending list is empty (the common case).
            m_pGameMgr->m_ArchetypeMgr.UpdateStructuralChanges();

            // The host's own turn, every frame (Stopped/Paused/Playing alike): Draw has LIONRender's own
            // system collect its entities (after GameMgr.Run() finished, when Playing) and issues the GPU commands.
            xgpu::tools::imgui::AddCustomRenderCallback([this, Avail](xgpu::cmd_buffer& CmdBuffer, const ImVec2&, const ImVec2&)
            {
                // Read at render time, so the click-to-pick further down this frame is already in. Every open Level draws its
                // own world, with its own selection outlined.
                xlionrender::SetSelectedEntity(m_State.m_SelectedEntity.m_Value);
                xlionrender::Draw(m_pGameMgr.get(), CmdBuffer, m_Camera.m_View.getW2C(), Avail.x, Avail.y);
            });

            // Gizmo (Move/Rotate/Scale tools, m_SceneTool 1/2/3) - drives the primary selection's
            // Transform through the command system (Translate/Rotate/Scale, xscene_commands_transform_gizmo.h).
            // Execute() fires once, on mouse-release, not per-frame - m_bGizmoWasUsing/m_GizmoBefore*
            // capture the value at drag-start so the undo entry's Before/After spans the whole drag.
            //
            // Three ImGuizmo requirements, all confirmed from its source (direct user report: "I can
            // not click on them"):
            //  - Draws into THIS window's draw list (SetDrawlist() with no argument): hover detection
            //    (IsHoveringWindow, via ComputeContext's mbMouseOver) requires g.HoveredWindow to be the
            //    draw list's OWNER window. The foreground draw list has no owner window, so hover never
            //    registered there.
            //  - ImGuizmo::BeginFrame() once per frame before Manipulate(): it resets mbOverGizmoHotspot,
            //    which HandleTranslation/Rotation/Scale OR-accumulate and then use to force MT_NONE
            //    ("another gizmo already claimed the hotspot this frame"). Never resetting it meant the
            //    first hovered frame latched it true forever, and no handle could ever be grabbed.
            //    BeginFrame's own full-viewport "gizmo" window is NoInputs (never becomes HoveredWindow)
            //    and draws nothing here, since SetDrawlist() below redirects to this window's list.
            //  - Hover is read with IsOver(Operation), which computes live from the mouse.
            //  - Runs BEFORE the viewport InvisibleButton, which is skipped entirely while the gizmo is
            //    hovered/in use (see below): CanActivate() requires !IsAnyItemHovered(), and that checks
            //    g.HoveredIdPreviousFrame too - so the button must not claim hover on the frames BEFORE
            //    the click either, not just the click frame.
            bool bGizmoInteracting = false;
            const bool bHasSelection = m_State.m_SelectedEntityId != xecs::scene::invalid_permanent_id_v;
            if (bHasSelection)
            {
                ImGuizmo::BeginFrame();
                ImGuizmo::SetOrthographic(false);
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(Min.x, Min.y, Avail.x, Avail.y);
            }

            // getV2CScales() deliberately flips Y for this engine's Vulkan NDC convention (see
            // its own comment, xgpu_view_inline.h) - correct for xgpu's actual GPU pipeline, but
            // ImGuizmo is a generic (OpenGL-convention) math library with no idea about that
            // flip, so feeding it getV2C() directly inverted its whole widget's Y axis (direct
            // user report: green arrow pointing down, gizmo visibly wrong). Undo just that one
            // flip for ImGuizmo's own copy by negating clip-space row 1 (every column's row-1
            // term, not just the diagonal - the off-axis frustum term at (1,2) needs it too).
            xmath::fmat4 GizmoProjection = m_Camera.m_View.getV2C();
            GizmoProjection.m_10 = -GizmoProjection.m_10;
            GizmoProjection.m_11 = -GizmoProjection.m_11;
            GizmoProjection.m_12 = -GizmoProjection.m_12;
            GizmoProjection.m_13 = -GizmoProjection.m_13;

            const viewport_tools::view ToolView
            { .m_W2V         = m_Camera.m_View.getW2V()
            , .m_Projection  = GizmoProjection
            , .m_W2C         = m_Camera.m_View.getW2C()
            , .m_Min         = Min
            , .m_Size        = Avail
            , .m_bLocalSpace = m_bLocalSpace
            , .m_bSnap       = ImGui::GetIO().KeyCtrl
            , .m_Eye         = m_Camera.m_View.getPosition()
            , .m_RayDir      = [this](float X, float Y) { return m_Camera.m_View.RayFromScreen(X, Y); }
            };

            // Component viewport tools (collider wireframes for the selection, and the active "Edit
            // Collider"-style edit) - drawn before the Transform gizmo so its handles stay on top.
            bGizmoInteracting |= m_ToolEditor.Run(m_CmdContext, m_State.isPlaying() || !m_bLevelWritable, ToolView);

            if (!m_ToolEditor.isActive() && m_SceneTool >= 1 && m_SceneTool <= 3 && bHasSelection && m_bLevelWritable)
            {
                if (auto* pXform = xscene::commands::ResolveTransform(m_CmdContext, m_State.m_SelectedEntityScene, m_State.m_SelectedEntityId))
                {
                    const auto Operation = m_SceneTool == 1 ? ImGuizmo::TRANSLATE : m_SceneTool == 2 ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
                    const auto Mode      = m_bLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;

                    xmath::fmat4 World{};
                    World.setupSRT(pXform->m_Scale, pXform->m_Rotation, pXform->m_Position);

                    // Snapshot the pre-drag value every frame we are NOT already mid-drag. The grab itself
                    // only happens inside Manipulate() below, and m_bGizmoWasUsing is already true by the
                    // next frame - so an "IsUsing() && !m_bGizmoWasUsing" check up here could never fire
                    // (direct user report: "the Undo is not working well"). Manipulate() doesn't modify
                    // the matrix on the activation frame itself, so this is still the true pre-drag value.
                    if (!m_bGizmoWasUsing)
                    {
                        m_GizmoBeforePosition = pXform->m_Position;
                        m_GizmoBeforeRotation = pXform->m_Rotation;
                        m_GizmoBeforeScale    = pXform->m_Scale;
                    }

                    if (ImGuizmo::Manipulate(reinterpret_cast<const float*>(&m_Camera.m_View.getW2V()), reinterpret_cast<const float*>(&GizmoProjection)
                        , Operation, Mode, reinterpret_cast<float*>(&World)))
                    {
                        // ImGuizmo writes the result back in the same memory layout it was given, so
                        // xmath's own Extract* (the exact inverse of the setupSRT above) reads it
                        // directly - no round-trip through ImGuizmo's DecomposeMatrixToComponents Euler
                        // angles, whose axis order doesn't match xmath::radian3's ZXY (direct user
                        // report: "the rotation is very strange"). Only the channel this tool edits is
                        // written, so the other two stay bit-exact instead of picking up float drift.
                        switch (m_SceneTool)
                        {
                        case 1: pXform->m_Position = World.ExtractPosition(); break;
                        case 2: pXform->m_Rotation       = World.ExtractRotation();
                                pXform->m_EditorRotation = pXform->m_Rotation.ToEuler();
                                break;
                        case 3: pXform->m_Scale = World.ExtractScale(); break;
                        }
                        pXform->MarkDirtyToPhysics();
                    }

                    bGizmoInteracting = ImGuizmo::IsOver(Operation) || ImGuizmo::IsUsing();

                    if (m_bGizmoWasUsing && !ImGuizmo::IsUsing())
                    {
                        // Mouse just released - commit exactly one undo entry for the whole drag, and none
                        // at all for a click that didn't actually change anything.
                        const char* pCmd = m_SceneTool == 1 ? "Translate" : m_SceneTool == 2 ? "Rotate" : "Scale";
                        const auto Before = m_SceneTool == 1 ? xscene::commands::PackBlob(m_GizmoBeforePosition)
                                          : m_SceneTool == 2 ? xscene::commands::PackBlob(m_GizmoBeforeRotation)
                                          :                    xscene::commands::PackBlob(m_GizmoBeforeScale);
                        const auto After  = m_SceneTool == 1 ? xscene::commands::PackBlob(pXform->m_Position)
                                          : m_SceneTool == 2 ? xscene::commands::PackBlob(pXform->m_Rotation)
                                          :                    xscene::commands::PackBlob(pXform->m_Scale);
                        if (Before != After)
                        {
                            xeditor::Run(m_Undo, std::format("{} -Scene {} -Id {} -Before {} -After {}", pCmd
                                , xscene::commands::FormatSceneGuid(m_State.m_SelectedEntityScene)
                                , xscene::commands::FormatEntityId(m_State.m_SelectedEntityId), Before, After));
                        }
                    }
                    m_bGizmoWasUsing = ImGuizmo::IsUsing();
                }
            }

            // The viewport InvisibleButton (camera-orbit input, hotkeys, click-to-pick) - skipped
            // entirely, with everything keyed off its item state, on any frame the gizmo is hovered or
            // being dragged. ImGuizmo's CanActivate() needs !IsAnyItemHovered(), which also checks
            // g.HoveredIdPreviousFrame (latched at frame start from the PRIOR frame) - so this button,
            // which spans the whole viewport, must not claim hover on the frames leading up to the
            // click either. Skipping it also keeps the picker's "missed everything -> ClearSelection"
            // branch from firing on a click meant for the gizmo, and keeps the camera from orbiting
            // mid-drag.
            if (!bGizmoInteracting)
            {
                ImGui::InvisibleButton("##LevelEditorViewport", Avail);
                m_Camera.HandleInput();

                // The Q/W/E/R tool keys are the Level/Viewport/Tool* actions (see session_actions: unavailable while the camera
                // flies - right mouse held - because xeditor_tools::camera::HandleInput reads Q/W/E for strafe/up-down then).
                // Esc finishes a viewport tool that is editing.
                if (ImGui::IsItemHovered() && !ImGui::GetIO().WantTextInput && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
                    m_ToolEditor.HandleHotkeys();

                // Click-to-select: CPU ray-pick against every rendered entity's rigid_body AABB
                // (xlionrender::Pick - xeditor_tools_picking.h under the hood, the same shared primitives
                // xskeleton.plugin's own PickWedge uses for bones). Gated on hover + not mid-orbit, same
                // shape as xskeleton_editor.h's own RenderViewport click handling. Routed through the
                // command/undo system (xscene_commands_selection.h), same as the Level Tree's own row
                // clicks, so Ctrl+Z undoes a viewport pick exactly like it undoes a tree-row pick.
                {
                    const bool bHovered  = ImGui::IsItemHovered();
                    const bool bOrbiting = ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle);
                    if (bHovered && !bOrbiting && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        const ImVec2 Mouse  = ImGui::GetIO().MousePos;
                        const auto   Origin = m_Camera.m_View.getPosition();
                        const auto   Dir    = m_Camera.m_View.RayFromScreen(Mouse.x, Mouse.y);

                        // No GPU ID/pick buffer in this editor (CPU AABB pick only). The grid is
                        // color-pass-only and is not in the entity pick set - but a ray through empty
                        // floor would still hit entity AABBs behind the ground. Cap MaxT at the
                        // y=0 plane so a closer grid hit clears selection instead of selecting through it.
                        float MaxT = std::numeric_limits<float>::max();
                        if (std::fabs(Dir.m_Y) > 1.0e-6f)
                        {
                            const float GroundT = -Origin.m_Y / Dir.m_Y;
                            if (GroundT > 1.0e-6f) MaxT = GroundT;
                        }
                        const auto Hit = xlionrender::Pick(m_pGameMgr.get(), Origin, Dir, MaxT);

                        xecs::scene::guid          HitScene{};
                        xecs::scene::permanent_id  HitId = xecs::scene::invalid_permanent_id_v;
                        if (Hit != xecs::component::entity::invalid_entity_v)
                        {
                            for (auto& SceneGuid : m_State.m_OpenScenes)
                            {
                                auto* pScene = m_pGameMgr->m_SceneMgr.Find(SceneGuid);
                                if (!pScene) continue;
                                if (auto It = pScene->m_RuntimeToLocal.find(Hit); It != pScene->m_RuntimeToLocal.end())
                                {
                                    HitScene = SceneGuid;
                                    HitId    = It->second;
                                    break;
                                }
                            }
                        }

                        const bool bCtrl  = ImGui::GetIO().KeyCtrl;
                        const bool bShift = ImGui::GetIO().KeyShift;
                        if (HitId != xecs::scene::invalid_permanent_id_v)
                        {
                            xeditor::Run(m_Undo, bCtrl
                                ? std::format("ToggleMultiSelect -Scene {} -Id {}", xscene::commands::FormatSceneGuid(HitScene), xscene::commands::FormatEntityId(HitId))
                                : std::format("Select -Scene {} -Id {}", xscene::commands::FormatSceneGuid(HitScene), xscene::commands::FormatEntityId(HitId)));
                        }
                        else if (!bCtrl && !bShift)
                        {
                            xeditor::Run(m_Undo, "ClearSelection");
                        }
                    }
                }
            }

            // Last, so it sits above the scene, the wireframes and the gizmo (a child window - while the
            // mouse is over it the viewport button isn't hovered, so no pick/orbit through it).
            m_ToolEditor.RenderOverlay(ToolView);
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
            xlevel::PollGameReload(m_CmdContext, m_GamePlugin, &session::RegisterHostComponents, m_SeenBuildSeq);
            if (Services().bInitialBuild && !m_GamePlugin.m_bBuilding) Services().bInitialBuild = false;   // first build done (or failed)

            // Scenes another Level editor saved while this one had them open (read-only here): reload them from disk.
            if (!m_State.m_ScenesToReload.empty() && !m_State.isPlaying())
            {
                auto Reload = std::move(m_State.m_ScenesToReload);
                m_State.m_ScenesToReload.clear();
                for (auto& SceneGuid : Reload)
                {
                    if (std::find(m_State.m_OpenScenes.begin(), m_State.m_OpenScenes.end(), SceneGuid) == m_State.m_OpenScenes.end()) continue;
                    xscene::CloseScene(*m_pGameMgr, m_State, SceneGuid);
                    xscene::OpenScene(*m_pGameMgr, m_State, xlevel::SceneResourceGuid(SceneGuid));
                }
                m_State.m_bEntityInspectorDirty = true;
            }

            // Nothing unsaved (a save, or undoing back to it) means this editor edits nothing: its Scene locks go.
            if (!xlevel::HasUnsavedDocumentChanges(m_State, m_Undo))
                if (auto* pHost = xeditor::host::current())
                    if (auto* pMe = xlevel::FindHostSession(m_Undo)) xlevel::ReleaseLevelEditAccess(*pHost, *pMe, m_State);

            // Closing the Level closes its editor (the shell drops it at the start of the next frame).
            if (m_bOpenRequested && m_State.m_CurrentLevel.empty() && m_State.m_OpenScenes.empty() && !m_State.m_bAwaitingSaveBeforeClose)
                m_bOpen = false;

            if (m_State.m_bPlayWorldRebuildRequested)
            {
                m_State.m_bPlayWorldRebuildRequested = false;
                m_pGameMgr.reset();
                CreateWorld();
                m_pGameMgr->EnableBuilders(true);
                RestoreWorld(xlevel::persist_mode::RestoreFromV1);
                xlevel::FinishEnterPlaying(m_CmdContext);
            }

            if (m_State.m_bStopRequested)
            {
                m_State.m_bStopRequested = false;
                StopPlay(m_State.m_PendingKeepTweaksCommands);
                m_State.m_PendingKeepTweaksCommands.clear();
            }
        }

        // True when the window the user is working in (keyboard/nav focus) is one of this editor's own panels.
        bool IsOneOfMyWindowsFocused() const noexcept
        {
            ImGuiWindow* pNav = ImGui::GetCurrentContext() ? ImGui::GetCurrentContext()->NavWindow : nullptr;
            if (pNav == nullptr) return false;
            for (const char* pName : { m_Names.m_Editor, m_Names.m_LevelTree, m_Names.m_Inspector, m_Names.m_SystemRegistry })
                if (ImGuiWindow* pMine = ImGui::FindWindowByName(pName); pMine && (pNav == pMine || pNav->RootWindow == pMine)) return true;
            return false;
        }

        // ---- The whole tool window: dockspace, panels, modals, the Play tick gate. ----
        // A peer root like Texture's own document_editor::Render(), but hand-implemented (not
        // xeditor::document_editor<T_DOC>) because a Level's own dockspace/panel shape (Level Tree + Inspector +
        // System Registry inside a further-nested dockspace, not a flat panel list) and always-open,
        // never-per-Guid-instanced lifecycle don't fit that template - see this file's own top comment.
        void Render() noexcept override
        {
            auto* pHost = xeditor::host::current();

            // Whichever editor the user last touched is the one the commands that name no Level act on.
            if (IsOneOfMyWindowsFocused()) g_pActiveLevelContext = &m_CmdContext;

            // Make this editor's actions live in its windows (the viewport window also makes Level/Viewport/... live).
            if (auto* pCtx = ActionContext())
            {
                pCtx->ScopeWindow(ImGui::FindWindowByName(m_Names.m_Editor),    m_Actions, "Viewport", "Entity");
                pCtx->ScopeWindow(ImGui::FindWindowByName(m_Names.m_LevelTree), m_Actions, "Entity");
                pCtx->GesturesWindow(ImGui::FindWindowByName(m_Names.m_Editor),    "Viewport", gestures::viewport);
                pCtx->GesturesWindow(ImGui::FindWindowByName(m_Names.m_LevelTree), "Level tree", gestures::tree);
                for (const char* pName : { m_Names.m_Inspector, m_Names.m_SystemRegistry })
                    pCtx->ScopeWindow(ImGui::FindWindowByName(pName), m_Actions);
            }

            std::string LevelTabName;
            if (!m_State.m_CurrentLevel.empty())
                xresource_editor::RemapGUIDToString(LevelTabName, xresource::full_guid{ m_State.m_CurrentLevel.m_Instance, m_State.m_CurrentLevel.m_Type });
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
                    [this]() { RenderParentEditorToolbar(); }, m_Names,
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

                // Ctrl+S / Ctrl+Z / Ctrl+Y are the Level/Save, Undo and Redo actions (see session_actions): resolved once a frame by
                // the host's ximgui::actions::context, only while one of this editor's windows has the focus.
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
                if (ImGui::BeginPopupModal("##PlayBusy", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
                {
                    ImGui::Text("Play is already active in another Level editor.");
                    ImGui::Text("Stop that Play first, then try again.");
                    if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                // Read-only is per Scene: the selected entity's Scene may be owned by another Level editor (see
                // TryGateLevelMutation). The Level Tree marks every such Scene itself.
                const bool bSelectedLocked = xlevel::IsSceneLockedByOther(m_CmdContext, m_State.m_SelectedEntityScene);
                m_bLevelWritable = !bSelectedLocked;
                const std::string ReadOnlyReason = bSelectedLocked
                    ? std::format("Read-only: this Scene is being edited in {}", xlevel::SceneOwnerName(m_State.m_SelectedEntityScene))
                    : std::string{};

                xlevel::editor_tabs::SetNextLevelEditorToolClass();
                xlevel::RenderLevelTreePanel(m_CmdContext, m_Names.m_LevelTree, m_Undo);
                m_State.m_bRenameRequested = false;      // F2 was offered to the tree this frame; a request that found no selected row is dropped, not kept for later

                xlevel::editor_tabs::SetNextLevelEditorToolClass();
                xscene::RenderEntityPropertiesPanel(m_CmdContext, m_Names.m_Inspector, m_EntityInspector, m_InspectorBridge, bSelectedLocked, ReadOnlyReason.c_str());

                xlevel::editor_tabs::SetNextLevelEditorToolClass();
                xlevel::RenderSystemRegistryPanel(*m_pGameMgr, m_State, m_Names.m_SystemRegistry);

                ImGui::SetNextWindowPos(ImVec2(250.0f, 90.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(1050.0f, 480.0f), ImGuiCond_FirstUseEver);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                xlevel::editor_tabs::SetNextLevelEditorToolClass();
                if (ImGui::Begin(m_Names.m_Editor))
                {
                    ximgui::toolbar::RenderToolbarHost(m_EditorToolbarHost, ImGui::GetContentRegionAvail(),
                        [this](const char* Name, ximgui::toolbar::axis Axis) { RenderEditorToolbar(Name, Axis); },
                        [this]() { RenderViewport(); });
                }
                ImGui::End();
                ImGui::PopStyleVar();

                xlevel::RenderReloadCompatibilityModal(m_CmdContext);
            }
        }

        // What the shell keeps for this editor (its by-name commands). Declared last so it is destroyed first.
        std::shared_ptr<void> m_ShellCommands;
    };

    // ---- session_actions: the bodies. Same conditions and calls the hand-written key checks / toolbar lambdas had. ----
    inline const char* session_actions::WhyNoSave() const noexcept
    {
        if (S().m_State.isPlaying()) return "not while playing";
        if (S().m_State.m_CurrentLevel.empty() && S().m_State.m_OpenScenes.empty()) return "nothing is open";
        if (!xlevel::HasUnsavedDocumentChanges(S().m_State, S().m_Undo)) return "no changes to save";
        return nullptr;
    }
    inline void session_actions::Save() noexcept
    {
        xlevel::SaveEverything(*S().m_pGameMgr, S().m_State);
        xlevel::MarkDocumentClean(S().m_State, S().m_Undo);
    }

    inline const char* session_actions::WhyNoUndo() const noexcept
    {
        if (S().m_State.isPlaying()) return "not while playing";
        if (S().m_Undo.GetUndoIndex() <= 0) return "nothing to undo";
        return nullptr;
    }
    inline void session_actions::Undo() noexcept { if (xlevel::MayUndoRedo(S().m_Undo, false)) S().m_Undo.Undo(); }

    inline const char* session_actions::WhyNoRedo() const noexcept
    {
        if (S().m_State.isPlaying()) return "not while playing";
        if (S().m_Undo.GetUndoIndex() >= static_cast<int>(S().m_Undo.GetHistoryCount())) return "nothing to redo";
        return nullptr;
    }
    inline void session_actions::Redo() noexcept { if (xlevel::MayUndoRedo(S().m_Undo, true)) S().m_Undo.Redo(); }

    // The selected entity (the primary selection), and whether its Scene may be edited from here.
    inline const char* session_actions::WhyNoDelete() const noexcept
    {
        const auto& St = S().m_State;
        if (St.m_SelectedEntityId == xecs::scene::invalid_permanent_id_v) return "nothing selected";
        if (xlevel::IsSceneLockedByOther(S().m_CmdContext, St.m_SelectedEntityScene)) return "its Scene is being edited by another Level";
        return nullptr;
    }
    inline void session_actions::Delete() noexcept
    {
        const auto& St = S().m_State;
        xeditor::Run(S().m_Undo, std::format("DeleteEntity -Scene {} -Id {}", xscene::commands::FormatSceneGuid(St.m_SelectedEntityScene), xscene::commands::FormatEntityId(St.m_SelectedEntityId)));
    }

    inline const char* session_actions::WhyNoRename() const noexcept
    {
        if (S().m_State.isPlaying()) return "not while playing";
        return WhyNoDelete();                // same needs: a selected entity in a Scene this Level may edit
    }
    inline void session_actions::Rename() noexcept { S().m_State.m_bRenameRequested = true; }

    inline const char* session_actions::WhyNoPlay() const noexcept
    {
        using play_state = level_state::play_state;
        if (S().m_State.m_PlayState == play_state::Playing) return "already playing";
        auto* pHost = xeditor::host::current();
        if (const auto* pGate = pHost ? pHost->find<play_gate>() : nullptr; pGate && pGate->m_IsBuilding()) return "Game.dll is building";
        if (pHost && pHost->is_play_active() && pHost->m_pPlayOwner != &S().m_State) return "another Level is playing";
        return nullptr;
    }
    inline void session_actions::Play() noexcept { (void)RequestPlay(S().m_CmdContext); }

    inline const char* session_actions::WhyNoStop() const noexcept
    {
        return S().m_State.m_PlayState == level_state::play_state::Stopped ? "not playing" : nullptr;
    }
    inline void session_actions::Stop() noexcept { (void)RequestStop(S().m_CmdContext, std::nullopt); }

    inline const char* session_actions::WhyNoTool() const noexcept { return ImGui::IsMouseDown(ImGuiMouseButton_Right) ? "the camera is flying" : nullptr; }
    inline void session_actions::ToolSelect() noexcept { S().SetSceneTool(0); }
    inline void session_actions::ToolMove()   noexcept { S().SetSceneTool(1); }
    inline void session_actions::ToolRotate() noexcept { S().SetSceneTool(2); }
    inline void session_actions::ToolScale()  noexcept { S().SetSceneTool(3); }

    // Same shape as xmaterial_editor.h's own g_Registration.
    inline const xeditor::auto_register_resource_editor g_Registration
    { xecs::level::type_guid_v
    , [](xresource::full_guid Guid, xresource_editor::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };
}

#endif // XLEVEL_SESSION_H

