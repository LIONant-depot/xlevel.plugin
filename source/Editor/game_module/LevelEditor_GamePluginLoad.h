#ifndef XLVL_NEW_LevelEditor_GAME_PLUGIN_LOAD_H
#define XLVL_NEW_LevelEditor_GAME_PLUGIN_LOAD_H
#pragma once
#include <fstream>

// Extracted from LevelEditor_GamePlugin.h (mechanical move, phase 3 of the kit split - see the umbrella
// file's own top comment). The shadow-copy + LoadLibrary/GetProcAddress mechanics: copying the
// compiler's real output to a generation-suffixed file that's actually loaded
// (CopyGamePluginForLoad), the two-call RegisterComponents/RegisterSystems sequence
// xecs_plugin_api.h's own comment requires (LoadGamePluginComponents, RegisterGamePluginSystems),
// and detach/unload (UnloadGamePlugin). Meant to be included via the umbrella (LevelEditor_GamePlugin.h)
// only, after LevelEditor_GamePluginLog.h and LevelEditor_GamePluginBuild.h (game_plugin_state).
#include "plugins/xscript_module.plugin/source/Runtime/xscript_registration.h"

namespace xlevel
{
    // Called from RegisterGamePluginSystems below (the one fixed choke point every reload already
    // goes through) rather than duplicated at each of ITS OWN call sites - guarantees this can never
    // be forgotten at some future new reload trigger. GetProcAddress returning null (an
    // older-generation DLL built before this export existed) just means an empty map - every
    // component then falls back to "uncategorized", exactly like it already does today.
    // Merge one DLL's XScript_GetComponentDisplayInfo into a display set (overwrites by name).
    inline void MergeComponentDisplayInfoFromModule( HMODULE hModule, xscene::component_display& Display ) noexcept
    {
        if (!hModule) return;
        auto* pGetInfo = reinterpret_cast<xscript::pfn_get_component_display_info>(GetProcAddress(hModule, xscript::kGetComponentDisplayInfoName));
        if (pGetInfo == nullptr) return;
        pGetInfo([](void* pUserData, std::uint64_t /*Guid*/, const char* pName, const char* pCategory, int Priority) noexcept
        {
            auto& Map = *reinterpret_cast<std::unordered_map<std::string, xscene::component_display_info>*>(pUserData);
            Map[pName] = { pCategory, Priority };
        }, &Display.m_Categories);
    }

    inline void LoadGameComponentDisplayInfo( game_plugin_state& Plugin ) noexcept
    {
        Plugin.m_Display.m_Categories.clear();
        // Engine DLLs first (Transform / Physics / Primitive categories), then Game.dll overlays.
        MergeComponentDisplayInfoFromModule(GetModuleHandleW(Plugin.m_CoreModule.c_str()), Plugin.m_Display);
        MergeComponentDisplayInfoFromModule(GetModuleHandleW(Plugin.m_RenderModule.c_str()), Plugin.m_Display);
        if (Plugin.isLoaded())
            MergeComponentDisplayInfoFromModule(Plugin.m_hModule, Plugin.m_Display);
    }

