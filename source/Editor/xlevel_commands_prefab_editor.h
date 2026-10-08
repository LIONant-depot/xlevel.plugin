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
    // OpenScene - opens a Scene in its own editor (a Scene Editor: the same editor as a Level's, with the Scene as its document), next to the ones already open. The Scene is not added to any Level (that is
    // dropping it on a Level, or AddScene). One writer per Scene: a Scene that a Level or another Scene Editor has open is not opened again: that editor is brought to the front and the reply says so.
    //================================================================================================
    struct open_scene_cmd : level_query_command
    {
        open_scene_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "OpenScene", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Opens a Scene in its own editor (a Scene Editor: its tree, inspector, viewport, undo, Save and Play), next to the ones already open; it is not added to any Level. A Scene that a Level or another editor already has open is not opened again: that editor is brought to the front. The commands of the scene editor then go to it as Name\\Command. Usage: OpenScene -Scene hexguid";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid, 16 hex digits", true, 1);
        }

        std::string Query() noexcept override
        {
            auto Arg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            if (std::holds_alternative<xerr>(Arg)) return "OpenScene: bad arguments";
            if (!g_OpenLevelSession) return "OpenScene: no editor available";
            const std::uint64_t Value = std::strtoull(std::get<std::string>(Arg).c_str(), nullptr, 16);
            return g_OpenLevelSession(xresource::full_guid{ .m_Instance = { Value }, .m_Type = xecs::scene::type_guid_v });
        }
        xcmdline::parser::handle m_hScene;
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
    // EditInContext - opens the prefab of an instance as the document of a Prefab Editor, placed where the instance is, with the Level's scenes as context (drawn faded, never picked, never
    // saved); the instance itself is left out of the draw and the pick (its prefab is there in its place). Saving the prefab brings every instance of it up to date, this one included
    // (live update). The Prefab Editor is a plain one in every other way; the context is the Level as it is saved. Usage: EditInContext -Scene hexguid -Id hexid
    //================================================================================================
    struct edit_in_context_query_cmd : level_query_command
    {
        edit_in_context_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "EditInContext", pDataBase) { RegisterArguments(); }
        const char* getCommandHelp() const noexcept override
        {
            return "Opens the prefab of an instance in a Prefab Editor, placed where the instance is, with the Level's scenes around it as context (faded, never picked or saved) and the instance itself left out. Save brings the instances up to date. Usage: EditInContext -Scene hexguid -Id hexid (the instance's root)";
        }
        void RegisterArguments() noexcept override
        {
            m_hScene = m_Parser.addOption("Scene", "Scene guid of the instance, 16 hex digits", true, 1);
            m_hId    = m_Parser.addOption("Id",    "Permanent id of the instance's root, 8 or 16 hex digits", true, 1);
        }

        std::string Query() noexcept override
        {
            auto SceneArg = m_Parser.getOptionArgAs<std::string>(m_hScene, 0);
            auto IdArg    = m_Parser.getOptionArgAs<std::string>(m_hId, 0);
            if (std::holds_alternative<xerr>(SceneArg) || std::holds_alternative<xerr>(IdArg)) return "EditInContext: bad arguments";
            const auto Scene = xscene::commands::ParseSceneGuid(std::get<std::string>(SceneArg));
            const auto Id    = xscene::commands::ParseEntityId(std::get<std::string>(IdArg));

            auto& Source = LevelContext();
            if (State().isPlaying())        return "EditInContext: not while playing";
            if (State().isPrefabEditor())   return "EditInContext: an instance inside a prefab is not edited in context yet";
            const auto* pScene = World().m_SceneMgr.Find(Scene);
            if (!pScene || !pScene->m_LocalToRuntime.contains(Id)) return "EditInContext: the instance was not found";
            const auto* pPI = xscene::FindPrefabInstance(World(), pScene->m_LocalToRuntime.at(Id));
            if (!pPI || pScene->m_InstanceMembers.contains(Id)) return "EditInContext: the entity is not the root of a prefab instance";

            const auto Prefab = xresource::full_guid{ .m_Instance = pPI->m_PrefabInstance.m_Instance, .m_Type = xecs::prefab::type_guid_v };
            for (auto* pOther : g_LevelContexts)
                if (pOther->State().isPrefabEditor() && pOther->State().m_CurrentPrefab.m_Instance == Prefab.m_Instance)
                    return "EditInContext: that prefab is already open in an editor (close it first)";
            if (!g_OpenLevelSession) return "EditInContext: no editor available";

            // The Level around the prefab needs its Game's modules to load whole: the editor of the prefab is opened under the Level's Game (not written into the prefab).
            if (const auto Game = GameOfLevel(State().m_CurrentLevel.m_Instance.m_Value); Game) { g_PrefabGameOverride[Prefab.m_Instance.m_Value] = Game; g_PrefabGameFrom[Prefab.m_Instance.m_Value] = "the Level"; }
            const auto Reply = g_OpenLevelSession(Prefab);
            level_context* pDoc = nullptr;
            for (auto* pOther : g_LevelContexts)
                if (pOther->State().isPrefabEditor() && pOther->State().m_CurrentPrefab.m_Instance == Prefab.m_Instance) pDoc = pOther;
            if (!pDoc) { ErasePrefabGameOverride(Prefab.m_Instance.m_Value); return "EditInContext: " + Reply; }

            std::string Note;
            if (const auto Why = EnterContextEdit(*pDoc, Source, Scene, Id, Note); !Why.empty())
            {
                // No plain Prefab Editor under the Level's Game is left behind: the editor this command opened is closed again (nothing in it was edited).
                (void)pDoc->m_Undo.Query("Close -Save 0");
                ErasePrefabGameOverride(Prefab.m_Instance.m_Value);
                return std::format("EditInContext: {}", Why);
            }
            return std::format("EditInContext: ok, {} context scene(s){}{}", pDoc->State().m_ContextScenes.size(), Note.empty() ? "" : " - ", Note);
        }
        xcmdline::parser::handle m_hScene, m_hId;
    };

    //================================================================================================
    // DescribeRoles - what each scene of this editor is in this session (document: picked, saved, undone; context: drawn faded, never picked) and what the render did with them last.
    //================================================================================================
    struct describe_roles_query_cmd : level_query_command
    {
        describe_roles_query_cmd(xundo::system& System, void* pDataBase) noexcept : level_query_command(System, "DescribeRoles", pDataBase) {}
        const char* getCommandHelp() const noexcept override
        {
            return "Says the role of each scene of this editor (Document: picked, saved, in the undo; Context: drawn first and faded, never picked, edited or saved), how many entities are context or hidden, whether it is editing in context, and what the last draw did (DrawnContext=, DrawnDocument= items drawn, Faded=). Usage: DescribeRoles";
        }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            auto&       Ctx   = LevelContext();
            const auto& S     = State();
            role_sets   Sets;
            BuildRoleSets(World(), S, Sets);

            const auto List = [](const std::vector<xecs::scene::guid>& Scenes)
            {
                std::string Out;
                for (const auto& Scene : Scenes) Out += (Out.empty() ? "" : ",") + xscene::commands::FormatSceneGuid(Scene);
                return Out;
            };
            std::string Out = "DescribeRoles: ok\n";
            Out += std::format("Document={}\n", List(S.m_OpenScenes));
            Out += std::format("Context={}\n", List(S.m_ContextScenes));
            Out += std::format("ContextEntities={}\nHiddenEntities={}\n", Sets.m_Context.size(), Sets.m_Hidden.size());
            Out += std::format("InContext={}\n", S.m_ContextEdit.m_bActive ? 1 : 0);
            if (S.m_ContextEdit.m_bActive)
            {
                const auto& E = S.m_ContextEdit;
                Out += std::format("Level={:016X}\nInstanceScene={}\nInstance={}\nPlaced={:.3f},{:.3f},{:.3f}\n", E.m_Level.m_Instance.m_Value, xscene::commands::FormatSceneGuid(E.m_Scene), xscene::commands::FormatEntityId(E.m_Root)
                                 , E.m_Placed.m_Position.m_X, E.m_Placed.m_Position.m_Y, E.m_Placed.m_Position.m_Z);
            }
            if (Ctx.m_DescribeRoleDraw) Out += Ctx.m_DescribeRoleDraw();
            return Out;
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
            // The change is turned down: the editor that handed it over (its template has it) and the others are brought back to the file (live update, prefabs_plan.md phase 6).
            LiveUpdatePrefabElsewhere(&State(), State().m_CurrentPrefab);
        }

        xcmdline::parser::handle m_hFolder;
    };
}

#endif // XLEVEL_COMMANDS_PREFAB_EDITOR_H
