#ifndef XLEVEL_COMMANDS_TEXT_H
#define XLEVEL_COMMANDS_TEXT_H
#pragma once

// DescribeText: the layout of the Text component of an entity (lines, glyphs, the box of the text), as the render module of the Level computes it - what the person sees, said in words, so a
// script (and a test) can check a label without a screenshot. The layout is the renderer's: this command only asks.
#include <cstdio>
#include <cstdlib>
#include "plugins/xlevel.plugin/source/Editor/xlevel_command_context.h"
#include "plugins/xscene.plugin/source/Editor/xscene_command_context.h"

namespace xlevel::commands
{
    struct describe_text_query_cmd : level_query_command
    {
        describe_text_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "DescribeText", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Says how the Text of an entity is laid out: Lines, Quads (glyphs with ink), Missing (characters the font has no glyph for) and Bounds (minX,minY,maxX,maxY of the text in em units, origin at the anchor). "
                   "Says why when there is no layout yet (no font, not compiled). Usage: DescribeText -Scene hexguid -Id hexid";
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
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg)) return "DescribeText: bad arguments";

            auto& Ctx = LevelContext();
            if (!Ctx.m_DescribeText) return "DescribeText: this Level has no render module";
            auto* pScene = World().m_SceneMgr.Find(xscene::commands::ParseSceneGuid(std::get<std::string>(SceneArg)));
            if (!pScene) return "DescribeText: scene not found";
            const auto It = pScene->m_LocalToRuntime.find(xscene::commands::ParseEntityId(std::get<std::string>(IdArg)));
            if (It == pScene->m_LocalToRuntime.end()) return "DescribeText: entity not found";
            return Ctx.m_DescribeText(It->second.m_Value);
        }
        xcmdline::parser::handle m_hScene, m_hId;
    };

    // DescribeTextDraw: what the renderer did with the Texts the last time it drew: how many labels and glyphs, in how many draw calls (one per font and depth mode, however many labels).
    struct describe_text_draw_query_cmd : level_query_command
    {
        describe_text_draw_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "DescribeTextDraw", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Says what the last draw of the Text components did: Labels and Glyphs sent, Draws (draw calls: one per font and depth mode, however many labels share them) and Dropped (labels left out because the "
                   "batch was full). Everything is 0 while no Text is in the Level or nothing has been drawn. Usage: DescribeTextDraw";
        }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto& Ctx = LevelContext();
            return Ctx.m_DescribeTextDraw ? Ctx.m_DescribeTextDraw() : "DescribeTextDraw: this Level has no render module";
        }
    };

    // DescribeScene: what the last forward render of the Level had (the Mesh, light and Environment components through xlionrender::instance)
    struct describe_scene_query_cmd : level_query_command
    {
        describe_scene_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "DescribeScene", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Says what the last render of the Level's Meshes and lights had: Meshes (entities with a Mesh), Drawn, Waiting (their GeomStatic not loaded yet), ConstantSlots (material slots drawn with the "
                   "constant material: their material instance is not loaded or does not resolve), Materials (made so far), Sun, Lights (points and spots), DrawsRendered (after culling). Usage: DescribeScene";
        }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto& Ctx = LevelContext();
            return Ctx.m_DescribeScene ? Ctx.m_DescribeScene() : "DescribeScene: this Level has no render module";
        }
    };

    // DumpGI: the GI's surfels of the last forward render to a CSV file (the GI baseline: the solver's light and the shown light per surfel)
    struct dump_gi_query_cmd : level_query_command
    {
        dump_gi_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "DumpGI", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Writes the GI's surfels of the last render of the Level to a CSV file: world position, normal, radius, the solver's light (unfiltered), the shown light (filtered), "
                   "validity, captured albedo; the GI setup on the first line. Says how many surfels. Also writes the GI's frame log (the last frames' stage times, counts and "
                   "memory: documentation/gi_dynamic_master_plan.md, Phase 0) to path.frames.csv. Usage: DumpGI -File path";
        }
        void RegisterArguments() noexcept override { m_hFile = m_Parser.addOption("File", "The CSV file to write", true, 1); }
        std::string Query() noexcept override
        {
            auto FileArg = m_Parser.getOptionArgAs<std::string>(m_hFile, 0);
            if (std::holds_alternative<xerr>(FileArg)) return "DumpGI: bad arguments";
            auto& Ctx = LevelContext();
            return Ctx.m_DumpGI ? Ctx.m_DumpGI(std::get<std::string>(FileArg)) : "DumpGI: this Level has no render module";
        }
        xcmdline::parser::handle m_hFile;
    };

    // PickRay: what a click in the viewport would select along a ray, so a script can ask "is this label (or primitive) where I think it is" without a mouse.
    struct pick_ray_query_cmd : level_query_command
    {
        pick_ray_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "PickRay", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Says which entity the ray picks (the closest one the viewport would select on a click): its Scene and Id, or 'nothing'. The ray is not capped by the ground. Usage: PickRay -Origin x,y,z -Dir x,y,z";
        }
        void RegisterArguments() noexcept override
        {
            m_hOrigin = m_Parser.addOption("Origin", "Where the ray starts: x,y,z in world units", true, 1);
            m_hDir    = m_Parser.addOption("Dir",    "The direction of the ray: x,y,z",            true, 1);
        }
        std::string Query() noexcept override
        {
            auto OriginArg = m_Parser.getOptionArgAs<std::string>(m_hOrigin, 0);
            auto DirArg    = m_Parser.getOptionArgAs<std::string>(m_hDir, 0);
            if (std::holds_alternative<xerr>(OriginArg) || std::holds_alternative<xerr>(DirArg)) return "PickRay: bad arguments";
            float O[3], D[3];
            // "x,y,z" -> 3 floats (strtof: what sscanf's %f does, without MSVC's C4996)
            const auto Parse3 = [](const std::string& S, float (&V)[3]) noexcept
            {
                const char* p = S.c_str();
                for (int i = 0; i < 3; ++i)
                {
                    char* pEnd = nullptr;
                    V[i] = std::strtof(p, &pEnd);
                    if (pEnd == p) return false;
                    p = pEnd;
                    if (i < 2) { if (*p != ',') return false; ++p; }
                }
                return true;
            };
            if (!Parse3(std::get<std::string>(OriginArg), O) || !Parse3(std::get<std::string>(DirArg), D))
                return "PickRay: Origin and Dir are x,y,z";
            auto& Ctx = LevelContext();
            return Ctx.m_PickRay ? Ctx.m_PickRay(xmath::fvec3(O[0], O[1], O[2]), xmath::fvec3(D[0], D[1], D[2])) : "PickRay: this Level has no render module";
        }
        xcmdline::parser::handle m_hOrigin, m_hDir;
    };
}

#endif // XLEVEL_COMMANDS_TEXT_H
