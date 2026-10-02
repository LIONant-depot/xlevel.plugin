#ifndef XLVL_NEW_LevelEditor_GAME_PLUGIN_BUILD_H
#define XLVL_NEW_LevelEditor_GAME_PLUGIN_BUILD_H
#pragma once

// Extracted from LevelEditor_GamePlugin.h (mechanical move, phase 3 of the kit split - see the umbrella
// file's own top comment). game_plugin_state (the one loaded-generation record threaded through
// build/load/play-session alike) and the staleness-check + cmake-invoking rebuild itself
// (BuildGamePluginIfStale), which builds the generated script project (LevelEditor_GameModuleSources.h). Meant to be included via the umbrella (LevelEditor_GamePlugin.h) only, after
// LevelEditor_GamePluginLog.h (LogGamePlugin).
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GameModuleSources.h"
#include "dependencies/xlog/source/xlog_build.h"
#include <optional>
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GameModuleEvents.h"

namespace xlevel
{
    // Outcome of a staleness check (see BuildGamePluginIfStale). Deliberately three-valued, not a
    // bool: once recompile-checks fire automatically (on window focus regained, or on pressing Play -
    // see ConsumeWindowFocusGained's own comment), most checks find nothing to do at all, and the
    // caller (PollGameReload) must be able to tell "nothing changed, don't touch the world" apart
    // from "a new generation was actually built, do the full destroy/recreate reload" - collapsing
    // both into one "succeeded" bool (as the original, button-only version of this did, when every
    // call was a deliberate user click that always meant "yes, reload") would destroy and rebuild the
    // entire runtime world on every single alt-tab back into the editor, whether or not anything
    // actually changed.
    enum class build_result : std::uint8_t { UpToDate, Rebuilt, Failed };

    // One loaded Game.dll generation. Slot is fixed at 1 (0 is xecs::plugin::host_v, reserved for
    // the host's own registrations) - LevelEditor only ever hosts one game plugin at a time, so there is
    // only ever one non-host slot to assign.
    //
    // m_Paths.m_Dll (fixed - the script project's own build output) and m_LoadedDllPath
    // (a generation-suffixed COPY of it, made fresh every load/reload, that's the one actually
    // LoadLibrary'd) are deliberately different files/paths - direct user requirement: "the dll
    // that we are loading should not be the same one that the compiler is compiling... the job of
    // the editor is to copy the new version of the dll with any symbols it may need for debugging."
    // This means a rebuild (whether the user's own external build or LevelEditor's own auto-build below)
    // never has to fight a file this process still has mapped - the compiler always writes to
    // m_Paths.m_Dll, which is NEVER the currently-loaded file.
    struct game_plugin_state
    {
        game_module_events m_Events;      // the editors that take part in a reload subscribe here
        HMODULE                m_hModule          = nullptr;
        xecs::plugin::token     m_Token            = {};
        script_project_paths    m_Paths;
        std::wstring            m_LoadedDllPath;

        // Human-readable outcome of the most recent build/load attempt - printf's own log lines are
        // invisible in a GUI app with no attached console, so RenderSystemRegistryPanel (or wherever
        // the Reload Game button lives) renders this directly instead of leaving the user with no
        // feedback at all about whether a reload actually did anything.
        std::string             m_LastStatus       = "Game.dll: not loaded yet";

        // Async build state - direct user requirement: compiling must never freeze the editor, the
        // Reload Game button must disable itself while a build is in flight, and a FAILED build
        // must leave the currently loaded generation completely untouched (never unloaded in the
        // first place) rather than tearing the world down first and hoping the build succeeds.
        // StartGameReload() kicks off BuildGamePluginIfStale() on a background thread
        // (std::async) and returns immediately; PollGameReload() (called once per frame, at the
        // same safe top-of-frame point the old synchronous reload used to run from) checks for
        // completion without blocking, and only THEN - and only on success - performs the actual,
        // destructive reload steps (destroy world, unload old generation, load new one). This is
        // exactly why the compiled/loaded DLL split exists at all: the build always targets
        // m_Paths.m_Dll, which is never the loaded file, so it can safely run in the background
        // while the OLD generation keeps running completely undisturbed, whether the build
        // eventually succeeds or fails.
        std::future<build_result> m_BuildFuture;
        bool                    m_bBuilding        = false;

