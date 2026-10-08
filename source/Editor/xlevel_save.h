#pragma once
#include "plugins/xlevel.plugin/source/Editor/xlevel_world_check.h"

// Saving the open level, its scenes and the library.
// Split out of xlevel_editor.h; included from there at the position this code used to occupy.
namespace xlevel
{
    // The single "Save" action - persists everything currently open (the Level's descriptor, the
    // open Scene's entities + its descriptor) plus the underlying project/library metadata, rather
    // than requiring separate Save Level/Save Scene actions the user has to remember to hit.
    // ---- Scene write locks between Level editors -------------------------------------------------------------------------
    // Several open Levels can share a Scene. The first editor to edit it owns it (a host write lock, taken by
    // TryGateLevelMutation); in every other editor that Scene is read-only until the owner saves (or undoes back to clean,
    // or closes) and the lock is released.

    // The host session (the open editor's borrowed document/undo) whose undo system this is.
    inline xeditor::session* FindHostSession(xundo::system& Undo) noexcept
    {
        if (auto* pHost = xeditor::host::current())
            for (auto& S : pHost->m_Sessions)
                if (S && &S->undo() == &Undo) return S.get();
        return nullptr;
    }

    inline level_context* FindContextOf(const level_state& State) noexcept
    {
        for (auto* p : g_LevelContexts) if (&p->State() == &State) return p;
        return nullptr;
    }

    inline xresource::full_guid SceneResourceGuid(xecs::scene::guid Scene) noexcept
    {
        return xresource::full_guid{ Scene.m_Instance, xecs::scene::type_guid_v };
    }

    // True when another Level editor owns (is editing) this Scene - it is read-only here.
    inline bool IsSceneLockedByOther(level_context& Ed, xecs::scene::guid Scene) noexcept
    {
        auto* pHost = xeditor::host::current();
        if (pHost == nullptr || Scene.empty()) return false;
        return !pHost->can_write(SceneResourceGuid(Scene), FindHostSession(Ed.m_Undo));
    }

    // The name of the Level editor that owns a Scene ("" when nobody does).
    inline std::string SceneOwnerName(xecs::scene::guid Scene) noexcept
    {
        auto* pHost = xeditor::host::current();
        auto* pWriter = pHost ? pHost->writer_for(SceneResourceGuid(Scene)) : nullptr;
        return pWriter ? pWriter->display_name() : std::string{};
    }

    inline bool HasUnsavedDocumentChanges(const level_state& State, const xundo::system& Undo) noexcept;        // xlevel_document_session.h

