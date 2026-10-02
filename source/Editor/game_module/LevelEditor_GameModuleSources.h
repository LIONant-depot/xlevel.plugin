#ifndef XLVL_NEW_LevelEditor_GAME_MODULE_SOURCES_H
#define XLVL_NEW_LevelEditor_GAME_MODULE_SOURCES_H
#pragma once

// The script project. A script module is only source files (its resource descriptor's source_db folder); the editor turns the
// project's referenced modules into a CMake project it generates under <project>\Cache\Script (never checked in, rebuilt from
// the modules), builds it with Visual Studio's generator, and gets Game.dll in the root of the project's compiled resources
// (<project>\Cache\Resources\Platforms\WINDOWS). The generated project links the game entry points of xscript_module.plugin
// and the import library of the editor's own ECS (LIONCore.lib: xECSV2 is compiled into LIONCore.dll, see xLIONCore), so the DLL shares the
// editor's one component registry. (It used to link xECSV2.lib, the stale import library of a DLL the build no longer makes: the game DLL then
// registered its components in a second registry that no world ever read.)
//
// The project is regenerated when the module list changes (AddProjectModuleReference and friends) and once when the project
// loads. The cmake reconfigure and build it needs ride the Game.dll reload triggers (window focus regained, Play pressed).
#include <fstream>
#include "plugins/xscript_module.plugin/source/Module/xscript_module_files.h"

namespace xlevel
{
    // Everything about where the script project's files are. Computed once on the main thread: the build runs on a background
    // thread and must not reach into the library manager.
    struct script_project_paths
    {
        std::filesystem::path m_Project;      // the project root (the folder that holds Descriptors and Project.config)
        std::filesystem::path m_Root;         // <project>\Cache\Script
        std::filesystem::path m_BuildDir;     // <Root>\Build: the Visual Studio solution and its intermediate files
        std::filesystem::path m_CMakeLists;   // <Root>\CMakeLists.txt, generated
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

    // The newest time at which anything the DLL is built from changed: the game entry files, the generated project (a module
    // added, removed or renamed) and every referenced module's own source files (a plain content edit changes none of the
    // others). It reads the script config and the library manager, so it is called on the main thread and its result is
    // handed to the build as a value. A module that does not resolve is skipped, like a stale reference everywhere else.
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
        for (auto& Ref : g_ScriptConfig.m_ModuleRefs)
        {
            std::wstring DescFolder;
            xresource_editor::g_LibMgr.getNodeInfo(Ref, [&](xresource_editor::library_db::info_node& Node)
            {
                const auto SlashPos = Node.m_Path.find_last_of(L'\\');
                DescFolder = (SlashPos == std::wstring::npos) ? Node.m_Path : Node.m_Path.substr(0, SlashPos);
            });
            if (DescFolder.empty()) continue;
            // the module's descriptor and the files it lists (a file that is only in the folder is not part of the build)
            const auto Module = xscript::module::Resolve(DescFolder, /*bWriteMigration*/ false);
            const auto T = xscript::module::NewestWrite(DescFolder, Module);
            if (T > Latest) Latest = T;
        }
        return Latest;
    }