        // The outcome of the last finished build, for every editor to react to in its own time (an editor that asked for
        // Play while the build ran starts playing; the others carry on). m_ResultSeq counts finished builds; each editor
        // remembers the last one it has seen.
        build_result            m_LastResult       = build_result::UpToDate;
        std::uint32_t           m_ResultSeq        = 0;

        // The module crashed while registering (see GuardedModuleCall): it stays loaded - what it half registered still points into it - but
        // none of its systems run, until the next reload swaps or unloads it.
        bool                    m_bCrashed         = false;
        bool                    m_bSimulateCrash   = false;      // diagnostic (SimulateModuleCrash): RegisterSystems of the module crashes on purpose

        bool isLoaded(void) const noexcept { return m_hModule != nullptr; }
    };

    // Set once at startup (like the editor state and world services), so a CLI/Console-driven command
    // (LevelEditor_Commands_PlaySession.h) can trigger the same Play/Pause/Stop transitions the menu-bar
    // buttons do without needing synthetic mouse input - same "one instance per process" assumption
    // those two globals already make.
    inline game_plugin_state* g_pGamePlugin = nullptr;

    //---------------------------------------------------------------------------
    // "The editor should try to recompile automatically; if it's already compiled (newer than the one we're using) just load"
    // (direct user direction). The DLL is only rebuilt when it is older than what it is built from: the timestamp check
    // decides whether to invoke cmake at all, and an up-to-date DLL never even sees a rebuild attempted.
    //
    // It blocks until the build ends, which is fine because its only caller runs it on a background thread (StartGameReload),
    // never on the render thread. It touches only Plugin.m_LastStatus, the log (which has a lock) and files and processes,
    // never the world, the editor state or anything ImGui.
    //
    // ModuleSourceTime is a plain value computed by the caller on the main thread (GetLatestModuleSourceWriteTime): that
    // function reads the script config and the library manager, which have no lock, and calling it from here crashed the
    // editor after a reload.
    // Runs one command line synchronously to completion, piping its stdout/stderr through
    // LogGamePlugin one line at a time and returning its exit code (or -1 if it couldn't even be
    // launched). Extracted so BuildGamePluginIfStale can run TWO commands in sequence (an explicit
    // reconfigure, then the actual build - see its own comment on why) without duplicating this
    // pipe-capture machinery, and without the fragile nested-quoting a single `cmd /c "A && B"` string
    // would need for two already-quoted `cmake` invocations.
    // Every process a Game.dll build starts (cmake, MSBuild, the compiler) runs in this job. Closing the editor kills the
    // job with it - a build left running would keep the precompiled header and the PDB locked and break the next build - and
    // CancelGameBuild stops a build on purpose.
    inline HANDLE GameBuildJob() noexcept
    {
        static HANDLE s_hJob = []() noexcept
        {
            HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION Info{};
            Info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (hJob) SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &Info, sizeof(Info));
            return hJob;
        }();
        return s_hJob;
    }

    // Kills whatever the running build started; the build then reports Failed.
    inline void CancelGameBuild() noexcept
    {
        if (auto hJob = GameBuildJob()) TerminateJobObject(hJob, 1);
    }

    // pAdapter, when there is one, turns the output into structured diagnostics (a diagnostic with the lines that belong to it is one event); the raw line
    // is still printed, as it always was. Without an adapter (no host) the line is just logged.
    inline int RunCmakeCommand( std::wstring CmdLine, const std::filesystem::path& WorkingDir, xlog::build_output_adapter* pAdapter = nullptr ) noexcept
    {
        SECURITY_ATTRIBUTES PipeSa{ .nLength = sizeof(PipeSa), .bInheritHandle = TRUE };
        HANDLE ReadPipe = nullptr, WritePipe = nullptr;
        CreatePipe(&ReadPipe, &WritePipe, &PipeSa, 0);
        SetHandleInformation(ReadPipe, HANDLE_FLAG_INHERIT, 0); // this process's own read end must NOT be inherited by the child

        STARTUPINFOW Si
        { .cb         = sizeof(Si)
        , .dwFlags    = STARTF_USESTDHANDLES
        , .hStdOutput = WritePipe
        , .hStdError  = WritePipe
        };
        PROCESS_INFORMATION Pi{};
        // CreateProcessW may write into the command-line buffer - a std::wstring's own data() must
        // stay mutable/writable for that, hence the copy into a plain array rather than passing
        // CmdLine.data() (or a string literal) directly.
        std::vector<wchar_t> CmdLineBuf(CmdLine.begin(), CmdLine.end());
        CmdLineBuf.push_back(L'\0');

        if (!CreateProcessW(nullptr, CmdLineBuf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, WorkingDir.c_str(), &Si, &Pi))
        {
            CloseHandle(ReadPipe);
            CloseHandle(WritePipe);
            LogGamePlugin(std::format("Game.dll: failed to launch cmake (err={})", GetLastError()));
            return -1;
        }
        // In the job before it runs a single instruction, so everything it starts is in it too.
        if (auto hJob = GameBuildJob()) AssignProcessToJobObject(hJob, Pi.hProcess);
        ResumeThread(Pi.hThread);

        // This process's own handle to the write end must close BEFORE reading, or ReadFile below
        // blocks forever waiting for a write-end closure that never comes (the child's copy alone
        // isn't enough - ReadFile only sees EOF once EVERY write handle, including this one, is
        // closed).
        CloseHandle(WritePipe);

        // Blocking reads, one line at a time, until the pipe closes (the child exiting closes its
        // own inherited write handle, which is what makes ReadFile finally return 0) - synchronous,
        // matching this whole function's own "runs only on an explicit user action" scope note above.
        {
            std::string LineBuffer;
            char        Chunk[512];
            DWORD       BytesRead = 0;
            while (ReadFile(ReadPipe, Chunk, sizeof(Chunk), &BytesRead, nullptr) && BytesRead > 0)
            {
                for (DWORD i = 0; i < BytesRead; ++i)
                {
                    if (Chunk[i] == '\n')
                    {
                        if (!LineBuffer.empty() && LineBuffer.back() == '\r') LineBuffer.pop_back();
                        if (pAdapter) { std::printf("%s\n", LineBuffer.c_str()); std::fflush(stdout); pAdapter->Feed(LineBuffer); }
                        else          LogGamePlugin(LineBuffer);
                        LineBuffer.clear();
                    }
                    else
                    {
                        LineBuffer.push_back(Chunk[i]);
                    }
                }
            }
            if (!LineBuffer.empty())
            {
                if (pAdapter) { std::printf("%s\n", LineBuffer.c_str()); std::fflush(stdout); pAdapter->Feed(LineBuffer); }
                else          LogGamePlugin(LineBuffer);
            }
            if (pAdapter) pAdapter->Finish();           // the block still pending is complete: the readers are done
        }
        CloseHandle(ReadPipe);

        WaitForSingleObject(Pi.hProcess, INFINITE);
        DWORD ExitCode = 1;
        GetExitCodeProcess(Pi.hProcess, &ExitCode);
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
        return static_cast<int>(ExitCode);
    }

    // The generator the editor itself was configured with, so the script project is built with the same Visual Studio.
    inline std::wstring ScriptProjectGeneratorArgs( const script_project_paths& P ) noexcept
    {
        std::string Generator = "Visual Studio 17 2022", Platform = "x64";
        if (std::ifstream Cache(P.m_XGpuBinDir / L"CMakeCache.txt"); Cache.is_open())
        {
            for (std::string Line; std::getline(Cache, Line); )
            {
                if (Line.starts_with("CMAKE_GENERATOR:INTERNAL="))          Generator = Line.substr(sizeof("CMAKE_GENERATOR:INTERNAL=") - 1);
                if (Line.starts_with("CMAKE_GENERATOR_PLATFORM:INTERNAL=")) Platform  = Line.substr(sizeof("CMAKE_GENERATOR_PLATFORM:INTERNAL=") - 1);
            }
        }
        std::wstring Args = std::format(L" -G \"{}\"", xstrtool::To(Generator));
        if (!Platform.empty()) Args += std::format(L" -A {}", xstrtool::To(Platform));
        return Args;
    }

    // Each configuration builds its own Game.dll (script_project_paths::m_Dll); a marker written after each build says which
    // configuration built the file there, so a DLL from before that split (or copied in by hand) is never loaded into an
    // editor of the other configuration - that crashes on the first call (the standard library and the ECS types differ).
    inline std::filesystem::path GamePluginConfigMarker( const script_project_paths& P ) noexcept
    {
        return P.m_BuildDir / std::format(L"Game.{}.config", P.m_Config);
    }

    // The DLL this editor would load exists and was built by this configuration: it can be loaded right away, even while a
    // newer one is being built.
    inline bool IsGamePluginUsable( const script_project_paths& P ) noexcept
    {
        std::error_code Ec;
        if (!std::filesystem::exists(P.m_Dll, Ec)) return false;
        std::wifstream In(GamePluginConfigMarker(P));
        std::wstring   S;
        std::getline(In, S);
        return S == P.m_Config;
    }

    // Something the DLL is built from is newer than the DLL (or there is no usable DLL at all).
    inline bool IsGamePluginStale( const script_project_paths& P, std::filesystem::file_time_type ModuleSourceTime ) noexcept
    {
        if (!IsGamePluginUsable(P)) return true;
        // Compared with the last successful build (the marker is written after each one), not with the DLL: when nothing needed
        // recompiling MSBuild leaves the DLL untouched, and it would then look stale at every launch.
        std::error_code Ec;
        const auto BuiltAt = std::filesystem::last_write_time(GamePluginConfigMarker(P), Ec);
        return Ec || ModuleSourceTime > BuiltAt;
    }

    inline build_result BuildGamePluginIfStale( game_plugin_state& Plugin, std::filesystem::file_time_type ModuleSourceTime ) noexcept
    {
        std::error_code Ec;
        const auto& P = Plugin.m_Paths;

        const bool bDllMissing  = !std::filesystem::exists(P.m_Dll, Ec);
        const bool bWrongConfig = !bDllMissing && !IsGamePluginUsable(P);
        const bool bStale       = IsGamePluginStale(P, ModuleSourceTime);
        const auto ConfigMarker = GamePluginConfigMarker(P);

        if (!bStale)
        {
            Plugin.m_LastStatus = "Game.dll: up to date, no rebuild needed";
            LogGamePlugin(Plugin.m_LastStatus);
            return build_result::UpToDate; // load it directly, no rebuild attempted
        }

        Plugin.m_LastStatus = std::format("Game.dll: {} - rebuilding via cmake...", bDllMissing ? "DLL missing" : bWrongConfig ? "built for another configuration" : "source newer than DLL");
        LogGamePlugin(Plugin.m_LastStatus);

        // MSBuild leaves a worker process alive after a build so a later one can reuse it, and that worker holds a lock on the
        // PDB. If the editor's process tree is killed uncleanly, the next build can then fail with LNK1201. Only this function
        // builds the script project, and it is one small target, so nothing is gained by reusing a worker.
        // MSBUILDDISABLENODEREUSE forces that onto every node a build spawns; the /nodeReuse:false switch alone did not.
        SetEnvironmentVariableW(L"MSBUILDDISABLENODEREUSE", L"1");

        // The whole build is ONE operation with an outcome: what a script reads to know "did the game build", and why not. Its output goes
        // through the build adapter; the translation units MSBuild names as it compiles them are what a successful incremental build really checked.
        // A full build (no DLL, or another configuration's) checks everything.
        auto* pLogs = xlog::hub::current();
        xlog::op_handle Op;
        std::optional<xlog::build_output_adapter> Adapter;
        if (pLogs)
        {
            Op = pLogs->Begin("game.build", { xlog::origin::type::Tool, "msbuild", 0 }
                , { xlog::ref::type::File, P.m_Dll.string(), 0, 0, 0, 0 }, "Build Game.dll"
                , std::format("Game.dll|{}|x64", std::filesystem::path(P.m_Config).string()));
            Adapter.emplace(*pLogs, Op);
        }
        auto* pAdapter = Adapter ? &*Adapter : nullptr;

        // An explicit reconfigure before every build: `cmake --build`'s own automatic reconfigure does not reliably notice that
        // the generated CMakeLists.txt changed (a module was added or removed). It is cheap when nothing changed. The
        // generator is only given the first time, when the build directory is created.
        // MSBuild would consider the other configuration's DLL up to date, so it goes first.
        if (bWrongConfig) std::filesystem::remove(P.m_Dll, Ec);

        std::wstring Configure = std::format(L"cmake -S \"{}\" -B \"{}\"", P.m_Root.wstring(), P.m_BuildDir.wstring());
        if (!std::filesystem::exists(P.m_BuildDir / L"CMakeCache.txt", Ec)) Configure += ScriptProjectGeneratorArgs(P);
        if (const auto ConfigureExit = RunCmakeCommand(Configure, P.m_Root, pAdapter); ConfigureExit != 0)
        {
            Plugin.m_LastStatus = std::format("Game.dll: cmake reconfigure FAILED (exit={}) - see the Logs (LogProblems -Operation {})", ConfigureExit, Op.Id());
            LogGamePlugin(Plugin.m_LastStatus);
            Op.Fail();
            return build_result::Failed;
        }

        // MSBuild decides whether the precompiled header is stale from cmake_pch.cxx, a one-line file CMake never rewrites, so
        // adding a header to (or removing one from) the PCH list does not make it rebuild. Touching that file does.
        if (const auto PchFile = P.m_BuildDir / L"CMakeFiles" / L"Game.dir" / L"cmake_pch.cxx"; std::filesystem::exists(PchFile, Ec))
            std::filesystem::last_write_time(PchFile, std::filesystem::file_time_type::clock::now(), Ec);

        const auto BuildExit = RunCmakeCommand(std::format(L"cmake --build \"{}\" --target Game --config {} -- /nodeReuse:false", P.m_BuildDir.wstring(), P.m_Config), P.m_Root, pAdapter);
        if (BuildExit != 0)
        {
            Plugin.m_LastStatus = std::format("Game.dll: BUILD FAILED (exit={}) - see the Logs (LogProblems -Operation {})", BuildExit, Op.Id());
            LogGamePlugin(Plugin.m_LastStatus);
            Op.Fail();
            return build_result::Failed; // the currently loaded generation is left completely untouched
        }

        std::filesystem::create_directories(ConfigMarker.parent_path(), Ec);
        std::wofstream(ConfigMarker, std::ios::trunc) << P.m_Config;

        Plugin.m_LastStatus = "Game.dll: rebuild succeeded";
        LogGamePlugin(Plugin.m_LastStatus);
        Op.SetCoverage((bDllMissing || bWrongConfig) ? xlog::coverage_kind::Complete : xlog::coverage_kind::Subjects);
        Op.Succeed();
        return build_result::Rebuilt;
    }

} // namespace xlevel

#endif // XLVL_NEW_LevelEditor_GAME_PLUGIN_BUILD_H
