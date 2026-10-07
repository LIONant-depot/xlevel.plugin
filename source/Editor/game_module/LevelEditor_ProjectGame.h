#ifndef XLVL_NEW_LevelEditor_PROJECT_GAME_H
#define XLVL_NEW_LevelEditor_PROJECT_GAME_H
#pragma once

// The project's Game resource (xgame.plugin): which script modules the project's code is made of, read from the resource's own Descriptor.txt, and what the thread that builds
// Game.dll needs to know about the resource pipeline making the CMake project of the game (it waits on files, never on the library manager).
//
// Everything here finds the Game from the project path and its guid (a Level names its Game), so it works before the library manager has scanned the project.
#include "plugins/xgame.plugin/source/Module/xgame_descriptor.h"
#include "plugins/xscript_module.plugin/source/Module/xscript_module_files.h"

#include <chrono>
#include <fstream>
#include <thread>

namespace xlevel
{
    inline std::filesystem::path ProjectRoot() noexcept { return xresource_editor::g_LibMgr.m_ProjectPath; }

    struct project_game
    {
        std::filesystem::path                       m_Folder;       // the Game's .desc folder, empty when the project has no Game resource (or the one it names is not there)
        std::vector<xscript::module::module_ref>    m_Modules;
        std::string                                 m_Error;        // the Game's Descriptor.txt exists and cannot be read
        bool HasGame() const noexcept { return !m_Folder.empty(); }
    };

    // A Level names its Game by the same guid type: one definition each, so a change of either is caught here.
    static_assert(xecs::level::game_type_guid_v == xgame::type_guid_v, "the Level's Game reference and the Game resource must have the same type guid");

    // A Game resource by its guid (0: none: no Game, no modules). The project has no Game of its own: every Level names the one it runs under.
    inline project_game ReadGame( std::uint64_t Game ) noexcept
    {
        project_game G;
        if (Game == 0) return G;
        G.m_Folder = xgame::FindGameFolder(ProjectRoot(), xgame::game_ref{ xresource::instance_guid{ Game } });
        if (G.m_Folder.empty()) return G;
        xgame::descriptor D;
        std::string Error;
        std::error_code Ec;
        if (xgame::Read(G.m_Folder, D, &Error)) G.m_Modules = D.m_Modules;
        else if (std::filesystem::exists(xgame::DescriptorFile(G.m_Folder), Ec)) G.m_Error = Error;      // a Game without a Descriptor.txt yet is a game without modules
        return G;
    }

    // Reads a Game's descriptor, lets Fn change it (it returns "" or why not), and writes it back. Returns "" or why it did not happen.
    template<typename T_FN>
    inline std::string EditGame( std::uint64_t Game, T_FN&& Fn ) noexcept
    {
        auto G = ReadGame(Game);
        if (!G.HasGame())       return Game ? "the Game is not in the project" : "no Game given (create one in the Asset Browser, type Game)";
        if (!G.m_Error.empty()) return "the descriptor of the Game cannot be read: " + G.m_Error;
        xgame::descriptor D;
        D.m_Modules = G.m_Modules;
        if (auto Why = Fn(D); !Why.empty()) return Why;
        std::string Error;
        if (!xgame::Write(G.m_Folder, D, &Error)) return "the Game could not be saved: " + Error;
        return {};
    }

    // The Game a Level runs under, as its Descriptor.txt says (0: it names none). The guid of the Game resource; read from disk, which is where SetLevelGame writes at once.
    inline std::uint64_t ReadLevelGame( const std::wstring& Project, std::uint64_t Level ) noexcept
    {
        xecs::level::descriptor D;
        xproperty::settings::context Context{};
        const std::wstring Path = std::format(L"{}/Descriptors/Level/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", Project, Level & 0xFF, (Level >> 8) & 0xFF, Level);
        if (auto Err = D.Serialize(true, Path, Context); Err) return 0;
        return D.m_Game.m_Instance.m_Value;
    }

