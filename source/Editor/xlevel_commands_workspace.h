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
#include "dependencies/xeditor/include/xeditor/gpu_log.h"
#include "plugins/xscene.plugin/source/Editor/xscene_system_usage.h"

// Defined with the game plugin state (game_module/LevelEditor_GamePluginLoad.h), which comes after this header.
namespace xlevel
{
    inline std::string GameModuleStatusText( const game_plugin_state* pPlugin ) noexcept;
    inline std::string SimulateModuleCrash( game_plugin_state* pPlugin, const std::string& State ) noexcept;
    inline std::string SimulateSnapshotFailure( game_plugin_state* pPlugin, const std::string& State ) noexcept;
}

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
            // A prefab that breaks a rule of a prefab (one root, references inside) is not written, and stays unsaved: the document is not clean.
            if (!xlevel::SaveEverything(World(), State) && State.isPrefabEditor())
                return "Save: the prefab was not saved (a prefab has one root, and references only its own entities); it stays unsaved";
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
            if (!State.HasDocument() && State.m_OpenScenes.empty())
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
                // A prefab that breaks a rule of a prefab is not written: the editor stays open (and unsaved) and says so, instead of closing on a save that did not happen.
                if (!xlevel::SaveEverything(World(), State) && State.isPrefabEditor())
                    return "Close: the prefab was not saved (a prefab has one root, and references only its own entities); the editor stays open and unsaved";
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
    // Property paths are shown as they are (a path and a value go into SetProperty in quotes, as text).
    //================================================================================================
    struct describe_entity_query_cmd : level_query_command
    {
        describe_entity_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "DescribeEntity", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lists every component on an entity (kind data/share/tag, plus \",builder\" for a builder component: one that only configures the entity while it is created in the game) with each property's path/value/TypeGuid - everything SetProperty needs - then which systems run on it, what they read/write, which don't run and why, and what removing each component would change. Usage: DescribeEntity -Scene hexguid -Id hexid"; }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits",      true, 1);
            m_hId    = m_Parser.addOption("Id",    "Entity permanent_id, 8 or 16 hex digits", true, 1);
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

            if (!xlioncore::Ecs(World()).IsAlive(Entity)) return "DescribeEntity: entity has no components";

            // Internal bookkeeping components (entity self-identity, parent/children, prefab plumbing)
            // are excluded - not addable/settable via AddComponent/SetProperty, same filter as
            // ListComponentTypes.
            const auto Components = xscene::UserComponents(xlioncore::Ecs(World()), Entity);

            std::string Out;
            for (auto pInfo : Components)
            {
                const char* pKind = pInfo->m_TypeID == xecs::component::type::id::SHARE ? "share"
                                  : pInfo->m_TypeID == xecs::component::type::id::TAG   ? (pInfo->m_bExclusiveTag ? "exclusive_tag" : "tag")
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
            Out += "\n" + xscene::system_usage::DescribeEntitySystems(World(), xscene::system_usage::SetOf(World(), Entity), Components);
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
            std::vector<const xecs::component::type::info*> Registered;
            xlioncore::Ecs(World()).ListComponentTypes(Registered);
            for (auto* pInfo : Registered)
            {
                // Same set the Add Component popup offers (DATA, SHARE and TAG).
                const char* pKind = pInfo->m_TypeID == xecs::component::type::id::DATA  ? "data"
                                  : pInfo->m_TypeID == xecs::component::type::id::SHARE ? "share"
                                  : pInfo->m_TypeID == xecs::component::type::id::TAG   ? (pInfo->m_bExclusiveTag ? "exclusive_tag" : "tag")
                                  :                                                        nullptr;
                if (!pKind || xscene::IsInternalComponent(pInfo)) continue;
                const auto Used = xscene::system_usage::UsedBy(Systems, pInfo->m_Guid.m_Value);
                // ",builder" marks a builder component (it only configures an entity while it is created in the game); ",editor" the state of an entity in the editor (not in the Add Component list).
                Out += std::format("{:016X}  {:<13}  {}{}\n", pInfo->m_Guid.m_Value, std::string(pKind) + (pInfo->m_bBuilder ? ",builder" : "") + (xscene::IsEditorStateComponent(pInfo) ? ",editor" : ""), pInfo->m_pName, Used.empty() ? "" : "   used by: " + Used);
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
                const bool bEvent = !S.m_bUpdate && (S.m_pInfo->m_ID == xecs::system::type::id::GLOBAL_EVENT || S.m_pInfo->m_ID == xecs::system::type::id::SYSTEM_EVENT);     // ListEventHandlers says what it handles
                const bool bNotPlaced = S.m_bUpdate && !World().m_SystemMgr.IsUpdateSystemPlaced(S.m_pInfo->m_Guid);
                Out += std::format("{}  [{}{}{}]\n", xscene::system_usage::SystemName(S),
                                   S.m_bUpdate ? std::format("update #{}", S.m_Order) : std::string(bEvent ? "event handler" : "notifier"), bNotPlaced ? ", NOT PLACED (does not run)" : "", S.m_bEnabled ? "" : ", DISABLED");
                if (S.m_bUpdate) if (const auto Needs = ConstraintNames(World().m_SystemMgr.GetUpdateSystemConstraints(S.m_pInfo->m_Guid)); !Needs.empty()) Out += std::format("    needs: {}\n", Needs);
                Out += xscene::system_usage::DescribeDeclaration(*S.m_pInfo, "    ");
            }
            if (!Out.empty()) Out += HierarchyText(World());
            return Out.empty() ? std::string("No systems registered.") : Out;
        }

        static std::string ConstraintNames(std::span<const xecs::system::constraint::info> Constraints) noexcept
        {
            std::string Out;
            for (auto& C : Constraints) { if (!Out.empty()) Out += ", "; Out += C.m_pName; }
            return Out;
        }

        // Who runs when: the update systems that run at the top level in order, and under each connector of a system the systems connected to it.
        static void TreeText(xecs::game_mgr::instance& GameMgr, const std::vector<xecs::system::update_system_row>& Rows, xecs::system::type::guid Parent, int Connector, int Depth, std::string& Out) noexcept
        {
            for (auto& Row : Rows)
            {
                if (!Row.m_bPlaced || Row.m_ParentGuid != Parent || (!Parent.empty() && Row.m_ParentConnector != Connector)) continue;
                Out += std::format("{:{}}{}{}\n", "", Depth * 2, Row.m_pName, Row.m_bEnabled ? "" : "  (DISABLED)");
                int c = 0;
                for (auto& C : GameMgr.m_SystemMgr.GetConnectors(Row.m_Guid))
                {
                    Out += std::format("{:{}}[{}] {}{}\n", "", Depth * 2 + 2, C.m_pName, C.m_pDescription, C.m_Provides.empty() ? std::string() : std::format("  (gives: {})", ConstraintNames(C.m_Provides)));
                    TreeText(GameMgr, Rows, Row.m_Guid, c, Depth + 2, Out);
                    ++c;
                }
            }
        }
        static std::string HierarchyText(xecs::game_mgr::instance& GameMgr) noexcept
        {
            std::string Out = "\nHierarchy (top level in order; a connector is shown in brackets with the systems connected to it):\n";
            const auto Rows = GameMgr.m_SystemMgr.GetUpdateSystemRows();
            TreeText(GameMgr, Rows, xecs::system::type::guid{}, -1, 1, Out);
            std::string Available;
            for (auto& Row : Rows)
                if (!Row.m_bPlaced) Available += std::format("  {}{}\n", Row.m_pName, Row.m_Requires.empty() ? std::string() : std::format("  (needs: {})", ConstraintNames(Row.m_Requires)));
            if (!Available.empty()) Out += "\nAvailable (not placed: they do not run until they are placed at the top level or in a connector that gives what they need):\n" + Available;
            return Out;
        }
    };

    //================================================================================================
    // ListEventHandlers - the events of the world (the physics ones, ...) with what each one tells and the systems that handle it, and what each handler declares. A handler runs when
    // the event is raised, not every frame: it is not in ListSystems' order.
    //================================================================================================
    struct list_event_handlers_query_cmd : level_query_command
    {
        list_event_handlers_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "ListEventHandlers", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Lists the events of the world with what each tells and when, and the systems that handle each (with the components each declares). A handler runs when the event is raised, not every frame. Usage: ListEventHandlers"; }
        void RegisterArguments() noexcept override {}

        std::string Query() noexcept override
        {
            std::string Out;
            for (const auto& G : xscene::system_usage::EventGroups(World()))
            {
                Out += std::format("{}  [{}, {} handler{}]\n", G.m_Name, G.m_bGlobal ? "event" : "system events", G.m_Handlers.size(), G.m_Handlers.size() == 1 ? "" : "s");
                if (!G.m_Help.empty()) Out += std::format("    {}\n", G.m_Help);
                for (const auto* pHandler : G.m_Handlers)
                {
                    Out += std::format("    handler: {}\n", pHandler->m_pName ? pHandler->m_pName : "(unnamed system)");
                    if (pHandler->m_Access.empty()) Out += "        (runs when the event is raised; it does not iterate entities: it reads what the event gives it)\n";
                    else                            Out += xscene::system_usage::DescribeDeclaration(*pHandler, "        ");
                }
            }
            return Out.empty() ? std::string("No events registered.") : Out;
        }
    };

    //================================================================================================
    // The edits of the System Registry: SetSystemParent (place a system at the top level or in a connector, last among its siblings), UnplaceSystem (take it out of the graph, what is connected
    // under it with it), MoveSystem (take the place of another system) and SetSystemEnabled. They are what the panel's drag and drop and its checkboxes run, and what an AI runs: the same
    // commands, all undoable. A system is named as ListSystems names it, or by its guid in hex. They are edits of the Level like any other: unsaved until the Level is saved (Save, Save All, Play
    // write the registry to Project.config\SystemOrder.config.txt with it), and a play session reverts them on Stop. The undo of each puts back a snapshot of the whole registry (the order, what
    // is enabled, the graph), which is what makes a change that moves several systems (the ones under a system that is taken out) one step.
    //================================================================================================
    struct system_registry_command : level_command
    {
        system_registry_command(xundo::system& System, const char* pName, void* pDataBase) noexcept : level_command(System, pName, pDataBase) {}

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            const auto Rows = World().m_SystemMgr.GetUpdateSystemRows();
            File.Write(static_cast<std::uint32_t>(Rows.size()));
            for (auto& R : Rows)
            {
                File.Write(static_cast<std::uint64_t>(R.m_Guid.m_Value));
                File.Write(static_cast<std::uint64_t>(R.m_ParentGuid.m_Value));
                File.Write(static_cast<std::int32_t>(R.m_ParentConnector));
                File.Write(static_cast<std::uint8_t>(R.m_bEnabled ? 1 : 0));
                File.Write(static_cast<std::uint8_t>(R.m_bPlaced ? 1 : 0));
            }
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            std::uint32_t Count = 0; File.Read(Count);
            std::vector<xecs::system::update_system_row> Rows;
            Rows.reserve(Count);
            for (std::uint32_t i = 0; i < Count; ++i)
            {
                std::uint64_t Guid = 0, Parent = 0; std::int32_t Connector = -1; std::uint8_t bEnabled = 0, bPlaced = 0;
                File.Read(Guid); File.Read(Parent); File.Read(Connector); File.Read(bEnabled); File.Read(bPlaced);
                Rows.push_back(xecs::system::update_system_row{ .m_Guid = xecs::system::type::guid{ Guid }, .m_pName = nullptr, .m_bEnabled = bEnabled != 0
                                                              , .m_ParentGuid = xecs::system::type::guid{ Parent }, .m_ParentConnector = Connector, .m_bPlaced = bPlaced != 0 });
            }
            World().m_SystemMgr.ApplyUpdateSystemRows(Rows);
        }

        // The system called Text (as ListSystems shows it), or the one with that guid in hex.
        static const xecs::system::update_system_row* Find(const std::vector<xecs::system::update_system_row>& Rows, const std::string& Text) noexcept
        {
            for (auto& R : Rows) if (R.m_pName && Text == R.m_pName) return &R;
            char* pEnd = nullptr;
            const auto Value = std::strtoull(Text.c_str(), &pEnd, 16);
            if (!Text.empty() && pEnd && *pEnd == 0)
                for (auto& R : Rows) if (R.m_Guid.m_Value == Value) return &R;
            return nullptr;
        }

        bool Arg(xcmdline::parser::handle Handle, std::string& Out) noexcept
        {
            auto A = m_Parser.getOptionArgAs<std::string>(Handle, 0);
            if (std::holds_alternative<xerr>(A)) return false;
            Out = std::get<std::string>(A);
            return true;
        }
    };

    struct set_system_parent_cmd : system_registry_command
    {
        set_system_parent_cmd(xundo::system& System, void* pDataBase) noexcept : system_registry_command(System, "SetSystemParent", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Places an update system at the top level of the frame, or in a connector of another system (last among the systems there); a place that does not give what the system needs refuses it (undoable). Usage: SetSystemParent -System name [-Parent name -Connector name]"; }
        void RegisterArguments() noexcept override
        {
            m_hSystem    = m_Parser.addOption("System",    "The system to place (its name, as ListSystems shows it, or its guid in hex)", true,  1);
            m_hParent    = m_Parser.addOption("Parent",    "The system that has the connector; leave out for the top level", false, 1);
            m_hConnector = m_Parser.addOption("Connector", "The name of the connector of the parent",                        false, 1);
        }
        std::string Redo() noexcept override
        {
            auto& Mgr = World().m_SystemMgr;
            const auto Rows = Mgr.GetUpdateSystemRows();

            std::string Child, Parent, Connector, Why;
            if (!Arg(m_hSystem, Child)) return "SetSystemParent: -System is required";
            const auto* pChild = Find(Rows, Child);
            if (!pChild) return std::format("SetSystemParent: no update system called '{}'", Child);

            if (!Arg(m_hParent, Parent))
            {
                if (!Mgr.CanPlaceUpdateSystem(pChild->m_Guid, xecs::system::type::guid{}, -1, &Why)) return "SetSystemParent: refused (" + Why + ")";
                Mgr.SetUpdateSystemParent(pChild->m_Guid, xecs::system::type::guid{}, -1);
                return {};
            }
            const auto* pParent = Find(Rows, Parent);
            if (!pParent) return std::format("SetSystemParent: no update system called '{}'", Parent);
            Arg(m_hConnector, Connector);

            int Index = -1, c = 0;
            for (auto& C : Mgr.GetConnectors(pParent->m_Guid)) { if (Connector == C.m_pName) Index = c; ++c; }
            if (Index < 0) return std::format("SetSystemParent: '{}' has no connector called '{}'", Parent, Connector);
            if (!Mgr.CanPlaceUpdateSystem(pChild->m_Guid, pParent->m_Guid, Index, &Why)) return "SetSystemParent: refused (" + Why + ")";
            Mgr.SetUpdateSystemParent(pChild->m_Guid, pParent->m_Guid, Index);
            return {};
        }
        xcmdline::parser::handle m_hSystem, m_hParent, m_hConnector;
    };

    struct unplace_system_cmd : system_registry_command
    {
        unplace_system_cmd(xundo::system& System, void* pDataBase) noexcept : system_registry_command(System, "UnplaceSystem", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Takes an update system out of the graph (what is connected under it too): it does not run until it is placed again with SetSystemParent (undoable). Usage: UnplaceSystem -System name"; }
        void RegisterArguments() noexcept override { m_hSystem = m_Parser.addOption("System", "The system (its name, as ListSystems shows it, or its guid in hex)", true, 1); }
        std::string Redo() noexcept override
        {
            std::string Name;
            if (!Arg(m_hSystem, Name)) return "UnplaceSystem: -System is required";
            auto& Mgr = World().m_SystemMgr;
            const auto Rows = Mgr.GetUpdateSystemRows();
            const auto* pSystem = Find(Rows, Name);
            if (!pSystem) return std::format("UnplaceSystem: no update system called '{}'", Name);
            Mgr.UnplaceUpdateSystem(pSystem->m_Guid);
            return {};
        }
        xcmdline::parser::handle m_hSystem;
    };

    struct move_system_cmd : system_registry_command
    {
        move_system_cmd(xundo::system& System, void* pDataBase) noexcept : system_registry_command(System, "MoveSystem", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "A system takes the place of another that is placed: the same connector (or the top level) when that place gives what it needs, and its position among the systems there (undoable). Usage: MoveSystem -System name -To name"; }
        void RegisterArguments() noexcept override
        {
            m_hSystem = m_Parser.addOption("System", "The system to move (its name, as ListSystems shows it, or its guid in hex)", true, 1);
            m_hTo     = m_Parser.addOption("To",     "The system whose place it takes",                                           true, 1);
        }
        std::string Redo() noexcept override
        {
            std::string Name, To;
            if (!Arg(m_hSystem, Name) || !Arg(m_hTo, To)) return "MoveSystem: -System and -To are required";
            auto& Mgr = World().m_SystemMgr;
            const auto Rows = Mgr.GetUpdateSystemRows();
            const auto* pSystem = Find(Rows, Name);
            const auto* pTarget = Find(Rows, To);
            if (!pSystem) return std::format("MoveSystem: no update system called '{}'", Name);
            if (!pTarget) return std::format("MoveSystem: no update system called '{}'", To);
            if (!Mgr.DropUpdateSystemOn(pSystem->m_Guid, pTarget->m_Guid)) return "MoveSystem: refused (the place of that system is not placed, or does not give what this one needs)";
            return {};
        }
        xcmdline::parser::handle m_hSystem, m_hTo;
    };

    struct set_system_enabled_cmd : system_registry_command
    {
        set_system_enabled_cmd(xundo::system& System, void* pDataBase) noexcept : system_registry_command(System, "SetSystemEnabled", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Enables or disables an update system (a disabled one is placed but does not run; undoable). Usage: SetSystemEnabled -System name -Enabled 0|1"; }
        void RegisterArguments() noexcept override
        {
            m_hSystem  = m_Parser.addOption("System",  "The system (its name, as ListSystems shows it, or its guid in hex)", true, 1);
            m_hEnabled = m_Parser.addOption("Enabled", "1 to enable it, 0 to disable it",                                    true, 1);
        }
        std::string Redo() noexcept override
        {
            std::string Name, Enabled;
            if (!Arg(m_hSystem, Name) || !Arg(m_hEnabled, Enabled)) return "SetSystemEnabled: -System and -Enabled are required";
            auto& Mgr = World().m_SystemMgr;
            const auto Rows = Mgr.GetUpdateSystemRows();
            const auto* pSystem = Find(Rows, Name);
            if (!pSystem) return std::format("SetSystemEnabled: no update system called '{}'", Name);
            Mgr.SetUpdateSystemEnabled(pSystem->m_Guid, Enabled != "0");
            return {};
        }
        xcmdline::parser::handle m_hSystem, m_hEnabled;
    };

    struct save_system_order_cmd : level_query_command
    {
        save_system_order_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "SaveSystemOrder", pDataBase) {}
        const char* getCommandHelp() const noexcept override { return "Saves the order of the update systems and what is connected to what (Project.config/SystemOrder.config.txt). Usage: SaveSystemOrder"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            if (auto Err = World().m_SystemMgr.Save(); Err) return "SaveSystemOrder: failed to write the file";
            return "SaveSystemOrder: saved";
        }
    };

    //================================================================================================
    // What became of the game module (Game.dll): loaded or not, and whether it crashed while registering (see GuardedModuleCall).
    //================================================================================================
    struct game_module_status_cmd : level_query_command
    {
        game_module_status_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "GameModuleStatus", pDataBase) {}
        const char* getCommandHelp() const noexcept override { return "The state of the game module (Game.dll): Loaded, Crashed (it crashed while registering and its systems are not running) and the status line. Usage: GameModuleStatus"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            return GameModuleStatusText(LevelContext().m_pGamePlugin);
        }
    };

    //================================================================================================
    // The modifier keys as the UI sees them (ImGui's io, which the keyboard window and every shortcut read), so a script can tell that a
    // key press made it all the way from the window's messages through xGPU's keyboard to ImGui.
    //================================================================================================
    struct input_state_cmd : level_query_command
    {
        input_state_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "InputState", pDataBase) {}
        const char* getCommandHelp() const noexcept override { return "The modifier keys as the UI sees them: Ctrl, Shift and Alt, each true or false. Usage: InputState"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            const auto& io = ImGui::GetIO();
            return std::format("Ctrl={}\nShift={}\nAlt={}", io.KeyCtrl, io.KeyShift, io.KeyAlt);
        }
    };

    //================================================================================================
    // Diagnostic, for the smoke tests: raises the error popup (as any command that fails in the UI does) and says where the popup is, so the
    // tests can check that a modal opens in the middle of the editor it belongs to.
    //================================================================================================
    struct raise_error_cmd : level_query_command
    {
        raise_error_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "RaiseError", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: shows the error popup with a message, centered on the editor that has the focus. Usage: RaiseError -Message text [-Style modal|toast|badge]"; }
        void RegisterArguments() noexcept override
        {
            m_hMessage = m_Parser.addOption("Message", "The text of the error", true, 1);
            m_hStyle = m_Parser.addOption("Style", "modal (default), toast or badge: how loudly it is told (every one is recorded in the Logs)", false, 1);
        }
        std::string Query() noexcept override
        {
            auto A = m_Parser.getOptionArgAs<std::string>(m_hMessage, 0);
            if (std::holds_alternative<xerr>(A)) return "RaiseError: -Message is required";
            auto S = m_Parser.getOptionArgAs<std::string>(m_hStyle, 0);
            const std::string Style = std::holds_alternative<xerr>(S) ? std::string("modal") : std::get<std::string>(S);
            if (Style != "modal" && Style != "toast" && Style != "badge") return "RaiseError: -Style is modal, toast or badge";
            xeditor::NotifyError(std::get<std::string>(A), Style == "modal" ? xeditor::notify_style::Modal : Style == "toast" ? xeditor::notify_style::Toast : xeditor::notify_style::Badge);
            return "RaiseError: raised";
        }
        xcmdline::parser::handle m_hMessage, m_hStyle;
    };

    // Diagnostic, for the smoke tests: the process dies of an access violation (the crash handler writes its record, as for a real one), so the Logs' importer can be shown a confirmed crash.
    struct simulate_crash_cmd : level_query_command
    {
        simulate_crash_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "SimulateCrash", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: crashes the process on purpose (an access violation), to test crash recovery. Usage: SimulateCrash -Confirm true"; }
        void RegisterArguments() noexcept override { m_hConfirm = m_Parser.addOption("Confirm", "true: yes, really crash", true, 1); }
        std::string Query() noexcept override
        {
            auto C = m_Parser.getOptionArgAs<std::string>(m_hConfirm, 0);
            if (std::holds_alternative<xerr>(C) || std::get<std::string>(C) != "true") return "SimulateCrash: -Confirm true is required";
            *static_cast<volatile int*>(nullptr) = 0;
            return "SimulateCrash: survived";
        }
        xcmdline::parser::handle m_hConfirm;
    };

    // Diagnostic, for the smoke tests: hands a line to the xGPU adapter exactly as xGPU's own callback would (Vulkan's messages have brackets and quotes: the line goes in quotes),
    // so the adapter's parsing (code, title, where) is tested without needing a real validation error.
    struct simulate_gpu_message_cmd : level_query_command
    {
        simulate_gpu_message_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "SimulateGpuMessage", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: gives a line to the xGPU adapter as xGPU's error or warning callback does. Usage: SimulateGpuMessage -Text text [-Severity error|warning]"; }
        void RegisterArguments() noexcept override
        {
            m_hText = m_Parser.addOption("Text", "The line", true, 1);
            m_hSeverity = m_Parser.addOption("Severity", "error (default) or warning", false, 1);
        }
        std::string Query() noexcept override
        {
            auto T = m_Parser.getOptionArgAs<std::string>(m_hText, 0);
            if (std::holds_alternative<xerr>(T)) return "SimulateGpuMessage: -Text is required";
            auto S = m_Parser.getOptionArgAs<std::string>(m_hSeverity, 0);
            const bool bWarning = !std::holds_alternative<xerr>(S) && std::get<std::string>(S) == "warning";
            const std::string Line = std::get<std::string>(T);
            xeditor::LogGpuMessage(Line, bWarning ? xlog::severity::Warning : xlog::severity::Error, "xlion.simulated");        // a made-up line: its own producer, never counted as the real thing
            return "SimulateGpuMessage: given";
        }
        xcmdline::parser::handle m_hText, m_hSeverity;
    };

    struct modal_state_cmd : level_query_command
    {
        modal_state_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "ModalState", pDataBase) {}
        const char* getCommandHelp() const noexcept override { return "Diagnostic: Open (is a modal window open), PopupCenter (where it is), Anchor (where the error popup was asked to open), EditorCenter (the middle of the editor that has the focus) and ViewportCenter (the middle of the application window). Usage: ModalState"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto* pHost = xeditor::host::current();
            const ImGuiWindow* pModal  = ImGui::GetTopMostPopupModal();
            const ImVec2       Popup   = pModal ? ImVec2(pModal->Pos.x + pModal->Size.x * 0.5f, pModal->Pos.y + pModal->Size.y * 0.5f) : ImVec2(0, 0);
            const ImVec2       Editor  = xeditor::EditorRect(true).GetCenter();
            const ImVec2       Anchor  = pHost ? pHost->m_Notifier.m_Anchor : ImVec2(0, 0);
            const ImVec2       Viewport= ImGui::GetMainViewport()->GetCenter();
            std::string        DrawerText = "unknown";           // the focused window's drawer: closed, or open:<tab index>
            if (pHost) if (ImGuiViewport* vp = xeditor::FocusedDrawerViewport()) { const auto& D = pHost->drawer_for(vp->ID); DrawerText = D.m_bOpen ? std::format("open:{}", D.m_ActiveTab) : std::string("closed"); }
            return std::format("Open={}\nPopupCenter={:.0f},{:.0f}\nAnchor={:.0f},{:.0f}\nEditorCenter={:.0f},{:.0f}\nViewportCenter={:.0f},{:.0f}\nToasts={}\nToastsRaised={}\nDrawer={}"
                , pModal != nullptr, Popup.x, Popup.y, Anchor.x, Anchor.y, Editor.x, Editor.y, Viewport.x, Viewport.y, pHost ? pHost->m_Toaster.size() : 0, pHost ? pHost->m_Toaster.m_Raised : 0, DrawerText);
        }
    };

    //================================================================================================
    // Diagnostic, for the smoke tests: makes the game module's RegisterSystems crash on purpose the next time a world registers its systems
    // (Stop, a reload, a new Level session), the way a module with a bad system does, so the recovery can be tried without a bad module.
    // "-State off" puts everything back, including the crashed flag, so the editor can go on being used.
    //================================================================================================
    struct simulate_module_crash_cmd : level_query_command
    {
        simulate_module_crash_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "SimulateModuleCrash", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: make the game module crash while it registers its systems the next time a world is made (on), or stop doing so and clear the crashed flag (off). Usage: SimulateModuleCrash -State on|off"; }
        void RegisterArguments() noexcept override { m_hState = m_Parser.addOption("State", "on or off", true, 1); }
        std::string Query() noexcept override
        {
            auto A = m_Parser.getOptionArgAs<std::string>(m_hState, 0);
            return SimulateModuleCrash(LevelContext().m_pGamePlugin, std::holds_alternative<xerr>(A) ? std::string{} : std::get<std::string>(A));
        }
        xcmdline::parser::handle m_hState;
    };

    // Diagnostic, for the smoke tests: the next Game.dll reload's snapshot restore brings no entity back (the way a snapshot that cannot be read does), which leaves the
    // open scenes naming entities the rebuilt world does not have. The editor must notice, say so, and rebuild the world from the saved level.
    struct simulate_snapshot_failure_cmd : level_query_command
    {
        simulate_snapshot_failure_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "SimulateSnapshotFailure", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Diagnostic: make the next Game.dll reload's snapshot restore bring nothing back (on), or stop doing so (off). Usage: SimulateSnapshotFailure -State on|off"; }
        void RegisterArguments() noexcept override { m_hState = m_Parser.addOption("State", "on or off", true, 1); }
        std::string Query() noexcept override
        {
            auto A = m_Parser.getOptionArgAs<std::string>(m_hState, 0);
            return SimulateSnapshotFailure(LevelContext().m_pGamePlugin, std::holds_alternative<xerr>(A) ? std::string{} : std::get<std::string>(A));
        }
        xcmdline::parser::handle m_hState;
    };
}

#endif // XLEVEL_COMMANDS_WORKSPACE_H
