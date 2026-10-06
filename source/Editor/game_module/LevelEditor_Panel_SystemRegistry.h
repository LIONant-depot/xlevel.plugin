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
#include "dependencies/xeditor/include/xeditor/widgets.h"
#include "plugins/xscene.plugin/source/Editor/xscene_system_usage.h"
#include "plugins/xscene.plugin/source/Editor/xscene_component_display.h"

namespace xlevel
{
    // The right-click menu of a system (or a component) that a script module defines: the file it is defined in, shown in Visual Studio (the solution of the game project: where the scripts are
    // written) or in the editor of the module.
    inline void RenderTypeSourceMenu(const xscene::type_source& Source) noexcept
    {
        const std::string File = Source.m_File.empty() ? Source.m_Path : Source.m_File;
        ImGui::TextDisabled("%s", File.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Open in Visual Studio", nullptr, false, static_cast<bool>(xscene::g_OpenTypeSourceInVisualStudio)))
            (void)xscene::g_OpenTypeSourceInVisualStudio(Source, false);
        if (ImGui::IsItemHovered()) xeditor::hint::Text("Opens the file in the Visual Studio of this Level's game project (the solution of the scripts), or starts it");
        if (ImGui::MenuItem(Source.m_ModuleName.empty() ? "Open in the module editor" : std::format("Open in the {} module editor", Source.m_ModuleName).c_str(), nullptr, false, static_cast<bool>(xscene::g_OpenTypeSource)))
            (void)xscene::g_OpenTypeSource(Source);
    }

    //---------------------------------------------------------------------------
    // Tooltip listing one system's declared components - how each one matches (must / one of /
    // none of / if present) and how it's accessed - tagging builder components, which exist only
    // while an entity is being created (doc/xecs_builder_components.md).
    //---------------------------------------------------------------------------
    inline void RenderSystemAccessTooltip(xlioncore::xECSEditor& Ecs, const xscene::component_display& Display, const xecs::system::type::info& Info, const char* pHeader
                                        , const char* pEmpty = "No declared components", const char* pAbout = nullptr) noexcept
    {
        if (!xeditor::hint::BeginCard()) return;
        ImGui::TextUnformatted(pHeader);
        // where the system is defined: the module and the file
        if (const auto From = xscene::DescribeSource(Display.SourceOf(true, Info.m_Guid.m_Value)); !From.empty())
            ImGui::TextDisabled("%s", From.c_str());
        if (pAbout && pAbout[0])                 // what it handles, said by the event
            xeditor::hint::Wrapped(pAbout);
        ImGui::Separator();

        if (Info.m_Access.empty())
        {
            ImGui::TextDisabled("%s", pEmpty);
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
        xeditor::hint::EndCard();
    }

    //---------------------------------------------------------------------------
    // The event handlers: the systems that run when something HAPPENS (the physics tells that a shape touched a sensor, ...), not every frame, so they are not in the order above. Grouped by the
    // event they handle: the event is a node (what it tells and when is its tooltip, the number of handlers is beside its name), and each handler is a row under it that says, like any system,
    // what it reads and writes when hovered, and where it is defined. An event nobody handles is listed too (it is there to be handled).
    //---------------------------------------------------------------------------
    // The header of a section (Event handlers, Notifiers, Builders): the grey of the theme's buttons, not the blue of a selection - a section is not selected. Only the header has it: what is inside keeps the
    // colors of the theme.
    inline bool RenderSystemRegistrySectionHeader(const std::string& Label) noexcept
    {
        ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0x58 / 255.0f, 0x58 / 255.0f, 0x58 / 255.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0x66 / 255.0f, 0x66 / 255.0f, 0x66 / 255.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0x72 / 255.0f, 0x72 / 255.0f, 0x72 / 255.0f, 1.0f));
        const bool bOpen = ImGui::CollapsingHeader(Label.c_str());      // closed until the person opens it
        ImGui::PopStyleColor(3);
        return bOpen;
    }

