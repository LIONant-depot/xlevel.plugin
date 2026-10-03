#ifndef XLVL_NEW_LevelEditor_GAME_MODULE_SOURCES_H
#define XLVL_NEW_LevelEditor_GAME_MODULE_SOURCES_H
#pragma once

// The script project. A script module is only source files (its resource descriptor's source_db folder); the project's Game resource lists the modules the game is made of, and the
// resource pipeline turns them into a CMake project it writes under <project>\Cache\Script\<Game guid> (one per Game resource; never checked in: the pipeline compiles each module into its CMake file, then the Game
// from those, see xscript_module_compiler and xgame_compiler). The editor builds that project with Visual Studio's generator and gets Game.dll in the root of the project's compiled
// resources (<project>\Cache\Resources\Platforms\WINDOWS\GameDll\<Game guid>). The project links the game entry points of xscript_module.plugin and the import library of the editor's own ECS
// (LIONCore.lib: xECSV2 is compiled into LIONCore.dll, see xLIONCore), so the DLL shares the editor's one component registry.
//
// Nothing in this file makes the project: the editor only waits for the pipeline to have made it (WaitForGameProject, LevelEditor_ProjectGame.h) and builds. The cmake reconfigure
// and build ride the Game.dll reload triggers (window focus regained, Play pressed, startup).
#include <fstream>
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_ProjectGame.h"

namespace xlevel
{
    // Everything about where the script project's files are. Computed once on the main thread: the build runs on a background
    // thread and must not reach into the library manager.
    struct script_project_paths
    {
        std::filesystem::path m_Project;      // the project root (the folder that holds Descriptors and Project.config)
        std::uint64_t         m_Game = 0;     // the Game these paths are of (the instance guid of the Game resource; 0: none, nothing is built or loaded)
        std::filesystem::path m_Root;         // <project>\Cache\Script\<Game>: the game project of that Game
        std::filesystem::path m_BuildDir;     // <Root>\Build: the Visual Studio solution and its intermediate files (the configure marker and the build markers are in it: per Game)
        std::filesystem::path m_CMakeLists;   // <Root>\CMakeLists.txt, written by the compile of the Game resource
        std::filesystem::path m_DllRoot;      // <project>\Cache\Resources\Platforms\WINDOWS\GameDll\<Game>: where the DLL of the Game goes
        std::filesystem::path m_Dll;          // m_DllRoot\Game.dll for Release, m_DllRoot\<Config>\Game.dll otherwise: the compiled
                                              // resource for the running configuration (one per configuration, so switching between
                                              // a Debug and a Release editor never forces a rebuild)
        std::filesystem::path m_PdbDir;       // where the linker writes Game.pdb for the running configuration
        std::filesystem::path m_LoadedDir;    // <Root>\Loaded: the copies of Game.dll that are actually loaded
        std::filesystem::path m_XGpuRoot;     // the xGPU checkout this editor was built from
        std::filesystem::path m_XGpuBinDir;   // its build directory: xLIONCore\<Config>\LIONCore.lib is under it
        std::wstring          m_Config;       // "Debug" or "Release": the configuration of the running editor

        std::filesystem::path RuntimeDir() const { return m_XGpuRoot / L"plugins" / L"xscript_module.plugin" / L"source" / L"Runtime"; }
    };

    // The same paths for another Game: its game project, build, DLL and loaded copies are its own (Games that share script modules do not disturb each other's configure and build).
    inline script_project_paths ForGame(script_project_paths P, std::uint64_t Game) noexcept
    {
        const std::wstring G = std::format(L"{:X}", Game);
        P.m_Game       = Game;
        P.m_Root       = P.m_Project / L"Cache" / L"Script" / G;
        P.m_BuildDir   = P.m_Root / L"Build";
        P.m_CMakeLists = P.m_Root / L"CMakeLists.txt";
        P.m_DllRoot    = P.m_Project / L"Cache" / L"Resources" / L"Platforms" / L"WINDOWS" / L"GameDll" / G;
        P.m_Dll        = (P.m_Config == L"Release" ? P.m_DllRoot : P.m_DllRoot / P.m_Config) / L"Game.dll";
        P.m_PdbDir     = P.m_BuildDir / L"GamePdb" / P.m_Config;
        P.m_LoadedDir  = P.m_Root / L"Loaded";
        return P;
    }

