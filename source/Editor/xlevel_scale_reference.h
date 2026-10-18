#ifndef XLEVEL_SCALE_REFERENCE_H
#define XLEVEL_SCALE_REFERENCE_H
#pragma once

// Scale references in the Level viewport: human figures (source/Tools/Editor/xeditor_scale_reference.h, 1.8 m tall, camera-facing) the viewport draws so
// the size of what is around them reads at a glance. Editor only: they live in the Level session (not in the Level or its Scenes), so they are never
// saved, exported or drawn by the game. A click on one selects it, the Move gizmo moves it, Delete removes it; the toolbar's Human button (and the
// ScaleReference command) adds one where the camera looks.
#include "dependencies/ImGuizmo/src/ImGuizmo.h"
#include "dependencies/xmath/source/xmath.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_viewport_tools.h"
#include "source/Tools/Editor/xeditor_scale_reference.h"
#include "imgui.h"
#include <format>
#include <string>
#include <vector>

namespace xlevel
{
    struct scale_reference
    {
        static constexpr float height_v = xeditor::scale_reference::height_v;     // meters

        std::vector<xmath::fvec3>   m_Figures;              // where the feet are, world
        int                         m_iSelected     = -1;
        xmath::fvec3                m_Eye           = xmath::fvec3::fromZero();     // the camera of the last frame drawn: where Add puts a figure
        xmath::fvec3                m_Look          = xmath::fvec3{ 0, 0, 1 };

        // Every frame of the viewport, before the gizmos: the figures, and the camera Add places from
        void Draw(const viewport_tools::view& View, ImDrawList& DL) noexcept
        {
            m_Eye  = View.m_Eye;
            m_Look = View.m_RayDir ? View.m_RayDir(View.m_Min.x + View.m_Size.x * 0.5f, View.m_Min.y + View.m_Size.y * 0.5f) : m_Look;
            const auto Project = [&](const xmath::fvec3& P, ImVec2& S) { return View.Project(P, S); };
            for (int i = 0; i < static_cast<int>(m_Figures.size()); ++i)
                xeditor::scale_reference::Draw(DL, m_Figures[i], View.m_Eye, Project, i == m_iSelected);
        }

        // A click: the figure under the mouse (the nearest when they overlap) is selected; false when none is there
        bool Pick(const viewport_tools::view& View, ImVec2 Mouse) noexcept
        {
            int   Best      = -1;
            float BestDist  = 0;
            const auto Project = [&](const xmath::fvec3& P, ImVec2& S) { return View.Project(P, S); };
            for (int i = 0; i < static_cast<int>(m_Figures.size()); ++i)
            {
                ImVec2 Lo, Hi;
                if (!xeditor::scale_reference::ScreenBounds(m_Figures[i], View.m_Eye, Project, Lo, Hi)) continue;
                if (Mouse.x < Lo.x || Mouse.x > Hi.x || Mouse.y < Lo.y || Mouse.y > Hi.y) continue;
                const float Dist = (m_Figures[i] - View.m_Eye).Length();
                if (Best < 0 || Dist < BestDist) { Best = i; BestDist = Dist; }
            }
            m_iSelected = Best;
            return Best >= 0;
        }

        // The Move gizmo of the selected figure; true while the mouse is over it or drags it (the viewport then leaves the mouse alone)
        bool Gizmo(const viewport_tools::view& View) noexcept
        {
            if (m_iSelected < 0 || m_iSelected >= static_cast<int>(m_Figures.size())) { m_iSelected = -1; return false; }
            auto& Feet = m_Figures[m_iSelected];
            xmath::fmat4 World = xmath::fmat4::fromTranslation(Feet);
            ImGuizmo::PushID("ScaleReference");
            const float Snap[3] = { 0.5f, 0.5f, 0.5f };
            if (ImGuizmo::Manipulate(reinterpret_cast<const float*>(&View.m_W2V), reinterpret_cast<const float*>(&View.m_Projection), ImGuizmo::TRANSLATE, ImGuizmo::WORLD
                , reinterpret_cast<float*>(&World), nullptr, View.m_bSnap ? Snap : nullptr))
                Feet = World.ExtractPosition();
            const bool bBusy = ImGuizmo::IsOver(ImGuizmo::TRANSLATE) || ImGuizmo::IsUsing();
            ImGuizmo::PopID();
            return bBusy;
        }

        // A new figure, selected: on the ground (y = 0) where the camera looks when that is near, else a few meters in front of the camera
        int Add() noexcept
        {
            xmath::fvec3 At = m_Eye + m_Look * 5.0f;
            if (m_Look.m_Y < -1.0e-3f)
                if (const float T = -m_Eye.m_Y / m_Look.m_Y; T > 0 && T < 60.0f) At = m_Eye + m_Look * T;
            m_Figures.push_back(At);
            return m_iSelected = static_cast<int>(m_Figures.size()) - 1;
        }

        void RemoveSelected() noexcept
        {
            if (m_iSelected < 0 || m_iSelected >= static_cast<int>(m_Figures.size())) return;
            m_Figures.erase(m_Figures.begin() + m_iSelected);
            m_iSelected = -1;
        }

        std::string Describe() const noexcept
        {
            std::string Text = std::format("ScaleReferences={} Selected={} Height={}m", m_Figures.size(), m_iSelected, height_v);
            for (std::size_t i = 0; i < m_Figures.size(); ++i) Text += std::format("\n  {}: feet at {:.2f},{:.2f},{:.2f}", i, m_Figures[i].m_X, m_Figures[i].m_Y, m_Figures[i].m_Z);
            return Text;
        }
    };
}

#endif // XLEVEL_SCALE_REFERENCE_H
