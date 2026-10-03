#ifndef XLVL_NEW_LevelEditor_GAME_MODULE_SOURCES_H
#define XLVL_NEW_LevelEditor_GAME_MODULE_SOURCES_H
#pragma once

// The script project. A script module is only source files (its resource descriptor's source_db folder); the project's Game resource lists the modules the game is made of, and the
// resource pipeline turns them into a CMake project it writes under <project>\Cache\Script (never checked in: the pipeline compiles each module into its CMake file, then the Game
// from those, see xscript_module_compiler and xgame_compiler). The editor builds that project with Visual Studio's generator and gets Game.dll in the root of the project's compiled
// resources (<project>\Cache\Resources\Platforms\WINDOWS). The project links the game entry points of xscript_module.plugin and the import library of the editor's own ECS
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
        std::filesystem::path m_Root;         // <project>\Cache\Script
        std::filesystem::path m_BuildDir;     // <Root>\Build: the Visual Studio solution and its intermediate files
        std::filesystem::path m_CMakeLists;   // <Root>\CMakeLists.txt, written by the compile of the Game resource
        std::filesystem::path m_DllRoot;      // <project>\Cache\Resources\Platforms\WINDOWS: where the compiled resource goes
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
        P.m_Root       = Project / L"Cache" / L"Script";
        P.m_BuildDir   = P.m_Root / L"Build";
        P.m_CMakeLists = P.m_Root / L"CMakeLists.txt";
        P.m_DllRoot    = Project / L"Cache" / L"Resources" / L"Platforms" / L"WINDOWS";
        P.m_Dll        = (P.m_Config == L"Release" ? P.m_DllRoot : P.m_DllRoot / P.m_Config) / L"Game.dll";
        P.m_PdbDir     = P.m_BuildDir / L"GamePdb" / P.m_Config;
        P.m_LoadedDir  = P.m_Root / L"Loaded";
        return P;
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
        for (const auto& Ref : ProjectModules())
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