    //---------------------------------------------------------------------------
    // Copies Plugin.m_CompiledDllPath (+ its matching .pdb, if present - direct user requirement:
    // "the job of the editor is to copy the new version of the dll with any symbols it may need
    // for debugging") to a fresh, generation-suffixed filename in the same directory - the file
    // that actually gets LoadLibrary'd. Never overwrites a previous generation's copy (each
    // Generation value names a distinct file) - by the time this runs, UnloadGamePlugin has already
    // deleted the previous one anyway (safe then: nothing still has it mapped). Returns the new
    // copy's path, or empty on failure (nothing compiled yet, or the copy itself failed).
    //---------------------------------------------------------------------------
    //
    // The copy imports the core by its name: when the plugin state runs on a copy of the core (a set made for a Level), the import is renamed to it, so that this Game.dll binds to THAT registry and no
    // other. The copy is named after the core it is bound to as well (two Levels of the same Game load two copies of it).
    inline std::wstring CopyGamePluginForLoad( const script_project_paths& P, std::uint32_t Generation, const std::wstring& CoreModule = engine::kCoreNameW ) noexcept
    {
        std::error_code Ec;
        if (!std::filesystem::exists(P.m_Dll, Ec))
        {
            LogGamePlugin(std::format("Game.dll: nothing compiled yet at {}", P.m_Dll.string()));
            return {};
        }

        // The loaded copies live under Cache\Script\Loaded, apart from the compiled resource. The compiled PDB is linked with
        // /PDBALTPATH set to its bare name, so the debugger looks for it next to the module it loaded: NewPdb is therefore
        // always the same bare name, not generation-suffixed like the DLL.
        std::filesystem::create_directories(P.m_LoadedDir, Ec);
        const bool bOriginalCore = _wcsicmp(CoreModule.c_str(), engine::kCoreNameW) == 0;
        const auto NewDll = P.m_LoadedDir / (bOriginalCore ? std::format(L"Game_loaded_{}.dll", Generation) : std::format(L"Game_loaded_{}_{}.dll", std::filesystem::path(CoreModule).stem().wstring(), Generation));
        const auto SrcPdb = P.m_PdbDir / L"Game.pdb";
        const auto NewPdb = P.m_LoadedDir / L"Game.pdb";

        if (bOriginalCore) std::filesystem::copy_file(P.m_Dll, NewDll, std::filesystem::copy_options::overwrite_existing, Ec);
        else if (!engine::PatchedCopy(P.m_Dll, NewDll, engine::kCoreName, std::filesystem::path(CoreModule).string())) Ec = std::make_error_code(std::errc::io_error);
        if (Ec)
        {
            LogGamePlugin(std::format("Game.dll: failed to copy {} -> {}", P.m_Dll.string(), NewDll.string()));
            return {};
        }

        // Best-effort - a missing/failed .pdb copy only degrades debugging, it's not a load failure.
        if (std::filesystem::exists(SrcPdb, Ec))
        {
            std::error_code PdbEc;
            std::filesystem::copy_file(SrcPdb, NewPdb, std::filesystem::copy_options::overwrite_existing, PdbEc);
        }

        // The DLLs the modules' third-party libraries bring (the generator lists them in Cache\Script\runtime_files.txt): next to the copy that is loaded,
        // where LoadLibraryExW looks for them. One that is still mapped by an earlier generation is already there and is left alone.
        {
            std::wifstream Manifest(P.m_Root / L"runtime_files.txt");
            for (std::wstring Line; std::getline(Manifest, Line); )
            {
                if (Line.empty()) continue;
                std::error_code RuntimeEc;
                const std::filesystem::path Source(Line);
                std::filesystem::copy_file(Source, P.m_LoadedDir / Source.filename(), std::filesystem::copy_options::overwrite_existing, RuntimeEc);
                if (RuntimeEc && !std::filesystem::exists(P.m_LoadedDir / Source.filename(), RuntimeEc)) LogGamePlugin(std::format("Game.dll: a runtime file of a library is missing: {}", Source.string()));
            }
        }
        return NewDll.wstring();
    }

    //---------------------------------------------------------------------------
    // Component-registry compatibility plan (Build/RELOAD_CRASH_REPORT.md's own follow-up design) -
    // "ping-pong" pre-flight: the shadow-copy step below was ALWAYS mandatory before any load, so
    // copying+loading a candidate DLL costs nothing extra done early, before touching the OLD
    // generation - it's a pure reorder, not added work. A game_plugin_candidate is an
    // ownership-holding intermediate: either it gets Commit'ed (ownership transfers to
    // game_plugin_state, exactly like the old single-shot LoadGamePluginComponents did) or Discard'ed
    // (FreeLibrary + delete the shadow copy, old generation never touched) - never both, never
    // neither.
    //---------------------------------------------------------------------------
    struct game_plugin_candidate
    {
        HMODULE      m_hModule = nullptr;
        std::wstring m_LoadedPath;
    };

