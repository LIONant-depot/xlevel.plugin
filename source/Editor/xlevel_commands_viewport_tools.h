#ifndef XLEVEL_COMMANDS_VIEWPORT_TOOLS_H
#define XLEVEL_COMMANDS_VIEWPORT_TOOLS_H
#pragma once

// EditTool - drives the viewport tool editor (xlevel_viewport_tools.h) from the Command Console, the
// same thing the Inspector's per-element "Edit ..." toggle and the W/E/R keys do. A Query, not an Edit:
// entering/leaving edit mode changes no scene content (the drags themselves commit SetProperty).
#include "plugins/xlevel.plugin/source/Editor/xlevel_command_context.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_viewport_tools.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_scale_reference.h"
#include <cstdlib>

namespace xlevel::commands
{
    struct edit_tool_query_cmd : level_query_command
    {
        edit_tool_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "EditTool", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Toggles a viewport edit tool on an element of the selected entity's component, sets its mode, or ends it; reports the state. "
                   "Usage: EditTool [-Element n] [-Component PhysicsColliderSphere] [-Mode Move|Rotate|Resize] [-Off 1]";
        }
        void RegisterArguments() noexcept override
        {
            m_hElement = m_Parser.addOption("Element", "Element index to toggle (first registered tool the selected entity has)", false, 1);
            m_hMode    = m_Parser.addOption("Mode",    "Move, Rotate or Resize",                                                   false, 1);
            m_hOff     = m_Parser.addOption("Off",     "1 = end editing (a flag with no value is never seen as set by the parser)",              false, 1);
            m_hComp    = m_Parser.addOption("Component", "Only tools of this component, e.g. PhysicsColliderSphere",                       false, 1);
        }

        std::string Query() noexcept override
        {
            auto* pEditor = static_cast<viewport_tools::editor*>(LevelContext().m_pToolEditor);
            if (!pEditor) return "EditTool: no Level viewport";
            auto& S = State();

            if (m_Parser.hasOption(m_hOff)) pEditor->End();

            if (m_Parser.hasOption(m_hElement))
            {
                if (S.isPlaying()) return "EditTool: not available while playing";
                auto Arg = m_Parser.getOptionArgAs<std::string>(m_hElement, 0);
                if (std::holds_alternative<xerr>(Arg)) return "EditTool: bad -Element";
                const int Element = std::atoi(std::get<std::string>(Arg).c_str());

                std::string Only;
                if (m_Parser.hasOption(m_hComp))
                    if (auto CompArg = m_Parser.getOptionArgAs<std::string>(m_hComp, 0); !std::holds_alternative<xerr>(CompArg)) Only = std::get<std::string>(CompArg);

                const viewport_tools::tool* pTool = nullptr;
                for (auto* p : viewport_tools::Registry())
                {
                    if (!Only.empty() && p->m_ArrayPath.substr(0, p->m_ArrayPath.find('/')) != Only) continue;
                    const auto T = xscene::commands::ResolvePropertyTarget(LevelContext(), S.m_SelectedEntityScene, S.m_SelectedEntityId, p->m_ComponentGuid);
                    if (T.m_pInfo && T.m_pInstance && Element >= 0 && Element < p->getElementCount({ T.m_pInstance })) { pTool = p; break; }
                }
                if (!pTool) return "EditTool: the selected entity has no editable element with that index";
                pEditor->Toggle(*pTool, S.m_SelectedEntityScene, S.m_SelectedEntityId, Element);
            }

            if (m_Parser.hasOption(m_hMode))
            {
                auto Arg = m_Parser.getOptionArgAs<std::string>(m_hMode, 0);
                const std::string M = std::holds_alternative<xerr>(Arg) ? std::string{} : std::get<std::string>(Arg);
                viewport_tools::mode Wanted;
                if      (M == "Move")   Wanted = viewport_tools::mode::MOVE;
                else if (M == "Rotate") Wanted = viewport_tools::mode::ROTATE;
                else if (M == "Resize") Wanted = viewport_tools::mode::RESIZE;
                else return "EditTool: -Mode must be Move, Rotate or Resize";
                if (pEditor->isActive() && !pEditor->m_pTool->SupportsMode(Wanted)) return std::format("EditTool: {} has no {} mode", pEditor->m_pTool->m_pLabel, M);
                pEditor->m_Mode = Wanted;
            }

            if (!pEditor->isActive()) return "Tool=None";
            std::string Elements;
            for (int E : pEditor->m_Elements) Elements += (Elements.empty() ? "" : ",") + std::to_string(E);
            static constexpr const char* ModeNames[] = { "Move", "Rotate", "Resize" };
            return std::format("Tool={} Mode={} Elements={}", pEditor->m_pTool->m_pLabel, ModeNames[static_cast<int>(pEditor->m_Mode)], Elements);
        }

