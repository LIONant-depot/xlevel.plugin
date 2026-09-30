#ifndef XLEVEL_TOOL_COLLIDER_BOX_H
#define XLEVEL_TOOL_COLLIDER_BOX_H
#pragma once

// PhysicsColliderBox's "Edit Collider" viewport tool (see xlevel_viewport_tools.h for the pattern).
// Resize = ImGuizmo's BOUNDS corner/edge handles (the opposite side stays put, like Unity's box
// collider handles); Move/Rotate = ImGuizmo translate/rotate about the box center.
//
// Placement mirrors body_builder exactly (xlioncore_physics_system.h): each box lives in the body
// frame (Transform Position/Rotation, no scale), with Center and Size multiplied by Transform.Scale
// along the box's own axes - what you see here is what Box3D gets.
#include "plugins/xlevel.plugin/source/Editor/xlevel_viewport_tools.h"
#include "dependencies/xLIONCore/src/physics/xlioncore_physics_collider.h"

namespace xlevel::viewport_tools
{
    struct collider_box_tool final : tool
    {
        using component = xlioncore::physics::physics_collider_box;

        collider_box_tool() noexcept : tool(component::typedef_v.m_Guid.m_Value, "PhysicsColliderBox/Boxes", "Edit Collider") {}

        static xmath::fmat4 BodyL2W(const xlioncore::transform& T) noexcept
        {
            xmath::fmat4 M;
            M.setupSRT(xmath::fvec3::fromOne(), T.m_Rotation, T.m_Position);
            return M;
        }

        static xmath::fmat4 BoxL2W(const xlioncore::transform& T, const xlioncore::physics::collider_box_shape& Box) noexcept
        {
            xmath::fmat4 Local;
            Local.setupSRT(Box.m_Size * T.m_Scale, Box.m_Orientation, Box.m_Center * T.m_Scale);
            return BodyL2W(T) * Local;
        }

        static bool isUsableScale(const xmath::fvec3& S) noexcept
        {
            return S.isFinite() && S.m_X != 0.0f && S.m_Y != 0.0f && S.m_Z != 0.0f;
        }

        int getElementCount(const target& Target) const noexcept override
        {
            return static_cast<int>(static_cast<const component*>(Target.m_pData)->m_Boxes.size());
        }

        void DrawSelected(const view& View, ImDrawList& DL, const target& Target, std::span<const int> EditedElements) const noexcept override
        {
            if (!Target.m_pTransform || !isUsableScale(Target.m_pTransform->m_Scale)) return;

            const auto& Boxes = static_cast<const component*>(Target.m_pData)->m_Boxes;
            for (int i = 0, n = static_cast<int>(Boxes.size()); i < n; ++i)
            {
                const bool bEdited = std::ranges::find(EditedElements, i) != EditedElements.end();
                View.WireBox( DL, BoxL2W(*Target.m_pTransform, Boxes[i])
                            , bEdited ? IM_COL32(170, 255, 160, 255) : IM_COL32(145, 244, 139, 170)
                            , bEdited ? 2.0f : 1.25f );
            }
        }

        bool EditElement(const view& View, target& Target, int Element, mode Mode) const noexcept override
        {
            if (!Target.m_pTransform || !isUsableScale(Target.m_pTransform->m_Scale)) return false;

            const auto& T     = *Target.m_pTransform;
            auto&       Box   = static_cast<component*>(Target.m_pData)->m_Boxes[Element];
            xmath::fmat4 World = BoxL2W(T, Box);

            static constexpr float LocalBounds[6] = { -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
            static constexpr float MoveSnap[3]    = { 0.1f, 0.1f, 0.1f };
            static constexpr float RotateSnap[3]  = { 15.0f, 15.0f, 15.0f };

            const ImGuizmo::OPERATION Op = Mode == mode::MOVE ? ImGuizmo::TRANSLATE : Mode == mode::ROTATE ? ImGuizmo::ROTATE : ImGuizmo::BOUNDS;
            const ImGuizmo::MODE      Space = (Mode == mode::RESIZE || View.m_bLocalSpace) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
            const float*              pSnap = !View.m_bSnap || Mode == mode::RESIZE ? nullptr : Mode == mode::MOVE ? MoveSnap : RotateSnap;

            if (ImGuizmo::Manipulate( reinterpret_cast<const float*>(&View.m_W2V), reinterpret_cast<const float*>(&View.m_Projection)
                                    , Op, Space, reinterpret_cast<float*>(&World), nullptr, pSnap
                                    , Mode == mode::RESIZE ? LocalBounds : nullptr ))
            {
                // Back into the body frame, then undo Transform.Scale - the exact inverse of BoxL2W.
                const xmath::fmat4 Local = BodyL2W(T).Inverse() * World;
                switch (Mode)
                {
                case mode::MOVE:
                    Box.m_Center = Local.ExtractPosition() / T.m_Scale;
                    break;
                case mode::ROTATE:
                    Box.m_Orientation    = Local.ExtractRotation();
                    Box.m_EditorRotation = Box.m_Orientation.ToEuler();
                    break;
                case mode::RESIZE:
                {
                    xmath::fvec3 AbsScale = T.m_Scale;
                    AbsScale.Abs();
                    Box.m_Size   = xmath::fvec3::Max(Local.ExtractScale() / AbsScale, xmath::fvec3{ 0.001f, 0.001f, 0.001f });
                }
                    Box.m_Center = Local.ExtractPosition() / T.m_Scale;
                    break;
                }
            }

            if (ImGuizmo::IsUsing()) return true;
            if (Mode != mode::RESIZE) return ImGuizmo::IsOver(Op);

            // BOUNDS anchors don't report through ImGuizmo::IsOver - test them directly. ImGuizmo puts
            // them on the camera-facing mid-planes: corners = the box's 12 edge midpoints, edge
            // midpoints = its 6 face centers. Testing all 18 is a harmless superset.
            for (int Axis = 0; Axis < 3; ++Axis)
            {
                for (int s = 0; s < 4; ++s)
                {
                    float P[3] = { 0, 0, 0 };
                    P[(Axis + 1) % 3] = (s & 1) ? 0.5f : -0.5f;
                    P[(Axis + 2) % 3] = (s & 2) ? 0.5f : -0.5f;
                    if (View.IsMouseNear(World * xmath::fvec3{ P[0], P[1], P[2] }, 9.0f)) return true;
                }
                float F[3] = { 0, 0, 0 };
                F[Axis] = 0.5f;
                if (View.IsMouseNear(World * xmath::fvec3{ F[0], F[1], F[2] }, 9.0f)) return true;
                F[Axis] = -0.5f;
                if (View.IsMouseNear(World * xmath::fvec3{ F[0], F[1], F[2] }, 9.0f)) return true;
            }
            return false;
        }
    };

    inline const collider_box_tool g_ColliderBoxTool;
    inline const registration      g_ColliderBoxToolRegistration{ g_ColliderBoxTool };
}

#endif // XLEVEL_TOOL_COLLIDER_BOX_H