    // Step 1: copy the already-known-good compiled DLL into a fresh generation-suffixed shadow file
    // and LoadLibrary it - no registry mutation at all, safe to call while an OLD generation is still
    // fully loaded and running. Returns an invalid candidate (m_hModule==nullptr) on any failure -
    // nothing was allocated, nothing to Discard.
    inline game_plugin_candidate PrepareGamePluginCandidate( const game_plugin_state& Plugin, std::uint32_t Generation ) noexcept
    {
        game_plugin_candidate Candidate;

        const std::wstring LoadedPath = CopyGamePluginForLoad(Plugin.m_Paths, Generation, Plugin.m_CoreModule);
        if (LoadedPath.empty())
        {
            LogGamePlugin("Game.dll: nothing to load");
            return Candidate;
        }

        // Plain printf, deliberately NOT xeditor::NotifyError() - xeditor::NotifyError() also arms the modal error-
        // popup (RenderErrorPopup, checked once per frame from the main loop's own top-level
        // scope). This function's own first caller (LevelEditor_Main.cpp's own startup code,
        // via LoadGamePluginComponents below) calls it BEFORE the main loop has rendered even one
        // frame - confirmed empirically (a deterministic, 100%-reproducible "Missing EndChild()"
        // ImGui assertion on frame 1) that arming the popup flag that early breaks ImGui's
        // window-stack bookkeeping. A missing/failed-to-load Game.dll is an expected, benign
        // condition anyway (nothing has been built yet on a fresh checkout) - a plain log line is
        // the right amount of ceremony for it, not a modal.
        DWORD LoadError = 0;
        HMODULE hModule = engine::LoadCopy(LoadedPath, LoadError);       // the DLL's own folder is searched for the DLLs it needs (the libraries' runtime files)
        if (hModule == nullptr)
        {
            LogGamePlugin(std::format("Game.dll: LoadLibrary failed for {}", std::filesystem::path(LoadedPath).filename().string()));
            return Candidate;
        }

        Candidate.m_hModule    = hModule;
        Candidate.m_LoadedPath = LoadedPath;
        return Candidate;
    }

    // Discards a Prepare'd-but-never-Commit'ed candidate - the compatibility check (Step 2's own
    // consumer) found it incompatible, or the caller otherwise changed its mind. FreeLibrary + delete
    // the shadow copy, leaving nothing behind; the OLD generation (if any) is completely unaffected,
    // since nothing about it was ever touched. Safe to call on an already-invalid candidate (no-op).
    inline void DiscardGamePluginCandidate( game_plugin_candidate& Candidate ) noexcept
    {
        if (Candidate.m_hModule) FreeLibrary(Candidate.m_hModule);
        if (!Candidate.m_LoadedPath.empty())
        {
            std::error_code Ec;
            std::filesystem::remove(std::filesystem::path(Candidate.m_LoadedPath), Ec);
        }
        Candidate = {};
    }

    // Step 2: the candidate's own full component manifest, by stable guid - available immediately,
    // zero registry mutation, since XScript_GetComponentDisplayInfo's data comes from a self-registration
    // list populated at LoadLibrary/static-init time, well before XecsPlugin_RegisterComponents is
    // ever called. Empty (not a failure) for an older-generation DLL built before this export existed
    // - every component then just can't be cross-checked, same "best-effort" posture as everywhere
    // else optional metadata is missing in this project.
    inline std::vector<xecs::scene::component_dependency> ProbeCandidateComponents( const game_plugin_candidate& Candidate ) noexcept
    {
        std::vector<xecs::scene::component_dependency> Result;
        if (!Candidate.m_hModule) return Result;

        auto* pGetInfo = reinterpret_cast<xscript::pfn_get_component_display_info>(GetProcAddress(Candidate.m_hModule, xscript::kGetComponentDisplayInfoName));
        if (!pGetInfo) return Result;

        pGetInfo([](void* pUserData, std::uint64_t Guid, const char* pName, const char*, int) noexcept
        {
            auto& Out = *reinterpret_cast<std::vector<xecs::scene::component_dependency>*>(pUserData);
            Out.push_back({ xecs::component::type::guid{Guid}, pName });
        }, &Result);

        return Result;
    }

