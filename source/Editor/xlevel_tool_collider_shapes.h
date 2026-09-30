#ifndef XLEVEL_TOOL_COLLIDER_SHAPES_H
#define XLEVEL_TOOL_COLLIDER_SHAPES_H
#pragma once

// "Edit Collider" viewport tools for PhysicsColliderSphere / Capsule / Cylinder (the box one lives in
// xlevel_tool_collider_box.h; see xlevel_viewport_tools.h for the pattern). Same feel for all four:
//   Move    (W)  ImGuizmo translate about the shape center
//   Rotate  (E)  ImGuizmo rotate (capsule / cylinder only - a sphere has nothing to rotate)
//   Resize  (R)  round handles on the shape: drag a radius handle to change Radius, a top / bottom handle
//                to change Height, with a live readout while dragging
//
// Sizes are shown exactly as Box3D receives them: Transform.Scale is applied through collider_scale
// (xlioncore_physics_collider.h), the one definition the physics builder also uses.
#include "plugins/xlevel.plugin/source/Editor/xlevel_viewport_tools.h"
#include "dependencies/xLIONCore/src/physics/xlioncore_physics_collider.h"
#include <cstdio>

namespace xlevel::viewport_tools
{
    namespace collider_shapes
    {
        namespace scale = xlioncore::physics::collider_scale;

        constexpr ImU32 kWire       = IM_COL32(145, 244, 139, 170);
        constexpr ImU32 kWireEdited = IM_COL32(170, 255, 160, 255);

        // The body's own frame (Transform position + rotation, no scale) and one shape's world frame in it.
        struct frame
        {
            xmath::fmat4    m_Body;                 // body local -> world (scale excluded)
            xmath::fvec3    m_Center;               // shape center, world
            xmath::fvec3    m_X, m_Y, m_Z;          // shape axes, world, unit
            xmath::fmat4    m_Gizmo;                // rotation + translation, for ImGuizmo
        };

        inline bool isUsableScale(const xmath::fvec3& S) noexcept
        {
            return S.isFinite() && S.m_X != 0.0f && S.m_Y != 0.0f && S.m_Z != 0.0f;
        }

        inline frame MakeFrame(const xlioncore::transform& T, const xmath::fvec3& Center, const xmath::fquat& Orientation) noexcept
        {
            frame F;
            F.m_Body.setupSRT(xmath::fvec3::fromOne(), T.m_Rotation, T.m_Position);
            const xmath::fmat4 Rot = F.m_Body * xmath::fmat4::fromRotation(Orientation);
            F.m_X      = Rot.RotateVector({ 1.0f, 0.0f, 0.0f }).NormalizeCopy();
            F.m_Y      = Rot.RotateVector({ 0.0f, 1.0f, 0.0f }).NormalizeCopy();
            F.m_Z      = Rot.RotateVector({ 0.0f, 0.0f, 1.0f }).NormalizeCopy();
            F.m_Center = F.m_Body * (Center * T.m_Scale);
            F.m_Gizmo.setupSRT(xmath::fvec3::fromOne(), Rot.ExtractRotation(), F.m_Center);
            return F;
        }

