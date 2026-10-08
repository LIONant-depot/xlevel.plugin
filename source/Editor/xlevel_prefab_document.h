#ifndef XLEVEL_PREFAB_DOCUMENT_H
#define XLEVEL_PREFAB_DOCUMENT_H
#pragma once

// The Prefab Editor's document (documentation/Editors/prefabs_plan.md, phase 5). A prefab is stored as a scene, so the editor of a prefab is the Level editor with a prefab in place of
// the Level: the prefab is open as a scene of its own guid (xecs::scene::instance::m_bPrefabDocument, which makes the scene manager read and write the prefab's folder, as a prefab), in
// State.m_OpenScenes like any scene - selection, the scene commands, the inspector, the gizmos, undo and Save are the ones of a scene. What this file adds is opening it, the scenes
// brought in to test against (context scenes), and the snapshots of the document that one writer per prefab needs.
#include "dependencies/xLIONRender/src/xlionrender_api.h"
#include <algorithm>
#include <atomic>
#include <filesystem>

namespace xlevel
{
    inline xresource::full_guid PrefabResourceGuid(const level_state& State) noexcept
    {
        return xresource::full_guid{ State.m_CurrentPrefab.m_Instance, xecs::prefab::type_guid_v };
    }

    // ---- editing in context (prefabs_plan.md, phase 7): where the document's root is ----------------------------------------------------------------------------------------------------

    // The Transform of the document's root (the one entity of the prefab with no parent); null when the document is not loaded.
    inline xlioncore::transform* DocumentRootTransform(xecs::game_mgr::instance& GameMgr, const level_state& State) noexcept
    {
        auto&       Ecs    = xlioncore::Ecs(GameMgr);
        const auto* pScene = GameMgr.m_SceneMgr.Find(State.PrefabScene());
        if (!pScene) return nullptr;
        for (const auto& [Id, Entity] : pScene->m_LocalToRuntime)
            if (!Ecs.ParentOf(Entity)) return xlioncore::ComponentOf<xlioncore::transform>(Ecs, Entity);
        return nullptr;
    }

    // Editing in context: the document has just been loaded (as the prefab holds it), so its root's Transform is what a save has to write (kept), and the root takes the place of the instance. Not an undo step and
    // not a change: the document stays as clean as it was.
    inline void PlaceDocumentRoot(xecs::game_mgr::instance& GameMgr, level_state& State) noexcept
    {
        auto& Edit = State.m_ContextEdit;
        if (!Edit.m_bActive) return;
        auto* pRoot = DocumentRootTransform(GameMgr, State);
        if (!pRoot) return;
        Edit.m_AsSaved       = *pRoot;
        pRoot->m_Position    = Edit.m_Placed.m_Position;
        pRoot->m_Rotation    = Edit.m_Placed.m_Rotation;
        pRoot->m_Scale       = Edit.m_Placed.m_Scale;
        pRoot->m_EditorRotation = Edit.m_Placed.m_Rotation.ToEuler();
        pRoot->MarkDirtyToPhysics();
    }

    // Runs Fn (a write of the document: a save, a snapshot) with the root's Transform as the prefab holds it, then puts the placement back: where the instance is belongs to the Level, not to the prefab.
    template<typename T_FN>
    inline auto WithRootAsSaved(xecs::game_mgr::instance& GameMgr, const level_state& State, T_FN&& Fn) noexcept
    {
        auto* pRoot = State.m_ContextEdit.m_bActive ? DocumentRootTransform(GameMgr, State) : nullptr;
        if (!pRoot) return Fn();
        const xlioncore::transform Placed = *pRoot;
        *pRoot = State.m_ContextEdit.m_AsSaved;
        auto Result = Fn();
        if (auto* pAgain = DocumentRootTransform(GameMgr, State)) *pAgain = Placed;         // (a save can spawn instances again: the pools may have moved)
        return Result;
    }

