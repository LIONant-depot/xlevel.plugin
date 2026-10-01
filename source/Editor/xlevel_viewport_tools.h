#ifndef XLEVEL_VIEWPORT_TOOLS_H
#define XLEVEL_VIEWPORT_TOOLS_H
#pragma once

// Viewport tools - per-component direct-manipulation editors for the Level Editor viewport (Unity's
// "Edit Collider" pattern). A tool binds to one component type (by GUID) and to one of its object
// arrays, so each array element gets its own "Edit" toggle in the Inspector's element row
// (xproperty::inspector::m_OnArrayElementRender). Several elements can be in edit mode at once.
//
// Split of responsibilities:
//  - tool (one per component type): draws the always-on visualization for the selected entity and
//    the handles for one element in edit mode; mutates the LIVE component only. No commands, no undo,
//    no state of its own.
//  - editor (one per Level session): which entity/component/elements are being edited and in which
//    W/E/R mode, the Inspector toggle, the viewport overlay, ending the edit when the selection or
//    the component goes away or Play starts, and committing whatever the tool changed during a drag
//    as ONE undo step of ordinary SetProperty commands (xscene::commands::SnapshotProperties) - so
//    prefab overrides, dirty tracking and Undo/Redo come for free.
//
// Adding a tool: derive from viewport_tools::tool and register one instance from any header the Level
// plugin includes: `inline const my_tool g_MyTool; inline const viewport_tools::registration g_MyToolReg{ g_MyTool };`
#include "dependencies/ImGuizmo/src/ImGuizmo.h"
#include "dependencies/xmath/source/xmath.h"
#include "dependencies/xproperty/source/examples/imgui/xPropertyImGuiInspector.h"
#include "dependencies/xLIONCore/src/transform/xlioncore_transform.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_property_edit.h"
#include "plugins/xscene.plugin/source/Editor/xscene_commands_transform_gizmo.h"
#include <span>
#include <functional>
#include <cmath>
#include <format>
#include <vector>
#include <string_view>
#include <algorithm>

namespace xlevel::viewport_tools
{
    enum class mode : std::uint8_t { MOVE, ROTATE, RESIZE };   // W / E / R

    //--------------------------------------------------------------------------------------------
    // The camera/viewport a tool draws into this frame.
    //--------------------------------------------------------------------------------------------
    struct view
    {
        xmath::fmat4    m_W2V;              // for ImGuizmo
        xmath::fmat4    m_Projection;       // for ImGuizmo (its OpenGL Y convention - see xlevel_session.h)
        xmath::fmat4    m_W2C;              // engine clip space (Vulkan Y)
        ImVec2          m_Min;              // viewport rect, screen pixels
        ImVec2          m_Size;
        bool            m_bLocalSpace = false;
        bool            m_bSnap       = false;  // Ctrl held
        xmath::fvec3    m_Eye;                                      // camera position, world
        std::function<xmath::fvec3(float, float)> m_RayDir;         // world ray direction through a screen pixel

        static constexpr float near_w_v = 1.0e-3f;

        ImVec2 ClipToScreen(const xmath::fvec4& C) const noexcept
        {
            return ImVec2( m_Min.x + (C.m_X / C.m_W * 0.5f + 0.5f) * m_Size.x
                         , m_Min.y + (C.m_Y / C.m_W * 0.5f + 0.5f) * m_Size.y );
        }

        // World point -> screen pixels; false when behind the near plane.
        bool Project(const xmath::fvec3& P, ImVec2& Out) const noexcept
        {
            const auto C = m_W2C * xmath::fvec4{ P.m_X, P.m_Y, P.m_Z, 1.0f };
            if (C.m_W < near_w_v) return false;
            Out = ClipToScreen(C);
            return true;
        }

