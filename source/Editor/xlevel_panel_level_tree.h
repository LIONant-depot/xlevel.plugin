#ifndef XLEVEL_PANEL_LEVEL_TREE_H
#define XLEVEL_PANEL_LEVEL_TREE_H
#pragma once
#include "dependencies/xLIONCore/src/tags/xlioncore_tags.h"
#include <set>
#include "dependencies/xeditor/include/xeditor/popup.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_world_check.h"

// The Level tree panel: Level -> Scenes -> Folders -> Entities, with drag and drop, the source control column and the Level's
// own edit commands. Meant to be included from xlevel_editor.h, after everything it calls.
#include "dependencies/xeditor/include/xeditor/shortcuts.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_selection.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_entity_lifecycle.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_scene_organization.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_commands_scene_dependency.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_commands_level.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_commands_source_control.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_source_control_cache.h"
#include "dependencies/xresource_pipeline_v2/source/editor/xresource_editor_panel_source_control.h"

// BeginDragDropTargetCustom: the window-wide highlight when a Level is dropped while another is open.
#include "imgui_internal.h"

namespace xlevel
{
    //---------------------------------------------------------------------------
    // Level Editor panel - one tree: Level -> Scenes -> Folders -> Entities. Clicking a Scene's label
    // opens it (loads its entities) WITHOUT closing any other already-open scene - any number of
    // scenes can be open/expanded at once, each independently. Clicking an Entity selects it for the
    // Properties panel. A scene's dependency edges render as a fixed "Dependencies" folder right
    // under that scene's own row, not a separate section. Owns its own ImGui::Begin/End - callable
    // directly from a main loop with no surrounding window boilerplate needed.
    //---------------------------------------------------------------------------
    // Dropping a Level from Resources mid-tree can mutate open scenes while rows are still drawing -
    // stash and open after ImGui::End (same OpenLevel + StartGameReload path as double-click).
    inline void RequestOpenLevelFromTreeDrop(const xresource::full_guid& LevelGuid) noexcept
    {
        QueueOpenLevel(LevelGuid);
    }

    // True when the current DESCRIPTOR_GUID drag is a Level asset (PeekOnly-safe).
    inline bool PeekDescriptorPayloadIsLevel(const ImGuiPayload* Peek) noexcept
    {
        if (!Peek || Peek->DataSize != sizeof(xresource_editor::drag_and_drop_folder_payload_t)) return false;
        return reinterpret_cast<const xresource_editor::drag_and_drop_folder_payload_t*>(Peek->Data)->m_Source.m_Type == xecs::level::type_guid_v;
    }

    // Inside an active BeginDragDropTarget / Custom: accept Level only, open on delivery.
    // Returns true when the payload is a Level (so callers skip Prefab/Scene handling and
    // let the window-wide target own the full-panel highlight).
    inline bool TryAcceptOpenLevelDescriptorDrop() noexcept
    {
        const ImGuiPayload* Peek = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID", ImGuiDragDropFlags_AcceptPeekOnly);
        if (!PeekDescriptorPayloadIsLevel(Peek)) return false;
        if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID"))
        {
            IM_ASSERT(Payload->DataSize == sizeof(xresource_editor::drag_and_drop_folder_payload_t));
            auto& Dropped = *reinterpret_cast<const xresource_editor::drag_and_drop_folder_payload_t*>(Payload->Data);
            if (Payload->IsDelivery())
                RequestOpenLevelFromTreeDrop(Dropped.m_Source);
        }
        return true;
    }

    // Renders the Level Tree's own source-control status/lock badge for a Scene or Level resource -
    // direct user request: "the 'level Tree' left source control column... similar to the one we
    // have done in all other views." Scoped to Scene/Level rows only, same granularity the Source
    // Control panel itself already uses (xresource_editor_panel_source_control.h's own top comment: a Scene is one
    // committable file as far as git is concerned - an Entity/Folder has no file of its own to track,
    // "a real, separate future feature," not this one). Draws directly into the CURRENT table cell
    // (call this right after TableSetColumnIndex for the dedicated "##SC" column), reusing the exact
    // same xresource_editor::DrawSourceControlBadge/GetSourceControlTooltipText helpers the Asset Tree and Source
    // Control panel already share, so all three views read identically.
    //
    // There is no existing "given only a full_guid, which library owns it" helper - getNodeInfo's own
    // global-search overload (xresource_editor_asset_mgr.h) loops every open library internally but never surfaces
    // which one matched - so this does that resolution itself via the per-library overload, same
    // "try each open library" idiom BuildSourceControlRows/CollectDistinctDepots already use elsewhere
    // for depot-wide scans. The Descriptor.txt path derivation (info.txt's own path -> sibling
    // Descriptor.txt -> strip the owning library's root) mirrors
    // xresource_editor_asset_browser_virtual_tree_tab.h's own tile-badge derivation exactly, so a Scene/Level's
    // badge here and its badge in the Asset Tree (if ever shown there) would always agree.
    // A resource's resolved SC identity - which library owns it, that library's real root path, and
    // both the single Descriptor.txt (status/lock badge granularity) and the whole containing .desc
    // folder (revert granularity - info.txt/Descriptor.txt/dependencies.txt all live there) as
    // library-root-relative keys. Factored out of what was RenderLevelTreeSourceControlBadge's own
    // inline lookup so the new "SC Revert" menu items below can resolve the SAME target the badge
    // itself represents, instead of re-deriving it a second time.
    struct level_tree_sc_target
    {
        xresource_editor::library::guid m_Library;
        std::wstring        m_RootPath;
        std::wstring        m_DescriptorPath; // library-relative, e.g. "Descriptors\...\Descriptor.txt"
        std::wstring        m_FolderPath;     // library-relative, the ".desc" folder containing it
    };

    inline std::optional<level_tree_sc_target> ResolveLevelTreeSourceControlTarget(const xresource::full_guid& ResourceGuid) noexcept
    {
        for (auto& Lib : xresource_editor::g_LibMgr.m_mLibraryDB)
        {
            std::wstring FolderPath, DescriptorPath;
            // NOT noexcept - getNodeInfo's own function_traits deduction (xresource_editor_asset_mgr.h) doesn't
            // handle a noexcept lambda's operator() type (the established noexcept-lambda trait trap,
            // see memory xgpu_xcontainer_noexcept_lambda_trait_trap - recurs anywhere a lambda is
            // passed to one of these FindAsReadOnly-style helpers).
            const bool bFound = xresource_editor::g_LibMgr.getNodeInfo(Lib.first, ResourceGuid, [&](const xresource_editor::library_db::info_node& Node)
            {
                const auto SlashPos = Node.m_Path.find_last_of(L'\\');
                FolderPath     = (SlashPos == std::wstring::npos) ? Node.m_Path : Node.m_Path.substr(0, SlashPos);
                DescriptorPath = (SlashPos == std::wstring::npos) ? Node.m_Path : (Node.m_Path.substr(0, SlashPos + 1) + L"Descriptor.txt");
                const auto& LibRoot = Lib.second->m_Library.m_Path;
                auto StripRoot = [&](std::wstring& P) noexcept
                {
                    if (P.size() > LibRoot.size() && P.compare(0, LibRoot.size(), LibRoot) == 0)
                    {
                        P = P.substr(LibRoot.size());
                        while (!P.empty() && (P.front() == L'\\' || P.front() == L'/'))
                            P.erase(P.begin());
                    }
                };
                StripRoot(FolderPath);
                StripRoot(DescriptorPath);
            });
            if (!bFound) continue;

            const auto RootPath = xresource_editor::commands::ResolveLibraryRootPath(Lib.first);
            if (RootPath.empty()) return std::nullopt;

            return level_tree_sc_target{ Lib.first, RootPath, DescriptorPath, FolderPath };
        }
        return std::nullopt;
    }

    inline void RenderLevelTreeSourceControlBadge(const xresource::full_guid& ResourceGuid) noexcept
    {
        const auto Target = ResolveLevelTreeSourceControlTarget(ResourceGuid);
        if (!Target) return;

        xresource_editor::asset_status_badge StatusBadge = xresource_editor::asset_status_badge::None;
        if (auto Status = xresource_editor::source_control::GetCachedFileStatus(Target->m_RootPath, Target->m_DescriptorPath))
            StatusBadge = Status->untracked ? xresource_editor::asset_status_badge::Untracked : xresource_editor::asset_status_badge::Modified;
        else if (xresource_editor::source_control::GetLastRefreshTime(Target->m_RootPath))
            StatusBadge = xresource_editor::asset_status_badge::Clean;

        xresource_editor::asset_lock_badge LockBadge = xresource_editor::asset_lock_badge::None;
        if (auto Lock = xresource_editor::source_control::GetCachedLockStatus(Target->m_RootPath, Target->m_DescriptorPath))
            LockBadge = (Lock->ownership == sc::LockOwnership::CurrentUser) ? xresource_editor::asset_lock_badge::LockedByMe : xresource_editor::asset_lock_badge::LockedByOther;

        if (StatusBadge == xresource_editor::asset_status_badge::None && LockBadge == xresource_editor::asset_lock_badge::None) return;

        constexpr float BadgeSize = 12.0f; // matches xresource_editor_asset_browser_virtual_tree_tab.h/files_tab's own badge size - direct user correction, never asked to change the icon size
        const ImVec2 CellMin  = ImGui::GetCursorScreenPos();
        const ImVec2 CellSize = ImGui::GetContentRegionAvail();
        const ImVec2 Center{ CellMin.x + CellSize.x * 0.5f, CellMin.y + ImGui::GetTextLineHeight() * 0.5f };
        xresource_editor::DrawSourceControlBadge(ImGui::GetWindowDrawList(), Center, BadgeSize, StatusBadge, LockBadge);

        // Invisible placeholder so the cell has a real item (row-height/clip participation) and a
        // hover target for the tooltip - the badge itself is drawn via raw ImDrawList primitives,
        // which never register as hoverable on their own.
        ImGui::Dummy(ImVec2(BadgeSize, ImGui::GetTextLineHeight()));
        if (ImGui::IsItemHovered())
        {
            const char* Title = ""; const char* Desc = "";
            xresource_editor::GetSourceControlTooltipText(StatusBadge, LockBadge, Title, Desc);
            xeditor::hint::PlaceAwayFromEdges(16.0f, ImVec2(380.0f, 220.0f)); ImGui::BeginTooltip();
            ImGui::Text("%s", Title);
            ImGui::TextDisabled("%s", Desc);
            ImGui::EndTooltip();
        }
    }

    // Deferred SC-Revert confirm request - a real Dear ImGui pitfall found live-testing this feature:
    // OpenPopup+BeginPopupModal called from INSIDE a row's own BeginPopupContextItem block only
    // renders successfully the SAME frame the MenuItem was clicked - on the VERY NEXT frame, that
    // row's own context-menu block is no longer entered (the right-click menu already closed), so
    // the nested BeginPopupModal call never runs, and Dear ImGui treats an OpenPopup'd id as
    // abandoned if nothing calls its matching Begin* the following frame - the modal flashed for
    // exactly one frame and vanished before any screenshot could ever catch it. Fix: the MenuItem
    // only records a REQUEST here; the actual OpenPopup/BeginPopupModal pair lives in
    // RenderLevelTreeSCRevertConfirmModal below, called from ONE stable, always-reached point
    // (RenderLevelTreePanel's own end), matching Dear ImGui's own documented "Delete.." button demo
    // idiom - a modal must be reachable every frame regardless of what triggered it.
    struct level_tree_sc_revert_request
    {
        xresource::full_guid m_ResourceGuid{};
        bool                  m_bWholeFolder = false;
        std::string           m_WarningText;
        bool                  m_bPending = false;
    };
    inline level_tree_sc_revert_request& LevelTreeSCRevertRequest() noexcept { static level_tree_sc_revert_request R; return R; }

