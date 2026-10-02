#ifndef XLVL_NEW_LevelEditor_GAME_PLUGIN_LOG_H
#define XLVL_NEW_LevelEditor_GAME_PLUGIN_LOG_H
#pragma once

#include "plugins/xlevel.plugin/source/Editor/xlevel_editor_tabs.h"
#include "dependencies/xlog/source/xlog_tab.h"

// The on-screen log surface for Game.dll build/load activity (the drawer's "Log" tab), now a view over the Logs
// (the xlog library, dependencies/xlog; documentation/Editors/DESIGN_logs.md) instead of a store of its own: the global vector and the global
// mutex are gone. LogGamePlugin may be called from any thread (the build runs on a worker); the panel reads the store on the host thread.
namespace xlevel
{
    // A status line of the Game.dll machinery ("Game.dll: rebuild succeeded", "... crashed while registering its systems"): printed (the process log, and the
    // smoke harness, read it from there) and recorded as an event of channel game.module. A module crash is a diagnostic, so it is a problem; every
    // other line is plain log (a failed BUILD is the game.build operation's business, not a second problem made from its status line).
    inline void LogGamePlugin( std::string_view Msg ) noexcept
    {
        std::printf("%.*s\n", static_cast<int>(Msg.size()), Msg.data());
        std::fflush(stdout);

        auto* pLogs = xlog::hub::current();
        if (!pLogs) return;
        const std::string Text(Msg);
        const bool bCrash  = Text.find("crashed while registering") != std::string::npos;
        const bool bFailed = Text.find("FAILED") != std::string::npos || Text.find("failed to") != std::string::npos;
        xlog::event E;
        E.m_Producer = "xlion.gamemodule"; E.m_Origin = { xlog::origin::type::System, "game.module", 0 };
        E.m_Channel = "game.module";
        E.m_Severity = (bCrash || bFailed) ? xlog::severity::Error : xlog::severity::Info;
        if (bCrash) { E.m_Kind = xlog::kind::Diagnostic; E.m_Code = "GAME.MODULE.CRASHED_REGISTERING"; }
        xlog::SetMessage(E, Text);
        pLogs->Emit(std::move(E));
    }

    inline void RenderGamePluginLogPanel( bool bEmbedded = false ) noexcept
    {
        ImGui::SetNextWindowPos(ImVec2(506, 530), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(480, 220), ImGuiCond_FirstUseEver);
        bool bWindowVisible = true;
        if (!bEmbedded)
        {
            bWindowVisible = ImGui::Begin(xlevel::editor_tabs::kGamePluginLogWindow);
            xeditor::diagnostics::Log("window begin: %s visible=%d", xlevel::editor_tabs::kGamePluginLogWindow, bWindowVisible ? 1 : 0);
        }
        if (bWindowVisible)
        {
            // The tab is the xlog library's: the Game.dll tab lists what the game machinery says (channels game.build and game.module).
            if (auto* pLogs = xlog::hub::current()) xlog::RenderTab(*pLogs, { .m_ChannelPrefix = "game." });
        }
        if (!bEmbedded)
        {
            ImGui::End();
            xeditor::diagnostics::Log("window end: %s", xlevel::editor_tabs::kGamePluginLogWindow);
        }
    }
} // namespace xlevel

#endif // XLVL_NEW_LevelEditor_GAME_PLUGIN_LOG_H