    // Opens a prefab as this editor's document: the scene of its guid, loaded from the prefab's folder, and the context scenes again when there are some (the world was rebuilt). "" or why not.
    inline std::string OpenPrefabDocument(xecs::game_mgr::instance& GameMgr, level_state& State, xresource::full_guid Prefab) noexcept
    {
        auto&                   Ecs = xlioncore::Ecs(GameMgr);
        const xecs::scene::guid Scene{ .m_Instance = Prefab.m_Instance };
        Ecs.FindOrCreateScene(Scene).m_bPrefabDocument = true;
        if (auto Err = Ecs.RequestLoadScene(Scene); Err)
            return std::string(Err.getMessage());

        State.m_CurrentPrefab    = xecs::prefab::guid{ .m_Instance = Prefab.m_Instance, .m_Type = xecs::prefab::type_guid_v };
        State.m_bLevelEditorOpen = true;
        if (std::find(State.m_OpenScenes.begin(), State.m_OpenScenes.end(), Scene) == State.m_OpenScenes.end())
            State.m_OpenScenes.push_back(Scene);

        for (const auto& Context : State.m_ContextScenes)
            if (auto Err = Ecs.RequestLoadScene(Context); Err)
                xeditor::NotifyToast(std::format("Failed to load the context scene {:016X}: {}", Context.m_Instance.m_Value, Err.getMessage()));
        State.m_bSelectRootPending = true;
        PlaceDocumentRoot(GameMgr, State);          // editing in context: the document is opened again (a Play rebuild, a game module reload): its root goes back to the instance's place
        return {};
    }

    // The root of a freshly opened prefab is the selection (the Inspector shows it, the tree row is lit, the gizmo is on it, and the camera turns to look at it: the viewport does that for every new selection),
    // unless the person already selected something of the prefab (a reopen after a Game.dll reload keeps it). Set directly, not through Select: it is no undo step and no change of the document. Waits until
    // the document is loaded. True when it is settled (selected, or nothing to do).
    inline bool SelectPrefabRootIfNone(xecs::game_mgr::instance& GameMgr, level_state& State) noexcept
    {
        if (!State.isPrefabEditor()) return true;
        auto&       Ecs    = xlioncore::Ecs(GameMgr);
        const auto* pScene = GameMgr.m_SceneMgr.Find(State.PrefabScene());
        if (!pScene || pScene->m_LocalToRuntime.empty()) return false;
        if (State.m_SelectedEntityId != xecs::scene::invalid_permanent_id_v && State.m_SelectedEntityScene == State.PrefabScene() && pScene->m_LocalToRuntime.contains(State.m_SelectedEntityId)) return true;
        if (State.m_bRootSelected) return true;                           // the prefab row itself is the selection: the person's
        for (const auto& [Id, Entity] : pScene->m_LocalToRuntime)
            if (!pScene->m_InstanceMembers.contains(Id) && !Ecs.ParentOf(Entity))
            {
                State.m_MultiSelectedEntityIds = { Id };
                State.m_MultiSelectOrder       = { Id };
                State.m_MultiSelectScene       = State.PrefabScene();
                State.m_SelectedEntityId       = Id;
                State.m_SelectedEntity         = Entity;
                State.m_SelectedEntityScene    = State.PrefabScene();
                State.m_bEntityInspectorDirty  = true;
                State.m_bRootSelected          = false;
                return true;
            }
        return false;
    }

    // Brings a scene in to test the prefab against: it is loaded (so it plays), and it is never picked, edited or saved, and it is no part of the prefab. "" or why not.
    inline std::string AddContextScene(xecs::game_mgr::instance& GameMgr, level_state& State, xecs::scene::guid Scene) noexcept
    {
        if (!State.isPrefabEditor())                                    return "this editor has no prefab open: context scenes are for a Prefab Editor";
        if (State.isPlaying())                                          return "not while playing";
        if (Scene.empty())                                              return "no scene given";
        if (Scene.m_Instance == State.m_CurrentPrefab.m_Instance)       return "the prefab is the document of this editor, not a context";
        if (std::find(State.m_ContextScenes.begin(), State.m_ContextScenes.end(), Scene) != State.m_ContextScenes.end())
            return std::format("the scene {:016X} is already a context scene of this editor", Scene.m_Instance.m_Value);
        if (auto Err = xlioncore::Ecs(GameMgr).RequestLoadScene(Scene); Err)
            return std::format("the scene could not be loaded: {}", Err.getMessage());
        State.m_ContextScenes.push_back(Scene);
        return {};
    }

