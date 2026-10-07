#ifndef XLEVEL_PREFAB_DOCUMENT_H
#define XLEVEL_PREFAB_DOCUMENT_H
#pragma once

// The Prefab Editor's document (documentation/Editors/prefabs_plan.md, phase 5). A prefab is stored as a scene, so the editor of a prefab is the Level editor with a prefab in place of
// the Level: the prefab is open as a scene of its own guid (xecs::scene::instance::m_bPrefabDocument, which makes the scene manager read and write the prefab's folder, as a prefab), in
// State.m_OpenScenes like any scene - selection, the scene commands, the inspector, the gizmos, undo and Save are the ones of a scene. What this file adds is opening it, the scenes
// brought in to test against (context scenes), and the snapshots of the document that one writer per prefab needs.
#include <atomic>
#include <filesystem>

namespace xlevel
{
    inline xresource::full_guid PrefabResourceGuid(const level_state& State) noexcept
    {
        return xresource::full_guid{ State.m_CurrentPrefab.m_Instance, xecs::prefab::type_guid_v };
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
        return {};
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
        const auto Err = Ecs.SaveScene(State.PrefabScene());
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
}

#endif // XLEVEL_PREFAB_DOCUMENT_H