    // The Game a Level runs under: the one it names. A Level has to name one (0: it does not, and does not know what to run).
    inline std::uint64_t GameOfLevel( std::uint64_t Level ) noexcept { return ReadLevelGame(ProjectRoot().wstring(), Level); }

    // The Game a prefab plays with in its Prefab Editor (prefabs_plan.md, D2), as the prefab's Descriptor.txt says (0: it names none: no scripts, components or systems of any module, as a Level that
    // names none). Read from disk, which is where SetPrefabGame writes at once; every save of the prefab keeps it.
    inline std::wstring PrefabFolder( const std::wstring& Project, std::uint64_t Prefab ) noexcept
    {
        return std::format(L"{}/Descriptors/Prefab/{:02X}/{:02X}/{:X}.desc", Project, Prefab & 0xFF, (Prefab >> 8) & 0xFF, Prefab);
    }
    inline std::uint64_t ReadPrefabGame( const std::wstring& Project, std::uint64_t Prefab ) noexcept
    {
        xecs::prefab::descriptor D;
        xproperty::settings::context Context{};
        if (auto Err = D.Serialize(true, PrefabFolder(Project, Prefab) + L"/Descriptor.txt", Context); Err) return 0;
        return D.m_Game.m_Instance.m_Value;
    }
    inline std::uint64_t GameOfPrefab( std::uint64_t Prefab ) noexcept { return ReadPrefabGame(ProjectRoot().wstring(), Prefab); }

    // The Game of the Level or the prefab a Level editor is opened for.
    inline std::uint64_t GameOfDocument( xresource::full_guid Guid ) noexcept
    {
        return Guid.m_Type == xecs::prefab::type_guid_v ? GameOfPrefab(Guid.m_Instance.m_Value) : GameOfLevel(Guid.m_Instance.m_Value);
    }

    // Names the Game a prefab plays with: its Descriptor.txt rewritten with it (the members stay as they are on disk). "" or why not. A prefab in the old format has no descriptor of the new one to
    // write into: UpgradeProject first.
    inline std::string WritePrefabGame( const std::wstring& Project, std::uint64_t Prefab, std::uint64_t Game ) noexcept
    {
        const auto Folder = PrefabFolder(Project, Prefab);
        std::error_code Ec;
        if (!std::filesystem::is_directory(std::filesystem::path(Folder), Ec)) return "the prefab is not in the project";
        xecs::prefab::descriptor D;
        xproperty::settings::context Context{};
        const auto File = Folder + L"/Descriptor.txt";
        if (!std::filesystem::exists(std::filesystem::path(File), Ec)) return "the prefab has no Descriptor.txt yet (it was never saved)";
        if (auto Err = D.Serialize(true, File, Context); Err) return std::format("the descriptor of the prefab cannot be read: {}", Err.getMessage());
        if (D.m_Root == xecs::scene::invalid_permanent_id_v) return "the prefab is in the old format (one Entity.txt): UpgradeProject converts it first";
        D.m_Game = xecs::level::game_ref{ xresource::instance_guid{ Game } };
        std::string Why;
        const bool bWritten = xscript::module::Retry([&]
        {
            xproperty::settings::context WriteContext{};
            if (auto Err = D.Serialize(false, File, WriteContext); Err) { Why = std::string(Err.getMessage()); return false; }
            return true;
        });
        return bWritten ? std::string{} : "the prefab could not be saved: " + Why;
    }

    //------------------------------------------------------------------------------------------------
    // What the thread that builds Game.dll needs to know about the Game project, as files. The Game resource and each of its modules are compiled by the resource pipeline
    // (the modules first, then the Game, which writes <project>/Cache/Script/<Game guid>/CMakeLists.txt); the build waits for that to be over, and has to be able to tell a compile that has
    // not run yet from one that failed.
    //------------------------------------------------------------------------------------------------
    struct game_inputs
    {
        std::filesystem::path               m_Descriptor;       // the Game's Descriptor.txt
        std::filesystem::path               m_Stamp;            // the Game's compiled resource: written by a compile that succeeded, stamped with the time that compile began
        std::filesystem::path               m_Log;              // the Game's Log.txt: written by every compile, successful or not
        std::filesystem::path               m_CMakeLists;       // the game project the compile writes (<project>/Cache/Script/<guid>/CMakeLists.txt): a stamp without it is a project that is not there (a cleared cache, an older layout)
        std::vector<std::filesystem::path>  m_ModuleLogs;       // the Log.txt of each module's compile: the last thing a compile of the module writes
        bool                                m_bHasGame = false;
    };

