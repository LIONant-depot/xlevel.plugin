#ifndef XLEVEL_COMMANDS_WORKSPACE_H
#define XLEVEL_COMMANDS_WORKSPACE_H
#pragma once

// Workspace + discovery commands - added proactively, direct user invitation: "Feel free to add
// commands that will be helpful to you or any missing command." Two real gaps this session's own
// live CLI-driven testing actually hit:
//
//   Undo/Redo were never exposed at all - only Ctrl+Z/Y in the UI. An AI/script-driven session had no
//   way to correct a mistake without touching a keyboard. Save was in the same boat - the only way to
//   persist changes to disk was File>Save or Ctrl+S.
//
//   DescribeEntity/ListComponentTypes close the discovery gap SetProperty/AddComponent always had:
//   both need a component's exact 16-hex-digit guid, and SetProperty ALSO needs a property's exact
//   path and TypeGuid - none of which were discoverable through ANY command. This session's own
//   testing had to read a raw .entity file off disk by hand to get this information even once - the
//   whole point of phase 5+ was to never need that.
#include "plugins/xlevel.plugin/source/Editor/xlevel_command_context.h"
#include "plugins/xscene.plugin/source/Editor/xscene_system_usage.h"

namespace xlevel::commands
{
    //================================================================================================
    // Undo/Redo - thin wrappers around xundo::system::Undo()/Redo() (the SAME System this command is
    // itself registered on, via query_command_base's own m_System). Deliberately Query, not Edit -
    // undoing/redoing must never itself become a new undo-able step (Execute() logs every successful
    // Edit command to history; wrapping Undo() in an Edit command would record "undo the undo" as its
    // own history entry, which makes no sense). GetUndoIndex() is the only public signal available to
    // tell "nothing happened" apart from "something did" - compared before/after rather than trusting
    // Undo()/Redo()'s own void-ish `system&` return.
    //================================================================================================
    struct undo_query_cmd : level_query_command
    {
        undo_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "Undo", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Undoes the last step (blocked while Play/Paused). Usage: Undo"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            // Same gate as the Ctrl+Z shortcut (the editor's main loop) - a CLI/Console-driven
            // agent shouldn't be able to do what the UI itself refuses to do while Playing/Paused.
            auto& State = get<level_context>().State();
            if (State.isPlaying()) return "Undo: blocked while Play/Paused";
            if (!xlevel::MayUndoRedo(m_System, false)) return "Undo: refused - it changes a Scene another Level is editing";
            const auto Before = m_System.GetUndoIndex();
            m_System.Undo();
            return m_System.GetUndoIndex() == Before ? "Nothing to undo" : "Undone";
        }
    };

    struct redo_query_cmd : level_query_command
    {
        redo_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "Redo", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Redoes the next step (blocked while Play/Paused). Usage: Redo"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto& State = get<level_context>().State();
            if (State.isPlaying()) return "Redo: blocked while Play/Paused";
            if (!xlevel::MayUndoRedo(m_System, true)) return "Redo: refused - it changes a Scene another Level is editing";
            const auto Before = m_System.GetUndoIndex();
            m_System.Redo();
            return m_System.GetUndoIndex() == Before ? "Nothing to redo" : "Redone";
        }
    };

    //================================================================================================
    // Save - wraps xlevel::SaveEverything (xlevel_editor.h), the SAME single "Save" action
    // File>Save/Ctrl+S already trigger. Gated on !State.isPlaying(), matching the existing Save-gating
    // rule exactly (the save-gating design notes) - Play already blocks Save in
    // the UI, and there's no reason a command should be allowed to bypass that.
    //================================================================================================
    struct save_query_cmd : level_query_command
    {
        save_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "Save", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Saves the currently open Level/Scenes to disk (blocked while Play/Paused). Usage: Save"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto& State = get<level_context>().State();
            if (State.isPlaying()) return "Save: blocked while Play/Paused";
            xlevel::SaveEverything(World(), State);
            xlevel::MarkDocumentClean(State, LevelContext().m_Undo);
            return "Saved";
        }
    };

    //================================================================================================
    // Close - File>Close for the current Level document. Query (not Edit): closing is a session
    // action, not an undoable scene mutation. When the Level is dirty, CLI/scripts must pass
    // -Save 1 (persist then close) or -Save 0 (discard then close) - there is no modal to click.
    // Clean Levels close with no -Save. Blocked while Play/Paused, same gate as Save/UI Close.
    //================================================================================================
    struct close_query_cmd : level_query_command
    {
        close_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "Close", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Closes the current Level (unload scenes, clear document). If modified, pass -Save 1 or -Save 0. Usage: Close [-Save 0|1]";
        }
        void RegisterArguments() noexcept override
        {
            m_hSave = m_Parser.addOption("Save", "When the Level has unsaved changes: 1/true = save then close, 0/false = discard then close. Ignored when clean.", false, 1);
        }
        std::string Query() noexcept override
        {
            auto& State = get<level_context>().State();
            if (State.isPlaying()) return "Close: blocked while Play/Paused";
            if (State.m_CurrentLevel.empty() && State.m_OpenScenes.empty())
                return "Close: nothing open";

            std::optional<bool> SaveOverride;
            if (auto SaveArg = m_Parser.getOptionArgAs<std::string>(m_hSave, 0); !std::holds_alternative<xerr>(SaveArg))
            {
                const auto& S = std::get<std::string>(SaveArg);
                SaveOverride = (S == "true" || S == "1");
            }

            const bool bDirty = xlevel::HasUnsavedDocumentChanges(State, LevelContext().m_Undo);
            if (bDirty && !SaveOverride.has_value())
                return "Close: Level has unsaved changes; pass -Save 1 (save) or -Save 0 (discard)";

            if (bDirty && SaveOverride.value())
            {
                xlevel::SaveEverything(World(), State);
                xlevel::MarkDocumentClean(State, LevelContext().m_Undo);
                xlevel::CloseLevel(World(), State, LevelContext().m_Undo);
                return "Saved and closed";
            }

            xlevel::CloseLevel(World(), State, LevelContext().m_Undo);
            return bDirty ? "Closed without saving" : "Closed";
        }
        xcmdline::parser::handle m_hSave;
    };

    //================================================================================================
    // DescribeEntity - every component on an entity, with every property's path/current value/type
    // guid - everything needed to build a working SetProperty (or confirm what AddComponent/
    // RemoveComponent already did). Reuses the exact xproperty::sprop::collector pattern already
    // proven safe with `noexcept` in phase 2/3/4's own component-snapshot code (unlike
    // dependencies/xcontainer/documentation/noexcept_lambda_trait_trap.md's own FindAsReadOnly callback, this one's fine).
    // Property paths are shown RAW, not Base64 - trivial for a human or AI to encode when building the
    // actual SetProperty call, and far more readable here than a wall of base64 would be.
    //================================================================================================
    struct describe_entity_query_cmd : level_query_command
    {
        describe_entity_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "DescribeEntity", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lists every component on an entity (kind data/share/tag, plus \",builder\" for a builder component: one that only configures the entity while it is created in the game) with each property's path/value/TypeGuid - everything SetProperty needs - then which systems run on it, what they read/write, which don't run and why, and what removing each component would change. Usage: DescribeEntity -Scene hexguid -Id hexid"; }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits",      true, 1);
            m_hId    = m_Parser.addOption("Id",    "Entity permanent_id, 8 hex digits", true, 1);
        }

        std::string Query() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg)) return "DescribeEntity: bad arguments";

            const auto SceneGuid = xscene::commands::ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id        = xscene::commands::ParseEntityId(std::get<std::string>(IdArg));
            auto* pScene = World().m_SceneMgr.Find(SceneGuid);
            if (!pScene) return std::format("DescribeEntity: Scene {} is not open", xscene::commands::FormatSceneGuid(SceneGuid));
            auto It = pScene->m_LocalToRuntime.find(Id);
            if (It == pScene->m_LocalToRuntime.end()) return "DescribeEntity: entity not found";
            auto Entity = It->second;

            auto& Details = World().m_ComponentMgr.getEntityDetails(Entity);
            if (!Details.m_pPool) return "DescribeEntity: entity has no components";

            // Internal bookkeeping components (entity self-identity, parent/children, prefab plumbing)
            // are excluded - not addable/settable via AddComponent/SetProperty, same filter as
            // ListComponentTypes.
            const auto Components = xscene::UserComponents(*Details.m_pPool->m_pArchetype);

            std::string Out;
            for (auto pInfo : Components)
            {
                const char* pKind = pInfo->m_TypeID == xecs::component::type::id::SHARE ? "share"
                                  : pInfo->m_TypeID == xecs::component::type::id::TAG   ? "tag"
                                  :                                                        "data";
                // ",builder" marks a builder component (a builder system consumes it when the entity is created in the game).
                Out += std::format("[{:016X}] {}  ({}{})\n", pInfo->m_Guid.m_Value, pInfo->m_pName, pKind, pInfo->m_bBuilder ? ",builder" : "");
                if (!pInfo->m_pPropertyTable) continue;
                // DATA lives in the entity's pool, SHARE on its family's share-entity, TAG nowhere.
                auto* pData = static_cast<std::byte*>(xscene::ResolveComponentPointer(World(), Entity, *pInfo));
                if (!pData) continue;

                xproperty::settings::context Context;
                xproperty::sprop::collector(pData, *pInfo->m_pPropertyTable, Context, [&](const char* pPropertyName, xproperty::any&& Data, const xproperty::type::members&, bool, const void*) noexcept
                {
                    std::array<char, 256> Buffer{};
                    const auto Len = xscene::commands::FormatPropertyValue(Buffer, Data);
                    const std::string ValueStr(Buffer.data(), Len > 0 ? static_cast<std::size_t>(Len) : 0);
                    const std::uint32_t TypeGuid = Data.m_pType ? Data.m_pType->m_GUID : 0;
                    Out += std::format("    {} = {}  (TypeGuid {:08X})\n", pPropertyName, ValueStr, TypeGuid);
                });
            }
            Out += "\n" + xscene::system_usage::DescribeEntitySystems(World(), Details.m_pPool->m_pArchetype->getComponentBits(), Components);
            return Out;
        }

        xcmdline::parser::handle m_hScene, m_hId;
    };

    //================================================================================================
    // ListComponentTypes - every registered DATA component type (guid + name) that AddComponent can
    // actually add - same iteration + IsInternalComponent filter the Entity Properties panel's own
    // "Add Component" combo already uses (xscene_panel_entity_properties.h), so this lists exactly what
    // that UI would offer, not a superset that would fail if handed to AddComponent.
    //================================================================================================
    struct list_component_types_query_cmd : level_query_command
    {
        list_component_types_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "ListComponentTypes", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lists every addable component type (guid, kind data/share/tag with \",builder\" added for builder components, name, and which systems use it). Usage: ListComponentTypes"; }
        void RegisterArguments() noexcept override {}

        std::string Query() noexcept override
        {
            const auto  Systems = xscene::system_usage::AllSystems(World());
            std::string Out;
            for (auto& Pair : xecs::component::mgr::s_Registry.m_ComponentInfoMap)
            {
                auto* pInfo = Pair.second;
                // Same set the Add Component popup offers (DATA, SHARE and TAG).
                const char* pKind = pInfo->m_TypeID == xecs::component::type::id::DATA  ? "data"
                                  : pInfo->m_TypeID == xecs::component::type::id::SHARE ? "share"
                                  : pInfo->m_TypeID == xecs::component::type::id::TAG   ? "tag"
                                  :                                                        nullptr;
                if (!pKind || xscene::IsInternalComponent(pInfo)) continue;
                const auto Used = xscene::system_usage::UsedBy(Systems, pInfo->m_Guid.m_Value);
                // ",builder" marks a builder component (it only configures an entity while it is created in the game).
                Out += std::format("{:016X}  {:<13}  {}{}\n", pInfo->m_Guid.m_Value, std::string(pKind) + (pInfo->m_bBuilder ? ",builder" : ""), pInfo->m_pName, Used.empty() ? "" : "   used by: " + Used);
            }
            return Out;
        }
    };

    //================================================================================================
    // ListSystems - every registered system in execution order, with what each one declares it
    // touches (must/one-of/none-of/if-present, reads/writes). Per-entity view: DescribeEntity.
    //================================================================================================
    struct list_systems_query_cmd : level_query_command
    {
        list_systems_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "ListSystems", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lists every system (update systems in execution order, then notifiers) with the components each declares and whether it reads or writes them. Usage: ListSystems"; }
        void RegisterArguments() noexcept override {}

        std::string Query() noexcept override
        {
            std::string Out;
            for (auto& S : xscene::system_usage::AllSystems(World(), false))
            {
                Out += std::format("{}  [{}{}]\n", xscene::system_usage::SystemName(S),
                                   S.m_bUpdate ? std::format("update #{}", S.m_Order) : std::string("notifier"), S.m_bEnabled ? "" : ", DISABLED");
                Out += xscene::system_usage::DescribeDeclaration(*S.m_pInfo, "    ");
            }
            return Out.empty() ? std::string("No systems registered.") : Out;
        }
    };
}

#endif // XLEVEL_COMMANDS_WORKSPACE_H