        // Move / Rotate through ImGuizmo. Writes the shape's Center (and Orientation if bRotatable) back in
        // the body's local space, undoing Transform.Scale - the exact inverse of MakeFrame. Returns
        // whether the gizmo is hovered or being dragged.
        inline bool ManipulateFrame(const view& View, const xlioncore::transform& T, frame& F, mode Mode, bool bRotatable
                                   , xmath::fvec3& Center, xmath::fquat* pOrientation, xmath::radian3* pEditorRotation) noexcept
        {
            static constexpr float MoveSnap[3]   = { 0.1f, 0.1f, 0.1f };
            static constexpr float RotateSnap[3] = { 15.0f, 15.0f, 15.0f };

            const ImGuizmo::OPERATION Op   = Mode == mode::MOVE ? ImGuizmo::TRANSLATE : ImGuizmo::ROTATE;
            const ImGuizmo::MODE      Space = View.m_bLocalSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
            const float*              pSnap = View.m_bSnap ? (Mode == mode::MOVE ? MoveSnap : RotateSnap) : nullptr;

            xmath::fmat4 World = F.m_Gizmo;
            if (ImGuizmo::Manipulate( reinterpret_cast<const float*>(&View.m_W2V), reinterpret_cast<const float*>(&View.m_Projection)
                                    , Op, Space, reinterpret_cast<float*>(&World), nullptr, pSnap ))
            {
                const xmath::fmat4 Local = F.m_Body.Inverse() * World;
                if (Mode == mode::MOVE)
                {
                    Center = Local.ExtractPosition() / T.m_Scale;
                }
                else if (bRotatable && pOrientation)
                {
                    *pOrientation = Local.ExtractRotation();
                    if (pEditorRotation) *pEditorRotation = pOrientation->ToEuler();
                }
            }
            return ImGuizmo::IsUsing() || ImGuizmo::IsOver(Op);
        }

        inline ImGuiID HandleId(int Element, int Slot) noexcept { return 0x5C000000u + static_cast<ImGuiID>(Element) * 16u + static_cast<ImGuiID>(Slot); }

        // Picks the camera-facing circle plane for a sphere's silhouette.
        inline void Silhouette(const view& View, ImDrawList& DL, const xmath::fvec3& C, float R, ImU32 Color, float Thickness) noexcept
        {
            const xmath::fvec3 N = (View.m_Eye - C).NormalizeSafeCopy();
            xmath::fvec3 U = N.Cross(std::fabs(N.m_Y) < 0.99f ? xmath::fvec3{ 0.0f, 1.0f, 0.0f } : xmath::fvec3{ 1.0f, 0.0f, 0.0f }).NormalizeCopy();
            const xmath::fvec3 V = N.Cross(U).NormalizeCopy();
            View.Circle(DL, C, U, V, R, Color, Thickness);
        }
    }

    //--------------------------------------------------------------------------------------------
    // Sphere
    //--------------------------------------------------------------------------------------------
    struct collider_sphere_tool final : tool
    {
        using component = xlioncore::physics::physics_collider_sphere;

        collider_sphere_tool() noexcept : tool(component::typedef_v.m_Guid.m_Value, "PhysicsColliderSphere/Spheres", "Edit Collider") {}

        int  getElementCount(const target& Target) const noexcept override { return static_cast<int>(static_cast<const component*>(Target.m_pData)->m_Spheres.size()); }
        bool SupportsMode(mode M) const noexcept override { return M != mode::ROTATE; }

        void DrawSelected(const view& View, ImDrawList& DL, const target& Target, std::span<const int> Edited) const noexcept override
        {
            using namespace collider_shapes;
            if (!Target.m_pTransform || !isUsableScale(Target.m_pTransform->m_Scale)) return;
            const auto& Spheres = static_cast<const component*>(Target.m_pData)->m_Spheres;
            for (int i = 0, n = static_cast<int>(Spheres.size()); i < n; ++i)
            {
                const auto  F = MakeFrame(*Target.m_pTransform, Spheres[i].m_Center, xmath::fquat::fromIdentity());
                const float R = scale::Sphere(Spheres[i], Target.m_pTransform->m_Scale);
                const bool  bEdited = std::ranges::find(Edited, i) != Edited.end();
                const ImU32 Col = bEdited ? kWireEdited : kWire;
                const float Th  = bEdited ? 2.0f : 1.25f;
                View.Circle(DL, F.m_Center, F.m_X, F.m_Y, R, Col, Th);
                View.Circle(DL, F.m_Center, F.m_Y, F.m_Z, R, Col, Th);
                View.Circle(DL, F.m_Center, F.m_X, F.m_Z, R, Col, Th);
                Silhouette(View, DL, F.m_Center, R, Col, Th);
            }
        }

