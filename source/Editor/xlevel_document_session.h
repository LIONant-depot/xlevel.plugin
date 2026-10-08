#ifndef XLEVEL_DOCUMENT_SESSION_H
#define XLEVEL_DOCUMENT_SESSION_H
#pragma once
#include "dependencies/xeditor/include/xeditor/popup.h"

// Level document session: dirty tracking (undo watermark), File>Save enable, Close, and
// save-before-open when double-clicking / dropping another Level. Included from the kit umbrella
// after SaveEverything / OpenLevel / CloseScene are declared.

namespace xlevel
{
    //---------------------------------------------------------------------------
    inline void MarkDocumentClean(level_state& State, xundo::system& Undo) noexcept
    {
        State.m_CleanUndoIndex = Undo.GetUndoIndex();
    }

    // A step that only moves the selection (a click in the tree or the viewport): it is an undo step, but it changes nothing of the document, so it never makes it "unsaved" (a Level whose
    // person only clicked an instance to Edit it Alone, closed with the Prefab Editor, must not ask whether to save).
    inline bool IsSelectionStep(std::string_view CommandString) noexcept
    {
        const auto Name = CommandString.substr(0, CommandString.find(' '));
        const auto Base = Name.substr(Name.find_last_of("\\/") == std::string_view::npos ? 0 : Name.find_last_of("\\/") + 1);
        return Base == "Select" || Base == "SelectLevel" || Base == "ToggleMultiSelect" || Base == "ClearSelection";
    }

    // The document has unsaved changes when a step other than a selection lies between the state it was saved in and the one it is in now.
    inline bool HasUnsavedDocumentChanges(const level_state& State, const xundo::system& Undo) noexcept
    {
        if (!State.HasDocument() && State.m_OpenScenes.empty()) return false;
        const int Now = Undo.GetUndoIndex(), Clean = State.m_CleanUndoIndex;
        if (Now == Clean) return false;
        if (Clean < 0 || static_cast<std::size_t>(Clean) > Undo.GetHistoryCount() || static_cast<std::size_t>(Now) > Undo.GetHistoryCount()) return true;       // (-1: never clean, see the prefab that could not be saved)
        for (int i = std::min(Now, Clean); i < std::max(Now, Clean); ++i)
            if (!IsSelectionStep(Undo.GetHistoryCommandString(static_cast<std::size_t>(i)))) return true;
        return false;
    }

    // Unload every open scene, clear CurrentLevel / selection, and Reset undo (old steps no longer
    // refer to a live document - see xundo::system::Reset).
    inline void CloseLevel(xecs::game_mgr::instance& GameMgr, level_state& State, xundo::system& Undo) noexcept
    {
        if (!State.HasDocument() && State.m_OpenScenes.empty())
        {
            Undo.Reset();
            State.m_CleanUndoIndex = 0;
            return;
        }

        // A Prefab Editor that closes without saving may have held a change of the prefab another editor handed it (Apply Overrides, one writer per prefab): that editor
        // still shows the change, the file does not have it. The others are brought back to the file (live update, prefabs_plan.md phase 6).
        const auto Discarded = State.isPrefabEditor() && HasUnsavedDocumentChanges(State, Undo) ? State.m_CurrentPrefab : xecs::prefab::guid{};

        const auto Scenes = State.m_OpenScenes;
        for (const auto SceneGuid : Scenes)
            xscene::CloseScene(GameMgr, State, SceneGuid);
        for (const auto SceneGuid : State.m_ContextScenes)         // a Prefab Editor's context scenes go with it
            xlioncore::Ecs(GameMgr).ReleaseLoadScene(SceneGuid);
        State.m_ContextScenes.clear();
        if (State.isPrefabEditor()) ErasePrefabGameOverride(State.m_CurrentPrefab.m_Instance.m_Value);
        if (State.isSceneEditor()) ErasePrefabGameOverride(State.m_CurrentScene.m_Instance.m_Value);        // (the Game a Scene Editor borrowed, GiveSceneAGameIfNone)        // (Edit in Context: the Game of the Level was this editor's only while it was open)
        State.m_ContextEdit = {};

        State.m_bLevelEditorOpen = false;
        State.m_CurrentLevel = {};
        State.m_CurrentPrefab = {};
        State.m_CurrentScene  = {};
        State.m_SelectedEntityId    = xecs::scene::invalid_permanent_id_v;
        State.m_SelectedEntity      = {};
        State.m_SelectedEntityScene = {};
        State.m_MultiSelectedEntityIds.clear();
        State.m_MultiSelectOrder.clear();
        State.m_MultiSelectScene = {};
        State.m_bRootSelected    = false;

        // CLI Close (or any immediate close) cancels a pending File>Close / open-other prompt.
        State.m_bAwaitingSaveBeforeClose = false;
        State.m_PendingOpenLevelAfterClose = {};
        State.m_bPendingOpenWantsGameReload = false;
        State.m_bPendingStartGameReloadAfterOpen = false;

        Undo.Reset();
        State.m_CleanUndoIndex = 0;

        if (!Discarded.empty()) LiveUpdatePrefabElsewhere(&State, Discarded);
    }