    // What the DLL says it defines and where (XScript_GetRegistrations; the module of each file is found from its path). Like ProbeCandidateComponents it needs nothing but the loaded library: the data
    // comes from lists filled at static-init time. false when the DLL has no such export (built before it existed).
    inline bool ProbeRegistrations( HMODULE hModule, std::vector<game_registration>& Out ) noexcept
    {
        Out.clear();
        if (!hModule) return false;
        auto* pGet = reinterpret_cast<xscript::pfn_get_registrations>(GetProcAddress(hModule, xscript::kGetRegistrationsName));
        if (!pGet) return false;
        pGet([](void* pUserData, int Kind, std::uint64_t Guid, const char* pName, const char* pFile) noexcept
        {
            game_registration R;
            R.m_Kind   = Kind;
            R.m_Guid   = Guid;
            R.m_Name   = pName ? pName : "";
            R.m_File   = pFile ? pFile : "";
            R.m_Module = xscript::module::ModuleGuidFromSourcePath(R.m_File);
            static_cast<std::vector<game_registration>*>(pUserData)->push_back(std::move(R));
        }, &Out);
        return true;
    }

    // Said once per generation, in the log: what the registrations show that the person should know.
    inline void ReportRegistrations( const game_plugin_state& Plugin ) noexcept
    {
        if (!Plugin.m_bHasRegistrations) return;
        const auto Listed = ReadGame(Plugin.m_Paths.m_Game).m_Modules;      // the modules of the Game that is loaded
        std::vector<std::uint64_t> NotListed;
        int nOutside = 0;
        std::string Example;
        for (const auto& R : Plugin.m_Registrations)
        {
            if (R.m_Module == 0)
            {
                if (!nOutside++) Example = std::format("{} in '{}'", R.m_Name, R.m_File);
                continue;
            }
            const bool bListed = std::any_of(Listed.begin(), Listed.end(), [&](const xscript::module::module_ref& M) { return M.m_Instance.m_Value == R.m_Module; });
            if (!bListed && std::find(NotListed.begin(), NotListed.end(), R.m_Module) == NotListed.end()) NotListed.push_back(R.m_Module);
        }
        if (nOutside) LogGamePlugin(std::format("Game.dll: {} type(s) are defined outside every script module, so no module owns them (the first: {}): an engine header included without XSCRIPT_IMPORT_ONLY registers its components a second time", nOutside, Example));
        for (const auto Module : NotListed)
            LogGamePlugin(std::format("Game.dll: it defines types of the script module {:X}, which the Game does not list (a module includes one of its headers): add that module to the Game", Module));
    }

    // A call into the game module, under a structured-exception handler: a module that registers badly (a system that queries a component
    // it never told the DLL about reads an unset bit id, for one) must not take the whole editor down with it. The call is plain function
    // + context so this function holds no C++ objects (__try cannot live next to destructors).
    inline bool RunGuarded( void (*pFn)(void*), void* pContext, unsigned long& Code ) noexcept
    {
        __try                                                                   { pFn(pContext); return true; }
        __except( (Code = GetExceptionCode()) == EXCEPTION_BREAKPOINT ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER ) { return false; }
    }

    // Runs pFn(pContext) guarded. On a crash: says so in the log and in the status line, and flags the module as crashed (it is left
    // loaded, see game_plugin_state::m_bCrashed). Returns false then.
    inline bool GuardedModuleCall( game_plugin_state& Plugin, const char* pWhat, void (*pFn)(void*), void* pContext ) noexcept
    {
        unsigned long Code = 0;
        if (RunGuarded(pFn, pContext, Code)) return true;

        Plugin.m_bCrashed   = true;
        Plugin.m_LastStatus = std::format("Game.dll crashed while registering {} ({}) - its systems are not running; fix the module and rebuild", pWhat
            , Code == EXCEPTION_ACCESS_VIOLATION ? "access violation" : std::format("exception {:08X}", Code));
        LogGamePlugin(Plugin.m_LastStatus);
        return false;
    }