        bool EditElement(const view& View, target& Target, int Element, mode Mode) const noexcept override
        {
            using namespace collider_shapes;
            if (!Target.m_pTransform || !isUsableScale(Target.m_pTransform->m_Scale)) return false;
            const auto& T = *Target.m_pTransform;
            auto& Sphere  = static_cast<component*>(Target.m_pData)->m_Spheres[Element];
            auto  F       = MakeFrame(T, Sphere.m_Center, xmath::fquat::fromIdentity());

            if (Mode == mode::MOVE)
                return ManipulateFrame(View, T, F, Mode, false, Sphere.m_Center, nullptr, nullptr);

            // Resize: six handles on the sphere, any of them sets the radius.
            bool bBusy = false;
            const float Largest = scale::Largest(T.m_Scale);
            const float Rw      = scale::Sphere(Sphere, T.m_Scale);
            const xmath::fvec3 Axes[3] = { F.m_X, F.m_Y, F.m_Z };
            for (int a = 0; a < 3; ++a)
                for (int s = 0; s < 2; ++s)
                {
                    const float Sign = s ? -1.0f : 1.0f;
                    const auto H = handle::Axis(View, HandleId(Element, a * 2 + s), F.m_Center, Axes[a] * Sign, Rw);
                    bBusy |= H.m_bHovered || H.m_bActive;
                    if (H.m_bChanged && Largest > 0.0f)
                    {
                        Sphere.m_Radius = std::max(H.m_T / Largest, 0.001f);
                        char Text[64]; std::snprintf(Text, sizeof(Text), "Radius %.3f", Sphere.m_Radius);
                        handle::Label(H, Text);
                    }
                }
            return bBusy;
        }
    };

    //--------------------------------------------------------------------------------------------
    // Capsule
    //--------------------------------------------------------------------------------------------
    struct collider_capsule_tool final : tool
    {
        using component = xlioncore::physics::physics_collider_capsule;

        collider_capsule_tool() noexcept : tool(component::typedef_v.m_Guid.m_Value, "PhysicsColliderCapsule/Capsules", "Edit Collider") {}

        int getElementCount(const target& Target) const noexcept override { return static_cast<int>(static_cast<const component*>(Target.m_pData)->m_Capsules.size()); }

        void DrawSelected(const view& View, ImDrawList& DL, const target& Target, std::span<const int> Edited) const noexcept override
        {
            using namespace collider_shapes;
            if (!Target.m_pTransform || !isUsableScale(Target.m_pTransform->m_Scale)) return;
            const auto& Capsules = static_cast<const component*>(Target.m_pData)->m_Capsules;
            for (int i = 0, n = static_cast<int>(Capsules.size()); i < n; ++i)
            {
                const auto& C    = Capsules[i];
                const auto  F    = MakeFrame(*Target.m_pTransform, C.m_Center, C.m_Orientation);
                const auto  Size = scale::Capsule(C, Target.m_pTransform->m_Scale);
                const bool  bEdited = std::ranges::find(Edited, i) != Edited.end();
                const ImU32 Col = bEdited ? kWireEdited : kWire;
                const float Th  = bEdited ? 2.0f : 1.25f;

                const xmath::fvec3 Top = F.m_Center + F.m_Y * Size.m_HalfSpine, Bottom = F.m_Center - F.m_Y * Size.m_HalfSpine;
                View.Circle(DL, Top,    F.m_X, F.m_Z, Size.m_Radius, Col, Th);
                View.Circle(DL, Bottom, F.m_X, F.m_Z, Size.m_Radius, Col, Th);
                constexpr float Pi = 3.14159265f;
                View.Arc(DL, Top,    F.m_X, F.m_Y, Size.m_Radius, 0.0f, Pi,  Col, Th);          // caps, in the two planes through the axis
                View.Arc(DL, Top,    F.m_Z, F.m_Y, Size.m_Radius, 0.0f, Pi,  Col, Th);
                View.Arc(DL, Bottom, F.m_X, F.m_Y, Size.m_Radius, Pi,   2.0f * Pi, Col, Th);
                View.Arc(DL, Bottom, F.m_Z, F.m_Y, Size.m_Radius, Pi,   2.0f * Pi, Col, Th);
                for (const auto& Side : { F.m_X, F.m_Z, F.m_X * -1.0f, F.m_Z * -1.0f })
                    View.Line(DL, Top + Side * Size.m_Radius, Bottom + Side * Size.m_Radius, Col, Th);
            }
        }

