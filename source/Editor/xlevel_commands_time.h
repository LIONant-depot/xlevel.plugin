#ifndef XLEVEL_COMMANDS_TIME_H
#define XLEVEL_COMMANDS_TIME_H
#pragma once

// The time of the game of a Level editor as commands (Name\SetTimeScale, Name\GetTimeScale): the speed slider next to Play, for a script.
// View state like the slider itself: not undoable, never dirties the Level.
#include "dependencies/xeditor/include/xeditor/commands.h"
#include "source/Tools/Editor/xeditor_document_editor.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_context.h"

#include <charconv>

namespace xlevel::commands
{
    struct set_time_scale_cmd : xundo::query_command_base
    {
        level_context* m_pEd;
        set_time_scale_cmd(xundo::system& System, level_context* pEd) noexcept : query_command_base(System, "SetTimeScale", nullptr), m_pEd(pEd) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Sets the speed of the game, the multiplier of its time: one of the stops of the slider next to Play (0.25 0.5 0.75 1 1.5 2 3). Usage: SetTimeScale -Scale 1.5"; }
        void RegisterArguments() noexcept override { m_hScale = m_Parser.addOption("Scale", "The multiplier: 0.25 0.5 0.75 1 1.5 2 3", true, 1); }
        std::string Query() noexcept override
        {
            if (!m_pEd || !m_pEd->m_pGame) return "SetTimeScale: this Level has no game";
            std::string Text; float Scale = 1.0f;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hScale, Text)) return "SetTimeScale: -Scale is required";
            const auto R = std::from_chars(Text.data(), Text.data() + Text.size(), Scale);
            const float Step = xlioncore::game_time::NearestScaleStep(Scale);
            if (R.ec != std::errc() || R.ptr != Text.data() + Text.size() || std::fabs(Step - Scale) > 0.001f)
                return "SetTimeScale: Scale is one of the stops of the slider: 0.25 0.5 0.75 1 1.5 2 3";
            m_pEd->m_pGame->m_Time.m_TimeScale = Step;
            return "SetTimeScale: done";
        }
        xcmdline::parser::handle m_hScale;
    };

    struct get_time_scale_cmd : xundo::query_command_base
    {
        level_context* m_pEd;
        get_time_scale_cmd(xundo::system& System, level_context* pEd) noexcept : query_command_base(System, "GetTimeScale", nullptr), m_pEd(pEd) {}
        const char* getCommandHelp() const noexcept override { return "The time of the game: multiplier, game time, frames and fixed steps computed. Usage: GetTimeScale"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            if (!m_pEd || !m_pEd->m_pGame) return "GetTimeScale: this Level has no game";
            const auto& T = m_pEd->m_pGame->m_Time;
            return std::format("scale: {:.3f}\npaused: {}\ntime: {:.3f}\nfixed time: {:.3f}\nframes computed: {}\nfixed steps computed: {}\n"
                , T.m_TimeScale, T.m_bPaused ? "true" : "false", T.m_Time, T.m_FixedTime, T.m_FramesComputed, T.m_FixedStepsComputed);
        }
    };
}

#endif // XLEVEL_COMMANDS_TIME_H