    // Step 3: the actual registry-mutating half - calls the candidate's own
    // XecsPlugin_RegisterComponents (the FIRST of the two-call sequence xecs_plugin_api.h's own
    // comment requires: every RegisterComponents call, host's and the plugin's, must happen before
    // ANY RegisterSystems call) and transfers ownership of the loaded module to Plugin. Only ever
    // meaningful once the OLD generation (if any) has already been fully torn down/unregistered - see
    // ReloadGameModule's own call site for the ordering this depends on. On failure, Discards the
    // candidate itself (nothing left dangling) and returns false with Plugin untouched.
    inline bool CommitGamePluginCandidate( xecs::game_mgr::instance& GameMgr, game_plugin_state& Plugin, game_plugin_candidate& Candidate, std::uint32_t Generation ) noexcept
    {
        assert( Plugin.isLoaded() == false );
        if (!Candidate.m_hModule)
        {
            Plugin.m_LastStatus += " | nothing to load";
            return false;
        }

        auto* pRegisterComponents = reinterpret_cast<xecs_plugin_pfn_register_components*>(GetProcAddress(Candidate.m_hModule, XECS_PLUGIN_REGISTER_COMPONENTS_NAME));
        if (pRegisterComponents == nullptr)
        {
            Plugin.m_LastStatus = std::format("Game.dll: missing export {}", XECS_PLUGIN_REGISTER_COMPONENTS_NAME);
            LogGamePlugin(Plugin.m_LastStatus);
            DiscardGamePluginCandidate(Candidate);
            return false;
        }

        Plugin.m_hModule       = Candidate.m_hModule;
        Plugin.m_LoadedDllPath = Candidate.m_LoadedPath;
        Plugin.m_bHasRegistrations = ProbeRegistrations(Plugin.m_hModule, Plugin.m_Registrations);
        Plugin.m_Token         = { .m_Slot = 1, .m_Generation = Generation };
        Plugin.m_bCrashed      = false;

        struct call { xecs_plugin_pfn_register_components* m_pFn; xecs::game_mgr::instance* m_pGameMgr; xecs::plugin::token m_Token; } Call{ pRegisterComponents, &GameMgr, Plugin.m_Token };
        const bool bRegistered = GuardedModuleCall(Plugin, "its components", [](void* p) noexcept { auto& C = *static_cast<call*>(p); C.m_pFn(*C.m_pGameMgr, C.m_Token); }, &Call);
        if (!bRegistered)
        {
            Candidate = {};         // Plugin owns the module now (the next reload unloads it)
            return false;
        }

        Plugin.m_LastStatus = std::format("Game.dll: loaded generation {} ({})", Generation, std::filesystem::path(Candidate.m_LoadedPath).filename().string());
        ReportRegistrations(Plugin);
        Candidate = {}; // ownership transferred to Plugin - Discard must never also free what Plugin now owns
        return true;
    }

    // The ONE remaining synchronous, single-shot caller (startup, generation 1) - nothing is running
    // yet to be incompatible with, so no candidate/compatibility dance is needed there. Thin
    // Prepare+Commit wrapper, byte-for-byte the same external behavior the old single-function version
    // had.
    inline bool LoadGamePluginComponents( xecs::game_mgr::instance& GameMgr, game_plugin_state& Plugin, std::uint32_t Generation ) noexcept
    {
        auto Candidate = PrepareGamePluginCandidate(Plugin, Generation);
        return CommitGamePluginCandidate(GameMgr, Plugin, Candidate, Generation);
    }

    // The SECOND call of the two-call sequence - only meaningful once every RegisterComponents
    // call (host's own, done by the caller, and the plugin's, done by
    // LoadGamePluginComponents above) has already happened. Without a plugin it only loads the engine DLLs' component display info.
    // Returns false when the module crashed while registering: GameMgr then holds whatever it half registered, and the caller must
    // throw that world away and make a fresh one (the module is flagged crashed, so a fresh one gets none of its systems).
    inline bool RegisterGamePluginSystems( xecs::game_mgr::instance& GameMgr, game_plugin_state& Plugin ) noexcept
    {
        bool bOk = true;
        if (Plugin.isLoaded() && !Plugin.m_bCrashed)
            if (auto* pRegisterSystems = reinterpret_cast<xecs_plugin_pfn_register_systems*>(GetProcAddress(Plugin.m_hModule, XECS_PLUGIN_REGISTER_SYSTEMS_NAME)))
            {
                struct call { xecs_plugin_pfn_register_systems* m_pFn; xecs::game_mgr::instance* m_pGameMgr; } Call{ pRegisterSystems, &GameMgr };
                if (Plugin.m_bSimulateCrash) bOk = GuardedModuleCall(Plugin, "its systems", [](void*) noexcept { *static_cast<volatile int*>(nullptr) = 0; }, nullptr);
                else                         bOk = GuardedModuleCall(Plugin, "its systems", [](void* p) noexcept { auto& C = *static_cast<call*>(p); C.m_pFn(*C.m_pGameMgr); }, &Call);
            }

        // The engine DLLs' components (Transform, Physics, ...) have categories and priorities too, so this runs even when
        // there is no Game.dll (a project without script modules).
        LoadGameComponentDisplayInfo(Plugin);
        return bOk;
    }

