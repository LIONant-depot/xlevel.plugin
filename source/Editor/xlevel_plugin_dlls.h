#ifndef XLEVEL_PLUGIN_DLLS_H
#define XLEVEL_PLUGIN_DLLS_H
#pragma once

// Generic self-registration loader for the engine's always-loaded DLLs (xLIONCore, xLIONRender, ...) -
// the SAME ABI Game.dll already implements (xecs_plugin_api.h's XecsPlugin_RegisterComponents/
// RegisterSystems), just resolved via GetModuleHandle instead of LoadLibraryW since these DLLs are
// already loaded through xLION.exe's own normal import-lib linking, never hot-reloaded like Game.dll.
// xLION.exe never includes a single component/system header from these DLLs - it only knows their
// module names, mirroring LevelEditor_GamePluginLoad.h's own resolve-and-call shape for Game.dll.
#include "dependencies/xECSV2/src/xecs_plugin_api.h"
#include <Windows.h>

namespace xlevel
{
    // Slot 0 (host_v) is the host program - never unloaded. Slot 1 is Game.dll (game_plugin_state's
    // own token, minted per reload generation). These engine DLLs are permanent (import-linked,
    // never FreeLibrary'd), so they MUST register as host_v: UnregisterPlugin(GameToken) full-resets
    // the shared registry and asserts every owner is either that Game token or host_v. Giving them
    // distinct slots (2, 3, ...) fails that assert on every Game unload / Level Editor exit, even
    // though the wipe itself is fine (RegisterHostComponents re-registers them on the next load).
    inline void RegisterEngineDLLComponents(xecs::game_mgr::instance& GameMgr, const wchar_t* pModuleName) noexcept
    {
        HMODULE hModule = GetModuleHandleW(pModuleName);
        if (!hModule) return; // not linked into this build (e.g. headless never links xLIONRender)

        if (auto* pRegisterComponents = reinterpret_cast<xecs_plugin_pfn_register_components*>(GetProcAddress(hModule, XECS_PLUGIN_REGISTER_COMPONENTS_NAME)))
            pRegisterComponents(GameMgr, xecs::plugin::host_v);
    }

    inline void RegisterEngineDLLSystems(xecs::game_mgr::instance& GameMgr, const wchar_t* pModuleName) noexcept
    {
        HMODULE hModule = GetModuleHandleW(pModuleName);
        if (!hModule) return;

        if (auto* pRegisterSystems = reinterpret_cast<xecs_plugin_pfn_register_systems*>(GetProcAddress(hModule, XECS_PLUGIN_REGISTER_SYSTEMS_NAME)))
            pRegisterSystems(GameMgr);
    }
}

#endif // XLEVEL_PLUGIN_DLLS_H