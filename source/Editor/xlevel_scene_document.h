#ifndef XLEVEL_SCENE_DOCUMENT_H
#define XLEVEL_SCENE_DOCUMENT_H
#pragma once

// The Scene Editor (documentation/Editors/prefabs_plan.md, "Scene Editor"): a Scene opened by itself, in an editor of its own, is the document of a Level editor session (as a prefab is the document of a Prefab
// Editor). The scene is the one entry of State.m_OpenScenes, the Level and prefab fields stay empty, the commands of the scene editor reach it as Name\Command, Save / Save All / undo / Play work as for a Level.
// Included after xlevel_prefab_document.h.

namespace xlevel
{
    // Opens a Scene as this editor's document: loaded from its folder, in m_OpenScenes. "" or why not.
    inline std::string OpenSceneDocument(xecs::game_mgr::instance& GameMgr, level_state& State, xresource::full_guid Scene) noexcept
    {
        const xecs::scene::guid Guid{ .m_Instance = Scene.m_Instance };
        if (auto Err = xlioncore::Ecs(GameMgr).RequestLoadScene(Guid); Err)
            return std::string(Err.getMessage());
        State.m_CurrentScene     = Guid;
        State.m_bLevelEditorOpen = true;
        if (std::find(State.m_OpenScenes.begin(), State.m_OpenScenes.end(), Guid) == State.m_OpenScenes.end())
            State.m_OpenScenes.push_back(Guid);
        return {};
    }

    // The editor (a Level's, or another Scene Editor's) that has this Scene open already: one writer per Scene, a second copy is never opened. Null when no editor has it.
    inline level_context* EditorHoldingScene(xecs::scene::guid Scene, const level_context* pExcept = nullptr) noexcept
    {
        for (auto* pContext : g_LevelContexts)
            if (pContext != pExcept && std::find(pContext->State().m_OpenScenes.begin(), pContext->State().m_OpenScenes.end(), Scene) != pContext->State().m_OpenScenes.end()) return pContext;
        return nullptr;
    }
}

#endif // XLEVEL_SCENE_DOCUMENT_H
