#ifndef XLEVEL_TOOL_ANIMATION_BONES_H
#define XLEVEL_TOOL_ANIMATION_BONES_H
#pragma once

// KUX1 (R66): the bones an Animation component's fields name, drawn over the selected character in the Level viewport - the Trail's chain,
// the Springs' and the Overlay's branches, the Aim bone's axis, the Copy's link, the Offset's chain (Spread). The renderer makes the segments
// from the pose it drew (xlionrender::scene_renderer::SeeBones) when this tool asks (animation::m_bSeeBones), so what is lit is what the
// runtime resolved: a misspelt bone lights nothing (its [AnimBones] line says why). Draw only: nothing to edit (see xlevel_viewport_tools.h).
#include "plugins/xlevel.plugin/source/Editor/xlevel_viewport_tools.h"
#include "dependencies/xLIONRender/src/xlionrender_scene_components.h"

namespace xlevel::viewport_tools
{
    struct animation_bones_tool final : tool
    {
        using component = xlionrender::animation;

        animation_bones_tool() noexcept : tool(xecs::component::type::info_v<component>.m_Guid.m_Value, "Animation/Bones", "Animation bones") {}     // (info_v: Animation names no guid, its typedef_v.m_Guid is empty)

        int  getElementCount(const target&) const noexcept override { return 0; }
        bool SupportsMode(mode) const noexcept override { return false; }
        bool EditElement(const view&, target&, int, mode) const noexcept override { return false; }

        void DrawSelected(const view& View, ImDrawList& DL, const target& Target, std::span<const int>) const noexcept override
        {
            const auto& A = *static_cast<const component*>(Target.m_pData);
            A.m_bSeeBones = true;                   // (the next frame's pose fills the segments)
            static constexpr ImU32 Colors[] = { IM_COL32(80, 220, 255, 230), IM_COL32(255, 170, 60, 230), IM_COL32(200, 120, 255, 200)
                                              , IM_COL32(255, 80, 80, 240), IM_COL32(255, 255, 90, 230), IM_COL32(120, 255, 140, 230) };
            if (!A.m_pSeen) return;
            for (const auto& S : *A.m_pSeen)
            {
                const ImU32 C = Colors[S.m_Kind < std::size(Colors) ? S.m_Kind : 0];
                View.Line(DL, S.m_A, S.m_B, C, S.m_Kind == 2 ? 1.5f : 3.0f);
                ImVec2 Pa, Pb;
                if (View.Project(S.m_A, Pa)) DL.AddCircleFilled(Pa, 3.5f, C);
                if (View.Project(S.m_B, Pb)) DL.AddCircleFilled(Pb, 3.5f, C);
            }
        }
    };

    inline const animation_bones_tool g_AnimationBonesTool;
    inline const registration         g_AnimationBonesToolRegistration{ g_AnimationBonesTool };
}

#endif // XLEVEL_TOOL_ANIMATION_BONES_H
