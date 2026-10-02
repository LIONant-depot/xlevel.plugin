#include "dependencies/xeditor/include/xeditor/hint.h"
#include "dependencies/xeditor/include/xeditor/shortcuts.h"
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
// right of the pair. Glyphs are Segoe MDL2 (U+E768 play, E769 pause, E71A stop, E893 step), merged into font 4.
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
        auto& State = Ed.State();
        const auto* pGate = xeditor::host::current()->find<play_gate>();
        const bool bBuilding = pGate && pGate->m_IsBuilding();
        // Another Level editor is playing: this one can neither Play nor Step until it stops.
        auto* pHost = xeditor::host::current();
        const bool bOtherPlaying = pHost && pHost->is_play_active() && pHost->m_pPlayOwner != &State;
        using play_state = level_state::play_state;
        constexpr const char* PlayIcon  = "\xEE\x9D\xA8";
        constexpr const char* PauseIcon = "\xEE\x9D\xA9";
        constexpr const char* StopIcon  = "\xEE\x9C\x9A";
        constexpr const char* StepIcon  = "\xEE\xA2\x93";

        // Captured once, before any button: clicking slot 2's Pause must not make slot 3 appear this same frame.
        const bool bStopped = State.m_PlayState == play_state::Stopped;
        const bool bPlaying = State.m_PlayState == play_state::Playing;
        const bool bPaused  = State.m_PlayState == play_state::Paused;

        auto Slot = [&](const char* Icon, bool bDisabled, bool bPressed, auto&& OnClick) noexcept
        {
            ImGui::PushFont(xgpu::tools::imgui::getFont(4));
            if (bPressed) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Header]);
            ImGui::BeginDisabled(bDisabled);
            if (ImGui::Button(Icon, Layout.m_ButtonSize)) OnClick();
            ImGui::EndDisabled();
            if (bPressed) ImGui::PopStyleColor();
            ImGui::PopFont();
        };
        auto Tip = [&](const char* Title, const char* Hint) noexcept
        {
            if (!Layout.m_bTooltips || !ImGui::IsItemHovered()) return;
            // The key the action has right now (Level/Play, Level/Stop), from the host; the other buttons have none.
            const std::string Keys = std::string_view(Title) == "Play" ? xeditor::ShortcutText("Level/Play")
                                   : std::string_view(Title) == "Stop" ? xeditor::ShortcutText("Level/Stop") : std::string{};
            xeditor::hint::Draw({ Title, Hint, Keys });
        };
        auto Next = [&]() noexcept { if (Layout.m_bHorizontal) ImGui::SameLine(); };

        // Centered on the two-slot pair ALWAYS, so slots 1 and 2 stay pixel-fixed when slot 3 appears.
        const float PairX = (ImGui::GetWindowWidth() - (Layout.m_ButtonSize.x * 2.0f + ImGui::GetStyle().ItemSpacing.x)) * 0.5f;

        // The speed of the game, left of Play: 0.25x to 3x, with snaps at the usual speeds (and right-click for 1x). It is the multiplier of
        // this editor's game time, so it is already set when Play is pressed.
        if (Layout.m_bHorizontal && Ed.m_pGame)
        {
            constexpr float SliderW = 120.0f;
            constexpr auto& Snaps = xlioncore::game_time::kScaleSteps;
            auto& Scale = Ed.m_pGame->m_Time.m_TimeScale;
            ImGui::SameLine(std::max(0.0f, PairX - SliderW - ImGui::GetStyle().ItemSpacing.x * 2.0f));
            ImGui::SetNextItemWidth(SliderW);
            if (ImGui::SliderFloat("##TimeScale", &Scale, Snaps.front(), Snaps.back(), "%.2fx", ImGuiSliderFlags_AlwaysClamp))
                Scale = xlioncore::game_time::NearestScaleStep(Scale);                      // no smooth values: always one of the stops
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) Scale = 1.0f;
            if (Layout.m_bTooltips && ImGui::IsItemHovered()) xeditor::hint::Text("Speed of the game: %.2fx  (0.25 0.5 0.75 1 1.5 2 3, right-click for 1x)", Scale);
            { // the snaps, as small marks under the slider
                const ImVec2 Min = ImGui::GetItemRectMin(), Max = ImGui::GetItemRectMax();
                const float  Grab = ImGui::GetStyle().GrabMinSize;
                for (float Snap : Snaps)
                {
                    const float X = Min.x + Grab * 0.5f + (Max.x - Min.x - Grab) * ((Snap - Snaps.front()) / (Snaps.back() - Snaps.front()));
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(X, Max.y - 3.0f), ImVec2(X, Max.y), IM_COL32(200, 200, 200, 160));
                }
            }
        }
        if (Layout.m_bHorizontal)
            ImGui::SameLine(PairX);

        Slot(bStopped ? PlayIcon : StopIcon, bBuilding || (bStopped && bOtherPlaying), false, [&]
        {
            if (bStopped) RequestPlay(Ed);
            else          RequestStop(Ed, std::nullopt);
        });
        if (bStopped && bOtherPlaying) { if (Layout.m_bTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) xeditor::hint::Text("Another Level is playing - stop it first"); }
        else Tip(bStopped ? "Play" : "Stop", bStopped ? "Start playback" : "Stop playback");

        Next();
        if (bPlaying)
        {
            Slot(PauseIcon, false, false, [&] { RequestPause(State); });
            Tip("Pause", "Pause playback");
        }
        else
        {
            Slot(StepIcon, bBuilding || (bStopped && bOtherPlaying), false, [&] { RequestStep(Ed); });
            Tip("Step", "Run one frame");
        }

        if (bPaused)
        {
            Next();
            Slot(PauseIcon, false, true, [&] { RequestResume(State); });
            Tip("Resume", "Resume playback");
        }
    }
}

#endif // XLEVEL_PANEL_PLAY_TRANSPORT_H