    // Writes <Root>\CMakeLists.txt from the referenced modules: their source files, one source_group per module so Visual
    // Studio's Solution Explorer shows a folder per module, and their headers force-included into the precompiled header.
    // A header is only listed in a target's sources for display; CMake does not compile it, so a header-only module (the
    // common case, since self-registration is inline by design) would never run its registration. Force-including it into the
    // PCH, which CMake does compile, guarantees it is compiled, with no generated stub.
    //
    // The file is only written when its content changes (and then this returns true): its time is one of the things the DLL is
    // compared against, and rewriting it at every launch would force a full rebuild every time the editor opens.
    inline bool RegenerateGameModuleSources(const script_project_paths& P) noexcept
    {
        struct module_entry { std::wstring m_Folder; std::wstring m_Name; std::vector<std::wstring> m_Files; std::vector<std::wstring> m_Sources; std::vector<std::wstring> m_Pch; xscript::module::descriptor m_Descriptor; };
        std::vector<module_entry> Modules;
        std::vector<std::wstring> RuntimeFiles;                          // the DLLs the libraries bring: copied next to the loaded Game.dll (see CopyGamePluginForLoad)
        for (auto& Ref : g_ScriptConfig.m_ModuleRefs)
        {
            std::wstring DescFolder;
            std::string  Name;
            xresource_editor::g_LibMgr.getNodeInfo(Ref, [&](xresource_editor::library_db::info_node& Node)
            {
                Name = Node.m_Info.m_Name;
                const auto SlashPos = Node.m_Path.find_last_of(L'\\');
                DescFolder = (SlashPos == std::wstring::npos) ? Node.m_Path : Node.m_Path.substr(0, SlashPos);
            });
            if (DescFolder.empty()) continue;
            // The descriptor is the truth: what it lists is built, nothing else. A module from before the descriptors gets one written from its folder, once.
            const auto Module = xscript::module::Resolve(DescFolder, /*bWriteMigration*/ true);
            const std::string Label = Name.empty() ? std::string("Module") : Name;
            if (!Module.m_Error.empty()) { xeditor::NotifyError(std::format("Script module '{}': its Descriptor.txt cannot be read ({}); the module is left out of the game project", Label, Module.m_Error)); continue; }
            {
                std::vector<std::string> Problems;
                Module.m_Descriptor.Validate(Problems);
                for (const auto& Missing : Module.m_Missing) Problems.push_back(std::format("File '{}' is listed but is not in source_db", Missing));
                for (const auto& Problem : Problems) xeditor::NotifyError(std::format("Script module '{}': {}", Label, Problem));
            }
            module_entry Entry;
            Entry.m_Folder     = xscript::module::SourceDb(DescFolder).wstring();
            Entry.m_Name       = Name.empty() ? L"Module" : xstrtool::To(Name);
            Entry.m_Descriptor = Module.m_Descriptor;
            for (auto& F : Module.m_Compiled)   { Entry.m_Files.push_back(F.wstring()); Entry.m_Sources.push_back(F.wstring()); }
            for (auto& F : Module.m_Headers)      Entry.m_Files.push_back(F.wstring());
            for (auto& F : Module.m_PchHeaders)   Entry.m_Pch.push_back(F.wstring());
            if (!Entry.m_Files.empty() || !Entry.m_Descriptor.m_Libraries.empty()) Modules.push_back(std::move(Entry));
        }
        // Forward slashes throughout: CMake treats a backslash as an escape character.
        auto Fwd = [](const std::filesystem::path& Path) noexcept { std::wstring S = Path.wstring(); std::ranges::replace(S, L'\\', L'/'); return S; };
        const auto Root = Fwd(P.m_XGpuRoot);

        std::wstring C = L"# Generated by the editor from the project's script modules - do not edit; it is rewritten when the module list changes.\n";
        C += L"cmake_minimum_required(VERSION 3.13)\nproject(Script LANGUAGES CXX)\n";
        C += L"add_definitions(-DUNICODE -D_UNICODE)\nset(CMAKE_CXX_STANDARD 20)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\n";
        C += L"set(CMAKE_SUPPRESS_REGENERATION true)\nset(CMAKE_CONFIGURATION_TYPES \"Debug;Release\" CACHE STRING \"\" FORCE)\n\n";
        C += std::format(L"set(XGPU_ROOT \"{}\")\n\n", Root);

        C += L"set(MODULE_SOURCES\n";
for (auto& M : Modules) for (auto& F : M.m_Files) C += L"  \"" + Fwd(F) + L"\"\n";
        C += L")\n\nset(MODULE_PCH_HEADERS\n";
for (auto& M : Modules)
            for (auto& F : M.m_Pch) C += L"  \"$<$<COMPILE_LANGUAGE:CXX>:" + Fwd(F) + L">\"\n";
        C += L")\n\n";

        // Each consumer needs its own copy of xtextfile's and xproperty's backend (no shared state), which is why they are compiled
        // into the DLL and, like the entry point, kept out of the precompiled header.
        C += L"set(BACKEND_SOURCES \"${XGPU_ROOT}/dependencies/xtextfile/source/xtextfile.cpp\" \"${XGPU_ROOT}/dependencies/xproperty/source/xcore/my_properties.cpp\")\n";
        C += L"add_library(Game SHARED \"${XGPU_ROOT}/plugins/xscript_module.plugin/source/Runtime/xscript_game_entry.cpp\" ${MODULE_SOURCES} ${BACKEND_SOURCES})\n";
        C += L"target_include_directories(Game PRIVATE \"${XGPU_ROOT}\"";
        for (const wchar_t* Dep : { L"xECSV2/src", L"xerr", L"xresource_guid", L"xtextfile", L"xproperty", L"xresource_pipeline_v2", L"xstrtool", L"xcontainer", L"xdelegate", L"xscheduler", L"xmath", L"xbits", L"box3d/include" })
            C += std::format(L" \"${{XGPU_ROOT}}/dependencies/{}\"", Dep);
        C += L")\ntarget_compile_definitions(Game PRIVATE XECS_BUILD_SHARED)\n";
        // What the modules' descriptors add: their defines and their third-party libraries. Every path in a descriptor is relative to the project.
        {
            auto ProjectPath = [&](const std::string& Relative) { return Fwd(P.m_Project / std::filesystem::path(xstrtool::To(Relative))); };
            auto Quoted = [](const std::string& Text) { return L" \"" + xstrtool::To(Text) + L"\""; };
            auto AddList = [&](const wchar_t* pCommand, const std::vector<std::string>& Items, auto&& Convert)
            {
                if (Items.empty()) return;
                C += std::format(L"{}(Game PRIVATE", pCommand);
                for (const auto& Item : Items) C += L" \"" + Convert(Item) + L"\"";
                C += L")\n";
            };
            for (auto& M : Modules)
            {
                const auto& D = M.m_Descriptor;
                auto Same = [](const std::string& Text) { return xstrtool::To(Text); };
                auto IsPath = [](const std::string& Text) { return Text.find_first_of("/\\") != std::string::npos; };
                if (!D.m_Defines.empty()) { C += L"# " + M.m_Name + L": defines\n"; AddList(L"target_compile_definitions", D.m_Defines, Same); }
                for (auto& L : D.m_Libraries)
                {
                    C += L"# " + M.m_Name + L": library " + xstrtool::To(L.m_Name) + L"\n";
                    AddList(L"target_include_directories", L.m_IncludeDirs, ProjectPath);
                    AddList(L"target_link_directories",    L.m_LibDirs,     ProjectPath);
                    AddList(L"target_link_libraries",      L.m_Libs,        [&](const std::string& Lib) { return IsPath(Lib) ? ProjectPath(Lib) : xstrtool::To(Lib); });
                    AddList(L"target_compile_definitions", L.m_Defines,     Same);
                    for (const auto& File : L.m_RuntimeFiles) RuntimeFiles.push_back(std::filesystem::path(ProjectPath(File)).make_preferred().wstring());
                }
            }
        }
        // The ECS is compiled into LIONCore.dll (xLIONCore), so that is the import library the game DLL needs to share the editor's registry.
        C += std::format(L"target_link_libraries(Game PRIVATE \"{}/xLIONCore/$<CONFIG>/LIONCore.lib\")\n", Fwd(P.m_XGpuBinDir));
        C += L"target_precompile_headers(Game PRIVATE \"$<$<COMPILE_LANGUAGE:CXX>:${XGPU_ROOT}/dependencies/xECSV2/src/xecs.h>\" ${MODULE_PCH_HEADERS})\n";
        C += L"set_source_files_properties(${BACKEND_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n";

        // The DLL goes to the compiled resources; the debugger finds symbols through the path embedded at link time, so the PDB
        // is linked with only its bare name (/PDBALTPATH) and the editor copies it next to the copy of the DLL it loads. That
        // leaves the compiler's own PDB free to be rewritten while a debugger is attached. /FS lets concurrent compiles share
        // one PDB, which /MP makes likely.
        const auto DllDir = Fwd(P.m_DllRoot);
        C += std::format(L"set_target_properties(Game PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG \"{0}/Debug\" RUNTIME_OUTPUT_DIRECTORY_RELEASE \"{0}\" PDB_OUTPUT_DIRECTORY \"${{CMAKE_BINARY_DIR}}/GamePdb\")\n", DllDir);
        C += L"set_property(TARGET Game APPEND_STRING PROPERTY LINK_FLAGS \" /PDBALTPATH:Game.pdb\")\n";
        C += L"target_compile_options(Game PRIVATE /FS /MP)\n\n";

        for (auto& M : Modules)
        {
            C += std::format(L"source_group(TREE \"{}\" PREFIX \"{}\" FILES\n", Fwd(M.m_Folder), M.m_Name);
            for (auto& F : M.m_Files) C += L"  \"" + Fwd(F) + L"\"\n";
            C += L")\n";
        }

        {
            std::wstring Manifest;
            for (auto& F : RuntimeFiles) Manifest += F + L"\n";
            const auto ManifestPath = P.m_Root / L"runtime_files.txt";
            std::wifstream ManifestIn(ManifestPath, std::ios::binary);
            const std::wstring Old = ManifestIn.is_open() ? std::wstring((std::istreambuf_iterator<wchar_t>(ManifestIn)), std::istreambuf_iterator<wchar_t>()) : std::wstring();
            if (Old != Manifest)
            {
                std::error_code ManifestEc;
                std::filesystem::create_directories(P.m_Root, ManifestEc);
                std::wofstream ManifestOut(ManifestPath, std::ios::trunc);
                ManifestOut << Manifest;
            }
        }
        {
            std::wifstream ExistingIn(P.m_CMakeLists, std::ios::binary);
            if (ExistingIn.is_open())
            {
                std::wstring Existing((std::istreambuf_iterator<wchar_t>(ExistingIn)), std::istreambuf_iterator<wchar_t>());
                if (Existing == C) return false;
            }
        }

        std::error_code Ec;
        std::filesystem::create_directories(P.m_CMakeLists.parent_path(), Ec);
        std::wofstream Out(P.m_CMakeLists, std::ios::trunc);
        Out << C;
        return true;
    }
}

#endif // XLVL_NEW_LevelEditor_GAME_MODULE_SOURCES_H