    // "SC Revert" menu item, shared by every Level Tree row that offers it (Entity/Prefab-Instance
    // root, Scene, Level). `bWholeFolder`: false = revert exactly the single Descriptor.txt the
    // row's own badge represents (Entity/Prefab-Instance - "should be simple", direct user framing);
    // true = enumerate every currently-pending file under that resource's whole .desc folder and
    // revert all of them in one command (Scene/Level - reverts entities/folders/dependencies
    // together, since they all live inside that one Descriptor.txt, or a Level's info.txt/
    // dependencies.txt alongside it) - warned via WarningText since the blast radius is much bigger
    // than a single row.
    inline void RenderLevelTreeSCRevertMenuItem(xundo::system& Undo, const xresource::full_guid& ResourceGuid, bool bWholeFolder, const char* WarningText) noexcept
    {
        const auto Target = ResolveLevelTreeSourceControlTarget(ResourceGuid);
        const bool bModified = Target && xresource_editor::source_control::GetCachedFileStatus(Target->m_RootPath, Target->m_DescriptorPath).has_value();

        if (ImGui::MenuItem("SC Revert...", nullptr, false, bModified))
        {
            auto& Req = LevelTreeSCRevertRequest();
            Req.m_ResourceGuid = ResourceGuid;
            Req.m_bWholeFolder = bWholeFolder;
            Req.m_WarningText  = WarningText;
            Req.m_bPending     = true;
        }
    }