        // A world-space segment, clipped against the near plane.
        void Line(ImDrawList& DL, const xmath::fvec3& A, const xmath::fvec3& B, ImU32 Color, float Thickness) const noexcept
        {
            auto CA = m_W2C * xmath::fvec4{ A.m_X, A.m_Y, A.m_Z, 1.0f };
            auto CB = m_W2C * xmath::fvec4{ B.m_X, B.m_Y, B.m_Z, 1.0f };
            if (CA.m_W < near_w_v && CB.m_W < near_w_v) return;
            if (CA.m_W < near_w_v) CA = CB + (CA - CB) * ((CB.m_W - near_w_v) / (CB.m_W - CA.m_W));
            else if (CB.m_W < near_w_v) CB = CA + (CB - CA) * ((CA.m_W - near_w_v) / (CA.m_W - CB.m_W));
            DL.AddLine(ClipToScreen(CA), ClipToScreen(CB), Color, Thickness);
        }

        // Wireframe of a unit cube (-0.5..0.5) placed by L2W.
        void WireBox(ImDrawList& DL, const xmath::fmat4& L2W, ImU32 Color, float Thickness) const noexcept
        {
            xmath::fvec3 P[8];
            for (int i = 0; i < 8; ++i)
                P[i] = L2W * xmath::fvec3{ (i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f };
            static constexpr int Edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
            for (auto& E : Edges) Line(DL, P[E[0]], P[E[1]], Color, Thickness);
        }

        // Circle (or part of one) of Radius around C in the plane spanned by the unit vectors U and V,
        // from angle A0 to A1 radians.
        void Arc(ImDrawList& DL, const xmath::fvec3& C, const xmath::fvec3& U, const xmath::fvec3& V, float Radius
                , float A0, float A1, ImU32 Color, float Thickness, int Segments = 48) const noexcept
        {
            const int N = std::max(2, static_cast<int>(Segments * std::fabs(A1 - A0) / 6.2831853f));
            xmath::fvec3 Prev = C + U * (Radius * std::cos(A0)) + V * (Radius * std::sin(A0));
            for (int i = 1; i <= N; ++i)
            {
                const float A = A0 + (A1 - A0) * static_cast<float>(i) / static_cast<float>(N);
                const xmath::fvec3 P = C + U * (Radius * std::cos(A)) + V * (Radius * std::sin(A));
                Line(DL, Prev, P, Color, Thickness);
                Prev = P;
            }
        }
        void Circle(ImDrawList& DL, const xmath::fvec3& C, const xmath::fvec3& U, const xmath::fvec3& V, float Radius, ImU32 Color, float Thickness) const noexcept
        {
            Arc(DL, C, U, V, Radius, 0.0f, 6.2831853f, Color, Thickness);
        }

        // Is the mouse within Radius pixels of the projected world point?
        bool IsMouseNear(const xmath::fvec3& P, float Radius) const noexcept
        {
            ImVec2 S;
            if (!Project(P, S)) return false;
            const ImVec2 M = ImGui::GetIO().MousePos;
            return (S.x - M.x) * (S.x - M.x) + (S.y - M.y) * (S.y - M.y) <= Radius * Radius;
        }
    };

    //--------------------------------------------------------------------------------------------
    // Draggable handle constrained to a world-space line (radius / height handles, ...). ImGuizmo
    // covers move / rotate / box resize; anything else a tool needs to drag goes through this. One
    // handle can be active at a time (the session has one viewport), so the state is global.
    //--------------------------------------------------------------------------------------------
    namespace handle
    {
        inline ImGuiID& ActiveId  (void) noexcept { static ImGuiID s = 0;    return s; }
        inline float&   GrabOffset(void) noexcept { static float   s = 0.0f; return s; }
        inline bool     isActive  (void) noexcept { return ActiveId() != 0; }

        struct result
        {
            bool    m_bHovered = false;
            bool    m_bActive  = false;
            bool    m_bChanged = false;     // dragged this frame: m_T is the new position along the axis
            float   m_T        = 0.0f;
            ImVec2  m_Screen   = {};
        };