    // What the build of a Game waits for (Game: the instance guid of the Game resource; the Game resource has to exist).
    inline game_inputs CaptureGameInputs(std::uint64_t Game) noexcept
    {
        game_inputs In;
        const auto G = ReadGame(Game);
        if (!Game || !G.HasGame()) return In;
        const auto Project = ProjectRoot();
        const std::uint64_t V = Game;
        const std::string Rest = std::format("Game/{:02X}/{:02X}/{:X}", V & 0xFF, (V >> 8) & 0xFF, V);
        In.m_bHasGame  = true;
        In.m_Descriptor = xgame::DescriptorFile(G.m_Folder);
        In.m_Stamp     = Project / "Cache" / "Resources" / "Platforms" / "WINDOWS" / Rest;
        In.m_Log       = Project / "Cache" / "Resources" / "Logs" / (Rest + ".log") / "Log.txt";
        In.m_CMakeLists = Project / xgame::ScriptFolderRelative(xgame::game_ref{ xresource::instance_guid{ Game } }) / "CMakeLists.txt";
        for (const auto& Module : G.m_Modules) In.m_ModuleLogs.push_back(Project / xgame::ModuleLogRelative(Module));
        return In;
    }

    enum class game_project_state : std::uint8_t
    { NoGame            // the project has no Game resource
    , Current           // the CMake project was made after everything it is made from last changed
    , Pending           // the pipeline has not made it yet
    , Failed            // the compile of the Game ran after the last change and did not succeed: its log says why
    };

    inline game_project_state CheckGameProject(const game_inputs& In) noexcept
    {
        if (!In.m_bHasGame) return game_project_state::NoGame;
        std::error_code Ec;
        auto Newest = std::filesystem::file_time_type::min();
        auto Consider = [&](const std::filesystem::path& File) noexcept
        {
            if (const auto T = std::filesystem::last_write_time(File, Ec); !Ec && T > Newest) Newest = T;
        };
        Consider(In.m_Descriptor);
        for (const auto& Log : In.m_ModuleLogs) Consider(Log);

        const auto Made = std::filesystem::last_write_time(In.m_Stamp, Ec);
        if (!Ec && Made >= Newest && std::filesystem::exists(In.m_CMakeLists, Ec)) return game_project_state::Current;

        // Not made since the last change: a compile that has not run yet, or one that ran and failed (it writes its log and not the stamp).
        const auto Ran = std::filesystem::last_write_time(In.m_Log, Ec);
        if (!Ec && Ran >= Newest)
        {
            std::ifstream Log(In.m_Log, std::ios::binary);
            const std::string Text((std::istreambuf_iterator<char>(Log)), std::istreambuf_iterator<char>());
            if (Text.find("[Error]") != std::string::npos) return game_project_state::Failed;
        }
        return game_project_state::Pending;
    }

    // Waits for the Game project to be made (the pipeline is working on it), up to Timeout. Returns the state it ended in; Pending means it timed out (or Cancel went up).
    inline game_project_state WaitForGameProject(const game_inputs& In, const std::atomic<bool>& Cancel, std::chrono::seconds Timeout = std::chrono::minutes(10)) noexcept
    {
        const auto End = std::chrono::steady_clock::now() + Timeout;
        for (;;)
        {
            const auto State = CheckGameProject(In);
            if (State != game_project_state::Pending) return State;
            if (Cancel.load() || std::chrono::steady_clock::now() >= End) return State;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

#endif // XLVL_NEW_LevelEditor_PROJECT_GAME_H