    // Called ONCE, unconditionally, near the end of RenderLevelTreePanel - see
    // level_tree_sc_revert_request's own comment for why this can't live inline at each MenuItem.
    inline void RenderLevelTreeSCRevertConfirmModal(xundo::system& Undo) noexcept
    {
        auto& Req = LevelTreeSCRevertRequest();
        if (Req.m_bPending)
        {
            ImGui::OpenPopup("SC Revert##LevelTreeConfirm");
            Req.m_bPending = false;
        }
        if (xeditor::BeginModal("SC Revert##LevelTreeConfirm"))
        {
            ImGui::TextUnformatted(Req.m_WarningText.c_str());
            ImGui::Separator();
            if (ImGui::Button("Discard Changes", ImVec2(160, 0)))
            {
                if (const auto Target = ResolveLevelTreeSourceControlTarget(Req.m_ResourceGuid))
                {
                    // Shared tail (xresource_editor_commands_source_control.h) with the Resources/Assets tabs' own
                    // whole-folder revert - same enumerate-then-batch-revert primitive, not
                    // reimplemented here. The single-file case is simple enough to stay inline.
                    if (Req.m_bWholeFolder)
                        xresource_editor::commands::RunRevertUnderFolder(Undo, Target->m_Library, Target->m_RootPath, Target->m_FolderPath);
                    else
                        xeditor::RunQuery(Undo, std::format("SourceControlRevert -Library {} -Path {}"
                            , xresource_editor::commands::FormatLibraryGuid(Target->m_Library), xresource_editor::commands::EncodeAssetPath(Target->m_DescriptorPath)));
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // Selects the "##SC" column (always table column 0) and draws the badge into it. The real
    // positional bug (direct user report: "some of them seems missing, and others look like they are
    // too far on the left") was Dear ImGui's own default table-column indent policy
    // (imgui_tables.cpp: "flags |= (column index == 0) ? IndentEnable : IndentDisable") - by default
    // ONLY column 0 receives the current tree-indent, every other column ignores it. Our layout has
    // that backwards for what we want (the badge sits in column 0, the actual tree lives in column
    // 1), so the badge drifted right with tree depth and clipped past its fixed 20px column while the
    // Name column's own tree drew with NO indent at all (a second, related user report: "the
    // indentation for the tree seems to be missing"). Fixed at the RenderLevelTreePanel table setup
    // via explicit ImGuiTableColumnFlags_IndentDisable/_IndentEnable per column - this function no
    // longer needs to fight the default itself.
    inline void RenderLevelTreeSourceControlBadgeColumn(const xresource::full_guid& ResourceGuid) noexcept
    {
        ImGui::TableSetColumnIndex(0);
        RenderLevelTreeSourceControlBadge(ResourceGuid);
    }

    // The columns of the tree: the source control badge, the entity's Enabled toggle, its Visible toggle (the eye), and the names (where the tree itself is).
    constexpr int kLevelTreeColumnSC = 0, kLevelTreeColumnEnabled = 1, kLevelTreeColumnVisible = 2, kLevelTreeColumnName = 3;

    // A one-glyph button in a narrow cell of the table (the Enabled and the Visible columns): the glyph is centered, in the color it is given, brighter under the mouse. True when it was clicked.
    inline bool RenderLevelTreeToggleCell(const char* pId, const char* pGlyph, ImU32 Color, const char* pTip) noexcept
    {
        const ImVec2 Size(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight());
        const bool   bClicked = ImGui::InvisibleButton(pId, Size);
        const bool   bHot     = ImGui::IsItemHovered();
        const ImVec2 Min      = ImGui::GetItemRectMin();
        const ImVec2 GlyphSize = ImGui::CalcTextSize(pGlyph);
        const ImU32  Shown    = bHot ? ImGui::GetColorU32(ImGuiCol_Text) : Color;
        ImGui::GetWindowDrawList()->AddText(ImVec2(Min.x + (Size.x - GlyphSize.x) * 0.5f, Min.y), Shown, pGlyph);
        if (bHot) xeditor::hint::Text("%s", pTip);
        return bClicked;
    }

    // ---- the editor's state of the entities (editor_disable, editor_no_render) as the Level Tree sets it ----
    // An entity of a scene: what the commands name.
    struct tree_entity { xecs::scene::guid m_Scene; xecs::scene::permanent_id m_Id; };

    // The roots and everything under them (the children that are entities of the same scene), each once.
    inline std::vector<tree_entity> ExpandWithDescendants(xecs::game_mgr::instance& GameMgr, const std::vector<tree_entity>& Roots) noexcept
    {
        auto& Ecs = xlioncore::Ecs(GameMgr);
        std::vector<tree_entity> Out;
        std::set<std::pair<std::uint64_t, std::uint32_t>> Seen;
        std::function<void(xecs::scene::guid, xecs::scene::permanent_id)> Walk = [&](xecs::scene::guid Scene, xecs::scene::permanent_id Id) noexcept
        {
            if (!Seen.insert({ Scene.m_Instance.m_Value, static_cast<std::uint32_t>(Id) }).second) return;
            auto* pScene = GameMgr.m_SceneMgr.Find(Scene);
            if (!pScene) return;
            auto It = pScene->m_LocalToRuntime.find(Id);
            if (It == pScene->m_LocalToRuntime.end() || !Ecs.IsAlive(It->second)) return;
            Out.push_back({ Scene, Id });
            if (auto* pChildren = Ecs.ChildrenOf(It->second))
            {
                const auto List = pChildren->m_List;                            // a copy: the commands that follow move entities around
                for (auto Child : List)
                    if (auto ChildIt = pScene->m_RuntimeToLocal.find(Child.m_Value); ChildIt != pScene->m_RuntimeToLocal.end()) Walk(Scene, ChildIt->second);
            }
        };
        for (const auto& Root : Roots) Walk(Root.m_Scene, Root.m_Id);
        return Out;
    }

    // Every entity of the open scenes.
    inline std::vector<tree_entity> AllEntitiesOfTheOpenScenes(xecs::game_mgr::instance& GameMgr, const level_state& State) noexcept
    {
        std::vector<tree_entity> Out;
        for (auto& SceneGuid : State.m_OpenScenes)
            if (auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid))
                for (auto& [Id, Entity] : pScene->m_LocalToRuntime) Out.push_back({ SceneGuid, Id });
        return Out;
    }

    // What the tree has selected: the entities of the multi selection, or the one that is selected.
    inline std::vector<tree_entity> SelectedEntities(const level_state& State) noexcept
    {
        std::vector<tree_entity> Out;
        if (!State.m_MultiSelectedEntityIds.empty())
            for (auto Id : State.m_MultiSelectedEntityIds) Out.push_back({ State.m_MultiSelectScene, Id });
        else if (State.m_SelectedEntityId != xecs::scene::invalid_permanent_id_v)
            Out.push_back({ State.m_SelectedEntityScene, State.m_SelectedEntityId });
        return Out;
    }

    // Gives the tag to the entities that do not have it (bAdd) or takes it from the ones that do: one command each, all of them one undo step.
    inline void SetEditorTag(level_context& Ed, xecs::game_mgr::instance& GameMgr, const std::vector<tree_entity>& Entities, xecs::component::type::guid Guid, bool bAdd, const char* pStepName) noexcept
    {
        auto& Ecs = xlioncore::Ecs(GameMgr);
        std::vector<std::string> Cmds;
        for (const auto& E : Entities)
        {
            auto* pScene = GameMgr.m_SceneMgr.Find(E.m_Scene);
            if (!pScene) continue;
            auto It = pScene->m_LocalToRuntime.find(E.m_Id);
            if (It == pScene->m_LocalToRuntime.end() || !Ecs.IsAlive(It->second)) continue;
            if (Ecs.HasComponent(It->second, Guid) == bAdd) continue;               // already as it is going to be
            Cmds.push_back(std::format("{} -Scene {} -Id {} -Component {:016X}", bAdd ? "AddComponent" : "RemoveComponent"
                , xscene::commands::FormatSceneGuid(E.m_Scene), xscene::commands::FormatEntityId(E.m_Id), Guid.m_Value));
        }
        (void)xeditor::RunGroup(Ed.m_Undo, pStepName, Cmds);
    }

    // How many entities of the open scenes the editor has disabled / hidden (counted every few frames: the headers say it).
    inline void UpdateLevelTreeEditorStateCounts(xecs::game_mgr::instance& GameMgr, level_state& State) noexcept
    {
        const int Frame = ImGui::GetFrameCount();
        if (Frame - State.m_EditorStateFrame < 20 && Frame >= State.m_EditorStateFrame) return;
        State.m_EditorStateFrame = Frame;
        auto&      Ecs         = xlioncore::Ecs(GameMgr);
        const auto DisableGuid = xecs::editor::disable_tag::typedef_v.m_Guid;
        const auto NoDrawGuid  = xecs::editor::no_render_tag::typedef_v.m_Guid;
        State.m_EditorDisabledCount = State.m_EditorHiddenCount = State.m_EditorEntityCount = 0;
        State.m_EditorSceneFlags.clear();
        for (auto& SceneGuid : State.m_OpenScenes)
            if (auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid))
            {
                std::uint8_t Flags = 0;
                for (auto& [Id, Entity] : pScene->m_LocalToRuntime)
                {
                    if (!Ecs.IsAlive(Entity)) continue;
                    ++State.m_EditorEntityCount;
                    const bool bDisabled = Ecs.HasComponent(Entity, DisableGuid), bHidden = Ecs.HasComponent(Entity, NoDrawGuid);
                    State.m_EditorDisabledCount += bDisabled ? 1 : 0;
                    State.m_EditorHiddenCount   += bHidden   ? 1 : 0;
                    Flags |= (bDisabled ? 1 : 0) | (bHidden ? 2 : 0);
                }
                State.m_EditorSceneFlags[SceneGuid.m_Instance.m_Value] = Flags;
            }
    }

    // What the editor has switched off UNDER an entity (not on the entity itself): first = something under it is disabled, second = something under it is hidden. A row says it with the same amber
    // as the header of its column. Worked out once per entity per frame (the tree asks for every visible row, and each answer is made of the answers of its children).
    template<class T_ECS>
    inline std::pair<bool, bool> EditorStateUnder(T_ECS& Ecs, level_state& State, xecs::component::entity Entity) noexcept
    {
        const int Frame = ImGui::GetFrameCount();
        if (State.m_EditorMemoFrame != Frame) { State.m_EditorMemo.clear(); State.m_EditorMemoFrame = Frame; }

        const auto DisableGuid = xecs::editor::disable_tag::typedef_v.m_Guid;
        const auto NoDrawGuid  = xecs::editor::no_render_tag::typedef_v.m_Guid;
        std::function<std::uint8_t(xecs::component::entity)> AtOrUnder = [&](xecs::component::entity E) noexcept -> std::uint8_t
        {
            if (auto It = State.m_EditorMemo.find(E.m_Value); It != State.m_EditorMemo.end()) return It->second;
            std::uint8_t Flags = (Ecs.HasComponent(E, DisableGuid) ? 1 : 0) | (Ecs.HasComponent(E, NoDrawGuid) ? 2 : 0);
            if (auto* pChildren = Ecs.ChildrenOf(E))
                for (auto Child : pChildren->m_List) Flags |= AtOrUnder(Child);
            State.m_EditorMemo[E.m_Value] = Flags;
            return Flags;
        };
        std::uint8_t Under = 0;
        if (auto* pChildren = Ecs.ChildrenOf(Entity))
            for (auto Child : pChildren->m_List) Under |= AtOrUnder(Child);
        return { (Under & 1) != 0, (Under & 2) != 0 };
    }

    // The two toggles of an entity's row. Enabled: the exclusive tag "editor_disable" (every system skips an entity that has it); Visible: the tag "editor_no_render" (the editor's view leaves it out).
    // Both are the editor's own state (saved with the scene, left out of the game by the scene compiler): the runtime tags (disable, no_render) are the game's.
    // A click acts on the entity AND all its descendants, as most editors do: the state the clicked entity is about to have is given to every one of them, all of them one undo step.
    inline void RenderLevelTreeEntityToggles(level_context& Ed, xecs::game_mgr::instance& GameMgr, xecs::scene::guid SceneGuid, xecs::scene::permanent_id Id, xecs::component::entity Entity) noexcept
    {
        auto&      Ecs         = xlioncore::Ecs(GameMgr);
        const auto DisableGuid = xecs::editor::disable_tag::typedef_v.m_Guid;
        const auto NoDrawGuid  = xecs::editor::no_render_tag::typedef_v.m_Guid;
        const bool bDisabled   = Ecs.HasComponent(Entity, DisableGuid);
        const bool bHidden     = Ecs.HasComponent(Entity, NoDrawGuid);
        const auto [bUnderDisabled, bUnderHidden] = EditorStateUnder(Ecs, Ed.State(), Entity);

        // The icon says it: normal when all is as it should be, red / slashed when this entity is switched off, and amber (the color of the header of the column) when it is not but something under it is.
        constexpr ImU32 Normal = IM_COL32(210, 210, 210, 255), Off = IM_COL32(220, 80, 80, 255), Dim = IM_COL32(110, 110, 110, 255), Amber = IM_COL32(240, 180, 60, 255);

        std::string EnabledTip = bDisabled ? "Disabled in the editor: no system sees this entity (the game's own disable is not touched). Click to enable it and everything under it (removes editor_disable)."
                                           : "Enabled. Click to disable it and everything under it in the editor (adds editor_disable).";
        if (!bDisabled && bUnderDisabled) EnabledTip += "\nSomething under it is disabled.";
        ImGui::TableSetColumnIndex(kLevelTreeColumnEnabled);
        if (RenderLevelTreeToggleCell("##Enabled", "\xEE\x9F\xA8", bDisabled ? Off : bUnderDisabled ? Amber : Normal, EnabledTip.c_str()))
            SetEditorTag(Ed, GameMgr, ExpandWithDescendants(GameMgr, { { SceneGuid, Id } }), DisableGuid, !bDisabled, bDisabled ? "Enable" : "Disable");

        std::string VisibleTip = bHidden ? "Hidden in the editor's view (the game still draws it). Click to show it and everything under it again (removes editor_no_render)."
                                         : "Drawn. Click to hide it and everything under it in the editor's view (adds editor_no_render).";
        if (!bHidden && bUnderHidden) VisibleTip += "\nSomething under it is hidden.";
        ImGui::TableSetColumnIndex(kLevelTreeColumnVisible);
        if (RenderLevelTreeToggleCell("##Visible", bHidden ? "\xEE\xB4\x9A" : "\xEE\x9E\xB3", bHidden ? Dim : bUnderHidden ? Amber : Normal, VisibleTip.c_str()))
            SetEditorTag(Ed, GameMgr, ExpandWithDescendants(GameMgr, { { SceneGuid, Id } }), NoDrawGuid, !bHidden, bHidden ? "Show" : "Hide");
    }

    // ---- folders, scenes and the Level: what is switched off inside them ----
    // The entities a folder holds, and those of the folders inside it (the roots: ExpandWithDescendants adds everything under them).
    template<class T_SCENE>
    inline std::vector<tree_entity> FolderRoots(const T_SCENE& Scene, xecs::scene::guid SceneGuid, xecs::scene::folder_id FolderId) noexcept
    {
        std::vector<tree_entity> Out;
        std::function<void(xecs::scene::folder_id)> Walk = [&](xecs::scene::folder_id Id) noexcept
        {
            for (const auto& F : Scene.m_Folders)
                if (F.m_Id == Id) for (auto E : F.m_Entities) Out.push_back({ SceneGuid, E });
            for (const auto& F : Scene.m_Folders)
                if (F.m_Parent == Id) Walk(F.m_Id);
        };
        Walk(FolderId);
        return Out;
    }

    // How many of the entities have the tag.
    inline int CountWithTag(xecs::game_mgr::instance& GameMgr, const std::vector<tree_entity>& Entities, xecs::component::type::guid Guid) noexcept
    {
        auto& Ecs = xlioncore::Ecs(GameMgr);
        int n = 0;
        for (const auto& E : Entities)
            if (auto* pScene = GameMgr.m_SceneMgr.Find(E.m_Scene))
                if (auto It = pScene->m_LocalToRuntime.find(E.m_Id); It != pScene->m_LocalToRuntime.end() && Ecs.IsAlive(It->second) && Ecs.HasComponent(It->second, Guid)) ++n;
        return n;
    }

    // The two toggles of a folder row: they act on everything inside it (the entities of the folder and of the folders in it, and everything under them). A folder has no tag of its own, so its icon says
    // how the inside is: normal when nothing is switched off, amber when some of it is, red / slashed when all of it is. A click gives everything the state the icon is not showing: all off, unless all
    // already are, then all back on - one undo step.
    template<class T_SCENE>
    inline void RenderLevelTreeFolderToggles(level_context& Ed, xecs::game_mgr::instance& GameMgr, xecs::scene::guid SceneGuid, const T_SCENE& Scene, xecs::scene::folder_id FolderId) noexcept
    {
        const auto Inside = ExpandWithDescendants(GameMgr, FolderRoots(Scene, SceneGuid, FolderId));
        if (Inside.empty()) return;                                          // an empty folder has nothing to switch
        const auto DisableGuid = xecs::editor::disable_tag::typedef_v.m_Guid;
        const auto NoDrawGuid  = xecs::editor::no_render_tag::typedef_v.m_Guid;
        const int  Total       = static_cast<int>(Inside.size());
        const int  nDisabled   = CountWithTag(GameMgr, Inside, DisableGuid);
        const int  nHidden     = CountWithTag(GameMgr, Inside, NoDrawGuid);

        constexpr ImU32 Normal = IM_COL32(210, 210, 210, 255), Off = IM_COL32(220, 80, 80, 255), Dim = IM_COL32(110, 110, 110, 255), Amber = IM_COL32(240, 180, 60, 255);

        const bool bAllDisabled = nDisabled == Total;
        std::string EnabledTip = bAllDisabled ? std::format("All {} entities inside are disabled in the editor. Click to enable them.", Total)
                               : nDisabled    ? std::format("{} of {} entities inside are disabled in the editor. Click to disable all of them.", nDisabled, Total)
                               :                std::format("Enabled. Click to disable the {} entities inside in the editor.", Total);
        ImGui::TableSetColumnIndex(kLevelTreeColumnEnabled);
        if (RenderLevelTreeToggleCell("##FolderEnabled", "\xEE\x9F\xA8", bAllDisabled ? Off : nDisabled ? Amber : Normal, EnabledTip.c_str()))
            SetEditorTag(Ed, GameMgr, Inside, DisableGuid, !bAllDisabled, bAllDisabled ? "Enable folder" : "Disable folder");

        const bool bAllHidden = nHidden == Total;
        std::string VisibleTip = bAllHidden ? std::format("All {} entities inside are hidden in the editor's view. Click to show them.", Total)
                               : nHidden    ? std::format("{} of {} entities inside are hidden in the editor's view. Click to hide all of them.", nHidden, Total)
                               :              std::format("Drawn. Click to hide the {} entities inside in the editor's view.", Total);
        ImGui::TableSetColumnIndex(kLevelTreeColumnVisible);
        if (RenderLevelTreeToggleCell("##FolderVisible", bAllHidden ? "\xEE\xB4\x9A" : "\xEE\x9E\xB3", bAllHidden ? Dim : nHidden ? Amber : Normal, VisibleTip.c_str()))
            SetEditorTag(Ed, GameMgr, Inside, NoDrawGuid, !bAllHidden, bAllHidden ? "Show folder" : "Hide folder");
    }

    // The row of a Scene or of the Level: there is no toggle (a scene or a level is not switched off this way), only the amber that says something inside is.
    inline void RenderLevelTreeContainerMarkers(std::uint64_t Key, bool bAnyDisabled, bool bAnyHidden, const char* pWhat) noexcept
    {
        constexpr ImU32 Amber = IM_COL32(240, 180, 60, 255);
        ImGui::PushID(static_cast<int>(Key & 0x7FFFFFFF));
        if (bAnyDisabled)
        {
            ImGui::TableSetColumnIndex(kLevelTreeColumnEnabled);
            (void)RenderLevelTreeToggleCell("##ContainerEnabled", "\xEE\x9F\xA8", Amber, std::format("Something in this {} is disabled in the editor.", pWhat).c_str());
        }
        if (bAnyHidden)
        {
            ImGui::TableSetColumnIndex(kLevelTreeColumnVisible);
            (void)RenderLevelTreeToggleCell("##ContainerVisible", "\xEE\x9E\xB3", Amber, std::format("Something in this {} is hidden in the editor's view.", pWhat).c_str());
        }
        ImGui::PopID();
    }

    // The header row of the tree. Narrow columns have only an icon (what it is, and how many entities it is on, is the hint of the header); the icon takes a color while the editor has anything disabled
    // or hidden - that is how the editor says the game does not look like its scene. A right-click on a header opens the menu of that column.
    inline void RenderLevelTreeHeaderRow(level_context& Ed, xecs::game_mgr::instance& GameMgr, level_state& State) noexcept
    {
        constexpr ImU32 Normal = IM_COL32(170, 170, 170, 255), Disabled = IM_COL32(240, 180, 60, 255), Hidden = IM_COL32(240, 180, 60, 255);           // amber: "something is switched off in here", the same as on the rows
        const auto DisableGuid = xecs::editor::disable_tag::typedef_v.m_Guid;
        const auto NoDrawGuid  = xecs::editor::no_render_tag::typedef_v.m_Guid;

        auto Cell = [&](const char* pId, const char* pGlyph, ImU32 Color, const char* pTopic, const char* pBody, const std::string& Detail) noexcept
        {
            const ImVec2 Size(ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight());
            ImGui::InvisibleButton(pId, Size);
            const bool   bHot   = ImGui::IsItemHovered();
            const ImVec2 Min    = ImGui::GetItemRectMin();
            const ImVec2 Glyph  = ImGui::CalcTextSize(pGlyph);
            ImGui::GetWindowDrawList()->AddText(ImVec2(Min.x + (Size.x - Glyph.x) * 0.5f, Min.y), bHot ? ImGui::GetColorU32(ImGuiCol_Text) : Color, pGlyph);
            if (bHot) xeditor::hint::Draw({ .m_Topic = pTopic, .m_Body = pBody, .m_Detail = Detail });
        };

        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

        ImGui::TableSetColumnIndex(kLevelTreeColumnSC);
        Cell("##HeaderSC", "", Normal, "Source control", "The status of the file of a Level, a Scene or a Prefab.", {});

        // Enabled
        ImGui::TableSetColumnIndex(kLevelTreeColumnEnabled);
        Cell("##HeaderEnabled", "\xEE\x9F\xA8", State.m_EditorDisabledCount ? Disabled : Normal, "Enabled"
            , "The power of each entity: click it to disable the entity and everything under it in the editor (editor_disable). Right-click here for the menu."
            , State.m_EditorDisabledCount ? std::format("{} of {} entities are disabled in the editor.", State.m_EditorDisabledCount, State.m_EditorEntityCount) : std::string("None is disabled."));
        if (ImGui::BeginPopupContextItem("##HeaderEnabledMenu"))
        {
            if (ImGui::MenuItem("Enable all", nullptr, false, State.m_EditorDisabledCount > 0))
                SetEditorTag(Ed, GameMgr, AllEntitiesOfTheOpenScenes(GameMgr, State), DisableGuid, false, "Enable all");
            ImGui::Separator();
            const auto Selected = ExpandWithDescendants(GameMgr, SelectedEntities(State));
            if (ImGui::MenuItem("Disable selected", nullptr, false, !Selected.empty())) SetEditorTag(Ed, GameMgr, Selected, DisableGuid, true,  "Disable selected");
            if (ImGui::MenuItem("Enable selected",  nullptr, false, !Selected.empty())) SetEditorTag(Ed, GameMgr, Selected, DisableGuid, false, "Enable selected");
            ImGui::EndPopup();
        }

        // Visible
        ImGui::TableSetColumnIndex(kLevelTreeColumnVisible);
        Cell("##HeaderVisible", "\xEE\x9E\xB3", State.m_EditorHiddenCount ? Hidden : Normal, "Visible"
            , "The eye of each entity: click it to hide the entity and everything under it in the editor's view (editor_no_render); the game still draws it. Right-click here for the menu."
            , State.m_EditorHiddenCount ? std::format("{} of {} entities are hidden in the editor.", State.m_EditorHiddenCount, State.m_EditorEntityCount) : std::string("None is hidden."));
        if (ImGui::BeginPopupContextItem("##HeaderVisibleMenu"))
        {
            if (ImGui::MenuItem("Show all", nullptr, false, State.m_EditorHiddenCount > 0))
                SetEditorTag(Ed, GameMgr, AllEntitiesOfTheOpenScenes(GameMgr, State), NoDrawGuid, false, "Show all");
            ImGui::Separator();
            const auto Selected = ExpandWithDescendants(GameMgr, SelectedEntities(State));
            if (ImGui::MenuItem("Hide selected", nullptr, false, !Selected.empty())) SetEditorTag(Ed, GameMgr, Selected, NoDrawGuid, true,  "Hide selected");
            if (ImGui::MenuItem("Show selected", nullptr, false, !Selected.empty())) SetEditorTag(Ed, GameMgr, Selected, NoDrawGuid, false, "Show selected");
            ImGui::EndPopup();
        }

        // Names: the tree has room, so its header says what it is
        ImGui::TableSetColumnIndex(kLevelTreeColumnName);
        ImGui::TextDisabled("Name");
    }

    void RenderLevelTreePanel(level_context& Ed, const char* pWindowName, xundo::system& Undo, bool bReadOnly = false) noexcept
    {
        auto& GameMgr = Ed.World();
        auto& State   = Ed.State();
        ImGui::SetNextWindowPos(ImVec2(915, 18), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360, 680), ImGuiCond_FirstUseEver);
        const bool bWindowVisible = ImGui::Begin(pWindowName);
        xeditor::diagnostics::Log("window begin: %s visible=%d", pWindowName, bWindowVisible ? 1 : 0);
        if (bWindowVisible)
        {
            if (bReadOnly) ImGui::BeginDisabled();
            if (State.m_CurrentLevel.empty())
            {
                if (!g_PendingOpenLevels.empty())
                    ImGui::TextDisabled("Opening the Level as soon as Game.dll has finished building...");
                else
                    ImGui::TextDisabled("Create or open a Level from the asset browser (or drop a Level here).");
                // Fill the rest of the panel so a Level dragged from Resources/asset browser can land
                // anywhere in the empty Level Tree - same OpenLevel path as double-clicking the asset.
                // Clamp: ImGui::InvisibleButton asserts size_arg.x/y != 0 (imgui_widgets.cpp) - on the
                // first frame / empty layout GetContentRegionAvail can be (0,0) and aborted startup.
                ImVec2 EmptyDropSize = ImGui::GetContentRegionAvail();
                if (EmptyDropSize.x < 1.0f) EmptyDropSize.x = 1.0f;
                if (EmptyDropSize.y < 1.0f) EmptyDropSize.y = 1.0f;
                ImGui::InvisibleButton("##EmptyLevelDrop", EmptyDropSize);
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID"))
                    {
                        IM_ASSERT(payload->DataSize == sizeof(xresource_editor::drag_and_drop_folder_payload_t));
                        auto& Dropped = *reinterpret_cast<const xresource_editor::drag_and_drop_folder_payload_t*>(payload->Data);
                        if (Dropped.m_Source.m_Type == xecs::level::type_guid_v)
                            xlevel::RequestOpenLevelFromTreeDrop(Dropped.m_Source);
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            else if (auto* pLevel = GameMgr.m_LevelMgr.Find(State.m_CurrentLevel))
            {
                std::string LevelLabel;
                xresource_editor::RemapGUIDToString(LevelLabel, xresource::full_guid{ State.m_CurrentLevel.m_Instance, State.m_CurrentLevel.m_Type });

                // Search box, visually matching the asset browser's own (RenderTreeSearchBar's own
                // comment). Adding entities/folders is right-click-in-place on a Scene/Folder row now
                // (BeginPopupContextItem, below).
                xeditor::RenderTreeSearchBar(State.m_TreeSearchString, ImGui::GetContentRegionAvail().x);
                UpdateLevelTreeEditorStateCounts(GameMgr, State);

                // The whole Level -> Scene -> Folder -> Entity hierarchy lives in one real
                // ImGui::BeginTable now (ImGui's own documented "tree inside a table" shape -
                // ImGuiTreeNodeFlags_SpanFullWidth on every tree row so hover/selection spans the Name
                // column). Column 1 ("Actions") is intentionally minimal today (just each row's own
                // remove/delete button) - the second column exists so future per-row content (type
                // badges, visibility toggles, etc.) has somewhere to go without another rewrite. Sized
                // to fill the rest of the window's height (ImGuiTableFlags_ScrollY so it scrolls
                // internally instead of pushing the window's own edge) rather than only as tall as its
                // content.
                // NoSavedSettings: ImGui persists per-table column widths in the editor's ini file across
                // sessions by table id+column-count hash - a width picked BEFORE the editor theme switched
                // the default font from Consolas to the wider proportional Segoe UI would otherwise keep
                // overriding the (now correct) 80px default below forever, clipping "Remove" to "Remov"
                // on every future launch. Neither column here is something a user meaningfully needs to
                // hand-resize and remember between sessions, so always-reset-to-default is the right
                // call, not a narrower one-off ini edit.
                // 2 columns: "##SC" (leftmost, narrow, unlabeled - matches files_tab's own SC column
                // convention) shows a Scene/Level's status/lock badge via
                // RenderLevelTreeSourceControlBadgeColumn; empty for every other row kind (Entity/
                // Folder/Dependencies/Runtime - none of them have a file of their own to track). The
                // old 3rd "Actions" column (a per-row Remove/X button) was removed - every row kind
                // that had one already offers the identical action via its own right-click context
                // menu (Scene's "Remove Scene", Entity's "Delete Entity", Folder's "Delete Folder", a
                // dependency entry's "Remove Dependency") - direct user observation the button was
                // redundant: "you should be able to right click to delete any of them."
                // Halved from the editor theme's own global Style.IndentSpacing (16.0f) - direct user
                // request, scoped to just this panel's tree via Push/PopStyleVar rather than editing
                // the shared theme default (which would also shrink every other tree in the app).
                ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 8.0f);
                // The theme's global CellPadding.x (6, the editor theme) applied on BOTH sides of a cell
                // left only 10 - 2*6 = -2px of actual content room in the halved 10px "##SC" column -
                // padding alone already exceeded the column width, so the 6px badge rendered past the
                // column (and past the window's own edge) instead of inside it - direct user report
                // "the source control icons are clipped" (after the column-width halving below).
                // Zeroed out (direct user suggestion) rather than just shrunk - CellPadding is a
                // whole-table style var, not per-column, but the Name column already has its own
                // visual breathing room from each row's icon glyph, so losing its padding too is fine.
                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 1.0f));
                if (ImGui::BeginTable("LevelTree", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoSavedSettings, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
                {
                    // IndentDisable/IndentEnable explicit on both columns (Dear ImGui's own default
                    // is the other way around - see RenderLevelTreeSourceControlBadgeColumn's own
                    // comment): the SC badge column must NOT drift with tree depth, and the Name
                    // column - where every TreeNodeEx in this panel actually lives - must actually
                    // show the tree's indentation, which the table default would otherwise suppress.
                    // "##SC" width history: 20 (original) -> 10 -> 14 -> 30 -> 21 (direct user
                    // request: "reduce the column by 30%" off of 30). BadgeSize itself is back to
                    // 12 (matching every other view - see RenderLevelTreeSourceControlBadge's own
                    // comment) - only the column's own empty margin shrinks here, not the icon.
                    ImGui::TableSetupColumn("##SC", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_IndentDisable, 21.0f);
                    // To the right of it: the Enabled toggle (the disable tag) and the Visible toggle (the eye: the disable_rendering tag) of the entities. Then the names.
                    ImGui::TableSetupColumn("##Enabled", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_IndentDisable, 21.0f);
                    ImGui::TableSetupColumn("##Visible", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_IndentDisable, 21.0f);
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_IndentEnable);
                    ImGui::TableSetupScrollFreeze(0, 1);                    // the headers stay while the tree scrolls
                    RenderLevelTreeHeaderRow(Ed, GameMgr, State);

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                    const std::string LevelLabelWithIcon = std::format("{} {}", xlevel::LevelIcon(), LevelLabel);
                    const bool bLevelOpen = ImGui::TreeNodeEx(LevelLabelWithIcon.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth
                        | (State.m_bRootSelected ?ImGuiTreeNodeFlags_Selected : 0));
                    // A click on the row (not its arrow) selects the Level: the Inspector shows its properties (its Game, its scenes). A right-click opens the menu below, and selects it too.
                    if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
                        xeditor::Run(Undo, "SelectLevel");

                    // Right-click: whole-folder SC Revert - direct user request, item 3 ("The Level
                    // will revert the level resource - everything in the folder of the resource").
                    // The Level row had no context menu at all before this.
                    if (ImGui::BeginPopupContextItem())
                    {
                        // The Game this Level runs under: one of the project's Games, or none (SetLevelGame: refused when the Game lacks a module the scenes need).
                        if (ImGui::BeginMenu("Game"))
                        {
                            const auto LevelValue = State.m_CurrentLevel.m_Instance.m_Value;
                            const auto Named      = xlevel::ReadLevelGame(xlevel::ProjectRoot().wstring(), LevelValue);
                            if (ImGui::MenuItem("(no Game)", nullptr, Named == 0, !State.isPlaying()))
                                xeditor::Run(Undo, std::format("SetLevelGame -Level {:016X}", LevelValue));
                            for (const auto& [Game, Name] : xlevel::commands::BuildAssetNameMap(xgame::type_guid_v))
                                if (ImGui::MenuItem(Name.c_str(), nullptr, Named == Game, !State.isPlaying()))
                                    xeditor::Run(Undo, std::format("SetLevelGame -Level {:016X} -Game {:016X}{:016X}", LevelValue, Game, xgame::type_guid_v.m_Value));
                            ImGui::EndMenu();
                        }
                        ImGui::Separator();
                        RenderLevelTreeSCRevertMenuItem(Undo, xresource::full_guid{ State.m_CurrentLevel.m_Instance, State.m_CurrentLevel.m_Type }
                            , /*bWholeFolder*/ true
                            , "Discard ALL local changes in this Level resource's folder (info.txt, Descriptor.txt, dependencies.txt)?\nThis cannot be undone.");
                        ImGui::EndPopup();
                    }

                    // DESCRIPTOR_GUID from Resources/asset browser:
                    //   Level  -> deferred to window-wide Custom target (full-panel highlight + OpenLevel)
                    //   Scene  -> AddScene to this Level (skips duplicates)
                    if (ImGui::BeginDragDropTarget())
                    {
                        const ImGuiPayload* PeekLevel = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID", ImGuiDragDropFlags_AcceptPeekOnly);
                        if (xlevel::PeekDescriptorPayloadIsLevel(PeekLevel))
                        {
                            // Leave Level for the panel-wide Custom target so the accept rect
                            // covers the whole Level Tree (same UX as the empty-panel drop).
                        }
                        else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID"))
                        {
                            IM_ASSERT(payload->DataSize == sizeof(xresource_editor::drag_and_drop_folder_payload_t));
                            auto& Dropped = *reinterpret_cast<const xresource_editor::drag_and_drop_folder_payload_t*>(payload->Data);
                            if (Dropped.m_Source.m_Type == xecs::scene::type_guid_v)
                            {
                                const xecs::scene::guid NewSceneGuid{ .m_Instance = Dropped.m_Source.m_Instance };
                                if (std::find(pLevel->m_Scenes.begin(), pLevel->m_Scenes.end(), NewSceneGuid) == pLevel->m_Scenes.end())
                                    xeditor::Run(Ed.m_Undo, std::format("AddScene -Level {:016X} -Scene {}"
                                        , State.m_CurrentLevel.m_Instance.m_Value
                                        , xscene::commands::FormatSceneGuid(NewSceneGuid)));
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    // Badge drawn AFTER the Name column's own TreeNodeEx (not before) - a
                    // SpanFullWidth tree row paints its Selected/Hover background across every table
                    // column, so a badge drawn earlier in submission order was silently painted over
                    // by that background on any highlighted row (direct user report: a selected row's
                    // badge "seemed missing"). Routed through *Column (not the plain helper) - it also
                    // neutralizes ImGui's own indent-into-column-0 table quirk, see that helper's own
                    // comment.
                    RenderLevelTreeSourceControlBadgeColumn(xresource::full_guid{ State.m_CurrentLevel.m_Instance, State.m_CurrentLevel.m_Type });
                    RenderLevelTreeContainerMarkers(State.m_CurrentLevel.m_Instance.m_Value, State.m_EditorDisabledCount > 0, State.m_EditorHiddenCount > 0, "Level");
                    if (!bLevelOpen) State.m_TreeExpandedScenes.clear();               // its Scene rows are not drawn: none of them is showing expanded

                    if (bLevelOpen)
                    {
                        for (std::size_t iScene = 0; iScene < pLevel->m_Scenes.size(); ++iScene)
                        {
                            ImGui::PushID(static_cast<int>(iScene));
                            const auto SceneGuid = pLevel->m_Scenes[iScene];

                            std::string SceneLabel;
                            xresource_editor::RemapGUIDToString(SceneLabel, xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type });

                            const bool bIsOpenScene = std::find(State.m_OpenScenes.begin(), State.m_OpenScenes.end(), SceneGuid) != State.m_OpenScenes.end();

                            // Another Level editor is editing this Scene: it is read-only here (browse and inspect, no edits) until
                            // that editor saves it.
                            const bool        bSceneLocked = xlevel::IsSceneLockedByOther(Ed, SceneGuid);
                            const std::string LockOwner    = bSceneLocked ? xlevel::SceneOwnerName(SceneGuid) : std::string{};

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                            const std::string SceneLabelWithIcon = bSceneLocked
                                ? std::format("{} {}  (read-only: edited in {})", xlevel::SceneIcon(), SceneLabel, LockOwner)
                                : std::format("{} {}", xlevel::SceneIcon(), SceneLabel);
                            // Open/loaded is STATUS, not selection focus. Still use Selected so
                            // TreeNode paints a fill, but tint Header* grey locally so it does not
                            // collide with entity-selection blue (ImGuiCol_Header from the editor theme).
                            if (bIsOpenScene)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.29f, 0.29f, 0.29f, 1.0f)); // ~0x4A
                                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.35f, 0.35f, 0.35f, 1.0f)); // ~0x59
                                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.40f, 0.40f, 0.40f, 1.0f)); // ~0x66
                            }
                            // A Level opens with its first Scene expanded: the load of the Level sets the flag, and it is used up here once that Scene is open (a Scene that is
                            // not loaded is not expanded: the row would just load it), so a person who collapses it afterwards is not argued with.
                            if (iScene == 0 && State.m_bExpandFirstScene && bIsOpenScene)
                            {
                                State.m_bExpandFirstScene = false;
                                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                            }
                            // A Scene that was showing expanded and is no longer open (CloseScene, a reload) collapses: "expanded" means "open" below, so left expanded it would be loaded again at once.
                            if (!bIsOpenScene && std::find(State.m_TreeExpandedScenes.begin(), State.m_TreeExpandedScenes.end(), SceneGuid) != State.m_TreeExpandedScenes.end())
                                ImGui::SetNextItemOpen(false, ImGuiCond_Always);
                            const bool bSceneExpanded = ImGui::TreeNodeEx(SceneLabelWithIcon.c_str(), ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth | (bIsOpenScene ? ImGuiTreeNodeFlags_Selected : 0));
                            {
                                auto& Expanded = State.m_TreeExpandedScenes;
                                const auto It = std::find(Expanded.begin(), Expanded.end(), SceneGuid);
                                if (bSceneExpanded && It == Expanded.end()) Expanded.push_back(SceneGuid);
                                else if (!bSceneExpanded && It != Expanded.end()) Expanded.erase(It);
                            }
                            if (bIsOpenScene)
                                ImGui::PopStyleColor(3);

                            // Two independent pieces of state used to collide: ImGui's own
                            // expand/collapse (bSceneExpanded, toggled by ImGuiTreeNodeFlags_OpenOnArrow
                            // on an ARROW click specifically) vs our own scene-residency tracking
                            // (bIsOpenScene/State.m_OpenScenes, previously driven ONLY by
                            // IsItemClicked() on the row's LABEL). Clicking the arrow expands the row
                            // WITHOUT satisfying IsItemClicked() the same way a label click does, so a
                            // row could sit expanded forever showing an inert "(click to open)"
                            // placeholder. Fixed by making "expanded" simply IMPLY "should be open" -
                            // whichever click actually toggled it, OpenScene's own residency check
                            // (State.m_OpenScenes) makes this a cheap no-op once already loaded, so
                            // calling it every frame the row is expanded is safe.
                            if ((bSceneExpanded && !bIsOpenScene) || ImGui::IsItemClicked())
                                xscene::OpenScene(GameMgr, State, xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type });

                            // Right-click: same "New Entity"/"New Folder" the Folder row's own menu
                            // offers (landing at this scene's root), plus removing the scene itself.
                            if (ImGui::BeginPopupContextItem())
                            {
                                ImGui::BeginDisabled(bSceneLocked);
                                if (auto* pMenuScene = GameMgr.m_SceneMgr.Find(SceneGuid))
                                    xscene::ShowCreateMenuItems(Ed, SceneGuid, *pMenuScene, xecs::scene::invalid_folder_id_v);
                                ImGui::EndDisabled();
                                ImGui::Separator();
                                if (ImGui::MenuItem("Remove Scene"))
                                {
                                    xeditor::Run(Ed.m_Undo, std::format("RemoveScene -Level {:016X} -Scene {}"
                                        , State.m_CurrentLevel.m_Instance.m_Value
                                        , xscene::commands::FormatSceneGuid(SceneGuid)));
                                    ImGui::EndPopup();
                                    if (bSceneExpanded) ImGui::TreePop();
                                    ImGui::PopID();
                                    break; // pLevel->m_Scenes was just mutated mid-iteration
                                }
                                ImGui::Separator();
                                // Whole-folder SC Revert - direct user request, item 2 ("revert all
                                // the entities etc... this should come with a warning") - a Scene's
                                // entities/folders/dependencies all live inside its one Descriptor.txt
                                // (see RenderLevelTreeSourceControlBadge's own top comment), so this is
                                // still a single-file revert, just with a bigger-blast-radius warning
                                // than the Entity row's own (item 1).
                                RenderLevelTreeSCRevertMenuItem(Undo, xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type }
                                    , /*bWholeFolder*/ true
                                    , "Discard ALL local changes to this Scene (every entity, folder, and dependency edit)?\nThis cannot be undone.");
                                ImGui::EndPopup();
                            }