        bool EditElement(const view& View, target& Target, int Element, mode Mode) const noexcept override
        {
            using namespace collider_shapes;
            if (!Target.m_pTransform || !isUsableScale(Target.m_pTransform->m_Scale)) return false;
            const auto& T = *Target.m_pTransform;
            auto& C       = static_cast<component*>(Target.m_pData)->m_Capsules[Element];
            auto  F       = MakeFrame(T, C.m_Center, C.m_Orientation);

            if (Mode != mode::RESIZE)
                return ManipulateFrame(View, T, F, Mode, true, C.m_Center, &C.m_Orientation, &C.m_EditorRotation);

            bool bBusy = false;
            const auto  Size   = scale::Capsule(C, T.m_Scale);
            const float Radial = scale::Radial(T.m_Scale), Axial = scale::Axial(T.m_Scale);

            // Radius: four handles around the equator.
            const xmath::fvec3 Around[4] = { F.m_X, F.m_X * -1.0f, F.m_Z, F.m_Z * -1.0f };
            for (int i = 0; i < 4; ++i)
            {
                const auto H = handle::Axis(View, HandleId(Element, i), F.m_Center, Around[i], Size.m_Radius);
                bBusy |= H.m_bHovered || H.m_bActive;
                if (H.m_bChanged && Radial > 0.0f)
                {
                    C.m_Radius = std::clamp(H.m_T / Radial, 0.001f, std::max(C.m_Height * 0.5f, 0.001f));
                    char Text[64]; std::snprintf(Text, sizeof(Text), "Radius %.3f", C.m_Radius);
                    handle::Label(H, Text);
                }
            }
            // Height: the two tips. The far tip stays put on the other side because the change is symmetric.
            for (int s = 0; s < 2; ++s)
            {
                const float Sign = s ? -1.0f : 1.0f;
                const auto  H    = handle::Axis(View, HandleId(Element, 4 + s), F.m_Center, F.m_Y * Sign, Size.m_HalfSpine + Size.m_Radius);
                bBusy |= H.m_bHovered || H.m_bActive;
                if (H.m_bChanged && Axial > 0.0f)
                {
                    const float Spine = (H.m_T - Size.m_Radius) / Axial;                   // local half spine
                    C.m_Height = std::max(2.0f * (std::max(Spine, 0.0f) + C.m_Radius), 2.0f * C.m_Radius);
                    char Text[64]; std::snprintf(Text, sizeof(Text), "Height %.3f", C.m_Height);
                    handle::Label(H, Text);
                }
            }
            return bBusy;
        }
    };

    //--------------------------------------------------------------------------------------------
    // Cylinder
    //--------------------------------------------------------------------------------------------
    struct collider_cylinder_tool final : tool
    {
        using component = xlioncore::physics::physics_collider_cylinder;

        collider_cylinder_tool() noexcept : tool(component::typedef_v.m_Guid.m_Value, "PhysicsColliderCylinder/Cylinders", "Edit Collider") {}

        int getElementCount(const target& Target) const noexcept override { return static_cast<int>(static_cast<const component*>(Target.m_pData)->m_Cylinders.size()); }

