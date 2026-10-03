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
#include "dependencies/xLIONCore/src/game/xlioncore_editor.h"
#include "dependencies/xLIONRender/src/xlionrender_api.h"
#include <Windows.h>
#include <memory>

namespace xlevel
{
    // The module of an engine DLL, loaded when nothing loaded it yet: the editor no longer imports these DLLs (it calls their interfaces, found by name), so the linker leaves the import out and the
    // loader does not bring them in. A copy of the core made for one Level is loaded by its own name the same way.
    inline HMODULE LoadEngineModule(const wchar_t* pName) noexcept
    {
        HMODULE hModule = GetModuleHandleW(pName);
        return hModule ? hModule : LoadLibraryW(pName);
    }

    // The xECSEditor of a copy of the core (see xlioncore_editor.h): the editor does not run xECS code itself, it asks the copy it was given for this interface and calls it. The copy is found by the
    // name of its module for now (the one LIONCore.dll); a manager that makes copies per Level will pass the name of the copy it made. Null when that module has no editor interface (or another version).
    struct ecs_editor_release { void operator()(xlioncore::xECSEditor* p) const noexcept { if (p) p->Release(); } };
    using ecs_editor_ptr = std::unique_ptr<xlioncore::xECSEditor, ecs_editor_release>;

    inline ecs_editor_ptr CreateEcsEditor(const wchar_t* pCoreModule = L"LIONCore.dll") noexcept
    {
        HMODULE hModule = LoadEngineModule(pCoreModule);
        auto* pCreate = hModule ? reinterpret_cast<xlioncore::pfn_create_editor>(GetProcAddress(hModule, xlioncore::kCreateEditorName)) : nullptr;
        if (!pCreate) { OutputDebugStringA("xECSEditor: the core module has no editor interface\n"); return {}; }
        ecs_editor_ptr pEditor(pCreate());
        if (pEditor && pEditor->Version() != xlioncore::xECSEditor::kVersion) { OutputDebugStringA("xECSEditor: the core module has another version of the interface\n"); pEditor.reset(); }
        return pEditor;
    }

    // The same for the render DLL: its xRenderEditor (xlionrender_api.h) draws and picks the worlds of the copy of the core it is bound to. Null when the module is not there (a build without it).
    struct render_editor_release { void operator()(xlionrender::xRenderEditor* p) const noexcept { if (p) p->Release(); } };
    using render_editor_ptr = std::unique_ptr<xlionrender::xRenderEditor, render_editor_release>;

    inline render_editor_ptr CreateRenderEditor(const wchar_t* pRenderModule = L"LIONRender.dll") noexcept
    {
        HMODULE hModule = LoadEngineModule(pRenderModule);
        auto* pCreate = hModule ? reinterpret_cast<xlionrender::pfn_create_editor>(GetProcAddress(hModule, xlionrender::kCreateEditorName)) : nullptr;
        if (!pCreate) { OutputDebugStringA("xRenderEditor: the render module has no editor interface\n"); return {}; }
        render_editor_ptr pEditor(pCreate());
        if (pEditor && pEditor->Version() != xlionrender::xRenderEditor::kVersion) { OutputDebugStringA("xRenderEditor: the render module has another version of the interface\n"); pEditor.reset(); }
        return pEditor;
    }

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