                            // Drag this Level-Tree scene onto another scene's Dependencies folder
                            // (same DESCRIPTOR_GUID + xresource_editor::drag_and_drop_folder_payload_t the asset
                            // browser emits for Scene assets). Also works as a drop onto the Level
                            // row (AddScene skips duplicates). SourceAllowNullID: TreeNodeEx items
                            // don't always have a stable ImGui ID the way Button/Selectable do.
                            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                            {
                                xresource_editor::drag_and_drop_folder_payload_t Payload{};
                                Payload.m_Source     = xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type };
                                Payload.m_bSelection = false;
                                ImGui::SetDragDropPayload("DESCRIPTOR_GUID", &Payload, sizeof(Payload));
                                ImGui::Text("%s", SceneLabel.c_str());
                                ImGui::EndDragDropSource();
                            }

                            // Drop a Prefab asset from the asset browser here to instantiate it -
                            // decodes the SAME "DESCRIPTOR_GUID" payload the browser's own asset icons
                            // already drag (see xresource_editor::drag_and_drop_folder_payload_t). ALSO accepts an
                            // entity dragged out of a folder back to loose/root (LEVEL_ENTITY_DRAG,
                            // reusing the same payload struct the prefab-creation drag already uses -
                            // it already carries exactly {SceneGuid, Id}).
                            if (bIsOpenScene && !bSceneLocked && ImGui::BeginDragDropTarget())
                            {
                                const ImGuiPayload* PeekLevel = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID", ImGuiDragDropFlags_AcceptPeekOnly);
                                if (xlevel::PeekDescriptorPayloadIsLevel(PeekLevel))
                                {
                                    // Level -> window-wide Custom (full-panel highlight)
                                }
                                else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID"))
                                {
                                    IM_ASSERT(payload->DataSize == sizeof(xresource_editor::drag_and_drop_folder_payload_t));
                                    auto& Dropped = *reinterpret_cast<const xresource_editor::drag_and_drop_folder_payload_t*>(payload->Data);
                                    if (Dropped.m_Source.m_Type == xecs::prefab::type_guid_v)
                                    {
                                        if (auto* pDropScene = GameMgr.m_SceneMgr.Find(SceneGuid))
                                        {
                                            const auto NewId = xscene::NextFreeEntityId(*pDropScene);
                                            xeditor::Run(Ed.m_Undo, std::format("InstantiatePrefab -Scene {} -Id {} -Prefab {:016X} -Folder {:08X}"
                                                , xscene::commands::FormatSceneGuid(SceneGuid), xscene::commands::FormatEntityId(NewId)
                                                , Dropped.m_Source.m_Instance.m_Value, static_cast<std::uint32_t>(xecs::scene::invalid_folder_id_v)));
                                        }
                                    }
                                }
                                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LEVEL_ENTITY_DRAG"))
                                {
                                    IM_ASSERT(payload->DataSize == sizeof(xscene::entity_drag_payload_t));
                                    auto& Dropped = *reinterpret_cast<const xscene::entity_drag_payload_t*>(payload->Data);
                                    xeditor::Run(Ed.m_Undo, std::format("MoveToFolder -Scene {} -Id {} -Folder {:08X}"
                                        , xscene::commands::FormatSceneGuid(Dropped.m_SceneGuid), xscene::commands::FormatEntityId(Dropped.m_Id)
                                        , static_cast<std::uint32_t>(xecs::scene::invalid_folder_id_v)));
                                }
                                ImGui::EndDragDropTarget();
                            }

                            // Badge after the Name column's own TreeNodeEx - see the Level row's
                            // identical comment above (SpanFullWidth Selected/Hover paint-over fix).
                            // The old "Remove" button (column 2) is gone - "Remove Scene" above
                            // (the row's own right-click context menu) already does the same thing.
                            RenderLevelTreeSourceControlBadgeColumn(xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type });
                            {
                                const auto FlagsIt = State.m_EditorSceneFlags.find(SceneGuid.m_Instance.m_Value);
                                const std::uint8_t Flags = FlagsIt == State.m_EditorSceneFlags.end() ? 0 : FlagsIt->second;
                                RenderLevelTreeContainerMarkers(SceneGuid.m_Instance.m_Value, (Flags & 1) != 0, (Flags & 2) != 0, "Scene");
                            }

                            if (bSceneExpanded)
                            {
                                if (bIsOpenScene)
                                {
                                    if (auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid))
                                    {
                                        // One entity row, used both for folder members and loose
                                        // (unfoldered) entities below - returns true if it just mutated
                                        // pScene->m_LocalToRuntime (deleted), telling the caller's own
                                        // loop over a SNAPSHOT (never the live map/vector directly - see
                                        // every call site below) that this id is now stale.
                                        // Forward-declared as std::function (not auto) - RenderEntityRow
                                        // and RenderChildEntities are mutually recursive (a row renders
                                        // its own children right after itself; rendering a child is just
                                        // calling RenderEntityRow again), the same "declare empty, assign
                                        // after both bodies are written" pattern RenderFolderChildren's
                                        // own self-recursion already uses below, just across a pair.
                                        std::function<bool(xecs::scene::permanent_id, xecs::component::entity)> RenderEntityRow;

                                        // An entity with a xecs::component::parent is never placed via
                                        // folder membership (see the Default-folder adoption pass below,
                                        // which excludes parented entities from its "unfoldered"
                                        // computation) - it's rendered here instead, nested directly
                                        // under its parent's own row. Snapshots the children list before
                                        // recursing (an entity delete/reparent fired from arbitrary depth
                                        // in this recursion must not invalidate an iterator/reference held
                                        // across those calls - same discipline RenderFolderChildren's own
                                        // folder walk already follows).
                                        std::function<void(xecs::component::entity)> RenderChildEntities = [&](xecs::component::entity Parent) noexcept
                                        {
                                            auto* pParentChildren = xlioncore::Ecs(GameMgr).ChildrenOf(Parent);
                                            if (pParentChildren == nullptr) return;

                                            auto ChildEntities = pParentChildren->m_List;
                                            for (auto ChildEntity : ChildEntities)
                                            {
                                                if (auto ChildIt = pScene->m_RuntimeToLocal.find(ChildEntity.m_Value); ChildIt != pScene->m_RuntimeToLocal.end())
                                                    RenderEntityRow(ChildIt->second, ChildEntity);
                                            }
                                        };

                                        RenderEntityRow = [&](xecs::scene::permanent_id Id, xecs::component::entity Entity) -> bool
                                        {
                                            const std::string EntityBaseName = xscene::EntityDisplayName(*pScene, Id);
                                            std::string EntityLabel = EntityBaseName;
                                            bool bHasChildren = false;
                                            bHasChildren = xlioncore::Ecs(GameMgr).ChildrenOf(Entity) != nullptr;
                                            auto* pPI = xscene::FindPrefabInstance(GameMgr, Entity);
                                            if (pPI)
                                            {
                                                std::string PrefabName;
                                                xresource_editor::RemapGUIDToString(PrefabName, pPI->m_PrefabInstance);
                                                EntityLabel += std::format(" (Prefab: {})", PrefabName);
                                            }

                                            // Search filters entities only (folders always stay visible
                                            // so a match nested inside one is still reachable).
                                            if (!State.m_TreeSearchString.empty() && !xeditor::ContainsCaseInsensitive(EntityLabel, State.m_TreeSearchString))
                                                return false;

                                            ImGui::PushID(static_cast<int>(Id));
                                            ImGui::TableNextRow();
                                            ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                                            const bool bEntitySelected = (State.m_SelectedEntityId == Id);
                                            const bool bMultiSelected  = (State.m_MultiSelectScene == SceneGuid) && State.m_MultiSelectedEntityIds.contains(Id);
                                            // Prefab instances render in blue, matching Unity's own
                                            // Hierarchy convention - real GameObjects/entities stay the
                                            // default text color. Unlike the "(Prefab: X)" label suffix
                                            // above (root-only, via the direct pPI check), the tint
                                            // applies to the WHOLE instance subtree - any entity
                                            // structurally inside a prefab instance is still part of it.
                                            // An entity WITH children renders like a folder (expandable,
                                            // arrow) so RenderChildEntities has somewhere to nest under;
                                            // a leaf keeps the plain bullet style every entity used to have.
                                            const bool bPartOfPrefabInstance = xscene::FindContainingPrefabInstance(GameMgr, Entity).m_pPI != nullptr;
                                            const ImGuiTreeNodeFlags SelFlag  = (bEntitySelected || bMultiSelected) ? ImGuiTreeNodeFlags_Selected : 0;
                                            const ImGuiTreeNodeFlags TreeFlags = bHasChildren
                                                ? (ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth | SelFlag)
                                                : (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_SpanFullWidth | SelFlag);
                                            // Inline rename, Explorer style. Starts from F2 on the selected row, the context menu's Rename, or a DOUBLE click on the name (a single click only
                                            // selects: a rename started by a slow second click was too easy to start by mistake). Enter / click-away commits, Esc cancels.
                                            auto BeginRename = [&]() noexcept
                                            {
                                                if (bSceneLocked) return;
                                                State.m_RenameScene      = SceneGuid;
                                                State.m_RenameId         = Id;
                                                State.m_bRenameFocus     = true;
                                                std::snprintf(State.m_RenameText.data(), State.m_RenameText.size(), "%s", EntityBaseName.c_str());
                                            };
                                            // F2 is the Level/Entity/Rename action: it only raises m_bRenameRequested, this row (the selected one) takes it.
                                            if (bEntitySelected && State.m_bRenameRequested && State.m_RenameId != Id && !State.isPlaying())
                                            {
                                                State.m_bRenameRequested = false;
                                                BeginRename();
                                            }
                                            const bool bRenaming = State.m_RenameId == Id && State.m_RenameScene == SceneGuid;

                                            bool bEntityOpen;
                                            if (bRenaming)
                                            {
                                                bEntityOpen = ImGui::TreeNodeEx("##renaming", TreeFlags & ~ImGuiTreeNodeFlags_SpanFullWidth);
                                                ImGui::SameLine(0.0f, 0.0f);
                                                if (State.m_bRenameFocus) { ImGui::SetKeyboardFocusHere(); State.m_bRenameFocus = false; }
                                                ImGui::SetNextItemWidth(-FLT_MIN);
                                                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));     // keep the row the same height as a label
                                                ImGui::InputText("##rename", State.m_RenameText.data(), State.m_RenameText.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                                                ImGui::PopStyleVar();
                                                if (ImGui::IsItemDeactivated())
                                                {
                                                    const std::string NewText = State.m_RenameText.data();
                                                    if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && NewText != EntityBaseName)
                                                    {
                                                        if (NewText.empty()) xeditor::Run(Ed.m_Undo, std::format("RenameEntity -Scene {} -Id {} -Clear 1", xscene::commands::FormatSceneGuid(SceneGuid), xscene::commands::FormatEntityId(Id)));
                                                        else                 xeditor::Run(Ed.m_Undo, std::format("RenameEntity -Scene {} -Id {} -Name {}", xscene::commands::FormatSceneGuid(SceneGuid), xscene::commands::FormatEntityId(Id), xeditor::Quote(NewText)));
                                                    }
                                                    State.m_RenameId = xecs::scene::invalid_permanent_id_v;
                                                }
                                            }
                                            else
                                            {
                                                if (bPartOfPrefabInstance) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 170, 255, 255));
                                                bEntityOpen = ImGui::TreeNodeEx(EntityLabel.c_str(), TreeFlags);
                                                if (bPartOfPrefabInstance) ImGui::PopStyleColor();
                                            }

                                            // Select on mouse-UP, not mouse-DOWN (ImGui::IsItemClicked
                                            // fires on press) - selecting on press reassigns
                                            // State.m_SelectedEntity (rebuilding the WHOLE Entity
                                            // Properties inspector, m_bEntityInspectorDirty) before a
                                            // drag onto one of its OWN property rows (e.g. an
                                            // EntityReference's drop target) could ever get going,
                                            // exactly the "select on press" pitfall most drag-capable
                                            // list/tree widgets (Windows Explorer included) avoid.
                                            // GetMouseDragDelta (not IsMouseDragging, which needs the
                                            // button still held to report anything - already false by
                                            // the time a release is detected) is the one ImGui query
                                            // documented to still reflect the drag distance on the
                                            // exact release frame.
                                            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                                            {
                                                const ImVec2 Drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                                                if (Drag.x == 0.0f && Drag.y == 0.0f) // released without ever dragging past the threshold
                                                {
                                                    // Routed through the command/undo system
                                                    // (xscene_commands_selection.h) instead of
                                                    // mutating State directly - phase 1 of
                                                    // the command/undo plan. Ctrl
                                                    // held = ToggleMultiSelect (multi-select only,
                                                    // primary selection untouched); plain click =
                                                    // Select (primary selection + reset multi-select
                                                    // to just this entity) - same two behaviors as
                                                    // before, just undoable now via Ctrl+Z.
                                                    if (ImGui::GetIO().KeyCtrl)
                                                        xeditor::Run(Ed.m_Undo, std::format("ToggleMultiSelect -Scene {} -Id {}", xscene::commands::FormatSceneGuid(SceneGuid), xscene::commands::FormatEntityId(Id)));
                                                    else
                                                    {
                                                        xeditor::Run(Ed.m_Undo, std::format("Select -Scene {} -Id {}", xscene::commands::FormatSceneGuid(SceneGuid), xscene::commands::FormatEntityId(Id)));
                                                    }
                                                }
                                            }

                                            // A double click on the NAME renames (not on the arrow that opens the children, which a double click would toggle twice, and not while playing)
                                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !bRenaming && !State.isPlaying() && !ImGui::GetIO().KeyCtrl)
                                            {
                                                const bool bOnArrow = !(TreeFlags & ImGuiTreeNodeFlags_Leaf)
                                                                   && ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].x < ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing();
                                                if (!bOnArrow) BeginRename();
                                            }

                                            // Three independent drag intents from the same row, picked
                                            // apart by the consumer at drop time (which target it landed
                                            // on), NOT by payload type name: dropping on the asset
                                            // browser creates a Prefab (xlevel::entity_to_prefab_drop);
                                            // dropping on a folder/scene-root row here reparents it;
                                            // dropping on an xecs::component::entity-typed property row
                                            // in the Entity Properties inspector assigns a reference (see
                                            // entity_inspector_bridge::m_OnEntityReferenceRender).
                                            // ImGui::SetDragDropPayload writes into ONE global payload
                                            // slot - calling it multiple times with different type-name
                                            // strings does NOT register multiple simultaneous payloads,
                                            // each call just overwrites the last, so only one name can
                                            // ever be shared here (same pattern as this codebase's own
                                            // "DESCRIPTOR_GUID" reuse).
                                            const bool bBeganDragSource = ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID);
                                            if (bBeganDragSource)
                                            {
                                                xscene::entity_drag_payload_t Payload{ SceneGuid, Id };
                                                ImGui::SetDragDropPayload("LEVEL_ENTITY_DRAG", &Payload, sizeof(Payload));
                                                ImGui::Text("%s", EntityLabel.c_str());
                                                ImGui::EndDragDropSource();
                                            }

                                            bool bDeleted = false;
                                            auto DoDeleteEntity = [&]() noexcept
                                            {
                                                // Routed through the command/undo system
                                                // (the command/undo plan, phase 4 -
                                                // xscene_commands_entity_lifecycle.h) instead of
                                                // calling DeleteEntitySubtree directly - selection/
                                                // multi-select survival cleanup (this row or a now-
                                                // deleted descendant of it) now lives in the command's
                                                // own DeleteSubtreeByPermanentId, shared with Undo so
                                                // it behaves identically from either direction.
                                                xeditor::Run(Ed.m_Undo, std::format("DeleteEntity -Scene {} -Id {}", xscene::commands::FormatSceneGuid(SceneGuid), xscene::commands::FormatEntityId(Id)));
                                                bDeleted = true;
                                            };

                                            // Making a prefab is drag-and-drop only (ctrl-click to
                                            // multi-select, drag any selected row onto the asset
                                            // browser - entity_to_prefab_drop::OnDrop, generalized to
                                            // check for an active multi-selection), matching the
                                            // existing single-entity convention rather than a separate
                                            // menu action.
                                            if (ImGui::BeginPopupContextItem())
                                            {
                                                // "New Entity" here creates a CHILD of this row - the
                                                // only entity-row context menu action previously offered
                                                // was Delete; there was no way at all to create an entity
                                                // parented under another entity (direct user report).
                                                // Routed through the same CreateEntity command
                                                // ShowCreateMenuItems uses (xlevel_editor.h),
                                                // just with -Parent instead of -Folder.
                                                ImGui::BeginDisabled(bSceneLocked);
                                                if (ImGui::MenuItem("New Entity"))
                                                {
                                                    const auto NewId = xscene::NextFreeEntityId(*pScene);
                                                    xeditor::Run(Ed.m_Undo, std::format("CreateEntity -Scene {} -Id {} -Folder {:08X} -Parent {}"
                                                        , xscene::commands::FormatSceneGuid(SceneGuid)
                                                        , xscene::commands::FormatEntityId(NewId)
                                                        , static_cast<std::uint32_t>(xecs::scene::invalid_folder_id_v)
                                                        , xscene::commands::FormatEntityId(Id)
                                                        ));
                                                }
                                                if (ImGui::MenuItem("Rename", xeditor::ShortcutText("Level/Entity/Rename", "F2").c_str(), false, !State.isPlaying())) BeginRename();
                                                ImGui::Separator();
                                                if (ImGui::MenuItem("Delete Entity", xeditor::ShortcutText("Level/Entity/Delete").c_str())) DoDeleteEntity();
                                                ImGui::EndDisabled();
                                                ImGui::Separator();
                                                // Single-file revert of exactly the resource this row's
                                                // own badge represents (the Prefab, if this is a prefab-
                                                // instance root; otherwise the owning Scene) - direct
                                                // user request, item 1 ("should be simple").
                                                RenderLevelTreeSCRevertMenuItem(Undo, pPI ? pPI->m_PrefabInstance : xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type }
                                                    , /*bWholeFolder*/ false
                                                    , "Discard local changes to this entity's owning resource?\nThis cannot be undone.");
                                                ImGui::EndPopup();
                                            }

                                            // Badge after the Name column's own TreeNodeEx - see the
                                            // Level row's identical comment above (SpanFullWidth
                                            // Selected/Hover paint-over fix). The old "X" button
                                            // (column 2) is gone - "Delete Entity" above (the row's own
                                            // right-click context menu) already does the same thing.
                                            // An entity has no file of its own (its data lives inside
                                            // its owning Scene's single file - see this panel's own top
                                            // comment) - direct user correction, "you forgot the
                                            // entities": show the OWNING SCENE's own badge here too, so
                                            // a scene's pending change is visible drilled all the way
                                            // down to whichever entity you're actually looking at, not
                                            // just at the Scene row itself. A PREFAB INSTANCE root is
                                            // the one exception - direct user follow-up, "also the
                                            // prefab instances": it references a REAL, separately-
                                            // tracked Prefab resource (its own file, own git status,
                                            // own lock state, independent of the scene it's placed in),
                                            // so that resource's OWN badge is the more specific, more
                                            // relevant signal here - shown instead of the owning
                                            // scene's, not alongside it (only one badge slot exists). A
                                            // non-root entity nested INSIDE a prefab instance subtree
                                            // still just shows the scene's own badge - it has no
                                            // resource identity of its own either.
                                            if (!bDeleted)
                                                RenderLevelTreeSourceControlBadgeColumn(pPI ? pPI->m_PrefabInstance : xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type });
                                            if (!bDeleted)
                                                RenderLevelTreeEntityToggles(Ed, GameMgr, SceneGuid, Id, Entity);

                                            // TreeNodeEx above (no NoTreePushOnOpen for a bHasChildren
                                            // row) already pushed a node onto ImGui's own ID/tree stack
                                            // whenever it returned bEntityOpen==true - that push MUST be
                                            // balanced by exactly one TreePop() call regardless of what
                                            // happens to the underlying entity afterward. Gating BOTH
                                            // calls on the same "!bDeleted" skips the TreePop the moment
                                            // a row that's both expanded AND just got deleted - corrupting
                                            // ImGui's stack, which surfaces as an unrelated-looking
                                            // IM_ASSERT/abort on a LATER frame or a later row. Only the
                                            // "render children" call itself should skip when deleted -
                                            // there's nothing left to walk into for a just-deleted subtree.
                                            if (bHasChildren && bEntityOpen)
                                            {
                                                if (!bDeleted) RenderChildEntities(Entity);
                                                ImGui::TreePop();
                                            }

                                            ImGui::PopID();
                                            return bDeleted;
                                        };

                                        // Recursive folder walk. Snapshots ids (never a live reference
                                        // into pScene->m_Folders) before recursing/rendering, then
                                        // re-finds each by id fresh right before use - the tree is
                                        // mutated in-place by "+ New Folder"/delete/reparent actions
                                        // fired from arbitrary depths in this same recursion, which
                                        // would invalidate any iterator/pointer held across those calls.
                                        std::function<void(xecs::scene::folder_id)> RenderFolderChildren = [&](xecs::scene::folder_id ParentId)
                                        {
                                            std::vector<xecs::scene::folder_id> ChildIds;
                                            for (auto& F : pScene->m_Folders)
                                            {
                                                if (F.m_Parent != ParentId) continue;
                                                ChildIds.push_back(F.m_Id);
                                            }

                                            // Alphabetical among siblings at this SAME level - the live-tree
                                            // analog of the saved file's own (depth, then name) ordering
                                            // (SaveSceneDescriptor), so what's on screen matches what's on disk.
                                            std::sort(ChildIds.begin(), ChildIds.end(), [&](auto A, auto B) noexcept
                                            {
                                                auto ItA = std::find_if(pScene->m_Folders.begin(), pScene->m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == A; });
                                                auto ItB = std::find_if(pScene->m_Folders.begin(), pScene->m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == B; });
                                                return ItA->m_Name < ItB->m_Name;
                                            });

                                            for (auto FolderId : ChildIds)
                                            {
                                                auto It = std::find_if(pScene->m_Folders.begin(), pScene->m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == FolderId; });
                                                if (It == pScene->m_Folders.end()) continue; // deleted earlier this same frame

                                                ImGui::PushID(static_cast<int>(FolderId));
                                                ImGui::TableNextRow();
                                                ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                                                const bool bFolderHasChildren = !It->m_Entities.empty() || std::any_of(pScene->m_Folders.begin(), pScene->m_Folders.end(), [&](auto& F) noexcept { return F.m_Parent == FolderId; });
                                                const std::string FolderLabel = std::format("{} {}", xlevel::FolderIcon(bFolderHasChildren), It->m_Name);
                                                const bool bFolderOpen = ImGui::TreeNodeEx(FolderLabel.c_str(), ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth);

                                                bool bFolderDeleted = false;
                                                if (ImGui::BeginPopupContextItem())
                                                {
                                                    ImGui::BeginDisabled(bSceneLocked);
                                                    xscene::ShowCreateMenuItems(Ed, SceneGuid, *pScene, FolderId);
                                                    ImGui::Separator();
                                                    if (ImGui::MenuItem("Delete Folder"))
                                                    {
                                                        xeditor::Run(Ed.m_Undo, std::format("DeleteFolder -Scene {} -Id {:08X}"
                                                            , xscene::commands::FormatSceneGuid(SceneGuid)
                                                            , static_cast<std::uint32_t>(FolderId)
                                                            ));
                                                        bFolderDeleted = true;
                                                    }
                                                    ImGui::EndDisabled();
                                                    ImGui::EndPopup();
                                                }
                                                if (bFolderDeleted)
                                                {
                                                    if (bFolderOpen) ImGui::TreePop();
                                                    ImGui::PopID();
                                                    continue; // It/this folder no longer exists - nothing left to render for it
                                                }

                                                if (ImGui::BeginDragDropTarget())
                                                {
                                                    // Drop a Prefab asset directly onto a folder row -
                                                    // matches the Scene row's own "DESCRIPTOR_GUID"
                                                    // handling, except the new instance lands in THIS
                                                    // folder instead of always landing loose at scene root.
                                                    const ImGuiPayload* PeekLevel = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID", ImGuiDragDropFlags_AcceptPeekOnly);
                                                    if (xlevel::PeekDescriptorPayloadIsLevel(PeekLevel))
                                                    {
                                                        // Level -> window-wide Custom (full-panel highlight)
                                                    }
                                                    else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID"))
                                                    {
                                                        IM_ASSERT(payload->DataSize == sizeof(xresource_editor::drag_and_drop_folder_payload_t));
                                                        auto& Dropped = *reinterpret_cast<const xresource_editor::drag_and_drop_folder_payload_t*>(payload->Data);
                                                        if (Dropped.m_Source.m_Type == xecs::prefab::type_guid_v)
                                                        {
                                                            const auto NewId = xscene::NextFreeEntityId(*pScene);
                                                            xeditor::Run(Ed.m_Undo, std::format("InstantiatePrefab -Scene {} -Id {} -Prefab {:016X} -Folder {:08X}"
                                                                , xscene::commands::FormatSceneGuid(SceneGuid), xscene::commands::FormatEntityId(NewId)
                                                                , Dropped.m_Source.m_Instance.m_Value, static_cast<std::uint32_t>(FolderId)));
                                                        }
                                                    }
                                                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LEVEL_ENTITY_DRAG"))
                                                    {
                                                        IM_ASSERT(payload->DataSize == sizeof(xscene::entity_drag_payload_t));
                                                        auto& Dropped = *reinterpret_cast<const xscene::entity_drag_payload_t*>(payload->Data);
                                                        if (Dropped.m_SceneGuid == SceneGuid)
                                                        {
                                                            // An entity with a xecs::component::parent is
                                                            // never placed via folder membership - it
                                                            // renders nested under its parent's own row
                                                            // instead (RenderChildEntities); dropping one
                                                            // onto a folder here is a no-op rather than
                                                            // creating a duplicate-looking entry. Pre-
                                                            // filtered here (rather than relying solely on
                                                            // move_to_folder_cmd's own identical check) so
                                                            // a normal invalid drop stays silent instead of
                                                            // logging a doomed command to the console.
                                                            bool bHasParent = false;
                                                            if (auto DroppedIt = pScene->m_LocalToRuntime.find(Dropped.m_Id); DroppedIt != pScene->m_LocalToRuntime.end())
                                                            {
                                                                bHasParent = xlioncore::Ecs(GameMgr).ParentOf(DroppedIt->second) != nullptr;
                                                            }
                                                            if (!bHasParent)
                                                                xeditor::Run(Ed.m_Undo, std::format("MoveToFolder -Scene {} -Id {} -Folder {:08X}"
                                                                    , xscene::commands::FormatSceneGuid(SceneGuid), xscene::commands::FormatEntityId(Dropped.m_Id)
                                                                    , static_cast<std::uint32_t>(FolderId)));
                                                        }
                                                    }
                                                    ImGui::EndDragDropTarget();
                                                }

                                                // Badge after the Name column's own TreeNodeEx - see
                                                // the Level row's identical comment above (SpanFullWidth
                                                // Selected/Hover paint-over fix). The old "X" button
                                                // (column 2) is gone - "Delete Folder" above (the row's
                                                // own right-click context menu) already does the same
                                                // thing. Same reasoning as RenderEntityRow's own badge -
                                                // a folder is purely an in-memory organizational node
                                                // inside its owning Scene's single file, so it shows
                                                // that scene's own badge too.
                                                RenderLevelTreeSourceControlBadgeColumn(xresource::full_guid{ SceneGuid.m_Instance, SceneGuid.m_Type });
                                                RenderLevelTreeFolderToggles(Ed, GameMgr, SceneGuid, *pScene, FolderId);

                                                if (bFolderOpen)
                                                {
                                                    RenderFolderChildren(FolderId);

                                                    // Snapshot this folder's own member ids too - a
                                                    // nested delete/reparent could otherwise mutate
                                                    // It->m_Entities while this exact loop walks it.
                                                    std::vector<xecs::scene::permanent_id> MemberIds;
                                                    if (auto FreshIt = std::find_if(pScene->m_Folders.begin(), pScene->m_Folders.end(), [&](auto& F) noexcept { return F.m_Id == FolderId; }); FreshIt != pScene->m_Folders.end())
                                                        MemberIds = FreshIt->m_Entities;
                                                    for (auto EId : MemberIds)
                                                        if (auto EIt = pScene->m_LocalToRuntime.find(EId); EIt != pScene->m_LocalToRuntime.end())
                                                            RenderEntityRow(EId, EIt->second);

                                                    ImGui::TreePop();
                                                }
                                                ImGui::PopID();
                                            }
                                        };

                                        // Special, fixed folder for this scene's dependencies - NOT a
                                        // real entry in pScene->m_Folders (so it can never be confused
                                        // with a user folder), synthesized here from
                                        // pScene->m_ParentScenes directly. Always the first child of the
                                        // scene's own row: "there are no parent scenes in reality, Scenes
                                        // have dependencies" - and the folder itself "can not be moved or
                                        // touched", so no delete/drag-source on the folder row, only a
                                        // drop target (drag a Scene from the asset browser OR from this Level Tree onto it to add a dependency)
                                        // and a per-entry delete button below.
                                        {
                                            ImGui::PushID("Dependencies");
                                            ImGui::TableNextRow();
                                            ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                                            const std::string DepLabel = std::format("{} Dependencies", xlevel::DependenciesIcon());
                                            const bool bDepOpen = ImGui::TreeNodeEx(DepLabel.c_str(), ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth);

                                            if (ImGui::BeginDragDropTarget())
                                            {
                                                const ImGuiPayload* PeekLevel = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID", ImGuiDragDropFlags_AcceptPeekOnly);
                                                if (xlevel::PeekDescriptorPayloadIsLevel(PeekLevel))
                                                {
                                                    // Level -> window-wide Custom (full-panel highlight)
                                                }
                                                else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DESCRIPTOR_GUID"))
                                                {
                                                    IM_ASSERT(payload->DataSize == sizeof(xresource_editor::drag_and_drop_folder_payload_t));
                                                    auto& Dropped = *reinterpret_cast<const xresource_editor::drag_and_drop_folder_payload_t*>(payload->Data);
                                                    if (Dropped.m_Source.m_Type == xecs::scene::type_guid_v && Dropped.m_Source.m_Instance != SceneGuid.m_Instance)
                                                    {
                                                        const xecs::scene::guid NewParent{ .m_Instance = Dropped.m_Source.m_Instance };
                                                        if (std::find(pScene->m_ParentScenes.begin(), pScene->m_ParentScenes.end(), NewParent) == pScene->m_ParentScenes.end())
                                                        {
                                                            xeditor::Run(Ed.m_Undo, std::format("AddSceneDependency -Scene {} -Parent {}"
                                                                , xscene::commands::FormatSceneGuid(SceneGuid)
                                                                , xscene::commands::FormatSceneGuid(NewParent)));
                                                        }
                                                    }
                                                }
                                                ImGui::EndDragDropTarget();
                                            }

                                            if (bDepOpen)
                                            {
                                                for (std::size_t iDep = 0; iDep < pScene->m_ParentScenes.size(); ++iDep)
                                                {
                                                    ImGui::PushID(static_cast<int>(iDep));
                                                    std::string DepName;
                                                    xresource_editor::RemapGUIDToString(DepName, xresource::full_guid{ pScene->m_ParentScenes[iDep].m_Instance, pScene->m_ParentScenes[iDep].m_Type });

                                                    ImGui::TableNextRow();
                                                    ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                                                    ImGui::TreeNodeEx(DepName.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_SpanFullWidth);

                                                    bool bDepRemoved = false;
                                                    if (ImGui::BeginPopupContextItem())
                                                    {
                                                        if (ImGui::MenuItem("Remove Dependency")) bDepRemoved = true;
                                                        ImGui::EndPopup();
                                                    }

                                                    // Old "X" button (column 2) is gone - "Remove
                                                    // Dependency" above (right-click) already does this.
                                                    if (bDepRemoved)
                                                    {
                                                        const auto ParentGuid = pScene->m_ParentScenes[iDep];
                                                        xlevel::RequestRemoveSceneDependency(GameMgr, Undo, SceneGuid, ParentGuid);
                                                        ImGui::PopID();
                                                        break; // pScene->m_ParentScenes was just mutated mid-iteration
                                                    }
                                                    ImGui::PopID();
                                                }
                                                ImGui::TreePop();
                                            }
                                            ImGui::PopID();
                                        }

                                        // Prune any folder membership id that no longer resolves to a
                                        // live entity - a folder's Entities[] list is never itself
                                        // scrubbed except through the specific "delete this one entity"/
                                        // "reparent this one entity" code paths, so anything that ever
                                        // got out of that silently accumulates: the folder's own header
                                        // count (Default's own "(N)" label included) then disagrees with
                                        // what actually renders underneath it, since the render loop
                                        // below already skips ids it can't resolve. Runs before both the
                                        // folder-count label and the auto-adopt pass right below, so a
                                        // pruned id can be correctly picked back up as "unfoldered" and
                                        // re-adopted into Default the SAME frame instead of just
                                        // vanishing from bookkeeping with no visible trace.
                                        for (auto& F : pScene->m_Folders)
                                            std::erase_if(F.m_Entities, [&](auto Id) noexcept { return !pScene->m_LocalToRuntime.contains(Id); });

                                        // Any entity not currently in any folder renders directly at
                                        // scene root, no wrapping folder - direct user request (reverses
                                        // an earlier one: "remove the default folder"). An entity with a
                                        // xecs::component::parent is neither "foldered" nor "unfoldered"
                                        // - it's rendered nested under its parent's own row instead
                                        // (RenderEntityRow's own RenderChildEntities call), so it must
                                        // never ALSO render again here as if it were loose.
                                        {
                                            std::unordered_set<xecs::scene::permanent_id> FolderedEntities;
                                            for (auto& F : pScene->m_Folders)
                                                for (auto EId : F.m_Entities) FolderedEntities.insert(EId);

                                            std::vector<xecs::scene::permanent_id> Unfoldered;
                                            int Unknown = 0;
                                            for (auto& Pair : pScene->m_LocalToRuntime)
                                            {
                                                if (FolderedEntities.contains(Pair.first)) continue;
                                                auto& Ecs = xlioncore::Ecs(GameMgr);
                                                if (!Ecs.IsAlive(Pair.second)) { ++Unknown; continue; }
                                                if (Ecs.ParentOf(Pair.second)) continue;
                                                Unfoldered.push_back(Pair.first);
                                            }
                                            // Said once per change, not once per frame: a diagnostic of the Logs, with a stable code.
                                            if (ImGuiStorage* pStorage = ImGui::GetStateStorage(); pStorage->GetInt(ImGui::GetID("level.tree.unknown"), 0) != Unknown)
                                            {
                                                pStorage->SetInt(ImGui::GetID("level.tree.unknown"), Unknown);
                                                if (Unknown)
                                                    if (auto* pLogs = xlog::hub::current())
                                                    {
                                                        xlog::event E;
                                                        E.m_Producer = "xlion.leveltree"; E.m_Origin = { xlog::origin::type::Editor, "level", 0 };
                                                        E.m_Severity = xlog::severity::Error; E.m_Kind = xlog::kind::Diagnostic; E.m_Channel = "level.tree";
                                                        E.m_Code = "LEVEL.TREE.UNKNOWN_ENTITIES";
                                                        xlog::SetMessage(E, std::format("{} entit{} of the scene are not known to the world and are not listed (the scene outlived the world that made them)", Unknown, Unknown == 1 ? "y" : "ies"));
                                                        pLogs->Emit(std::move(E));
                                                    }
                                            }

                                            for (auto Id : Unfoldered)
                                                if (auto EIt = pScene->m_LocalToRuntime.find(Id); EIt != pScene->m_LocalToRuntime.end())
                                                    RenderEntityRow(Id, EIt->second);
                                        }

                                        RenderFolderChildren(xecs::scene::invalid_folder_id_v); // root-level user folders

                                        // Prefabs are still instantiated by dragging one from the asset
                                        // browser onto this scene's own row in the tree (see the drop
                                        // target attached to it above) - Unity-style, no button.
                                    }
                                }
                                else
                                {
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                                    ImGui::TextDisabled("(click to open)");
                                }
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }

                        // "Runtime" - a read-only row, sibling to this Level's own Scene rows, listing every
                        // live ECS entity that belongs to no open Scene. Entities spawned while the game runs
                        // sit directly under it (direct user request: "those entities don't belong in any
                        // scene"); the ECS's own system entities go in sub-folders - "Prefabs" (the prefab
                        // templates instances are copied from) and "Share Components" (the entity holding
                        // each distinct share-component value). Still no drag-drop, context menu or delete -
                        // read-only, synthesized every frame. Counts are per-archetype sums; the entity
                        // lists are only walked while their folder is expanded.
                        {
                            using runtime_kind = xlioncore::xECSEditor;
                            constexpr auto SPAWNED = xlioncore::xECSEditor::SPAWNED, PREFAB = xlioncore::xECSEditor::PREFAB, SHARE = xlioncore::xECSEditor::SHARE;
                            constexpr int  KIND_COUNT = xlioncore::xECSEditor::KIND_COUNT;

                            const auto IsInOpenScene = [&](xecs::component::entity E) noexcept
                            {
                                for (auto& OpenSceneGuid : State.m_OpenScenes)
                                    if (auto* pOpenScene = GameMgr.m_SceneMgr.Find(OpenSceneGuid); pOpenScene && pOpenScene->m_RuntimeToLocal.contains(E.m_Value))
                                        return true;
                                return false;
                            };

                            std::array<int, KIND_COUNT> Count{};
                            xlioncore::Ecs(GameMgr).CountRuntimeEntities(Count);

                            // Scene entities are never prefab templates or share-entities, so they only
                            // ever need subtracting from the spawned count.
                            for (auto& OpenSceneGuid : State.m_OpenScenes)
                                if (auto* pOpenScene = GameMgr.m_SceneMgr.Find(OpenSceneGuid))
                                    Count[SPAWNED] -= static_cast<int>(pOpenScene->m_LocalToRuntime.size());
                            Count[SPAWNED] = std::max(0, Count[SPAWNED]);

                            const int RuntimeCount = Count[SPAWNED] + Count[PREFAB] + Count[SHARE];

                            const auto RenderEntities = [&](xlioncore::xECSEditor::runtime_kind Kind) noexcept
                            {
                                std::vector<xlioncore::xECSEditor::runtime_entity> Entities;
                                xlioncore::Ecs(GameMgr).ListRuntimeEntities(Kind, Entities);
                                for (const auto& [E, Components] : Entities)
                                {
                                    if (Kind == SPAWNED && IsInOpenScene(E)) continue;

                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                                    ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(E.m_Value))
                                        , ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth
                                        , "Entity 0x%llX  (%s)", static_cast<unsigned long long>(E.m_Value), Components.c_str());
                                }
                            };

                            const auto RenderSubFolder = [&](const char* pName, xlioncore::xECSEditor::runtime_kind Kind) noexcept
                            {
                                if (Count[Kind] == 0) return;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                                if (ImGui::TreeNodeEx(pName, ImGuiTreeNodeFlags_SpanFullWidth, "%s %s (%d)", xlevel::FolderIcon(true), pName, Count[Kind]))
                                {
                                    RenderEntities(Kind);
                                    ImGui::TreePop();
                                }
                            };

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(kLevelTreeColumnName);
                            const std::string RuntimeLabel = std::format("{} Runtime ({})", xlevel::FolderIcon(RuntimeCount != 0), RuntimeCount);

                            // Distinct color (not a distinct icon - FolderIcon's own codepoints are
                            // already confirmed to render as blank tofu boxes in this font build, see
                            // e10_asset_tree_polish_pass2 memory, so a new icon here would be equally
                            // unreliable) - direct user request: "a special icon... or better yet a
                            // different color" to visually set this read-only, synthesized row apart
                            // from real user folders at a glance.
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 200, 90, 255));
                            const bool bRuntimeOpen = ImGui::TreeNodeEx("##RuntimeRow", (RuntimeCount == 0 ? ImGuiTreeNodeFlags_Leaf : 0) | ImGuiTreeNodeFlags_SpanFullWidth, "%s", RuntimeLabel.c_str());
                            ImGui::PopStyleColor();

                            if (bRuntimeOpen)
                            {
                                RenderSubFolder("Prefabs",          PREFAB);
                                RenderSubFolder("Share Components", SHARE);
                                RenderEntities(SPAWNED);
                                ImGui::TreePop();
                            }
                        }

                        // Adding a Scene to this Level is now drag-and-drop onto the Level's own row
                        // (see the drop target attached to it above) - Unity-style, no button.

                        ImGui::TreePop();
                    }
                    ImGui::EndTable();
                }
                ImGui::PopStyleVar(2); // IndentSpacing + CellPadding, both pushed unconditionally above BeginTable

                // Window-wide Level drop (xresource_editor FilesBackgroundDropTarget pattern): ImGui picks the
                // smallest accepting target, so row Prefab/Scene targets still win for those types.
                // Rows PeekOnly-skip Level, so this large ContentRegionRect owns Level highlight + open.
                if (ImGui::BeginDragDropTargetCustom(ImGui::GetCurrentWindow()->ContentRegionRect, ImGui::GetID("LevelTreeLevelDrop")))
                {
                    xlevel::TryAcceptOpenLevelDescriptorDrop();
                    ImGui::EndDragDropTarget();
                }
                // Scene dependencies now render as a fixed "Dependencies" folder directly under each
                // Scene's own row inside the tree above, not a separate section here.
            }

            // Stable, always-reached call site for the SC Revert confirm modal - see
            // level_tree_sc_revert_request's own comment for why this can't live inline at the
            // MenuItem that requests it.
            RenderLevelTreeSCRevertConfirmModal(Undo);
            if (bReadOnly) ImGui::EndDisabled();
        }
        ImGui::End();
        xeditor::diagnostics::Log("window end: %s", pWindowName);
    }

} // namespace xlevel

#endif // XLEVEL_PANEL_LEVEL_TREE_H