        // Where along the line (Origin + t * Dir, Dir unit) the mouse ray passes closest. False when the
        // ray runs parallel to the line - no meaningful answer then.
        inline bool MouseParam(const view& V, const xmath::fvec3& Origin, const xmath::fvec3& Dir, float& OutT) noexcept
        {
            const ImVec2 M = ImGui::GetIO().MousePos;
            const xmath::fvec3 D  = V.m_RayDir(M.x, M.y).NormalizeCopy();
            const xmath::fvec3 W  = V.m_Eye - Origin;
            const float b = D.Dot(Dir), d = D.Dot(W), e = Dir.Dot(W);
            const float Denom = 1.0f - b * b;
            if (Denom < 1.0e-5f) return false;
            OutT = (e - b * d) / Denom;
            return true;
        }

        // One handle: a dot at Origin + T * Dir. Left-drag it and the returned m_T follows the mouse along
        // the line (without a jump at the grab point).
        inline result Axis(const view& V, ImGuiID Id, const xmath::fvec3& Origin, const xmath::fvec3& Dir, float T) noexcept
        {
            result R;
            ImDrawList& DL = *ImGui::GetWindowDrawList();
            if (!V.Project(Origin + Dir * T, R.m_Screen)) return R;

            const ImVec2 M = ImGui::GetIO().MousePos;
            const bool bOtherBusy = (ActiveId() != 0 && ActiveId() != Id) || ImGuizmo::IsUsingAny();
            R.m_bHovered = !bOtherBusy && ImGui::IsWindowHovered()
                        && (R.m_Screen.x - M.x) * (R.m_Screen.x - M.x) + (R.m_Screen.y - M.y) * (R.m_Screen.y - M.y) <= 8.0f * 8.0f;

            if (ActiveId() == 0 && R.m_bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                float Grab;
                if (MouseParam(V, Origin, Dir, Grab)) { ActiveId() = Id; GrabOffset() = Grab - T; }
            }
            if (ActiveId() == Id)
            {
                R.m_bActive = true;
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) { ActiveId() = 0; R.m_bActive = false; }
                else if (float Now; MouseParam(V, Origin, Dir, Now)) { R.m_T = Now - GrabOffset(); R.m_bChanged = true; }
            }
            if (R.m_bHovered || R.m_bActive) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            const ImVec4 A = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
            const ImU32 Fill = R.m_bActive ? ImGui::ColorConvertFloat4ToU32(A) : R.m_bHovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(200, 200, 200, 255);
            DL.AddCircleFilled(R.m_Screen, 6.5f, IM_COL32(0, 0, 0, 255));
            DL.AddCircleFilled(R.m_Screen, 5.2f, Fill);
            return R;
        }

        // A small readout next to a handle while it is being dragged ("Radius 0.75").
        inline void Label(const result& R, const char* pText) noexcept
        {
            ImDrawList& DL = *ImGui::GetWindowDrawList();
            const ImVec2 P(R.m_Screen.x + 12.0f, R.m_Screen.y - 22.0f);
            const ImVec2 S = ImGui::CalcTextSize(pText);
            DL.AddRectFilled(ImVec2(P.x - 5.0f, P.y - 2.0f), ImVec2(P.x + S.x + 5.0f, P.y + S.y + 2.0f), IM_COL32(20, 20, 20, 220), 3.0f);
            DL.AddText(P, IM_COL32(240, 240, 240, 255), pText);
        }
    }

    //--------------------------------------------------------------------------------------------
    // The component instance a tool works on - live pool pointers, valid for the current call only.
    //--------------------------------------------------------------------------------------------
    struct target
    {
        void*                        m_pData      = nullptr;
        const xlioncore::transform*  m_pTransform = nullptr;   // null if the entity has none (tools may then skip)
        bool                         m_bSelected  = true;
    };

    //--------------------------------------------------------------------------------------------
    // A component's viewport tool.
    //--------------------------------------------------------------------------------------------
    struct tool
    {
        std::uint64_t       m_ComponentGuid;
        std::string_view    m_ArrayPath;    // Inspector path of the edited array, e.g. "PhysicsColliderBox/Boxes"
        const char*         m_pLabel;       // "Edit Collider" - Inspector toggle, overlay title, undo step name

        constexpr tool(std::uint64_t ComponentGuid, std::string_view ArrayPath, const char* pLabel) noexcept
            : m_ComponentGuid(ComponentGuid), m_ArrayPath(ArrayPath), m_pLabel(pLabel) {}
        virtual ~tool() = default;

