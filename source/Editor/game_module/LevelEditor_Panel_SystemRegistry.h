#ifndef XLVL_NEW_LevelEditor_PANEL_SYSTEM_REGISTRY_H
#define XLVL_NEW_LevelEditor_PANEL_SYSTEM_REGISTRY_H
#pragma once

// Extracted from LevelEditor_Kit.h (mechanical move, phase 1 of the kit split - see that
// file's own top comment). Meant to be included via the umbrella (LevelEditor_Kit.h) only -
// not designed to be included standalone.
//
// LevelEditor_Theme.h included directly (not just relying on LevelEditor_Main.cpp's own later include)
// for UnityCheckbox - same self-sufficiency reasoning as level/LevelEditor_Panel_LevelTree.h's own top comment.
#include "source/Editors/LevelEditor/LevelEditor_Theme.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "plugins/xscene.plugin/source/Editor/xscene_system_usage.h"
#include "plugins/xscene.plugin/source/Editor/xscene_component_display.h"

namespace xlevel
{
    //---------------------------------------------------------------------------
    // Tooltip listing one system's declared components - how each one matches (must / one of /
    // none of / if present) and how it's accessed - tagging builder components, which exist only
    // while an entity is being created (doc/xecs_builder_components.md).
    //---------------------------------------------------------------------------
    inline void RenderSystemAccessTooltip(xlioncore::xECSEditor& Ecs, const xecs::system::type::info& Info, const char* pHeader) noexcept
    {
        xeditor::hint::PlaceAwayFromEdges(16.0f, ImVec2(380.0f, 220.0f));
        if (!ImGui::BeginTooltip()) return;
        ImGui::TextUnformatted(pHeader);
        // where the system is defined: the module and the file
        if (const auto From = xscene::DescribeSource(xscene::SourceOfType(true, Info.m_Guid.m_Value)); !From.empty())
            ImGui::TextDisabled("%s", From.c_str());
        ImGui::Separator();

        if (Info.m_Access.empty())
        {
            ImGui::TextDisabled("No declared components");
        }
        else if (ImGui::BeginTable("##Access", 3, ImGuiTableFlags_SizingFixedFit))
        {
            for (auto& Access : Info.m_Access)
            {
                using match  = xecs::system::type::match;
                using access = xecs::system::type::access;
                const char* pMatch  = Access.m_Match == match::MUST   ? "Must"
                                    : Access.m_Match == match::ONE_OF ? "One of"
                                    : Access.m_Match == match::NONE_OF? "None of"
                                    :                                    "If present";
                const char* pAccess = Access.m_Access == access::WRITE ? "write"
                                    : Access.m_Access == access::READ  ? "read"
                                    :                                     "";
                const auto* pComponent = Ecs.FindComponentType(Access.m_ComponentGuid);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%s", pMatch);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(Access.m_pComponentName ? Access.m_pComponentName : "?");
                if (pComponent && pComponent->m_bBuilder)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(Builder Component)");
                }
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", pAccess);
            }
            ImGui::EndTable();
        }
        ImGui::EndTooltip();
    }

    //---------------------------------------------------------------------------
    // System Registry panel - lists every registered Update system (xecs::system::mgr::
    // GetUpdateSystemRows), letting the user drag-reorder (via each row's own name Selectable, grip
    // glyph included - see its own comment below) and enable/disable each one directly, making
    // execution order/enabled DATA instead of whatever order GameMgr.RegisterSystems<...>() happened
    // to list them in at compile time. While State.isPlaying(), edits still go through the exact same
    // mgr calls - they're already only ever transient in that state, since GameMgr.Stop() calls
    // RestoreFromSnapshot() on the way out - this panel just surfaces that distinction with a note so
    // it isn't a silent surprise later.
    //---------------------------------------------------------------------------
    void RenderSystemRegistryPanel(xecs::game_mgr::instance& GameMgr, xlevel::level_state& State, const char* pWindowName) noexcept
    {
        // Stacked below the Entity Properties panel (18,18 / 480x500) rather than at the Level
        // Editor panel's own (915,18) spot, so the two don't land on top of each other on a
        // completely fresh imgui.ini - purely a first-launch default, freely rearrangeable/dockable
        // afterward like every other panel here.
        ImGui::SetNextWindowPos(ImVec2(18, 530), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(480, 220), ImGuiCond_FirstUseEver);
        const bool bWindowVisible = ImGui::Begin(pWindowName);
        xeditor::diagnostics::Log("window begin: %s visible=%d", pWindowName, bWindowVisible ? 1 : 0);
        if (bWindowVisible)
        {
            if (State.isPlaying())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
                ImGui::TextWrapped("Play Mode - changes here are temporary and revert on Stop.");
                ImGui::PopStyleColor();
                ImGui::Separator();
            }

            bool bChanged = false;

            // Update systems on top, builder systems below, split by a draggable divider.
            const bool   bHasBuilders  = !GameMgr.m_SystemMgr.m_BuilderSystems.empty();
            static float s_TopFraction = 0.65f;
            const float  TotalHeight   = ImGui::GetContentRegionAvail().y;
            ImGui::BeginChild("##UpdateSystems", ImVec2(0.0f, bHasBuilders ? std::max(40.0f, TotalHeight * s_TopFraction) : 0.0f));

            auto Rows      = GameMgr.m_SystemMgr.GetUpdateSystemRows();
            if (Rows.empty())
            {
                ImGui::TextDisabled("No Update systems registered.");
            }
            // BordersV removed - direct user comparison against Unity's own component header row
            // (checkbox + icon + name, no vertical divider at all between them): a line between the
            // checkbox and its label reads as two unrelated cells stitched together, not one row.
            // CellPadding.x tightened locally (not globally - every other table in the app still wants
            // the ambient value) - the gap between the checkbox and the name text is column0's own
            // trailing padding PLUS column1's own leading padding, so both matter here.
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2.0f, ImGui::GetStyle().CellPadding.y));
            ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 8.0f);     // the same as the Level tree: see xlevel_panel_level_tree.h
            if (ImGui::BeginTable("SystemRegistry", 3, ImGuiTableFlags_RowBg))
            {
                // Just wide enough for the checkbox itself + a hair of breathing room - not an
                // arbitrary wide column. Direct user comparison against Unity's own tightly-grouped
                // checkbox+label found first +12, then +4, still too wide. Sized off GetFontSize()
                // directly, not GetFrameHeight() - the checkbox itself is drawn with FramePadding
                // pushed to (0,0) below, so using the ambient (unpushed) FrameHeight here would size
                // this column for a bigger box than the one actually drawn.
                // Only the Name column indents (the tree lives there): a table's indent is enabled for column 0 alone unless said otherwise.
                ImGui::TableSetupColumn("##Grip",  ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_IndentDisable, 8.0f);
                ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_IndentDisable, ImGui::GetFontSize() + 1.0f);
                ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_IndentEnable);

                // A dragged system dropped on the system Target becomes its sibling and takes its place.
                auto DropOnSystem = [&](xecs::system::type::guid Source, const xecs::system::update_system_row& Target) noexcept
                {
                    auto& Mgr = GameMgr.m_SystemMgr;
                    if (Source == Target.m_Guid) return;
                    const auto Before = Mgr.GetUpdateSystemRows();
                    const xecs::system::update_system_row* pSrc = nullptr;
                    for (auto& R : Before) if (R.m_Guid == Source) pSrc = &R;
                    if (!pSrc) return;
                    if (pSrc->m_ParentGuid != Target.m_ParentGuid || pSrc->m_ParentConnector != Target.m_ParentConnector)
                        if (!Mgr.SetUpdateSystemParent(Source, Target.m_ParentGuid, Target.m_ParentConnector)) return;
                    // the order among the systems of that connector (or of the top level): one step at a time toward the target
                    for (int Guard = 0; Guard < 64; ++Guard)
                    {
                        const auto Now = Mgr.GetUpdateSystemRows();
                        std::vector<xecs::system::type::guid> Siblings;
                        for (auto& R : Now) if (R.m_ParentGuid == Target.m_ParentGuid && R.m_ParentConnector == Target.m_ParentConnector) Siblings.push_back(R.m_Guid);
                        const auto IndexOf = [&](xecs::system::type::guid G) { return static_cast<int>(std::find(Siblings.begin(), Siblings.end(), G) - Siblings.begin()); };
                        const int is = IndexOf(Source), it = IndexOf(Target.m_Guid);
                        if (is == it || is >= static_cast<int>(Siblings.size()) || it >= static_cast<int>(Siblings.size())) break;
                        Mgr.MoveUpdateSystem(Source, it > is ? 1 : -1);
                    }
                };
                auto Payload = [&]() noexcept -> std::optional<xecs::system::type::guid>
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LevelEditor_SYSTEM_REORDER"))
                    {
                        IM_ASSERT(payload->DataSize == sizeof(xecs::system::type::guid));
                        return *reinterpret_cast<const xecs::system::type::guid*>(payload->Data);
                    }
                    return std::nullopt;
                };

                // The rows as a tree: the update systems that run at the top level in order; a system with connectors is a node that opens to its
                // The icons come from the editor's icon font (Segoe MDL2 Assets), as the Level tree's do: a gear for a system, a link for a connector
                // (where other systems link in). The colors are the theme's: nothing here picks its own.
                auto Glyph = [](unsigned CodePoint) noexcept
                {
                    std::string s;
                    s += static_cast<char>(0xE0 | (CodePoint >> 12));
                    s += static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F));
                    s += static_cast<char>(0x80 | (CodePoint & 0x3F));
                    return s;
                };
                // The drag handle: two columns of three dots in the first cell. The whole row drags; this only says that it does.
                auto Grip = []() noexcept
                {
                    ImGui::TableSetColumnIndex(0);
                    const ImVec2 P = ImGui::GetCursorScreenPos();
                    const float  H = ImGui::GetFontSize();
                    const ImU32  Col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    for (int y = 0; y < 3; ++y) for (int x = 0; x < 2; ++x)
                        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(P.x + 1.0f + x * 3.0f, P.y + H * 0.25f + y * 3.5f), ImVec2(P.x + 2.5f + x * 3.0f, P.y + H * 0.25f + y * 3.5f + 1.5f), Col);
                };
                // Where the system comes from: the module that defines it, as a quiet tag at the right of the row; the right-click menu opens its file.
                // Drawn with the draw list over the row (it is no item, so it takes nothing from the drag and the drop of the row).
                auto DrawSourceTag = [](bool bSystem, std::uint64_t Guid) noexcept
                {
                    const auto Source = xscene::SourceOfType(bSystem, Guid);
                    if (Source.m_bKnown && Source.m_Module != 0 && !Source.m_ModuleName.empty())
                    {
                        const ImVec2 Min = ImGui::GetItemRectMin(), Max = ImGui::GetItemRectMax();
                        const float  W = ImGui::CalcTextSize(Source.m_ModuleName.c_str()).x;
                        if (Max.x - Min.x > W + 160.0f)
                            ImGui::GetWindowDrawList()->AddText(ImVec2(Max.x - W - 6.0f, Min.y + (Max.y - Min.y - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), Source.m_ModuleName.c_str());
                    }
                    if (Source.m_bKnown && !Source.m_bBuiltIn && !Source.m_Path.empty() && ImGui::BeginPopupContextItem("##typesource"))
                    {
                        if (ImGui::MenuItem(std::format("Open {}", Source.m_File.empty() ? Source.m_Path : Source.m_File).c_str(), nullptr, false, static_cast<bool>(xscene::g_OpenTypeSource)))
                            xscene::g_OpenTypeSource(Source);
                        ImGui::EndPopup();
                    }
                };
                constexpr unsigned kSystemIcon    = 0xE713;
                constexpr unsigned kConnectorIcon = 0xE71B;    // Link: the same glyph the Level tree gives its Dependencies



                // The rows as a tree: the update systems that run at the top level in order; a system with connectors is a node that opens to its
                // connectors (the places where other systems connect, named by the system), and a connector is a node that opens to the systems
                // connected to it.
                auto DrawSystems = [&](auto&& Self, xecs::system::type::guid Parent, int Connector) noexcept -> void
                {
                    for (std::size_t r = 0; r < Rows.size(); ++r)
                    {
                        if (Rows[r].m_ParentGuid != Parent || (!Parent.empty() && Rows[r].m_ParentConnector != Connector)) continue;
                        const auto  Row = Rows[r];                                  // a copy: a drop below changes the rows
                        const auto  Connectors = GameMgr.m_SystemMgr.GetConnectors(Row.m_Guid);
                        const std::size_t i = r;                                    // Rows is index-aligned with m_UpdaterSystems (see GetUpdateSystemRows)

                        ImGui::PushID(static_cast<int>(Row.m_Guid.m_Value & 0x7FFFFFFF));
                        ImGui::TableNextRow();

                        Grip();
                        ImGui::TableSetColumnIndex(1);
                        bool bEnabled = Row.m_bEnabled;
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                        if (level_editor::theme::UnityCheckbox("##Enabled", &bEnabled))
                        {
                            GameMgr.m_SystemMgr.SetUpdateSystemEnabled(Row.m_Guid, bEnabled);
                            bChanged = true;
                        }
                        ImGui::PopStyleVar();

                        // ONE real, visible item is both the drag source and the drop target (two overlapping interactive items did not work: see
                        // documentation/ImGui/overlapping_invisible_buttons.md): the node itself, which spans the row; the arrow opens it.
                        ImGui::TableSetColumnIndex(2);
                        if (!bEnabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        const bool bOpen = ImGui::TreeNodeEx(std::format("{} {}", Glyph(kSystemIcon), Row.m_pName ? Row.m_pName : "(unnamed system)").c_str()
                            , ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen
                            | (Connectors.empty() ? ImGuiTreeNodeFlags_Leaf : 0));
                        if (!bEnabled) ImGui::PopStyleColor();
                        DrawSourceTag(true, Row.m_Guid.m_Value);
                        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && i < GameMgr.m_SystemMgr.m_UpdaterSystems.size())
                            RenderSystemAccessTooltip(xlioncore::Ecs(GameMgr), *GameMgr.m_SystemMgr.m_UpdaterSystems[i].first
                                , std::format("{}  (runs #{})", Row.m_pName ? Row.m_pName : "(unnamed system)", i).c_str());

                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                        {
                            ImGui::SetDragDropPayload("LevelEditor_SYSTEM_REORDER", &Row.m_Guid, sizeof(Row.m_Guid));
                            ImGui::TextUnformatted(Row.m_pName ? Row.m_pName : "(unnamed system)");
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (auto Source = Payload()) { DropOnSystem(*Source, Row); bChanged = true; }
                            ImGui::EndDragDropTarget();
                        }

                        if (bOpen)
                        {
                            for (int c = 0; c < static_cast<int>(Connectors.size()); ++c)
                            {
                                bool bHasChildren = false;
                                for (auto& Other : Rows) if (Other.m_ParentGuid == Row.m_Guid && Other.m_ParentConnector == c) { bHasChildren = true; break; }

                                ImGui::PushID(c);
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(2);
                                const bool bConnectorOpen = ImGui::TreeNodeEx(std::format("{} {}", Glyph(kConnectorIcon), Connectors[c].m_pName).c_str()
                                    , ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen
                                    | (bHasChildren ? 0 : ImGuiTreeNodeFlags_Leaf));
                                if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                                {
                                    ImGui::BeginTooltip();
                                    ImGui::TextUnformatted(std::format("{} - {}", Row.m_pName, Connectors[c].m_pName).c_str());
                                    ImGui::Separator();
                                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                                    ImGui::TextUnformatted(Connectors[c].m_pDescription);
                                    ImGui::PopTextWrapPos();
                                    ImGui::EndTooltip();
                                }
                                if (ImGui::BeginDragDropTarget())                  // dropping a system on a connector connects it
                                {
                                    if (auto Source = Payload())
                                        if (GameMgr.m_SystemMgr.SetUpdateSystemParent(*Source, Row.m_Guid, c)) bChanged = true;
                                    ImGui::EndDragDropTarget();
                                }
                                if (bConnectorOpen)
                                {
                                    Self(Self, Row.m_Guid, c);
                                    ImGui::TreePop();
                                }
                                ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                };
                DrawSystems(DrawSystems, xecs::system::type::guid{}, -1);
                ImGui::EndTable();
            }
            ImGui::PopStyleVar(2); // matches the CellPadding + IndentSpacing pushes above BeginTable - unconditional, since BeginTable can return false

            // Dropping a system here runs it at the top level again (last), in the order of the frame.
            ImGui::Selectable("(drop a system here to run it at the top level)", false, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LevelEditor_SYSTEM_REORDER"))
                    if (GameMgr.m_SystemMgr.SetUpdateSystemParent(*reinterpret_cast<const xecs::system::type::guid*>(payload->Data), xecs::system::type::guid{}, -1)) bChanged = true;
                ImGui::EndDragDropTarget();
            }

            ImGui::EndChild();

            // Builder systems (doc/xecs_builder_components.md) have no order/enable - they run once per
            // entity while it's created, only when builders are on (Play / the game). Hover one for its
            // declared components.
            if (bHasBuilders)
            {
                ImGui::InvisibleButton("##SystemsSplitter", ImVec2(-FLT_MIN, 6.0f));
                if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (ImGui::IsItemActive() && TotalHeight > 0.0f)
                    s_TopFraction = std::clamp(s_TopFraction + ImGui::GetIO().MouseDelta.y / TotalHeight, 0.1f, 0.9f);
                {
                    const ImVec2 Min = ImGui::GetItemRectMin();
                    const ImVec2 Max = ImGui::GetItemRectMax();
                    const float  Y   = (Min.y + Max.y) * 0.5f;
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(Min.x, Y), ImVec2(Max.x, Y), ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive : ImGuiCol_Separator), 2.0f);
                }

                ImGui::TextDisabled("Builders - run once per entity at creation (Play / game)");
                if (ImGui::BeginListBox("##Builders", ImVec2(-FLT_MIN, -FLT_MIN)))
                {
                    for (auto& Builder : GameMgr.m_SystemMgr.m_BuilderSystems)
                    {
                        ImGui::Selectable(Builder.first->m_pName);
                        if (ImGui::IsItemHovered()) RenderSystemAccessTooltip(xlioncore::Ecs(GameMgr), *Builder.first, Builder.first->m_pName);
                        if (const auto Source = xscene::SourceOfType(true, Builder.first->m_Guid.m_Value); Source.m_bKnown && !Source.m_bBuiltIn && !Source.m_Path.empty() && ImGui::BeginPopupContextItem("##typesource"))
                        {
                            if (ImGui::MenuItem(std::format("Open {}", Source.m_File.empty() ? Source.m_Path : Source.m_File).c_str(), nullptr, false, static_cast<bool>(xscene::g_OpenTypeSource)))
                                xscene::g_OpenTypeSource(Source);
                            ImGui::EndPopup();
                        }
                    }
                    ImGui::EndListBox();
                }
            }

            // Persisted immediately, but only on an actual edit this frame (not every frame the
            // window happens to be open) - mirrors Unity's own Script Execution Order behavior.
            // While playing, Move/SetEnabled above only ever mutate the live, in-memory order;
            // GameMgr.Stop()'s RestoreFromSnapshot() discards it, so writing to disk here would just
            // save a value about to be thrown away.
            if (bChanged && !State.isPlaying())
            {
                if (auto Err = GameMgr.m_SystemMgr.Save(); Err)
                    xeditor::NotifyToast(std::format("Failed to save System Registry order: {}", Err.getMessage()));
            }
        }
        ImGui::End();
        xeditor::diagnostics::Log("window end: %s", pWindowName);
    }
} // namespace xlevel

#endif // XLVL_NEW_LevelEditor_PANEL_SYSTEM_REGISTRY_H