    inline void RenderEventHandlers(xecs::game_mgr::instance& GameMgr, const xscene::component_display& Display, const std::vector<xscene::system_usage::event_group>& Groups) noexcept
    {
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 8.0f);
        if (ImGui::BeginTable("##EventHandlers", 1, ImGuiTableFlags_RowBg))
        {
            for (std::size_t g = 0; g < Groups.size(); ++g)
            {
                const auto& Group = Groups[g];
                ImGui::PushID(static_cast<int>(g));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (Group.m_Handlers.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                const bool bOpen = ImGui::TreeNodeEx(std::format("\xEE\xA5\x85 {}  ({})", Group.m_Name, Group.m_Handlers.empty() ? std::string("no handlers") : std::format("{}", Group.m_Handlers.size())).c_str()   // E945: the lightning bolt of the icon font
                    , ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow | (Group.m_Handlers.empty() ? ImGuiTreeNodeFlags_Leaf : 0));
                if (Group.m_Handlers.empty()) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && !Group.m_Help.empty())
                {
                    if (xeditor::hint::BeginCard())
                    {
                        ImGui::TextUnformatted(Group.m_Name.c_str());
                        ImGui::Separator();
                        xeditor::hint::Wrapped(Group.m_Help);
                        xeditor::hint::EndCard();
                    }
                }
                if (bOpen)
                {
                    for (const auto* pHandler : Group.m_Handlers)
                    {
                        ImGui::PushID(static_cast<int>(pHandler->m_Guid.m_Value & 0x7FFFFFFF));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TreeNodeEx(pHandler->m_pName ? pHandler->m_pName : "(unnamed system)", ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet);
                        const ImVec2 Min = ImGui::GetItemRectMin(), Max = ImGui::GetItemRectMax();
                        const auto Source = Display.SourceOf(true, pHandler->m_Guid.m_Value);
                        if (Source.m_bKnown && Source.m_Module != 0 && !Source.m_ModuleName.empty())
                        {
                            const float W = ImGui::CalcTextSize(Source.m_ModuleName.c_str()).x;
                            if (Max.x - Min.x > W + 160.0f)
                                ImGui::GetWindowDrawList()->AddText(ImVec2(Max.x - W - 6.0f, Min.y + (Max.y - Min.y - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), Source.m_ModuleName.c_str());
                        }
                        if (ImGui::IsItemHovered())
                            RenderSystemAccessTooltip(xlioncore::Ecs(GameMgr), Display, *pHandler, std::format("{}  (handles {})", pHandler->m_pName ? pHandler->m_pName : "(unnamed system)", Group.m_Name).c_str()
                                , "It runs when the event is raised and does not iterate entities: it reads what the event gives it, and can look entities up", Group.m_Help.c_str());
                        if (Source.m_bKnown && !Source.m_bBuiltIn && !Source.m_Path.empty() && ImGui::BeginPopupContextItem("##typesource"))
                        {
                            RenderTypeSourceMenu(Source);
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }

    //---------------------------------------------------------------------------
    // The notifiers: systems that run when an ENTITY changes - one is created (what initializes it), destroyed, moved to another archetype, or has components added, removed or changed - for the
    // entities that match what the system declares. Grouped by what triggers them; hover one for what it reads and writes.
    //---------------------------------------------------------------------------
    inline const char* NotifierKind(xecs::system::type::id Id) noexcept
    {
        using id = xecs::system::type::id;
        switch (Id)
        {
        case id::NOTIFY_CREATE:             return "an entity is created";
        case id::NOTIFY_DESTROY:            return "an entity is destroyed";
        case id::NOTIFY_MODIFIED:           return "an entity is modified";
        case id::NOTIFY_MOVE_IN:            return "an entity moves into an archetype that matches";
        case id::NOTIFY_MOVE_OUT:           return "an entity moves out of an archetype that matched";
        case id::NOTIFY_COMPONENT_CHANGE:   return "a component of an entity changes";
        case id::NOTIFY_COMPONENT_ADDED:    return "a component is added to an entity";
        case id::NOTIFY_COMPONENT_REMOVE:   return "a component is removed from an entity";
        case id::POOL_FAMILY_CREATE:        return "a pool family is created (a new value of a share component)";
        case id::POOL_FAMILY_DESTROY:       return "a pool family is destroyed";
        default:                            return "an entity changes";
        }
    }

    // xECS keeps the event handlers in the list of the notifiers too: they have their own section, so they are not counted here.
    inline std::size_t CountNotifiers(xecs::game_mgr::instance& GameMgr) noexcept
    {
        using id = xecs::system::type::id;
        std::size_t n = 0;
        for (auto& Notifier : GameMgr.m_SystemMgr.m_NotifierSystems) if (Notifier.first->m_ID != id::GLOBAL_EVENT && Notifier.first->m_ID != id::SYSTEM_EVENT) ++n;
        return n;
    }

    inline void RenderNotifiers(xecs::game_mgr::instance& GameMgr, const xscene::component_display& Display) noexcept
    {
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 8.0f);
        if (ImGui::BeginTable("##Notifiers", 1, ImGuiTableFlags_RowBg))
        {
            using id = xecs::system::type::id;
            for (const id Kind : { id::NOTIFY_CREATE, id::NOTIFY_DESTROY, id::NOTIFY_MODIFIED, id::NOTIFY_MOVE_IN, id::NOTIFY_MOVE_OUT, id::NOTIFY_COMPONENT_CHANGE, id::NOTIFY_COMPONENT_ADDED, id::NOTIFY_COMPONENT_REMOVE, id::POOL_FAMILY_CREATE, id::POOL_FAMILY_DESTROY })
            {
                std::vector<const xecs::system::type::info*> Systems;
                for (auto& Notifier : GameMgr.m_SystemMgr.m_NotifierSystems) if (Notifier.first->m_ID == Kind) Systems.push_back(Notifier.first);
                if (Systems.empty()) continue;
                ImGui::PushID(static_cast<int>(Kind));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool bOpen = ImGui::TreeNodeEx(std::format("When {}  ({})", NotifierKind(Kind), Systems.size()).c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow);
                if (bOpen)
                {
                    for (const auto* pInfo : Systems)
                    {
                        ImGui::PushID(static_cast<int>(pInfo->m_Guid.m_Value & 0x7FFFFFFF));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TreeNodeEx(pInfo->m_pName ? pInfo->m_pName : "(unnamed system)", ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet);
                        const ImVec2 Min = ImGui::GetItemRectMin(), Max = ImGui::GetItemRectMax();
                        const auto Source = Display.SourceOf(true, pInfo->m_Guid.m_Value);
                        if (Source.m_bKnown && Source.m_Module != 0 && !Source.m_ModuleName.empty())
                        {
                            const float W = ImGui::CalcTextSize(Source.m_ModuleName.c_str()).x;
                            if (Max.x - Min.x > W + 160.0f)
                                ImGui::GetWindowDrawList()->AddText(ImVec2(Max.x - W - 6.0f, Min.y + (Max.y - Min.y - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), Source.m_ModuleName.c_str());
                        }
                        if (ImGui::IsItemHovered())
                            RenderSystemAccessTooltip(xlioncore::Ecs(GameMgr), Display, *pInfo, std::format("{}  (runs when {}, for the entities it matches)", pInfo->m_pName ? pInfo->m_pName : "(unnamed system)", NotifierKind(Kind)).c_str());
                        if (Source.m_bKnown && !Source.m_bBuiltIn && !Source.m_Path.empty() && ImGui::BeginPopupContextItem("##typesource"))
                        {
                            RenderTypeSourceMenu(Source);
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
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
    void RenderSystemRegistryPanel(xecs::game_mgr::instance& GameMgr, xlevel::level_state& State, xundo::system& Undo, const xscene::component_display& Display, const char* pWindowName) noexcept
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

            // Every edit of the registry is a command (SetSystemParent, UnplaceSystem, MoveSystem, SetSystemEnabled: undoable, and the ones an AI runs too); the commands save the registry.
            const auto Hex = [](xecs::system::type::guid G) noexcept { return std::format("{:016X}", G.m_Value); };
            const auto RunCommand = [&](const std::string& Line) noexcept { xeditor::Run(Undo, Line); };

            // Update systems on top, builder systems below, split by a draggable divider.
            const bool   bHasBuilders  = !GameMgr.m_SystemMgr.m_BuilderSystems.empty();
            const auto   EventGroups   = xscene::system_usage::EventGroups(GameMgr);
            const bool   bHasEvents    = !EventGroups.empty();
            const std::size_t nNotifiers = CountNotifiers(GameMgr);
            const bool   bHasNotifiers = nNotifiers != 0;
            const bool   bHasBottom    = true;                                              // below the update systems: the unused systems, the event handlers, the notifiers and the builders
            static float s_TopFraction = 0.65f;
            const float  TotalHeight   = ImGui::GetContentRegionAvail().y;
            ImGui::BeginChild("##UpdateSystems", ImVec2(0.0f, bHasBottom ? std::max(40.0f, TotalHeight * s_TopFraction) : 0.0f));

            auto Rows      = GameMgr.m_SystemMgr.GetUpdateSystemRows();

            // The system being dragged (any system row of this panel, placed or not), and why a place would refuse it: a place only takes a system when it gives everything the system needs.
            auto Dragged = []() noexcept -> std::optional<xecs::system::type::guid>
            {
                const ImGuiPayload* payload = ImGui::GetDragDropPayload();
                if (payload && payload->IsDataType("LevelEditor_SYSTEM_REORDER") && payload->DataSize == sizeof(xecs::system::type::guid))
                    return *reinterpret_cast<const xecs::system::type::guid*>(payload->Data);
                return std::nullopt;
            };
            // Inside a drop target: true when the dragged system can be placed there. When it cannot the target takes nothing, and says why under the mouse.
            auto TakesDraggedSystem = [&](xecs::system::type::guid Parent, int Connector) noexcept
            {
                const auto Source = Dragged();
                if (!Source) return false;
                std::string Why;
                if (GameMgr.m_SystemMgr.CanPlaceUpdateSystem(*Source, Parent, Connector, &Why)) return true;
                ImGui::SetTooltip("%s", Why.c_str());
                return false;
            };
            // The constraints a system needs, as small tags after its name (the item just drawn is the row).
            auto DrawNeeds = [](std::span<const xecs::system::constraint::info> Needs, const std::string& Label) noexcept
            {
                if (Needs.empty()) return;
                const ImVec2 Min = ImGui::GetItemRectMin(), Max = ImGui::GetItemRectMax();
                const float  H   = ImGui::GetFontSize();
                float X = Min.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(Label.c_str()).x + 10.0f;
                auto* pList = ImGui::GetWindowDrawList();
                for (auto& N : Needs)
                {
                    const float W = ImGui::CalcTextSize(N.m_pName).x + 8.0f;
                    if (X + W > Max.x - 4.0f) break;
                    const float Y = Min.y + (Max.y - Min.y - H) * 0.5f;
                    pList->AddRectFilled(ImVec2(X, Y - 1.0f), ImVec2(X + W, Y + H + 1.0f), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
                    pList->AddText(ImVec2(X + 4.0f, Y), ImGui::GetColorU32(ImGuiCol_TextDisabled), N.m_pName);
                    X += W + 4.0f;
                }
            };

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
                auto DrawSourceTag = [&Display](bool bSystem, std::uint64_t Guid) noexcept
                {
                    const auto Source = Display.SourceOf(bSystem, Guid);
                    if (Source.m_bKnown && Source.m_Module != 0 && !Source.m_ModuleName.empty())
                    {
                        const ImVec2 Min = ImGui::GetItemRectMin(), Max = ImGui::GetItemRectMax();
                        const float  W = ImGui::CalcTextSize(Source.m_ModuleName.c_str()).x;
                        if (Max.x - Min.x > W + 160.0f)
                            ImGui::GetWindowDrawList()->AddText(ImVec2(Max.x - W - 6.0f, Min.y + (Max.y - Min.y - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), Source.m_ModuleName.c_str());
                    }
                    if (Source.m_bKnown && !Source.m_bBuiltIn && !Source.m_Path.empty() && ImGui::BeginPopupContextItem("##typesource"))
                    {
                        RenderTypeSourceMenu(Source);
                        ImGui::EndPopup();
                    }
                };
                constexpr unsigned kSystemIcon    = 0xE713;
                constexpr unsigned kConnectorIcon = 0xE71B;    // Link: the same glyph the Level tree gives its Dependencies

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
                    if (Source == Target.m_Guid) return;
                    RunCommand(std::format("MoveSystem -System {} -To {}", Hex(Source), Hex(Target.m_Guid)));
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



                // The rows as a tree: the update systems that run at the top level in order; a system with connectors is a node that opens to its
                // connectors (the places where other systems connect, named by the system), and a connector is a node that opens to the systems
                // connected to it.
                auto DrawSystems = [&](auto&& Self, xecs::system::type::guid Parent, int Connector) noexcept -> void
                {
                    for (std::size_t r = 0; r < Rows.size(); ++r)
                    {
                        if (!Rows[r].m_bPlaced || Rows[r].m_ParentGuid != Parent || (!Parent.empty() && Rows[r].m_ParentConnector != Connector)) continue;
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
                            RunCommand(std::format("SetSystemEnabled -System {} -Enabled {}", Hex(Row.m_Guid), bEnabled ? 1 : 0));
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
                        DrawNeeds(Row.m_Requires, std::format("{} {}", Glyph(kSystemIcon), Row.m_pName ? Row.m_pName : "(unnamed system)"));
                        DrawSourceTag(true, Row.m_Guid.m_Value);
                        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && i < GameMgr.m_SystemMgr.m_UpdaterSystems.size())
                            RenderSystemAccessTooltip(xlioncore::Ecs(GameMgr), Display, *GameMgr.m_SystemMgr.m_UpdaterSystems[i].first
                                , std::format("{}  (runs #{})", Row.m_pName ? Row.m_pName : "(unnamed system)", i).c_str());

                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                        {
                            ImGui::SetDragDropPayload("LevelEditor_SYSTEM_REORDER", &Row.m_Guid, sizeof(Row.m_Guid));
                            ImGui::TextUnformatted(Row.m_pName ? Row.m_pName : "(unnamed system)");
                            ImGui::EndDragDropSource();
                        }
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (TakesDraggedSystem(Row.m_ParentGuid, Row.m_ParentConnector))
                                if (auto Source = Payload()) DropOnSystem(*Source, Row);
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
                                // While a system is dragged, the connectors that cannot take it are dimmed.
                                const auto DraggedNow = Dragged();
                                const bool bRefuses   = DraggedNow && !GameMgr.m_SystemMgr.CanPlaceUpdateSystem(*DraggedNow, Row.m_Guid, c);
                                if (bRefuses) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                const bool bConnectorOpen = ImGui::TreeNodeEx(std::format("{} {}", Glyph(kConnectorIcon), Connectors[c].m_pName).c_str()
                                    , ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen
                                    | (bHasChildren ? 0 : ImGuiTreeNodeFlags_Leaf));
                                if (bRefuses) ImGui::PopStyleColor();
                                {
                                    // what the connector gives, as tags after its name
                                    DrawNeeds(Connectors[c].m_Provides, std::format("{} {}", Glyph(kConnectorIcon), Connectors[c].m_pName));
                                }
                                if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                                {
                                    if (xeditor::hint::BeginCard())
                                    {
                                        ImGui::TextUnformatted(std::format("{} - {}", Row.m_pName, Connectors[c].m_pName).c_str());
                                        ImGui::Separator();
                                        xeditor::hint::Wrapped(Connectors[c].m_pDescription);
                                        if (!Connectors[c].m_Provides.empty())
                                        {
                                            ImGui::Separator();
                                            ImGui::TextUnformatted("The systems connected here are given:");
                                            for (auto& G : Connectors[c].m_Provides) xeditor::hint::Bullet(std::format("{} - {}", G.m_pName, G.m_pDescription));
                                        }
                                        xeditor::hint::EndCard();
                                    }
                                }
                                if (ImGui::BeginDragDropTarget())                  // dropping a system on a connector connects it (when the connector gives what the system needs)
                                {
                                    if (TakesDraggedSystem(Row.m_Guid, c))
                                        if (auto Source = Payload())
                                            RunCommand(std::format("SetSystemParent -System {} -Parent {} -Connector {}", Hex(*Source), Hex(Row.m_Guid), xeditor::Quote(Connectors[c].m_pName)));
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

            // The top level of the frame: a system dropped here runs once every frame, after the others (it gives nothing: a system that needs a constraint cannot be placed here).
            {
                const auto DraggedNow = Dragged();
                const bool bRefuses   = DraggedNow && !GameMgr.m_SystemMgr.CanPlaceUpdateSystem(*DraggedNow, xecs::system::type::guid{}, -1);
                if (bRefuses) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::Selectable("(drop a system here to run it once every frame, at the top level)", false, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f));
                if (bRefuses) ImGui::PopStyleColor();
                if (ImGui::BeginDragDropTarget())
                {
                    if (TakesDraggedSystem(xecs::system::type::guid{}, -1))
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LevelEditor_SYSTEM_REORDER"))
                            RunCommand(std::format("SetSystemParent -System {}", Hex(*reinterpret_cast<const xecs::system::type::guid*>(payload->Data))));
                    ImGui::EndDragDropTarget();
                }
            }

            ImGui::EndChild();

            // Builder systems (doc/xecs_builder_components.md) have no order/enable - they run once per
            // entity while it's created, only when builders are on (Play / the game). Hover one for its
            // declared components.
            if (bHasBottom)
            {
                // The divider between the update systems and what is below them: drag it up or down (the position is kept while the editor runs). Tall enough to grab, with a grip in the middle
                // that says it moves, and it lights up under the mouse.
                ImGui::InvisibleButton("##SystemsSplitter", ImVec2(-FLT_MIN, 10.0f));
                const bool bSplitterHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
                if (bSplitterHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (ImGui::IsItemActive() && TotalHeight > 0.0f)
                    s_TopFraction = std::clamp(s_TopFraction + ImGui::GetIO().MouseDelta.y / TotalHeight, 0.1f, 0.9f);
                {
                    const ImVec2 Min = ImGui::GetItemRectMin();
                    const ImVec2 Max = ImGui::GetItemRectMax();
                    const float  Y   = (Min.y + Max.y) * 0.5f;
                    auto* pList = ImGui::GetWindowDrawList();
                    pList->AddLine(ImVec2(Min.x, Y), ImVec2(Max.x, Y), ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive : bSplitterHot ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator), bSplitterHot ? 3.0f : 2.0f);
                    xeditor::DrawSplitterGrip(pList, Min, Max, true, bSplitterHot);
                }

                ImGui::BeginChild("##BottomSystems", ImVec2(0.0f, 0.0f));

                // The unused systems, the first part: the ones nobody placed. They do not run. A system dragged from here into the tree above is placed (the places that cannot take it are dimmed);
                // a placed one dragged onto this part is taken out of the graph (and what is connected under it with it).
                {
                    std::size_t nUnused = 0;
                    for (auto& R : Rows) if (!R.m_bPlaced) ++nUnused;
                    const bool bUnusedOpen = RenderSystemRegistrySectionHeader(std::format("Unused systems ({})##UnusedSystemsHeader", nUnused));
                    if (ImGui::IsItemHovered() && !Dragged()) xeditor::hint::Text("Systems nobody placed: they do not run. Drag one into the tree above to place it (the places that cannot take it are dimmed); drag a placed system here to take it out of the graph");
                    const auto TakeOut = [&]() noexcept
                    {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("LevelEditor_SYSTEM_REORDER"))
                        {
                            RunCommand(std::format("UnplaceSystem -System {}", Hex(*reinterpret_cast<const xecs::system::type::guid*>(payload->Data))));
                        }
                    };
                    if (ImGui::BeginDragDropTarget()) { TakeOut(); ImGui::EndDragDropTarget(); }

                    if (bUnusedOpen)
                    {
                        if (nUnused == 0) ImGui::TextDisabled("Every system is placed.");
                        for (auto& Row : Rows)
                        {
                            if (Row.m_bPlaced) continue;
                            ImGui::PushID(static_cast<int>(Row.m_Guid.m_Value & 0x7FFFFFFF));
                            const std::string Label = std::format("{} {}", Glyph(kSystemIcon), Row.m_pName ? Row.m_pName : "(unnamed system)");
                            ImGui::TreeNodeEx(Label.c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
                            DrawNeeds(Row.m_Requires, Label);
                            DrawSourceTag(true, Row.m_Guid.m_Value);
                            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
                            {
                                ImGui::SetDragDropPayload("LevelEditor_SYSTEM_REORDER", &Row.m_Guid, sizeof(Row.m_Guid));
                                ImGui::TextUnformatted(Row.m_pName ? Row.m_pName : "(unnamed system)");
                                ImGui::EndDragDropSource();
                            }
                            if (ImGui::BeginDragDropTarget()) { TakeOut(); ImGui::EndDragDropTarget(); }          // dropped on a row of the part: the same as on its header
                            ImGui::PopID();
                        }
                    }
                }

                if (bHasEvents)
                {
                    std::size_t nHandlers = 0;
                    for (auto& G : EventGroups) nHandlers += G.m_Handlers.size();
                    if (RenderSystemRegistrySectionHeader(std::format("Event handlers ({})##EventHandlersHeader", nHandlers))) RenderEventHandlers(GameMgr, Display, EventGroups);
                    if (ImGui::IsItemHovered()) xeditor::hint::Text("Systems that run when something happens (the physics tells that a shape touched a sensor, ...), not every frame");
                }
                if (bHasNotifiers)
                {
                    if (RenderSystemRegistrySectionHeader(std::format("Notifiers ({})##NotifiersHeader", nNotifiers))) RenderNotifiers(GameMgr, Display);
                    if (ImGui::IsItemHovered()) xeditor::hint::Text("Systems that run when an entity is created (what initializes it), destroyed, moved or changed - for the entities they match");
                }
                if (bHasBuilders && RenderSystemRegistrySectionHeader(std::format("Builders ({})##BuildersHeader", GameMgr.m_SystemMgr.m_BuilderSystems.size())))
                {
                    for (auto& Builder : GameMgr.m_SystemMgr.m_BuilderSystems)
                    {
                        ImGui::Selectable(Builder.first->m_pName);
                        if (ImGui::IsItemHovered()) RenderSystemAccessTooltip(xlioncore::Ecs(GameMgr), Display, *Builder.first, Builder.first->m_pName);
                        if (const auto Source = Display.SourceOf(true, Builder.first->m_Guid.m_Value); Source.m_bKnown && !Source.m_bBuiltIn && !Source.m_Path.empty() && ImGui::BeginPopupContextItem("##typesource"))
                        {
                            RenderTypeSourceMenu(Source);
                            ImGui::EndPopup();
                        }
                    }
                }
                if (bHasBuilders && ImGui::IsItemHovered()) xeditor::hint::Text("Run once per entity while it is created, only when builders are on (Play / the game): they add what a builder component asks for");
                ImGui::EndChild();
            }

        }
        ImGui::End();
        xeditor::diagnostics::Log("window end: %s", pWindowName);
    }
} // namespace xlevel

#endif // XLVL_NEW_LevelEditor_PANEL_SYSTEM_REGISTRY_H