    //---------------------------------------------------------------------------
    // Detach/quiesce + unload - called with the OLD world already destroyed (see PollGameReload
    // below), so xecs::component::mgr::UnregisterPlugin's own full-reset is exactly the correct,
    // sufficient operation (see its own comment for why). A no-op if nothing is loaded.
    //---------------------------------------------------------------------------
    inline void UnloadGamePlugin( game_plugin_state& Plugin ) noexcept
    {
        if (!Plugin.isLoaded()) return;

        Plugin.m_Display.m_Categories.clear();

        if (auto* pUnregister = reinterpret_cast<xecs_plugin_pfn_unregister*>(GetProcAddress(Plugin.m_hModule, XECS_PLUGIN_UNREGISTER_NAME)))
            pUnregister(Plugin.m_Token);

        Plugin.Registry().UnregisterPlugin(Plugin.m_Token);

        FreeLibrary(Plugin.m_hModule);
        Plugin.m_hModule  = nullptr;
        Plugin.m_Registrations.clear();
        Plugin.m_bHasRegistrations = false;
        Plugin.m_Token    = {};
        Plugin.m_bCrashed = false;

        // The shadow copy (see game_plugin_state's own comment for why it exists) is safe to
        // delete now that nothing has it mapped - best-effort; a leftover file here would be
        // cosmetic, never a correctness problem. The .pdb is NOT
        // Plugin.m_LoadedDllPath-with-a-different-extension anymore - CopyGamePluginForLoad's own
        // comment explains why it's always the fixed bare name "Game.pdb" (matching the
        // script project's PDB name), never generation-suffixed like the .dll itself.
        if (!Plugin.m_LoadedDllPath.empty())
        {
            std::error_code Ec;
            const std::filesystem::path LoadedDll = Plugin.m_LoadedDllPath;
            std::filesystem::remove(LoadedDll, Ec);
            std::filesystem::remove(LoadedDll.parent_path() / L"Game.pdb", Ec);
            Plugin.m_LoadedDllPath.clear();
        }
    }

    // The GameModuleStatus / SimulateModuleCrash commands (declared in xlevel_commands_workspace.h).
    inline std::string GameModuleStatusText() noexcept
    {
        if (!g_pGamePlugin) return "GameModuleStatus: no game module support in this build";
        return std::format("Loaded={}\nCrashed={}\n{}", g_pGamePlugin->isLoaded(), g_pGamePlugin->m_bCrashed, g_pGamePlugin->m_LastStatus);
    }

    inline std::string SimulateSnapshotFailure( const std::string& State ) noexcept
    {
        if (!g_pGamePlugin)                          return "SimulateSnapshotFailure: no game module support in this build";
        if (State != "on" && State != "off")         return "SimulateSnapshotFailure: -State on|off is required";
        g_pGamePlugin->m_bSimulateSnapshotFailure = (State == "on");
        return "SimulateSnapshotFailure: " + State;
    }

    inline std::string SimulateModuleCrash( const std::string& State ) noexcept
    {
        if (!g_pGamePlugin)                          return "SimulateModuleCrash: no game module support in this build";
        if (State != "on" && State != "off")         return "SimulateModuleCrash: -State on|off is required";
        g_pGamePlugin->m_bSimulateCrash = (State == "on");
        if (State == "off") g_pGamePlugin->m_bCrashed = false;
        return "SimulateModuleCrash: " + State;
    }

} // namespace xlevel

#endif // XLVL_NEW_LevelEditor_GAME_PLUGIN_LOAD_H