        xcmdline::parser::handle m_hElement, m_hMode, m_hOff, m_hComp;
    };

    // ScaleReference - the human figures of the viewport (xlevel_scale_reference.h): what the toolbar's Human button, a click and the Move
    // gizmo do. A Query: they are editor only, not in the Level (no undo, nothing to save).
    struct scale_reference_query_cmd : level_query_command
    {
        scale_reference_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "ScaleReference", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Adds, moves, selects or removes the 1.8 m human figures of the viewport (editor only: never saved or exported); reports them. "
                   "Usage: ScaleReference [-Add 1] [-Select n] [-At x,y,z (moves the selected)] [-Remove 1 (the selected)]";
        }
        void RegisterArguments() noexcept override
        {
            m_hAdd    = m_Parser.addOption("Add",    "1 = add one where the camera looks (selected)", false, 1);
            m_hSelect = m_Parser.addOption("Select", "Index to select (-1 none)",                     false, 1);
            m_hAt     = m_Parser.addOption("At",     "x,y,z: where the feet of the selected go",      false, 1);
            m_hRemove = m_Parser.addOption("Remove", "1 = remove the selected",                       false, 1);
        }
        std::string Query() noexcept override
        {
            auto* pRefs = static_cast<scale_reference*>(LevelContext().m_pScaleReference);
            if (!pRefs) return "ScaleReference: no Level viewport";
            const auto Arg = [&](xcmdline::parser::handle H) { auto A = m_Parser.getOptionArgAs<std::string>(H, 0); return std::holds_alternative<xerr>(A) ? std::string{} : std::get<std::string>(A); };
            if (m_Parser.hasOption(m_hAdd))    pRefs->Add();
            if (m_Parser.hasOption(m_hSelect))
            {
                const int i = std::atoi(Arg(m_hSelect).c_str());
                pRefs->m_iSelected = i >= 0 && i < static_cast<int>(pRefs->m_Figures.size()) ? i : -1;
            }
            if (m_Parser.hasOption(m_hAt))
            {
                if (pRefs->m_iSelected < 0) return "ScaleReference: -At needs a selected figure";
                const std::string Text = Arg(m_hAt);
                const char*       p    = Text.c_str();
                char*             pEnd = nullptr;
                float             V[3];
                for (int i = 0; i < 3; ++i)
                {
                    V[i] = std::strtof(p, &pEnd);
                    if (pEnd == p || (i < 2 && *pEnd != ',')) return "ScaleReference: -At must be x,y,z";
                    p = pEnd + (i < 2 ? 1 : 0);
                }
                const xmath::fvec3 P{ V[0], V[1], V[2] };
                pRefs->m_Figures[pRefs->m_iSelected] = P;
            }
            if (m_Parser.hasOption(m_hRemove)) pRefs->RemoveSelected();
            return pRefs->Describe();
        }

        xcmdline::parser::handle m_hAdd, m_hSelect, m_hAt, m_hRemove;
    };
}

#endif // XLEVEL_COMMANDS_VIEWPORT_TOOLS_H