    inline std::string RemoveContextScene(xecs::game_mgr::instance& GameMgr, level_state& State, xecs::scene::guid Scene) noexcept
    {
        if (State.isPlaying()) return "not while playing";
        const auto It = std::find(State.m_ContextScenes.begin(), State.m_ContextScenes.end(), Scene);
        if (It == State.m_ContextScenes.end()) return std::format("the scene {:016X} is not a context scene of this editor", Scene.m_Instance.m_Value);
        xlioncore::Ecs(GameMgr).ReleaseLoadScene(Scene);
        State.m_ContextScenes.erase(It);
        return {};
    }

    // ---- snapshots of the document ---------------------------------------------------------------------------------------------------------------------------------------------------

    // A new empty folder that this editor owns (until it closes: ReleasePrefabSnapshots), to hold a state of the document.
    inline std::wstring NewPrefabSnapshotFolder(level_state& State) noexcept
    {
        static std::atomic<std::uint32_t> s_Count{ 0 };
        std::error_code Ec;
        const auto Folder = (std::filesystem::temp_directory_path(Ec) / std::format(L"xlion_prefab_doc_{:X}_{}_{}", State.m_CurrentPrefab.m_Instance.m_Value, reinterpret_cast<std::uintptr_t>(&State), ++s_Count)).wstring();
        std::filesystem::remove_all(std::filesystem::path(Folder), Ec);
        std::filesystem::create_directories(std::filesystem::path(Folder), Ec);
        State.m_PrefabSnapshots.push_back(Folder);
        return Folder;
    }

    // A folder somebody else wrote (the saved state of the prefab, from another editor): this editor owns it from here.
    inline void AdoptPrefabSnapshot(level_state& State, const std::wstring& Folder) noexcept { State.m_PrefabSnapshots.push_back(Folder); }

    inline void ReleasePrefabSnapshots(level_state& State) noexcept
    {
        std::error_code Ec;
        for (const auto& Folder : State.m_PrefabSnapshots) std::filesystem::remove_all(std::filesystem::path(Folder), Ec);
        State.m_PrefabSnapshots.clear();
    }

    // A prefab as it was saved to a folder (Descriptor.txt, entity_db, ComponentDeps.txt) becomes the prefab's own folder: the entity files of the old state go, the others (info.txt) stay. False when it
    // could not be copied (the prefab's folder may then be half written: the caller says so).
    inline bool CopyPrefabFolder(const std::wstring& From, const std::wstring& To) noexcept
    {
        std::error_code Ec;
        if (!std::filesystem::exists(std::filesystem::path(From) / L"Descriptor.txt", Ec)) return false;
        std::filesystem::create_directories(std::filesystem::path(To), Ec);
        std::filesystem::remove_all(std::filesystem::path(To) / L"entity_db", Ec);
        std::filesystem::remove(std::filesystem::path(To) / L"Entity.txt", Ec);
        Ec.clear();
        std::filesystem::copy(std::filesystem::path(From), std::filesystem::path(To), std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, Ec);
        return !Ec;
    }

    // The document, as the prefab it would save, written to Folder (its own folder is not touched). Refused, like a save, when it breaks a rule of a prefab (one root, references inside).
    inline xerr WritePrefabDocumentTo(xecs::game_mgr::instance& GameMgr, const level_state& State, const std::wstring& Folder) noexcept
    {
        auto& Ecs   = xlioncore::Ecs(GameMgr);
        auto& Scene = Ecs.FindOrCreateScene(State.PrefabScene());
        Scene.m_FolderOverride = Folder;
        const auto Err = WithRootAsSaved(GameMgr, State, [&] { return Ecs.SaveScene(State.PrefabScene()); });
        Scene.m_FolderOverride.clear();
        return Err;
    }