        void DrawSelected(const view& View, ImDrawList& DL, const target& Target, std::span<const int> Edited) const noexcept override
        {
            using namespace collider_shapes;
            if (!Target.m_pTransform || !isUsableScale(Target.m_pTransform->m_Scale)) return;
            const auto& Cylinders = static_cast<const component*>(Target.m_pData)->m_Cylinders;
            for (int i = 0, n = static_cast<int>(Cylinders.size()); i < n; ++i)
            {
                const auto& C    = Cylinders[i];
                const auto  F    = MakeFrame(*Target.m_pTransform, C.m_Center, C.m_Orientation);
                const auto  Size = scale::Cylinder(C, Target.m_pTransform->m_Scale);
                const bool  bEdited = std::ranges::find(Edited, i) != Edited.end();
                const ImU32 Col = bEdited ? kWireEdited : kWire;
                const float Th  = bEdited ? 2.0f : 1.25f;

                const xmath::fvec3 Top = F.m_Center + F.m_Y * Size.m_HalfHeight, Bottom = F.m_Center - F.m_Y * Size.m_HalfHeight;
                View.Circle(DL, Top,    F.m_X, F.m_Z, Size.m_Radius, Col, Th);
                View.Circle(DL, Bottom, F.m_X, F.m_Z, Size.m_Radius, Col, Th);
                for (const auto& Side : { F.m_X, F.m_Z, F.m_X * -1.0f, F.m_Z * -1.0f })
                    View.Line(DL, Top + Side * Size.m_Radius, Bottom + Side * Size.m_Radius, Col, Th);
            }
        }

        bool EditElement(const view& View, target& Target, int Element, mode Mode) const noexcept override
        {
            using namespace collider_shapes;
            if (!Target.m_pTransform || !isUsableScale(Target.m_pTransform->m_Scale)) return false;
            const auto& T = *Target.m_pTransform;
            auto& C       = static_cast<component*>(Target.m_pData)->m_Cylinders[Element];
            auto  F       = MakeFrame(T, C.m_Center, C.m_Orientation);

            if (Mode != mode::RESIZE)
                return ManipulateFrame(View, T, F, Mode, true, C.m_Center, &C.m_Orientation, &C.m_EditorRotation);

            bool bBusy = false;
            const auto  Size   = scale::Cylinder(C, T.m_Scale);
            const float Radial = scale::Radial(T.m_Scale), Axial = scale::Axial(T.m_Scale);

            const xmath::fvec3 Around[4] = { F.m_X, F.m_X * -1.0f, F.m_Z, F.m_Z * -1.0f };
            for (int i = 0; i < 4; ++i)
            {
                const auto H = handle::Axis(View, HandleId(Element, i), F.m_Center, Around[i], Size.m_Radius);
                bBusy |= H.m_bHovered || H.m_bActive;
                if (H.m_bChanged && Radial > 0.0f)
                {
                    C.m_Radius = std::max(H.m_T / Radial, 0.001f);
                    char Text[64]; std::snprintf(Text, sizeof(Text), "Radius %.3f", C.m_Radius);
                    handle::Label(H, Text);
                }
            }
            for (int s = 0; s < 2; ++s)
            {
                const float Sign = s ? -1.0f : 1.0f;
                const auto  H    = handle::Axis(View, HandleId(Element, 4 + s), F.m_Center, F.m_Y * Sign, Size.m_HalfHeight);
                bBusy |= H.m_bHovered || H.m_bActive;
                if (H.m_bChanged && Axial > 0.0f)
                {
                    C.m_Height = std::max(2.0f * H.m_T / Axial, 0.001f);
                    char Text[64]; std::snprintf(Text, sizeof(Text), "Height %.3f", C.m_Height);
                    handle::Label(H, Text);
                }
            }
            return bBusy;
        }
    };

    inline const collider_sphere_tool   g_ColliderSphereTool;
    inline const collider_capsule_tool  g_ColliderCapsuleTool;
    inline const collider_cylinder_tool g_ColliderCylinderTool;
    inline const registration           g_ColliderSphereToolRegistration  { g_ColliderSphereTool };
    inline const registration           g_ColliderCapsuleToolRegistration { g_ColliderCapsuleTool };
    inline const registration           g_ColliderCylinderToolRegistration{ g_ColliderCylinderTool };
}

#endif // XLEVEL_TOOL_COLLIDER_SHAPES_H
