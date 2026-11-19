#ifndef XLEVEL_COMMANDS_PLAY_TOOLS_H
#define XLEVEL_COMMANDS_PLAY_TOOLS_H
#pragma once

// The Play tools as commands (Tomas 2026-11-19; the viewport's T key and left click, xlevel_session.h): Name\FireBox fires the cannon's box
// along a ray, Name\PlayTool picks the tool the viewport's left button is (Game, Cannon, Hand). Play state, not undoable, never dirties the Level.
#include "dependencies/xeditor/include/xeditor/commands.h"
#include "source/Tools/Editor/xeditor_document_editor.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_context.h"

#include <cstdio>

namespace xlevel::commands
{
    struct fire_box_cmd : xundo::query_command_base
    {
        level_context* m_pEd;
        fire_box_cmd(xundo::system& System, level_context* pEd) noexcept : query_command_base(System, "FireBox", nullptr), m_pEd(pEd) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "In Play: the cannon's box (0.3 m, 2 kg, 12 m/s) from From along Dir, as the Cannon tool's left click fires it at the mouse. Usage: FireBox -From 0,1.5,-3 -Dir 0,0,1"; }
        void RegisterArguments() noexcept override
        {
            m_hFrom = m_Parser.addOption("From", "x,y,z: where the box starts (metres, world)", true, 1);
            m_hDir  = m_Parser.addOption("Dir",  "x,y,z: where it flies (any length)", true, 1);
        }
        std::string Query() noexcept override
        {
            if (!m_pEd || !m_pEd->m_FireBox) return "FireBox: this Level has no viewport";
            std::string F, D; xmath::fvec3 From, Dir;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hFrom, F) || !xeditor::cmd_util::GetArg(m_Parser, m_hDir, D)) return "FireBox: -From and -Dir are required";
            if (std::sscanf(F.c_str(), "%f,%f,%f", &From.m_X, &From.m_Y, &From.m_Z) != 3 || std::sscanf(D.c_str(), "%f,%f,%f", &Dir.m_X, &Dir.m_Y, &Dir.m_Z) != 3) return "FireBox: -From and -Dir are x,y,z";
            return m_pEd->m_FireBox(From, Dir);
        }
        xcmdline::parser::handle m_hFrom, m_hDir;
    };

    struct hand_cmd : xundo::query_command_base
    {
        level_context* m_pEd;
        hand_cmd(xundo::system& System, level_context* pEd) noexcept : query_command_base(System, "Hand", nullptr), m_pEd(pEd) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "In Play: the hand (the H key held over the viewport) as a command - grab the body a ray hits, pull it toward a point (a spring, stays until Release), let go. Usage: Hand -Grab 0,1.5,-3 -Dir 0,0,1 / Hand -Pull 0,1.2,-1 / Hand -Release 1"; }
        void RegisterArguments() noexcept override
        {
            m_hGrab    = m_Parser.addOption("Grab",    "x,y,z: the ray's start (metres, world); with -Dir", false, 1);
            m_hDir     = m_Parser.addOption("Dir",     "x,y,z: the ray's direction (any length)", false, 1);
            m_hPull    = m_Parser.addOption("Pull",    "x,y,z: where the held point is pulled", false, 1);
            m_hRelease = m_Parser.addOption("Release", "1 = let go of what is held (a flag needs a value to be seen)", false, 1);
        }
        std::string Query() noexcept override
        {
            if (!m_pEd || !m_pEd->m_Hand) return "Hand: this Level has no viewport";
            auto V = [&](xcmdline::parser::handle H, xmath::fvec3& Out) { std::string S; return xeditor::cmd_util::GetArg(m_Parser, H, S) && std::sscanf(S.c_str(), "%f,%f,%f", &Out.m_X, &Out.m_Y, &Out.m_Z) == 3; };
            xmath::fvec3 A(0, 0, 0), B(0, 0, 0);
            if (m_Parser.hasOption(m_hRelease)) return m_pEd->m_Hand("Release", A, B);
            if (V(m_hPull, A)) return m_pEd->m_Hand("Pull", A, B);
            if (V(m_hGrab, A) && V(m_hDir, B)) return m_pEd->m_Hand("Grab", A, B);
            return "Hand: -Grab x,y,z -Dir x,y,z, -Pull x,y,z or -Release 1";
        }
        xcmdline::parser::handle m_hGrab, m_hDir, m_hPull, m_hRelease;
    };

    struct play_tool_cmd : xundo::query_command_base
    {
        level_context* m_pEd;
        play_tool_cmd(xundo::system& System, level_context* pEd) noexcept : query_command_base(System, "PlayTool", nullptr), m_pEd(pEd) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "The tool the viewport's left button is in Play (the T key cycles them; the viewport's HUD line says it): Game (clicks select), Cannon (a click fires a box at the mouse), Hand (a drag pulls a body). Usage: PlayTool -Mode Cannon"; }
        void RegisterArguments() noexcept override { m_hMode = m_Parser.addOption("Mode", "Game, Cannon or Hand", true, 1); }
        std::string Query() noexcept override
        {
            if (!m_pEd || !m_pEd->m_SetPlayTool) return "PlayTool: this Level has no viewport";
            std::string M; if (!xeditor::cmd_util::GetArg(m_Parser, m_hMode, M)) return "PlayTool: -Mode is required";
            return m_pEd->m_SetPlayTool(M);
        }
        xcmdline::parser::handle m_hMode;
    };
}

#endif // XLEVEL_COMMANDS_PLAY_TOOLS_H
