#ifndef XLVL_NEW_LevelEditor_COMPONENT_COMPATIBILITY_H
#define XLVL_NEW_LevelEditor_COMPONENT_COMPATIBILITY_H
#pragma once

// Phase 2 of the component-registry compatibility plan (Build/RELOAD_CRASH_REPORT.md's own follow-up
// design - see the "LevelEditor component-registry compatibility" section appended to the shared plan doc).
// One shared check reused by four trigger points: live Game.dll hot-reload (a candidate DLL's own
// XScript_GetComponentDisplayInfo manifest), normal scene Open (the already-loaded live registry),
// removing a Script-Module project reference (a trial-build candidate's manifest), and a source-control
// pull (routes through the same hot-reload gate as an ordinary edit - no separate check needed there).
// Deliberately just the pure diff here - no UI yet. The confirm-modal shape (A: strip missing
// components and continue / B: cancel) gets built alongside its first real call site (Phase 3) rather
// than designed in isolation, since guessing the interaction shape before a real caller exists risks
// getting it wrong and having to redo it.

#include <map>
#include "plugins/xscene.plugin/source/Editor/xscene_component_display.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_ProjectGame.h"

namespace xlevel
{
    // Cross-references every entry in Required against whatever "is this type currently available"
    // predicate the caller supplies - IsAvailable is deliberately a predicate, not a fixed source, so
    // the SAME function serves both "check against the live registry" (xecs::component::mgr::
    // findComponentTypeInfo, already-loaded DLL) and "check against a not-yet-committed candidate
    // DLL's own manifest" (XScript_GetComponentDisplayInfo output, pre-flight probe) without either caller
    // needing to know which. Returns only the entries NOT available, deduplicated by guid (a scene can
    // list the same component more than once if this is ever called with several scenes' manifests
    // merged together upstream).
    inline std::vector<xecs::scene::component_dependency> CheckComponentCompatibility
    ( const std::vector<xecs::scene::component_dependency>& Required
    , const std::function<bool(xecs::component::type::guid)>& IsAvailable
    ) noexcept
    {
        std::vector<xecs::scene::component_dependency> Missing;
        for (auto& Dep : Required)
        {
            if (IsAvailable(Dep.m_Guid)) continue;
            if (std::find_if(Missing.begin(), Missing.end(), [&](auto& M) noexcept { return M.m_Guid == Dep.m_Guid; }) != Missing.end())
                continue;
            Missing.push_back(Dep);
        }
        return Missing;
    }

    //================================================================================================
    // Scenes, modules and Games - the data-only check. A scene's ComponentDeps.txt says which module defines each component it uses (xecs::scene::component_dependency::m_Module); a Game
    // resource lists the script modules it is made of. A Game can run a scene when it lists every module the scene needs (and the scenes it depends on need: they load with it). Nothing here
    // loads a DLL or a world: it reads files, so it works before anything is built and on a fresh checkout.
    //================================================================================================
    struct module_need
    {
        std::vector<std::string>    m_Components;       // the components of the scene(s) that this module defines
        std::vector<std::uint64_t>  m_Scenes;           // the scenes that use them
    };

    struct scene_module_needs
    {
        std::map<std::uint64_t, module_need> m_Modules;     // by module guid
        module_need                          m_Unknown;     // components whose module nobody could say (the file is older than module tracking)
        std::vector<std::uint64_t>           m_Visited;     // the scenes that were read (the scene, and its parents when asked)
    };

    inline xecs::scene::guid MakeSceneGuid( std::uint64_t Value ) noexcept { return xecs::scene::guid{ xresource::instance_guid{ Value } }; }

    // The scenes a scene depends on (its ParentScenes, from its Descriptor.txt).
    inline std::vector<std::uint64_t> ReadSceneParents( const std::wstring& Project, std::uint64_t Scene ) noexcept
    {
        std::vector<std::uint64_t> Out;
        xecs::scene::descriptor D;
        xproperty::settings::context Context{};
        if (auto Err = D.Serialize(true, xecs::scene::details::SceneFolder(std::wstring_view(Project), MakeSceneGuid(Scene)) + L"/Descriptor.txt", Context); Err) return Out;
        for (const auto& P : D.m_ParentScenes) Out.push_back(P.m_Instance.m_Value);
        return Out;
    }

    // The scenes of a Level (its Descriptor.txt).
    inline std::vector<std::uint64_t> ReadLevelScenes( const std::wstring& Project, std::uint64_t Level ) noexcept
    {
        std::vector<std::uint64_t> Out;
        xecs::level::descriptor D;
        xproperty::settings::context Context{};
        const std::wstring Path = std::format(L"{}/Descriptors/Level/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", Project, Level & 0xFF, (Level >> 8) & 0xFF, Level);
        if (auto Err = D.Serialize(true, Path, Context); Err) return Out;
        for (const auto& S : D.m_Scenes) Out.push_back(S.m_Instance.m_Value);
        return Out;
    }

