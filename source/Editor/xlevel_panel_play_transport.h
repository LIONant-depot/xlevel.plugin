#include "dependencies/xeditor/include/xeditor/hint.h"
#include "dependencies/xeditor/include/xeditor/shortcuts.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_visual_studio.h"
#include "source/Tools/Editor/xeditor_play_transport.h"     // the buttons and the speed slider (shared by every editor)
#ifndef XLEVEL_PANEL_PLAY_TRANSPORT_H
#define XLEVEL_PANEL_PLAY_TRANSPORT_H
#pragma once

// Play / Step / Pause / Stop buttons, shared by the menu-bar transport and the editor toolbar - they differ
// only in layout (transport_layout). All behavior lives in xlevel_play_session.h (RequestPlay/...), which
// the CLI Play/Pause/Step/Stop commands use too.
//
// Unity-style, worked out to keep the mouse from ever landing on a moved button: two fixed slots, centered as
// a PAIR - slot 1 is Play/Stop, slot 2 is Step while Stopped/Paused or Pause while Playing (same position, only
// the icon/action changes). A third slot - Pause again, pressed, to Resume - appears only while Paused, to the
// right of the pair. Glyphs are Segoe MDL2 (U+E768 play, E769 pause, E71A stop, E893 step, E943 code), merged into font 4.
// At the right of all of them, in a place of its own (it does not move when Resume appears), the button that goes to Visual Studio: where the scripts are written.
namespace xlevel
{
    struct transport_layout
    {
        ImVec2 m_ButtonSize;
        bool   m_bHorizontal;   // false: stacked top-to-bottom (toolbar docked left/right), no centering
        bool   m_bTooltips;
    };

    inline void RenderPlayTransport( level_context& Ed, const transport_layout& Layout ) noexcept
    {
        namespace pt = xeditor::play_transport;
        auto& State = Ed.State();
        const auto* pGate = xeditor::host::current()->find<play_gate>();
        using play_state = level_state::play_state;
        constexpr const char* CodeIcon  = "\xEE\xA5\x83";
        const pt::layout L{ Layout.m_ButtonSize, Layout.m_bHorizontal, Layout.m_bTooltips };
        const float PairX = pt::PairX(L);

        // The speed of the game, left of Play: the multiplier of this editor's game time, so it is already set when Play is pressed.
        if (Layout.m_bHorizontal && Ed.m_pGame)
            pt::SpeedLeftOfPair(Ed.m_pGame->m_Time.m_TimeScale, xlioncore::game_time::kScaleSteps, L);

        // A Level with an error (its Game lacks what its scenes need) cannot be played: it would run without the systems and components its scenes use
        pt::actions A;
        A.m_bBusy      = pGate && pGate->m_IsBuilding(Ed);
        A.m_WhyNotPlay = State.m_WhyNotPlay;
        A.m_PlayKeys   = xeditor::ShortcutText("Level/Play");
        A.m_StopKeys   = xeditor::ShortcutText("Level/Stop");
        A.m_Play       = [&] { RequestPlay(Ed); };
        A.m_Stop       = [&] { RequestStop(Ed, std::nullopt); };
        A.m_Pause      = [&] { RequestPause(State); };
        A.m_Resume     = [&] { RequestResume(State); };
        A.m_Step       = [&] { RequestStep(Ed); };
        pt::Buttons(State.m_PlayState == play_state::Stopped ? pt::state::Stopped : State.m_PlayState == play_state::Playing ? pt::state::Playing : pt::state::Paused, A, L);

        auto Slot = [&](const char* Icon, bool bDisabled, bool bPressed, auto&& OnClick) noexcept { if (pt::Slot(Icon, L, bDisabled, bPressed)) OnClick(); };
        auto Tip = [&](const char* Title, const char* Hint) noexcept { if (Layout.m_bTooltips && ImGui::IsItemHovered()) xeditor::hint::Draw({ Title, Hint }); };

        // The way to Visual Studio, after the room the three transport slots may take (and a little more).
        if (Layout.m_bHorizontal) ImGui::SameLine(PairX + (Layout.m_ButtonSize.x + ImGui::GetStyle().ItemSpacing.x) * 3.0f + 16.0f);
        const bool bNoProject = !Ed.m_GameSolution || Ed.m_GameSolution().empty();
        Slot(CodeIcon, bNoProject, false, [&] { RequestOpenVisualStudio(Ed, false); });
        if (bNoProject) { if (Layout.m_bTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) xeditor::hint::Text("No game project to open yet: this Level's Game has no script modules, or has not been built"); }
        else Tip("Visual Studio", "Open this Level's game project in Visual Studio, or go back to it when it is open: where the scripts are written");
    }
}

#endif // XLEVEL_PANEL_PLAY_TRANSPORT_H