        virtual int  getElementCount (const target& Target) const noexcept = 0;

        // Which of Move / Rotate / Resize make sense for this shape (a sphere has no rotation to edit).
        virtual bool SupportsMode    (mode) const noexcept { return true; }

        // Always-on visualization while the owning entity is selected (e.g. the green collider wire).
        // EditedElements tells which elements are in edit mode (they may want a stronger look).
        virtual void DrawSelected    (const view&, ImDrawList&, const target&, std::span<const int> EditedElements) const noexcept = 0;

        // Handles for one element in edit mode (called inside ImGuizmo::PushID(Element)). Returns true
        // while the mouse is over a handle or dragging, so the viewport doesn't pick/orbit under it.
        virtual bool EditElement     (const view&, target&, int Element, mode Mode) const noexcept = 0;
    };

    inline std::vector<const tool*>& Registry() noexcept
    {
        static std::vector<const tool*> s_Tools;
        return s_Tools;
    }

    struct registration
    {
        explicit registration(const tool& Tool) noexcept { Registry().push_back(&Tool); }
    };

    inline const tool* FindTool(std::uint64_t ComponentGuid, std::string_view ArrayPath) noexcept
    {
        for (auto* p : Registry())
            if (p->m_ComponentGuid == ComponentGuid && p->m_ArrayPath == ArrayPath) return p;
        return nullptr;
    }

    //--------------------------------------------------------------------------------------------
    // The Level session's active edit. One tool/entity at a time (like Unity), any number of that
    // component's elements.
    //--------------------------------------------------------------------------------------------
    struct editor
    {
        const tool*                         m_pTool     = nullptr;
        xecs::scene::guid                   m_Scene     = {};
        xecs::scene::permanent_id           m_Id        = xecs::scene::invalid_permanent_id_v;
        std::vector<int>                    m_Elements;
        mode                                m_Mode      = mode::RESIZE;
        bool                                m_bWasUsing = false;
        xscene::commands::property_snapshot m_Before;   // component state when the current drag started

        bool isActive(void) const noexcept { return m_pTool != nullptr; }

        bool isEditing(const tool& Tool, xecs::scene::guid Scene, xecs::scene::permanent_id Id, int Element) const noexcept
        {
            return m_pTool == &Tool && m_Scene == Scene && m_Id == Id && std::ranges::find(m_Elements, Element) != m_Elements.end();
        }

        void End(void) noexcept
        {
            handle::ActiveId() = 0;
            m_pTool     = nullptr;
            m_Elements.clear();
            m_bWasUsing = false;
        }

        void Toggle(const tool& Tool, xecs::scene::guid Scene, xecs::scene::permanent_id Id, int Element) noexcept
        {
            if (m_pTool != &Tool || m_Scene != Scene || m_Id != Id)
            {
                End();
                m_pTool = &Tool;
                m_Scene = Scene;
                m_Id    = Id;
                m_Mode  = mode::RESIZE;
            }
            if (auto It = std::ranges::find(m_Elements, Element); It != m_Elements.end()) m_Elements.erase(It);
            else                                                                          m_Elements.push_back(Element);
            if (m_Elements.empty()) End();
        }