    // The paths of the editor and the project, for no Game yet (m_Game 0): ForGame says where a Game's files are.
    inline script_project_paths MakeScriptProjectPaths(const std::filesystem::path& Project) noexcept
    {
        TCHAR szModulePath[MAX_PATH];
        GetModuleFileName(NULL, szModulePath, MAX_PATH);
        const std::filesystem::path ExeDir = std::filesystem::path(szModulePath).parent_path();   // .../Build/<BuildDirName>/<Config>

        script_project_paths P;
        P.m_Project    = Project;
        P.m_Config     = ExeDir.filename().wstring();
        P.m_XGpuBinDir = ExeDir.parent_path();
        P.m_XGpuRoot   = P.m_XGpuBinDir.parent_path().parent_path();
        return ForGame(std::move(P), 0);
    }

    // Before each Game had its own folder the one game project was Cache\Script itself (CMakeLists.txt, runtime_files.txt, Build, Loaded) and its DLL was in the root of the compiled resources
    // of the platform. Nothing reads those any more: they are removed, best effort (a file that is in use stays; it is not in the way of anything).
    inline void RemoveLegacyScriptFolder(const script_project_paths& P) noexcept
    {
        std::error_code Ec;
        const auto Root = P.m_Project / L"Cache" / L"Script";
        for (const wchar_t* pName : { L"CMakeLists.txt", L"runtime_files.txt" }) std::filesystem::remove(Root / pName, Ec);
        for (const wchar_t* pName : { L"Build", L"Loaded" }) std::filesystem::remove_all(Root / pName, Ec);
        const auto Platform = P.m_Project / L"Cache" / L"Resources" / L"Platforms" / L"WINDOWS";
        std::filesystem::remove(Platform / L"Game.dll", Ec);
        std::filesystem::remove_all(Platform / L"Debug", Ec);
    }

    // The project this example edits sits next to the xGPU checkout.
    inline script_project_paths MakeScriptProjectPaths() noexcept
    {
        auto P = MakeScriptProjectPaths(std::filesystem::path{});
        return MakeScriptProjectPaths(P.m_XGpuRoot / L"example.lionprj");
    }

    // The newest time at which anything the DLL is built from changed: the game entry files, the generated project (written by the compile of the Game resource when a module,
    // a file or a library was added, removed or renamed) and every module's own source files (a plain content edit changes none of the others). It reads the Game resource, so it is
    // called on the main thread and its result is handed to the build as a value. A module that does not resolve is skipped, like a stale reference everywhere else.
    inline std::filesystem::file_time_type GetLatestModuleSourceWriteTime(const script_project_paths& P) noexcept
    {
        std::error_code Ec;
        auto Latest = std::filesystem::file_time_type::min();
        auto Consider = [&](const std::filesystem::path& File) noexcept
        {
            if (const auto T = std::filesystem::last_write_time(File, Ec); !Ec && T > Latest) Latest = T;
        };
        Consider(P.m_CMakeLists);
        Consider(P.RuntimeDir() / L"xscript_game_entry.cpp");
        Consider(P.RuntimeDir() / L"xscript_registration.h");
        for (const auto& Ref : P.m_Game ? ReadGame(P.m_Game).m_Modules : std::vector<xscript::module::module_ref>{})
        {
            const auto DescFolder = xgame::FindModuleFolder(P.m_Project, Ref);
            if (DescFolder.empty()) continue;
            // the module's descriptor and the files it lists (a file that is only in the folder is not part of the build)
            const auto Module = xscript::module::Resolve(DescFolder, /*bWriteMigration*/ false);
            const auto T = xscript::module::NewestWrite(DescFolder, Module);
            if (T > Latest) Latest = T;
        }
        return Latest;
    }
}

#endif // XLVL_NEW_LevelEditor_GAME_MODULE_SOURCES_H
