#ifndef XLEVEL_WORLD_CHECK_H
#define XLEVEL_WORLD_CHECK_H
#pragma once

// Is a scene in step with the world it lives in? A scene keeps a map from its entities to the runtime entities of the world that made them. When a world is
// rebuilt under a scene (a Game.dll reload whose snapshot did not bring the entities back) the map outlives them: it names entities the new world never
// created. Reading them is an assert in the Debug build and memory that is not there in the Release one, so everything that walks a scene's entities asks first.
// (Included after xecs's game manager and scene headers, by the files that walk scenes.)

#include "dependencies/xLIONCore/src/game/xlioncore_editor.h"

namespace xlevel
{
    // How many of the scene's entities the world does not know.
    inline std::size_t CountUnknownEntities(xecs::game_mgr::instance& GameMgr, const xecs::scene::instance& Scene) noexcept
    {
        auto& Ecs = xlioncore::Ecs(GameMgr);                      // the world is asked through the interface of its core: the editor reads no pool of its own (ecs_gate.py)
        std::size_t Unknown = 0;
        for (const auto& Pair : Scene.m_LocalToRuntime)
            if (!Ecs.IsAlive(Pair.second)) ++Unknown;
        return Unknown;
    }

    // The same over the scenes a level has open.
    template<class T_SCENE_GUIDS>
    inline std::size_t CountUnknownOpenEntities(xecs::game_mgr::instance& GameMgr, const T_SCENE_GUIDS& OpenScenes) noexcept
    {
        std::size_t Unknown = 0;
        for (const auto& SceneGuid : OpenScenes)
            if (auto* pScene = GameMgr.m_SceneMgr.Find(SceneGuid)) Unknown += CountUnknownEntities(GameMgr, *pScene);
        return Unknown;
    }
}

#endif // XLEVEL_WORLD_CHECK_H
