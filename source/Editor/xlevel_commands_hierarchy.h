#ifndef XLEVEL_COMMANDS_HIERARCHY_H
#define XLEVEL_COMMANDS_HIERARCHY_H
#pragma once

// GetWorldPose: where an entity is in the WORLD. A root's Transform is its world pose; a child's Transform is relative to its parent, and its world pose is what the transform system
// derived (the parent component, see xlioncore_hierarchy.h). This says which it is and the pose, so a script (and a test) can check a hierarchy without a screenshot.
#include <cstdio>
#include "dependencies/xLIONCore/src/transform/xlioncore_hierarchy.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_command_context.h"
#include "plugins/xscene.plugin/source/Editor/xscene_command_context.h"

namespace xlevel::commands
{
    struct get_world_pose_query_cmd : level_query_command
    {
        get_world_pose_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "GetWorldPose", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Says where an entity is in the world: Child=1 when it has a parent (its Transform is relative to the parent, the world pose is derived every frame by the render module, so it "
                   "is the pose of the last frame drawn), Child=0 for a root (its Transform is the world pose); Position, Rotation (x,y,z,w) and Scale. Usage: GetWorldPose -Scene hexguid -Id hexid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits",         true, 1);
            m_hId    = m_Parser.addOption("Id",    "Entity permanent_id, 8 or 16 hex digits", true, 1);
        }
        std::string Query() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg)) return "GetWorldPose: bad arguments";

            auto* pScene = World().m_SceneMgr.Find(xscene::commands::ParseSceneGuid(std::get<std::string>(SceneArg)));
            if (!pScene) return "GetWorldPose: scene not found";
            const auto It = pScene->m_LocalToRuntime.find(xscene::commands::ParseEntityId(std::get<std::string>(IdArg)));
            if (It == pScene->m_LocalToRuntime.end()) return "GetWorldPose: entity not found";

            auto& Ecs = xlioncore::Ecs(World());
            const xecs::component::entity Entity{ It->second.m_Value };
            const xecs::component::type::info* pInfo = nullptr;
            auto* pTransform = static_cast<const xlioncore::transform*>(Ecs.ResolveComponent(Entity, xlioncore::transform::typedef_v.m_Guid, pInfo));
            if (!pTransform) return "GetWorldPose: the entity has no Transform";

            const auto* pParent = Ecs.ParentOf(Entity);
            const auto  W       = xlioncore::WorldOf(*pTransform, pParent);
            char Text[320];
            std::snprintf(Text, sizeof(Text), "GetWorldPose: ok\nChild=%d\nPosition=%.4f,%.4f,%.4f\nRotation=%.4f,%.4f,%.4f,%.4f\nScale=%.4f,%.4f,%.4f"
                , pParent ? 1 : 0, W.m_Position.m_X, W.m_Position.m_Y, W.m_Position.m_Z, W.m_Rotation.m_X, W.m_Rotation.m_Y, W.m_Rotation.m_Z, W.m_Rotation.m_W, W.m_Scale.m_X, W.m_Scale.m_Y, W.m_Scale.m_Z);
            return Text;
        }
        xcmdline::parser::handle m_hScene, m_hId;
    };
}

#endif // XLEVEL_COMMANDS_HIERARCHY_H