        // Inspector hook (xproperty::inspector::m_OnArrayElementRender) - the per-element toggle,
        // drawn in the element row's right column.
        void RenderInspectorToggle(const xscene::scene_state& State, const xecs::component::type::info& Info, std::string_view ArrayPath, int Index, bool bReadOnly) noexcept
        {
            const tool* pTool = FindTool(Info.m_Guid.m_Value, ArrayPath);
            if (!pTool) return;

            const bool bOn = isEditing(*pTool, State.m_SelectedEntityScene, State.m_SelectedEntityId, Index);
            ImGui::PushID(Index);
            ImGui::BeginDisabled(bReadOnly);
            if (bOn)
            {
                // Pressed-toggle look from the theme accent (CheckMark) - the Inspector overrides the
                // Header colors with neutral greys, so those can't carry "on" here.
                const ImVec4 A = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(A.x * 0.40f, A.y * 0.40f, A.z * 0.40f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(A.x * 0.50f, A.y * 0.50f, A.z * 0.50f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border,        A);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            if (ImGui::Button(std::format("\xEE\x9C\x8F  {}", pTool->m_pLabel).c_str()))     // Segoe MDL2 "Edit"
                Toggle(*pTool, State.m_SelectedEntityScene, State.m_SelectedEntityId, Index);
            if (bOn) { ImGui::PopStyleVar(); ImGui::PopStyleColor(3); }
            ImGui::EndDisabled();
            xproperty::inspector::Tooltip((bOn ? std::format("{} in the Scene view\nW Move   E Rotate   R Resize   Esc Done\nCtrl snaps", pTool->m_pLabel) : std::format("{} in the Scene view", pTool->m_pLabel)).c_str(), true);
            ImGui::PopID();
        }

        // The Level/Viewport tool actions (Q W E R, the toolbar) land here while a tool is editing: W/E/R switch the mode
        // (when the tool has it), Q finishes. ToolIndex is the scene tool index: 0 = Q, 1 = W, 2 = E, 3 = R.
        void SetToolIndex(int ToolIndex) noexcept
        {
            if (!isActive()) return;
            if (ToolIndex == 0) { End(); return; }
            const auto Mode = static_cast<mode>(ToolIndex - 1);
            if (m_pTool->SupportsMode(Mode)) m_Mode = Mode;
        }

        // Esc finishes the tool - called only while the viewport owns the keyboard.
        void HandleHotkeys(void) noexcept
        {
            if (isActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) End();
        }

        // Draws every registered tool's visualization for the selected entity and runs the active
        // edit's handles. Returns true while a handle is hovered or dragged. Must run inside the
        // viewport window after ImGuizmo::BeginFrame/SetRect.
        bool Run(xscene::scene_context& Ed, bool bIsPlaying, const view& View) noexcept
        {
            auto& State = Ed.m_State;
            if (m_pTool && (bIsPlaying || State.m_SelectedEntityScene != m_Scene || State.m_SelectedEntityId != m_Id)) End();
            if (State.m_SelectedEntityId == xecs::scene::invalid_permanent_id_v) return false;

            const auto* pXform = xscene::commands::ResolveTransform(Ed, State.m_SelectedEntityScene, State.m_SelectedEntityId);
            ImDrawList& DL          = *ImGui::GetWindowDrawList();
            bool        bInteracting = false;

            for (const tool* pTool : Registry())
            {
                const auto Resolved = xscene::commands::ResolvePropertyTarget(Ed, State.m_SelectedEntityScene, State.m_SelectedEntityId, pTool->m_ComponentGuid);
                const bool bMine    = m_pTool == pTool;
                if (!Resolved.m_pInfo || !Resolved.m_pInstance)
                {
                    if (bMine) End();               // component removed while editing
                    continue;
                }

                target Target{ Resolved.m_pInstance, pXform };
                if (bMine)
                {
                    const int Count = pTool->getElementCount(Target);
                    std::erase_if(m_Elements, [Count](int E) noexcept { return E >= Count; });   // element deleted while editing
                    if (m_Elements.empty()) { End(); }
                }

                pTool->DrawSelected(View, DL, Target, m_pTool == pTool ? std::span<const int>(m_Elements) : std::span<const int>{});
                if (m_pTool != pTool) continue;

                if (!pTool->SupportsMode(m_Mode)) m_Mode = mode::RESIZE;

                if (!m_bWasUsing) m_Before = xscene::commands::SnapshotProperties(Resolved.m_pInstance, *Resolved.m_pInfo->m_pPropertyTable);

                for (const int E : m_Elements)
                {
                    ImGuizmo::PushID(E);
                    bInteracting |= pTool->EditElement(View, Target, E, m_Mode);
                    ImGuizmo::PopID();
                }

                const bool bUsing = ImGuizmo::IsUsingAny() || handle::isActive();
                if (m_bWasUsing && !bUsing) Commit(Ed, Resolved);
                m_bWasUsing = bUsing;
            }
            return bInteracting;
        }

        // The small mode bar in the viewport's top-left corner while editing.
        void RenderOverlay(const view& View) noexcept
        {
            if (!isActive()) return;

            // Top-right corner (the left edge is where the Scene toolbar docks). Width computed from the
            // content: an auto-width child placed against the parent's right edge shrinks to fit the
            // space left there instead.
            const ImGuiStyle& Style      = ImGui::GetStyle();
            const ImVec2      Padding    = ImVec2(6.0f, 5.0f);
            const float       ModeW      = 58.0f;
            const std::string Count      = m_Elements.size() > 1 ? std::format("({})", m_Elements.size()) : std::string{};
            const float       Width      = Padding.x * 2.0f + 2.0f
                                         + ImGui::CalcTextSize(m_pTool->m_pLabel).x
                                         + (Count.empty() ? 0.0f : Style.ItemSpacing.x + ImGui::CalcTextSize(Count.c_str()).x)
                                         + 12.0f + ModeW * 3.0f + 2.0f * 2.0f
                                         + 10.0f + ImGui::CalcTextSize("Done").x + Style.FramePadding.x * 2.0f;

            ImGui::SetCursorScreenPos(ImVec2(View.m_Min.x + View.m_Size.x - Width - 10.0f, View.m_Min.y + 10.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Padding);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(Style.Colors[ImGuiCol_WindowBg].x, Style.Colors[ImGuiCol_WindowBg].y, Style.Colors[ImGuiCol_WindowBg].z, 0.92f));
            if (ImGui::BeginChild("##ViewportToolOverlay", ImVec2(Width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(m_pTool->m_pLabel);
                if (!Count.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", Count.c_str()); }
                ImGui::SameLine(0.0f, 12.0f);

                auto ModeButton = [&](const char* pLabel, mode M, const char* pTip)
                {
                    const bool bOn = m_Mode == M;
                    ImGui::BeginDisabled(!m_pTool->SupportsMode(M));
                    if (bOn) ImGui::PushStyleColor(ImGuiCol_Button, Style.Colors[ImGuiCol_Header]);
                    if (ImGui::Button(pLabel, ImVec2(ModeW, 0.0f))) m_Mode = M;
                    if (bOn) ImGui::PopStyleColor();
                    ImGui::EndDisabled();
                    xproperty::inspector::Tooltip(pTip, true);
                    ImGui::SameLine(0.0f, 2.0f);
                };
                ModeButton("Move",   mode::MOVE,   "Move the center (W)");
                ModeButton("Rotate", mode::ROTATE, "Rotate (E)");
                ModeButton("Resize", mode::RESIZE, "Drag a face or corner handle to resize (R)");

                ImGui::SameLine(0.0f, 10.0f);
                if (ImGui::Button("Done")) End();
                xproperty::inspector::Tooltip("Finish editing (Esc)");
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

    private:

        // Commits what the drag changed as one undo step. If the edit is refused (read-only level,
        // edit gate), the live data goes back to how it was.
        void Commit(xscene::scene_context& Ed, const xscene::commands::resolved_property_target& Resolved) noexcept
        {
            const auto After = xscene::commands::SnapshotProperties(Resolved.m_pInstance, *Resolved.m_pInfo->m_pPropertyTable);
            const auto Cmds  = xscene::commands::MakeSetPropertyCommands(m_Scene, m_Id, m_pTool->m_ComponentGuid, m_Before, After);
            if (Cmds.empty() || xeditor::RunGroup(Ed.m_Undo, m_pTool->m_pLabel, Cmds)) return;

            for (const auto& B : m_Before)
            {
                if (B.m_Path.ends_with("[]")) continue;
                const auto It = std::ranges::find(After, B.m_Path, &xscene::commands::property_snapshot_entry::m_Path);
                if (It != After.end() && It->m_Value != B.m_Value)
                    xscene::commands::SetLivePropertyValue(Resolved, B.m_Path, B.m_TypeGuid, B.m_Value);
            }
        }
    };
}

#endif // XLEVEL_VIEWPORT_TOOLS_H
