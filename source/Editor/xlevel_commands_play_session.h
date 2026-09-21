#ifndef XLEVEL_COMMANDS_PLAYSESSION_H
#define XLEVEL_COMMANDS_PLAYSESSION_H
#pragma once

// Play/Pause/Stop/GetPlayState - added mid-session, direct user observation while watching this
// session drive the app via CLI: "I see you are using the mouse for play/pause/undo.... I think
// those should be query commands too... makes your life easier" (plus a follow-up: "you may also
// want to add a query to know what the current state is"). Every field these touch
// (State.m_PlayState/m_bPlayRequested/m_bStopRequested, GamePlugin.m_bBuilding) was already a plain
// flag the menu-bar buttons themselves just set and let the existing per-frame polling
// (PollGameReload/the deferred-Stop consumption, both in the editor's main loop) pick up - these
// commands set the SAME flags rather than re-implementing any of that machinery, so a CLI/AI-driven
// Play/Stop behaves identically to a real click, transport recompile-check and all.
//
// Deliberately Query, not Edit - none of these mutate SCENE CONTENT the way CreateEntity/SetProperty
// etc. do (same reasoning OpenLevel/Undo/Redo/Save already established, xlevel_commands_workspace.h);
// undo-routing a transport-state change makes no sense.
#include "plugins/xlevel.plugin/source/Editor/xlevel_command_context.h"

namespace xlevel::commands
{
    inline const char* PlayStateName(xlevel::level_state::play_state S) noexcept
    {
        switch (S)
        {
        case xlevel::level_state::play_state::Stopped: return "Stopped";
        case xlevel::level_state::play_state::Playing: return "Playing";
        case xlevel::level_state::play_state::Paused:  return "Paused";
        }
        return "Unknown";
    }

    //================================================================================================
    // Play - mirrors the menu-bar "Play"/"Resume" button exactly (the editor's main loop): from
    // Stopped, kicks off the same recompile-check (StartGameReload, XECS_BUILD_SHARED builds) or the
    // same direct Save+enter-Playing fallback (non-shared builds); from Paused, just resumes. A no-op
    // (with an explanatory result, not silence) if already Playing or a build is already in flight -
    // same guard the button's own BeginDisabled already enforces.
    //================================================================================================
    struct play_query_cmd : level_query_command
    {
        play_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "Play", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Starts Play (from Stopped, recompile-checks first) or resumes it (from Paused). Usage: Play"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            return xlevel::RequestPlay(LevelContext());
        }
    };

    //================================================================================================
    // Pause - mirrors the "Pause" button: only meaningful while actually Playing.
    //================================================================================================
    struct pause_query_cmd : level_query_command
    {
        pause_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "Pause", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Pauses a running Play session. Usage: Pause"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            return xlevel::RequestPause(State());
        }
    };

    //================================================================================================
    // Step - mirrors the toolbar "Step" button: runs exactly one frame. From Stopped it starts Play first
    // (same path as Play) and lands Paused after the first tick; from Paused it ticks once and stays Paused.
    //================================================================================================
    struct step_query_cmd : level_query_command
    {
        step_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "Step", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Runs one frame (from Stopped: starts Play, ticks once, lands Paused; from Paused: ticks once). Usage: Step"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            return xlevel::RequestStep(LevelContext());
        }
    };

    //================================================================================================
    // Stop - mirrors the "Stop" button: routes through the SAME RequestStop (xlevel_play_session.h) the
    // button itself calls, which sets the deferred flag (m_bStopRequested) consumed at the same clean
    // top-of-frame point PollGameReload runs from - never performed immediately here, for the exact
    // reason the button's own comment gives (StopPlaySession's destroy/recreate can't safely run
    // nested inside an active ImGui frame, and Query() runs outside one anyway, so deferring is not
    // just safe but the ONLY correct way to trigger it from here too).
    //
    // -Keep answers "keep property tweaks made during Play?" up front - for AI/script use, there is no
    // confirmation dialog for a script to click (same reasoning as -Force on the Asset File commands,
    // E10_Commands_AssetFiles.h: "there is no dialog to click"). Omitting it when there IS something
    // to ask about defers to the same confirmation modal the UI shows (RequestStop sets the identical
    // m_bAwaitingKeepTweaksAnswer flag either way) - Stop stays on hold until it's answered one way or
    // the other, by a human or a follow-up -Keep call.
    //================================================================================================
    struct stop_query_cmd : level_query_command
    {
        stop_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "Stop", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Stops Play/Pause, reverting to the pre-Play disk state. If properties changed while Playing, pass -Keep true|false to decide up front, or answer the confirmation dialog. Usage: Stop [-Keep true|false]"; }
        void RegisterArguments() noexcept override
        {
            m_hKeep = m_Parser.addOption("Keep", "true to keep property tweaks made while Playing, false to discard them - answers the 'keep changes?' question up front (for AI/script use - there is no dialog to click)", false, 1);
        }
        std::string Query() noexcept override
        {
            std::optional<bool> KeepOverride;
            if (auto KeepArg = m_Parser.getOptionArgAs<std::string>(m_hKeep, 0); !std::holds_alternative<xerr>(KeepArg))
            {
                const auto& S = std::get<std::string>(KeepArg);
                KeepOverride = (S == "true" || S == "1");
            }

            return xlevel::RequestStop(LevelContext(), KeepOverride);
        }
        xcmdline::parser::handle m_hKeep;
    };

    //================================================================================================
    // GetPlayState - the read-only counterpart the other three need to be useful headlessly: no
    // synthetic mouse click can show a CLI/AI caller what the menu-bar buttons currently look like.
    //================================================================================================
    struct get_play_state_query_cmd : level_query_command
    {
        get_play_state_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "GetPlayState", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Reports the current Play/Pause/Stop transport state. Usage: GetPlayState"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto& State = get<level_context>().State();
            const auto* pGate = xeditor::host::current()->find<play_gate>();
            const bool  bBuilding = pGate && pGate->m_IsBuilding();
            return std::format("PlayState={} Building={} PlayRequested={} StopRequested={}"
                , PlayStateName(State.m_PlayState), bBuilding, State.m_bPlayRequested, State.m_bStopRequested);
        }
    };
}

#endif // XLEVEL_COMMANDS_PLAYSESSION_H