    // Adds what a scene needs (and, with bTransitive, what the scenes it depends on need) to Needs. Reads ComponentDeps.txt; a scene that was never saved has none.
    inline void AddSceneModuleNeeds( scene_module_needs& Needs, const std::wstring& Project, std::uint64_t Scene, bool bTransitive ) noexcept
    {
        if (std::find(Needs.m_Visited.begin(), Needs.m_Visited.end(), Scene) != Needs.m_Visited.end()) return;
        Needs.m_Visited.push_back(Scene);

        auto Add = [&](module_need& Need, const std::string& Component)
        {
            if (std::find(Need.m_Components.begin(), Need.m_Components.end(), Component) == Need.m_Components.end()) Need.m_Components.push_back(Component);
            if (std::find(Need.m_Scenes.begin(), Need.m_Scenes.end(), Scene) == Need.m_Scenes.end()) Need.m_Scenes.push_back(Scene);
        };
        for (const auto& Dep : xecs::scene::LoadSceneComponentDependencies(std::wstring_view(Project), MakeSceneGuid(Scene)))
        {
            if (Dep.m_Module == 0) continue;                                                // the engine's or the editor's: no module has to provide it
            Add(Dep.m_Module == xecs::scene::unknown_module_v ? Needs.m_Unknown : Needs.m_Modules[Dep.m_Module], Dep.m_Name);
        }
        if (bTransitive)
            for (const auto Parent : ReadSceneParents(Project, Scene)) AddSceneModuleNeeds(Needs, Project, Parent, true);
    }

    // The modules of Needs that a Game (the modules it lists) does not have.
    inline std::vector<std::pair<std::uint64_t, module_need>> MissingModules( const scene_module_needs& Needs, const std::vector<xscript::module::module_ref>& GameModules ) noexcept
    {
        std::vector<std::pair<std::uint64_t, module_need>> Missing;
        for (const auto& [Module, Need] : Needs.m_Modules)
        {
            const bool bHas = std::any_of(GameModules.begin(), GameModules.end(), [&](const xscript::module::module_ref& M) { return M.m_Instance.m_Value == Module; });
            if (!bHas) Missing.emplace_back(Module, Need);
        }
        return Missing;
    }

    // The scenes of the project (saved ones: their ComponentDeps.txt) that use components of a module, and which components. The scene itself, not the scenes it depends on.
    inline std::vector<std::pair<std::uint64_t, std::vector<std::string>>> ScenesUsingModule( const std::wstring& Project, std::uint64_t Module ) noexcept
    {
        std::vector<std::pair<std::uint64_t, std::vector<std::string>>> Found;
        for (const auto& [Instance, Name] : xlevel::commands::BuildAssetNameMap(xecs::scene::type_guid_v))
        {
            scene_module_needs Needs;
            AddSceneModuleNeeds(Needs, Project, Instance, /*bTransitive*/ false);
            if (const auto It = Needs.m_Modules.find(Module); It != Needs.m_Modules.end()) Found.emplace_back(Instance, It->second.m_Components);
        }
        std::ranges::sort(Found);
        return Found;
    }

    // ---- where a type comes from, for the hints (xscene::type_source) ---------------------------------------------------------------------------------------------------------------

    // The name of a script module, from the library (kept for a moment: the hints ask every frame). The module's guid when the library does not know it.
    inline std::string ModuleDisplayName( game_plugin_state& Plugin, std::uint64_t Module ) noexcept
    {
        const auto Now = std::chrono::steady_clock::now();
        if (Plugin.m_ModuleNames.empty() || Now - Plugin.m_ModuleNamesAt > std::chrono::seconds(2))
        {
            Plugin.m_ModuleNames   = xlevel::commands::BuildAssetNameMap(xscript::module::type_guid_v);
            Plugin.m_ModuleNamesAt = Now;
        }
        const auto It = Plugin.m_ModuleNames.find(Module);
        return It == Plugin.m_ModuleNames.end() ? std::format("{:X}", Module) : It->second;
    }

    inline std::string RelativeToSourceDb( std::string File ) noexcept
    {
        std::ranges::replace(File, '\\', '/');
        const auto At = xscript::module::Lower(File).rfind("/source_db/");
        return At == std::string::npos ? File : File.substr(At + 11);
    }

    // Where a component (or a system) is defined: the module and the file when the loaded Game.dll says, built in when it is not in it, unknown when no Game.dll is loaded or it is
    // older than the registrations.
    inline xscene::type_source ResolveTypeSource( bool bSystem, std::uint64_t Guid ) noexcept
    {
        xscene::type_source S;
        auto* pPlugin = g_pGamePlugin;
        if (!pPlugin) return S;
        if (pPlugin->isLoaded() && !pPlugin->m_bHasRegistrations) return S;               // a Game.dll that does not tell
        S.m_bKnown = true;
        for (const auto& R : pPlugin->m_Registrations)
        {
            if (R.m_Kind != (bSystem ? 1 : 0) || R.m_Guid != Guid) continue;
            S.m_Module = R.m_Module;
            S.m_Path   = R.m_File;
            if (R.m_Module)
            {
                S.m_ModuleName = ModuleDisplayName(*pPlugin, R.m_Module);
                S.m_File       = RelativeToSourceDb(R.m_File);
            }
            else S.m_bBuiltIn = true;                                                       // defined outside every module: the engine's header, registered a second time
            return S;
        }
        S.m_bBuiltIn = true;                                                                // not in the game module's list: the engine's or the editor's own
        return S;
    }

    // The hook the panels use. Registered when the program starts: it has to be there before any panel draws, and nothing else owns it.
    inline const bool g_TypeSourceRegistered = (xscene::g_SourceOfType = &ResolveTypeSource, true);

    // The modules a Game resource lists (read from its Descriptor.txt: the Game does not have to be the project's, or loaded). Empty and false when the Game is not in the project.
    inline bool ReadGameModules( std::uint64_t Game, std::vector<xscript::module::module_ref>& Out ) noexcept
    {
        Out.clear();
        const auto Folder = xgame::FindGameFolder(ProjectRoot(), xgame::game_ref{ xresource::instance_guid{ Game } });
        if (Folder.empty()) return false;
        xgame::descriptor D;
        if (xgame::Read(Folder, D)) Out = D.m_Modules;
        return true;
    }
}

#endif // XLVL_NEW_LevelEditor_COMPONENT_COMPATIBILITY_H