    // The document becomes what Folder holds (a prefab, as WritePrefabDocumentTo wrote it): its entities are unloaded and loaded again, with the same ids. The selection is put back on its entity.
    inline xerr ReadPrefabDocumentFrom(xecs::game_mgr::instance& GameMgr, level_state& State, const std::wstring& Folder) noexcept
    {
        auto& Ecs   = xlioncore::Ecs(GameMgr);
        auto& Scene = Ecs.FindOrCreateScene(State.PrefabScene());
        Scene.m_FolderOverride = Folder;
        Ecs.ReleaseLoadScene(State.PrefabScene());
        const auto Err = Ecs.RequestLoadScene(State.PrefabScene());
        Scene.m_FolderOverride.clear();
        Scene.m_PendingChanges.clear();
        PlaceDocumentRoot(GameMgr, State);          // editing in context: the root of what was read is what a save writes; the root goes to the instance's place

        State.m_SelectedEntity = {};
        if (State.m_SelectedEntityId != xecs::scene::invalid_permanent_id_v && State.m_SelectedEntityScene == State.PrefabScene())
        {
            if (auto It = Scene.m_LocalToRuntime.find(State.m_SelectedEntityId); It != Scene.m_LocalToRuntime.end()) State.m_SelectedEntity = It->second;
            else                                                                                                     State.m_SelectedEntityId = xecs::scene::invalid_permanent_id_v;
        }
        std::erase_if(State.m_MultiSelectedEntityIds, [&](xecs::scene::permanent_id Id) noexcept { return State.m_MultiSelectScene == State.PrefabScene() && !Scene.m_LocalToRuntime.contains(Id); });
        std::erase_if(State.m_MultiSelectOrder,       [&](xecs::scene::permanent_id Id) noexcept { return State.m_MultiSelectScene == State.PrefabScene() && !Scene.m_LocalToRuntime.contains(Id); });
        State.m_bEntityInspectorDirty = true;
        return Err;
    }

    // Live update (prefabs_plan.md 3.6, phase 6): the instances of a prefab in an editor's world (it is theirs, or one they nest) are spawned again from it with their recipes - same ids,
    // overrides kept, nothing written, the editor not dirty (no undo step). A gizmo drag in progress ends first; the selection is found again by its id. Not while playing: a running game
    // keeps what it started with (Stop reopens the world from the files). bFromFile: the prefab changed on disk (the template is read again); otherwise the template in memory is the new one.
    inline int LiveUpdatePrefab(level_context& Ctx, xecs::prefab::guid Prefab, bool bFromFile) noexcept
    {
        auto& State = Ctx.State();
        if (State.isPlaying() || !Ctx.m_pWorld) return 0;
        if (Ctx.m_EndGizmoDrag) Ctx.m_EndGizmoDrag();
        const int n = xlioncore::Ecs(Ctx.World()).LiveUpdatePrefab(Prefab, bFromFile);
        xscene::ResolveSelection(Ctx.World(), State);
        return n;
    }

    // The prefab's file changed: every other editor brings its instances of it up to date (its own world is the caller's to update).
    inline void LiveUpdatePrefabElsewhere(const level_state* pExcept, xecs::prefab::guid Prefab) noexcept
    {
        for (auto* pOther : g_LevelContexts)
            if (&pOther->State() != pExcept) LiveUpdatePrefab(*pOther, Prefab, /*bFromFile*/ true);
    }

    // ---- the roles of the entities of an editing session (prefabs_plan.md 3.7, phase 7) -------------------------------------------------------------------------------------------------------

    // Which entities of this editor's world are context (the ones of the context scenes: drawn first and faded, never picked) and which are hidden (the instance being edited in context: its prefab, the
    // document, is drawn in its place), as the raw runtime values the render takes, sorted. Nothing while playing: a play session shows the game as it is. Built when asked (the entities of one frame's
    // draw): a context of tens of thousands of entities is a sort of that many numbers - the day that costs, the render DLL can be told the scenes instead.
    struct role_sets
    {
        std::vector<std::uint64_t> m_Context;
        std::vector<std::uint64_t> m_Hidden;

        xlionrender::roles Roles(const float* Fade, float Alpha) const noexcept
        {
            xlionrender::roles R;
            R.m_pContext = m_Context.data(); R.m_nContext = static_cast<int>(m_Context.size());
            R.m_pHidden  = m_Hidden.data();  R.m_nHidden  = static_cast<int>(m_Hidden.size());
            R.m_FadeAlpha = Alpha;
            for (int i = 0; i < 3; ++i) R.m_FadeColor[i] = Fade[i];
            return R;
        }
    };

