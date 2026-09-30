#ifndef XLEVEL_DOCUMENT_SESSION_H
#define XLEVEL_DOCUMENT_SESSION_H
#pragma once

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

    inline bool HasUnsavedDocumentChanges(const level_state& State, const xundo::system& Undo) noexcept
    {
        if (State.m_CurrentLevel.empty() && State.m_OpenScenes.empty()) return false;
        return Undo.GetUndoIndex() != State.m_CleanUndoIndex;
    }

    // Unload every open scene, clear CurrentLevel / selection, and Reset undo (old steps no longer
    // refer to a live document - see xundo::system::Reset).
    inline void CloseLevel(xecs::game_mgr::instance& GameMgr, level_state& State, xundo::system& Undo) noexcept
    {
        if (State.m_CurrentLevel.empty() && State.m_OpenScenes.empty())
        {
            Undo.Reset();
            State.m_CleanUndoIndex = 0;
            return;
        }

        const auto Scenes = State.m_OpenScenes;
        for (const auto SceneGuid : Scenes)
            xscene::CloseScene(GameMgr, State, SceneGuid);

        State.m_bLevelEditorOpen = false;
        State.m_CurrentLevel = {};
        State.m_SelectedEntityId    = xecs::scene::invalid_permanent_id_v;
        State.m_SelectedEntity      = {};
        State.m_SelectedEntityScene = {};
        State.m_MultiSelectedEntityIds.clear();
        State.m_MultiSelectOrder.clear();
        State.m_MultiSelectScene = {};

        // CLI Close (or any immediate close) cancels a pending File>Close / open-other prompt.
        State.m_bAwaitingSaveBeforeClose = false;
        State.m_PendingOpenLevelAfterClose = {};
        State.m_bPendingOpenWantsGameReload = false;
        State.m_bPendingStartGameReloadAfterOpen = false;

        Undo.Reset();
        State.m_CleanUndoIndex = 0;
    }

    // Applies Save/Don't Save resolution then Close and optional OpenLevel.
    inline void FinishPendingDocumentAction(xecs::game_mgr::instance& GameMgr, level_state& State, xundo::system& Undo, bool bSaveFirst) noexcept
    {
        if (bSaveFirst)
        {
            SaveEverything(GameMgr, State);
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
        if (State.m_CurrentLevel.empty() && State.m_OpenScenes.empty()) return;
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
        if (LevelGuid.m_Type != xecs::level::type_guid_v) return;
        if (std::find(g_PendingOpenLevels.begin(), g_PendingOpenLevels.end(), LevelGuid) == g_PendingOpenLevels.end())
            g_PendingOpenLevels.push_back(LevelGuid);
    }

    // Same OpenPopup-every-frame convention as RenderKeepTweaksModal / RenderErrorPopup.
    inline void RenderSaveBeforeCloseModal(xecs::game_mgr::instance& GameMgr, level_state& State, xundo::system& Undo) noexcept
    {
        if (State.m_bAwaitingSaveBeforeClose)
            ImGui::OpenPopup("Save changes?##E29Document");

        if (ImGui::BeginPopupModal("Save changes?##E29Document", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool bOpeningOther = !State.m_PendingOpenLevelAfterClose.empty();
            ImGui::TextUnformatted(State.m_CurrentLevel.empty()
                ? "The open scene(s) have unsaved changes."
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