    // "" when every open scene of this editor (the prefab of a Prefab Editor, the Scene of a Scene Editor, the scenes of a Level) loaded whole; otherwise why the document is not written. A scene
    // some of whose entities could not be loaded (a component of theirs is not registered: its game module is not in the Game the editor works under) would be written lossy: the entities that
    // loaded lost their references to the others at load, and a prefab is written whole from what loaded. Such a document is never written (xecs::scene::mgr::SaveScene refuses it too).
    inline std::string LossyDocumentProblem(xecs::game_mgr::instance& GameMgr, const level_state& State) noexcept
    {
        for (const auto& SceneGuid : State.m_OpenScenes)
            if (const auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid); pScene && !pScene->m_UnloadedEntities.empty())
                return std::format("{} {:016X} was not saved: {} of its entities could not be loaded (a component of theirs is not registered: the game module that defines it is not in the Game this editor works under), "
                                   "so nothing of it was written and its files are as they were. Give it a Game that lists that module, then open it again"
                    , State.isPrefabEditor() && SceneGuid == State.PrefabScene() ? "The prefab" : "The scene", SceneGuid.m_Instance.m_Value, pScene->m_UnloadedEntities.size());
        return {};
    }

    // The Level's Save. Saves the Level and every open Scene this editor may write - a Scene another Level editor owns is left alone (its
    // edits are that editor's to save). The other editors that have one of the Scenes this editor owned open reload it.
    // Returns false when something that had to be saved was not (the toast says what). A document that did not load whole is not written at all (LossyDocumentProblem), and
    // a prefab document with no unsaved change is not written either (it is the file: the whole-prefab rewrite would only reformat it).
    bool SaveEverything(xecs::game_mgr::instance& GameMgr, level_state& State) noexcept
    {
        bool              bAllSaved = true;
        auto*             pHost = xeditor::host::current();
        auto*             pEd   = FindContextOf(State);
        xeditor::session* pMe   = pEd ? FindHostSession(pEd->m_Undo) : nullptr;
        std::vector<xecs::scene::guid> Owned;

        if (const auto Why = LossyDocumentProblem(GameMgr, State); !Why.empty())
        {
            xeditor::NotifyToast(Why);
            return false;
        }
        const bool bPrefabUnchanged = State.isPrefabEditor() && pEd && !HasUnsavedDocumentChanges(State, pEd->m_Undo);

        if (!State.m_CurrentLevel.empty() && GameMgr.m_LevelMgr.Find(State.m_CurrentLevel))
        {
            if (auto Err = xlioncore::Ecs(GameMgr).SaveLevel(State.m_CurrentLevel); Err)
            {
                bAllSaved = false;
                xeditor::NotifyToast(std::format("Failed to save Level: {}", Err.getMessage()));
            }
        }

        // The system registry of the Level's game (what is placed where, the order, what is enabled) is part of the Level: its edits are in the undo history and are written with it. (Not while
        // playing: what a play session changes is reverted on Stop.)
        if (!State.isPlaying())
            if (auto Err = GameMgr.m_SystemMgr.Save(); Err)
                xeditor::NotifyToast(std::format("Failed to save System Registry order: {}", Err.getMessage()));

        for (auto& SceneGuid : State.m_OpenScenes)
        {
            if (pEd && IsSceneLockedByOther(*pEd, SceneGuid)) continue;     // another editor owns it
            if (bPrefabUnchanged && SceneGuid == State.PrefabScene()) continue;  // the prefab is what its file holds: nothing to write (Play of a clean prefab, a Save with no change)
            if (pHost && pMe && pHost->writer_for(SceneResourceGuid(SceneGuid)) == pMe) Owned.push_back(SceneGuid);

            // Plain console log, NOT xeditor::NotifyError() - this is routine save progress (every normal save
            // has SOME pending changes, that's the whole point of saving), not a failure. Routing it
            // through xeditor::NotifyError() before this exact distinction existed meant an ordinary Save popped
            // an "Error" modal every time.
            if (auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid))
            {
                std::printf("[SaveEverything] scene has %zu pending entity change(s)\n", pScene->m_PendingChanges.size());
                std::fflush(stdout);
            }
            // A scene out of step with its world would be saved from entities that are not there: refuse, and say why (the file on disk stays as it was).
            if (auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid))
                if (const auto Unknown = CountUnknownEntities(GameMgr, *pScene); Unknown)
                {
                    xeditor::NotifyToast(std::format("Scene {:016X} was not saved: {} of its entities are not in the world (the scene is out of step with it). Reopen the Level.", SceneGuid.m_Instance.m_Value, Unknown));
                    bAllSaved = false;
                    continue;
                }
            // Editing in context: the document's root is where the instance is, the prefab's own root is what is written (WithRootAsSaved).
            if (auto Err = WithRootAsSaved(GameMgr, State, [&] { return xlioncore::Ecs(GameMgr).SaveScene(SceneGuid); }); Err)
            {
                bAllSaved = false;
                xeditor::NotifyToast(std::format("{}: {}", State.isPrefabEditor() && SceneGuid == State.PrefabScene() ? "Failed to save the Prefab" : "Failed to save Scene", Err.getMessage()));
            }
        }

        // A prefab saved from its editor changed on disk: the other editors bring their instances of it up to date (live update, prefabs_plan.md phase 6). This editor's own world (its
        // context scenes) was brought up to date by the save itself.
        if (State.isPrefabEditor() && bAllSaved && !bPrefabUnchanged)
            LiveUpdatePrefabElsewhere(&State, State.m_CurrentPrefab);

        // Save is local: the Level and its Scenes. The renames and moves of the resource view are its own (its Save button), and Save All saves everything.

        for (auto* pOther : g_LevelContexts)
        {
            if (pOther == pEd) continue;
            auto& OtherState = pOther->State();
            for (auto& SceneGuid : Owned)
                if (std::find(OtherState.m_OpenScenes.begin(), OtherState.m_OpenScenes.end(), SceneGuid) != OtherState.m_OpenScenes.end()
                    && std::find(OtherState.m_ScenesToReload.begin(), OtherState.m_ScenesToReload.end(), SceneGuid) == OtherState.m_ScenesToReload.end())
                    OtherState.m_ScenesToReload.push_back(SceneGuid);
        }
        return bAllSaved;
    }
}