    // Applies Save/Don't Save resolution then Close and optional OpenLevel.
    inline void FinishPendingDocumentAction(xecs::game_mgr::instance& GameMgr, level_state& State, xundo::system& Undo, bool bSaveFirst) noexcept
    {
        if (bSaveFirst)
        {
            // A prefab that breaks a rule of a prefab is not written: the editor stays open and unsaved (nothing closes on a save that did not happen).
            if (!SaveEverything(GameMgr, State) && State.isPrefabEditor())
            {
                State.m_PendingOpenLevelAfterClose = {};
                State.m_bPendingOpenWantsGameReload = false;
                State.m_bPendingStartGameReloadAfterOpen = false;
                State.m_bLevelEditorOpen = true;
                xeditor::NotifyToast("The prefab was not saved (a prefab has one root, and references only its own entities): the editor stays open");
                return;
            }
            MarkDocumentClean(State, Undo);
        }

        const auto PendingOpen = State.m_PendingOpenLevelAfterClose;
        const bool bWantReload = State.m_bPendingOpenWantsGameReload;
        State.m_PendingOpenLevelAfterClose = {};
        State.m_bPendingOpenWantsGameReload = false;

        CloseLevel(GameMgr, State, Undo);

        if (!PendingOpen.empty() && PendingOpen.m_Type == xecs::level::type_guid_v)
        {
            OpenLevel(GameMgr, State, PendingOpen);
            MarkDocumentClean(State, Undo);
            // Caller / next-frame consumer starts GameReload when this stays true.
            State.m_bPendingStartGameReloadAfterOpen = bWantReload;
        }
        else
        {
            State.m_bPendingStartGameReloadAfterOpen = false;
        }
    }

    // Close current Level. If dirty, opens Save/Don't Save/Cancel modal.
    inline void RequestCloseLevel(xecs::game_mgr::instance& GameMgr, level_state& State, xundo::system& Undo) noexcept
    {
        if (State.isPlaying()) return;
        if (!State.HasDocument() && State.m_OpenScenes.empty()) return;
        if (State.m_bAwaitingSaveBeforeClose) return;

        State.m_PendingOpenLevelAfterClose = {};
        State.m_bPendingOpenWantsGameReload = false;
        State.m_bPendingStartGameReloadAfterOpen = false;

        if (!HasUnsavedDocumentChanges(State, Undo))
        {
            CloseLevel(GameMgr, State, Undo);
            return;
        }

        State.m_bAwaitingSaveBeforeClose = true;
    }

    // A Level is a resource like any other: opening one (a double-click, a drop, the OpenLevel command) gets it its own
    // editor, next to the ones already open. Whoever wants one open queues it here; the shell opens the editors at a clean
    // point of the frame (a new editor builds a whole world, which must not happen in the middle of drawing another).
    inline std::vector<xresource::full_guid> g_PendingOpenLevels;

    inline void QueueOpenLevel(xresource::full_guid LevelGuid) noexcept
    {
        if (LevelGuid.m_Type != xecs::level::type_guid_v && LevelGuid.m_Type != xecs::prefab::type_guid_v && LevelGuid.m_Type != xecs::scene::type_guid_v) return;      // a prefab opens in its own editor too (a Prefab Editor)
        if (std::find(g_PendingOpenLevels.begin(), g_PendingOpenLevels.end(), LevelGuid) == g_PendingOpenLevels.end())
            g_PendingOpenLevels.push_back(LevelGuid);
    }

    // Same OpenPopup-every-frame convention as RenderKeepTweaksModal / RenderErrorPopup.
    inline void RenderSaveBeforeCloseModal(xecs::game_mgr::instance& GameMgr, level_state& State, xundo::system& Undo, const ImVec2* pCenter = nullptr) noexcept
    {
        // One question per editor: the id carries the document's guid. Two editors asked at once (the dock's close button closes every tab of the window) must not share one popup, or the buttons of one
        // answer for the other.
        char Title[96];
        std::snprintf(Title, sizeof(Title), "Save changes?###LevelDocument%016llX", static_cast<unsigned long long>(State.isPrefabEditor() ? State.m_CurrentPrefab.m_Instance.m_Value : State.isSceneEditor() ? State.m_CurrentScene.m_Instance.m_Value : State.m_CurrentLevel.m_Instance.m_Value));
        if (State.m_bAwaitingSaveBeforeClose)
            ImGui::OpenPopup(Title);

        if (xeditor::BeginModal(Title, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings, pCenter))
        {
            const bool bOpeningOther = !State.m_PendingOpenLevelAfterClose.empty();
            ImGui::TextUnformatted(State.isPrefabEditor() ? "The prefab has unsaved changes."
                : State.isSceneEditor()                  ? "The scene has unsaved changes."
                : State.m_CurrentLevel.empty()           ? "The open scene(s) have unsaved changes."
                                                         : "The current Level has unsaved changes.");
            ImGui::TextWrapped(bOpeningOther
                ? "Save before opening the other Level?"
                : "Save before closing?");
            ImGui::Separator();

            if (ImGui::Button("Save", ImVec2(110.0f, 0.0f)))
            {
                State.m_bAwaitingSaveBeforeClose = false;
                FinishPendingDocumentAction(GameMgr, State, Undo, /*bSaveFirst*/ true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(110.0f, 0.0f)))
            {
                State.m_bAwaitingSaveBeforeClose = false;
                FinishPendingDocumentAction(GameMgr, State, Undo, /*bSaveFirst*/ false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110.0f, 0.0f)))
            {
                State.m_bAwaitingSaveBeforeClose = false;
                State.m_PendingOpenLevelAfterClose = {};
                State.m_bPendingOpenWantsGameReload = false;
                State.m_bPendingStartGameReloadAfterOpen = false;
                State.m_bLevelEditorOpen = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

} // namespace xlevel

#endif // XLEVEL_DOCUMENT_SESSION_H