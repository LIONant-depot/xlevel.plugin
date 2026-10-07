#ifndef XLEVEL_COMMANDS_PREFAB_EDITOR_H
#define XLEVEL_COMMANDS_PREFAB_EDITOR_H
#pragma once

// The commands of the Prefab Editor (documentation/Editors/prefabs_plan.md, phase 5): opening a prefab in its own editor, the scenes brought in to test it against, and the one
// undoable change that comes from outside (another editor's save of the prefab). Everything else a Prefab Editor does is the scene editor's commands, addressed to it by name
// (Name\CreateEntity ...), exactly as to a Level.
#include "plugins/xlevel.plugin/source/Editor/xlevel_command_context.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_prefab_document.h"

namespace xlevel::commands
{
    //================================================================================================
    // OpenPrefab - opens a prefab in its own editor, next to the ones already open (as OpenLevel does for a Level). The editor loads the prefab as a scene of its own: a prefab
    // stored in the old format (one Entity.txt) is refused until UpgradeProject has converted it. The shell provides the function that creates the editor.
    //================================================================================================
    struct open_prefab_cmd : level_query_command
    {
        open_prefab_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "OpenPrefab", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Opens a Prefab in its own editor (a Prefab Editor: its tree, inspector, viewport, undo, Save and Play), next to the ones already open. The commands of the scene editor then go to it as Name\\Command. Usage: OpenPrefab -Prefab hexguid";
        }
        void RegisterArguments() noexcept override
        {
            m_hPrefab = m_Parser.addOption("Prefab", "Prefab instance guid, 16 hex digits", true, 1);
        }