    inline void BuildRoleSets(xecs::game_mgr::instance& GameMgr, const level_state& State, role_sets& Out) noexcept
    {
        Out.m_Context.clear();
        Out.m_Hidden.clear();
        if (State.m_ContextScenes.empty() || State.isPlaying()) return;

        for (const auto& Scene : State.m_ContextScenes)
            if (const auto* pScene = GameMgr.m_SceneMgr.Find(Scene))
                for (const auto& [Value, Id] : pScene->m_RuntimeToLocal) Out.m_Context.push_back(Value);
        std::sort(Out.m_Context.begin(), Out.m_Context.end());

        // The instance and everything under it (its members, and what was added under them in the scene).
        if (State.m_ContextEdit.m_bActive)
            if (const auto* pScene = GameMgr.m_SceneMgr.Find(State.m_ContextEdit.m_Scene))
                if (const auto It = pScene->m_LocalToRuntime.find(State.m_ContextEdit.m_Root); It != pScene->m_LocalToRuntime.end())
                {
                    auto&                                     Ecs = xlioncore::Ecs(GameMgr);
                    std::vector<xecs::component::entity>      Stack{ It->second };
                    while (!Stack.empty())
                    {
                        const auto Entity = Stack.back();
                        Stack.pop_back();
                        Out.m_Hidden.push_back(Entity.m_Value);
                        if (const auto* pChildren = Ecs.ChildrenOf(Entity)) Stack.insert(Stack.end(), pChildren->m_List.begin(), pChildren->m_List.end());
                    }
                    std::sort(Out.m_Hidden.begin(), Out.m_Hidden.end());
                }
    }

    // ---- Edit in Context (prefabs_plan.md, phase 7) ---------------------------------------------------------------------------------------------------------------------------------------

    // Doc is a Prefab Editor that has just been opened on the prefab of the instance (Root, in Scene) of Source's Level: the Level's scenes become its context scenes (as the Level has them saved), the
    // document's root goes where the instance is, and the instance is left out of the draw and the pick. "" or why not (the prefab editor stays open as a plain one in that case).
    inline std::string EnterContextEdit(level_context& Doc, level_context& Source, xecs::scene::guid Scene, xecs::scene::permanent_id Root, std::string& Note) noexcept
    {
        auto& PS = Doc.State();
        auto& SS = Source.State();
        if (!PS.isPrefabEditor())                                   return "the editor opened is not a Prefab Editor";
        if (PS.m_ContextEdit.m_bActive)                             return "that prefab is already open in context";
        if (SS.isPrefabEditor())                                    return "an instance inside a prefab is not edited in context yet (open the prefab it is in, and the inner prefab from its own instance)";
        if (SS.m_CurrentLevel.empty())                              return "the instance is not in a Level";

        const auto* pScene = Source.World().m_SceneMgr.Find(Scene);
        if (!pScene || !pScene->m_LocalToRuntime.contains(Root))   return "the instance was not found";
        auto& Ecs    = xlioncore::Ecs(Source.World());
        const auto Entity = pScene->m_LocalToRuntime.at(Root);
        auto* pT     = xlioncore::ComponentOf<xlioncore::transform>(Ecs, Entity);
        if (!pT)                                                    return "the instance has no Transform to place the prefab at";

        auto& Edit    = PS.m_ContextEdit;
        Edit.m_Level  = SS.m_CurrentLevel;
        Edit.m_Scene  = Scene;
        Edit.m_Root   = Root;
        Edit.m_Placed = xlioncore::WorldOf(*pT, Ecs.ParentOf(Entity));

        int nContext = 0;
        for (const auto& Open : SS.m_OpenScenes)
        {
            if (const auto* pOpen = Source.World().m_SceneMgr.Find(Open); pOpen && pOpen->m_bPrefabDocument) continue;
            if (const auto Why = AddContextScene(Doc.World(), PS, Open); !Why.empty()) Note += std::format("{}context scene {:016X}: {}", Note.empty() ? "" : "; ", Open.m_Instance.m_Value, Why);
            else ++nContext;
        }
        Edit.m_bActive = true;
        PlaceDocumentRoot(Doc.World(), PS);

        if (const auto It = pScene->m_PendingChanges.find(Root); It != pScene->m_PendingChanges.end() && It->second.m_New > 0)         // (the scene of the context is still loading here: ask the Level)
            Note += std::format("{}the instance is not in the saved scene yet: it is not hidden in the context (save the Level and open it again)", Note.empty() ? "" : "; ");
        if (nContext == 0) Note += std::format("{}the Level has no scene open", Note.empty() ? "" : "; ");

        PS.m_bEntityInspectorDirty = true;
        if (Doc.m_AimCamera) Doc.m_AimCamera(Edit.m_Placed.m_Position);
        return {};
    }
}

#endif // XLEVEL_PREFAB_DOCUMENT_H