        std::string Query() noexcept override
        {
            auto Arg = m_Parser.getOptionArgAs<std::string>(m_hPrefab, 0);
            if (std::holds_alternative<xerr>(Arg)) return "OpenPrefab: bad arguments";
            if (!g_OpenLevelSession) return "OpenPrefab: no editor available";
            const std::uint64_t Value = std::strtoull(std::get<std::string>(Arg).c_str(), nullptr, 16);
            return g_OpenLevelSession(xresource::full_guid{ .m_Instance = { Value }, .m_Type = xecs::prefab::type_guid_v });
        }
        xcmdline::parser::handle m_hPrefab;
    };

    //================================================================================================
    // AddContextScene / RemoveContextScene / ListContextScenes - the scenes a Prefab Editor has brought in to test the prefab against. They are loaded, so they play with it;
    // they are never picked, edited or saved, and they are no part of the prefab (a reference from the prefab to one of them is refused when it is saved). Not undoable (they
    // are the editor's, not the document's) and not kept when the editor closes.
    //================================================================================================
    struct add_context_scene_cmd : level_query_command
    {
        add_context_scene_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "AddContextScene", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Brings a Scene into a Prefab Editor to test the prefab against: it is loaded and plays with the prefab, and it is never picked, edited or saved. Usage: AddContextScene -Scene hexguid";
        }
        void RegisterArguments() noexcept override { m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1); }
        std::string Query() noexcept override
        {
            auto Arg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            if (std::holds_alternative<xerr>(Arg)) return "AddContextScene: bad arguments";
            const auto Scene = xscene::commands::ParseSceneGuid(std::get<std::string>(Arg));
            if (auto Why = AddContextScene(World(), State(), Scene); !Why.empty()) return "AddContextScene: " + Why;
            return std::format("AddContextScene: ok, {} context scene(s)", State().m_ContextScenes.size());
        }
        xcmdline::parser::handle m_hScene;
    };

    struct remove_context_scene_cmd : level_query_command
    {
        remove_context_scene_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "RemoveContextScene", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override { return "Takes a context scene out of a Prefab Editor. Usage: RemoveContextScene -Scene hexguid"; }
        void RegisterArguments() noexcept override { m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1); }
        std::string Query() noexcept override
        {
            auto Arg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            if (std::holds_alternative<xerr>(Arg)) return "RemoveContextScene: bad arguments";
            const auto Scene = xscene::commands::ParseSceneGuid(std::get<std::string>(Arg));
            if (auto Why = RemoveContextScene(World(), State(), Scene); !Why.empty()) return "RemoveContextScene: " + Why;
            return std::format("RemoveContextScene: ok, {} context scene(s)", State().m_ContextScenes.size());
        }
        xcmdline::parser::handle m_hScene;
    };

    struct list_context_scenes_query_cmd : level_query_command
    {
        list_context_scenes_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "ListContextScenes", pDataBase) {}
        const char* getCommandHelp() const noexcept override { return "Lists the context scenes of a Prefab Editor (guid, name, how many entities). Usage: ListContextScenes"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            const auto Names = BuildAssetNameMap(xecs::scene::type_guid_v);
            std::string Out;
            for (const auto& Scene : State().m_ContextScenes)
            {
                const auto  It      = Names.find(Scene.m_Instance.m_Value);
                const auto* pScene  = World().m_SceneMgr.Find(Scene);
                Out += std::format("{}  {}  entities={}\n", xscene::commands::FormatSceneGuid(Scene), It != Names.end() ? It->second : std::string("(unnamed)"), pScene ? pScene->m_LocalToRuntime.size() : 0u);
            }
            return Out.empty() ? "(no context scenes)\n" : Out;
        }
    };

    //================================================================================================
    // ReplacePrefabDocument - the prefab document of this Prefab Editor becomes what a folder holds (a prefab as it would be saved): one writer per prefab. When another editor
    // saves a prefab that is open here (Apply Overrides of one of its instances, in a Level), it does not write the file: it hands the saved state to this editor, which takes it
    // as this undoable step - so the change is visible, makes the document dirty, and can be undone before it is saved. Undo brings back the document as it was (a snapshot this
    // editor keeps until it closes). Usage: ReplacePrefabDocument -Folder path
    //================================================================================================
    struct replace_prefab_document_cmd : level_command
    {
        replace_prefab_document_cmd(xundo::system& System, void* pDataBase) noexcept : level_command(System, "ReplacePrefabDocument", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Replaces the prefab document of a Prefab Editor with a prefab saved in a folder (undoable): how a save of the prefab from another editor reaches the one that has it open. Usage: ReplacePrefabDocument -Folder path";
        }
        void RegisterArguments() noexcept override { m_hFolder = m_Parser.addOption("Folder", "The .desc folder of a prefab as it was saved (Descriptor.txt, entity_db)", true, 1); }

        std::string Redo() noexcept override
        {
            auto Arg = m_Parser.getOptionArgAs<std::string>(m_hFolder, 0);
            if (std::holds_alternative<xerr>(Arg)) return "ReplacePrefabDocument: bad arguments";
            if (!State().isPrefabEditor()) return "ReplacePrefabDocument: this editor has no prefab open";
            if (State().isPlaying())       return "ReplacePrefabDocument: not while playing";

            const std::wstring Folder = std::filesystem::path(std::get<std::string>(Arg)).wstring();
            std::error_code    Ec;
            if (!std::filesystem::exists(std::filesystem::path(Folder) / L"Descriptor.txt", Ec)) return "ReplacePrefabDocument: the folder holds no prefab (no Descriptor.txt)";

            if (auto Err = ReadPrefabDocumentFrom(World(), State(), Folder); Err)
                return std::format("ReplacePrefabDocument: {}", Err.getMessage());
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override
        {
            // The document as it is now: what Undo goes back to. A document that breaks a rule of a prefab (two roots) cannot be written: Undo then has nothing to go back to.
            std::string Snapshot;
            if (State().isPrefabEditor())
            {
                const auto Folder = NewPrefabSnapshotFolder(State());
                if (auto Err = WritePrefabDocumentTo(World(), State(), Folder); !Err) Snapshot = std::filesystem::path(Folder).string();
            }
            xeditor::WriteString(File, Snapshot);
        }

        void Undo(xundo::undo_file& File) noexcept override
        {
            const std::string Snapshot = xeditor::ReadString(File);
            if (Snapshot.empty() || !State().isPrefabEditor()) return;
            if (auto Err = ReadPrefabDocumentFrom(World(), State(), std::filesystem::path(Snapshot).wstring()); Err)
                xeditor::NotifyToast(std::format("ReplacePrefabDocument Undo: {}", Err.getMessage()));
        }

        xcmdline::parser::handle m_hFolder;
    };
}

#endif // XLEVEL_COMMANDS_PREFAB_EDITOR_H
