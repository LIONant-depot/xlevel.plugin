#ifndef XLEVEL_VISUAL_STUDIO_H
#define XLEVEL_VISUAL_STUDIO_H
#pragma once

// The way back to Visual Studio, where the scripts are written: the button at the right of the transport (Play, Step, Pause, Stop) and the OpenInVisualStudio command do the same thing.
// The game project of a Game is a CMake project that Visual Studio opens as the solution CMake wrote into its Build folder when the Game was first built. If a Visual Studio that was started on
// that solution is running, its window comes forward; otherwise the solution is handed to Windows, which starts one.
//
// A file of the scripts (the one a system or a component is defined in) opens in that same Visual Studio: through its automation object (DTE.ItemOperations.OpenFile), found in the running object
// table by the process id of the Visual Studio that was started on the solution; when none is running one is started on the solution and the file opens in it as soon as it can be reached.
//
// A running Visual Studio is recognised by its command line (Windows starts it with the path of the solution), so one in which the solution was opened afterwards (File > Open) is not found: another
// is started then.
#include "plugins/xlevel.plugin/source/Editor/xlevel_context.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#if defined(_WIN32)
#include <tlhelp32.h>
#include <oleauto.h>
#endif
#undef ERROR
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace xlevel
{
    // The solution of the game project whose Build folder this is ("Script" is the name of the project the Game resource writes); empty when it is not made yet.
    inline std::filesystem::path GameSolutionIn(const std::filesystem::path& BuildDir) noexcept
    {
        if (BuildDir.empty()) return {};
        std::error_code Ec;
        const auto Solution = BuildDir / L"Script.sln";
        return std::filesystem::is_regular_file(Solution, Ec) ? Solution : std::filesystem::path{};
    }

#if defined(_WIN32)
    namespace details
    {
        inline std::wstring Lowered(std::wstring Text) noexcept { for (auto& c : Text) c = static_cast<wchar_t>(std::towlower(c)); return Text; }

        // The command line of a process of this user (empty when it cannot be read): ProcessCommandLineInformation of ntdll's NtQueryInformationProcess.
        inline std::wstring CommandLineOf(HANDLE Process) noexcept
        {
            using query_t = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
            static const auto Query = reinterpret_cast<query_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
            if (!Query) return {};
            constexpr ULONG ProcessCommandLineInformation = 60;
            std::vector<unsigned char> Buffer(4096);
            for (int Try = 0; Try < 3; ++Try)
            {
                ULONG Needed = 0;
                const LONG Status = Query(Process, ProcessCommandLineInformation, Buffer.data(), static_cast<ULONG>(Buffer.size()), &Needed);
                if (Status == 0)
                {
                    struct unicode_string { USHORT m_Length, m_MaximumLength; PWSTR m_Buffer; };                // ntdll's UNICODE_STRING (winternl.h is not included)
                    const auto& Text = *reinterpret_cast<const unicode_string*>(Buffer.data());
                    return std::wstring(Text.m_Buffer, Text.m_Length / sizeof(wchar_t));
                }
                if (Needed <= Buffer.size()) return {};
                Buffer.resize(Needed);
            }
            return {};
        }

        // The window of the Visual Studio that was started on this solution: its process is devenv.exe with the solution's path in its command line. 0 when there is none.
        inline HWND VisualStudioWith(const std::filesystem::path& Solution, DWORD* pProcess = nullptr) noexcept
        {
            const std::wstring Wanted = Lowered(Solution.wstring());
            const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (Snapshot == INVALID_HANDLE_VALUE) return nullptr;
            PROCESSENTRY32W Entry{ sizeof(Entry) };
            DWORD Found = 0;
            for (BOOL More = Process32FirstW(Snapshot, &Entry); More && !Found; More = Process32NextW(Snapshot, &Entry))
            {
                if (_wcsicmp(Entry.szExeFile, L"devenv.exe") != 0) continue;
                if (const HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Entry.th32ProcessID))
                {
                    if (Lowered(CommandLineOf(Process)).find(Wanted) != std::wstring::npos) Found = Entry.th32ProcessID;
                    CloseHandle(Process);
                }
            }
            CloseHandle(Snapshot);
            if (!Found) return nullptr;

            struct search { DWORD m_Process; HWND m_Window; } Search{ Found, nullptr };
            EnumWindows([](HWND Window, LPARAM User) -> BOOL
            {
                auto& S = *reinterpret_cast<search*>(User);
                DWORD Owner = 0;
                GetWindowThreadProcessId(Window, &Owner);
                if (Owner != S.m_Process || !IsWindowVisible(Window) || GetWindow(Window, GW_OWNER) || GetWindowTextLengthW(Window) == 0) return TRUE;
                S.m_Window = Window;
                return FALSE;
            }, reinterpret_cast<LPARAM>(&Search));
            if (pProcess) *pProcess = Found;
            return Search.m_Window;
        }
    }

    namespace details
    {
        // A late-bound call of a method or a property of an automation object.
        inline HRESULT Invoke(IDispatch* pObject, const wchar_t* pName, WORD Flags, VARIANT* pArgs, UINT nArgs, VARIANT* pResult) noexcept
        {
            DISPID  Id   = 0;
            LPOLESTR Name = const_cast<LPOLESTR>(pName);
            if (const HRESULT Hr = pObject->GetIDsOfNames(IID_NULL, &Name, 1, LOCALE_USER_DEFAULT, &Id); FAILED(Hr)) return Hr;
            DISPPARAMS Params{ pArgs, nullptr, nArgs, 0 };
            return pObject->Invoke(Id, IID_NULL, LOCALE_USER_DEFAULT, Flags, &Params, pResult, nullptr, nullptr);
        }

        // The automation object (DTE) of the Visual Studio with this process id: it registers itself in the running object table as "!VisualStudio.DTE.<version>:<process id>".
        inline IDispatch* DteOfProcess(DWORD Process) noexcept
        {
            IRunningObjectTable* pTable = nullptr;
            if (FAILED(GetRunningObjectTable(0, &pTable))) return nullptr;
            IDispatch*    pDte  = nullptr;
            IEnumMoniker* pEnum = nullptr;
            if (SUCCEEDED(pTable->EnumRunning(&pEnum)))
            {
                const std::wstring Suffix = std::format(L":{}", Process);
                IBindCtx* pContext = nullptr;
                CreateBindCtx(0, &pContext);
                IMoniker* pMoniker = nullptr;
                while (!pDte && pEnum->Next(1, &pMoniker, nullptr) == S_OK)
                {
                    LPOLESTR pName = nullptr;
                    if (SUCCEEDED(pMoniker->GetDisplayName(pContext, nullptr, &pName)) && pName)
                    {
                        const std::wstring Name(pName);
                        CoTaskMemFree(pName);
                        if (Name.starts_with(L"!VisualStudio.DTE.") && Name.ends_with(Suffix))
                        {
                            IUnknown* pUnknown = nullptr;
                            if (SUCCEEDED(pTable->GetObject(pMoniker, &pUnknown)) && pUnknown)
                            {
                                pUnknown->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&pDte));
                                pUnknown->Release();
                            }
                        }
                    }
                    pMoniker->Release();
                }
                if (pContext) pContext->Release();
                pEnum->Release();
            }
            pTable->Release();
            return pDte;
        }

        // DTE.ItemOperations.OpenFile(File): the file opens in that Visual Studio.
        inline HRESULT OpenFileInDte(IDispatch* pDte, const std::wstring& File) noexcept
        {
            VARIANT Operations; VariantInit(&Operations);
            HRESULT Hr = Invoke(pDte, L"ItemOperations", DISPATCH_PROPERTYGET, nullptr, 0, &Operations);
            if (FAILED(Hr) || Operations.vt != VT_DISPATCH || !Operations.pdispVal) { VariantClear(&Operations); return FAILED(Hr) ? Hr : E_FAIL; }
            VARIANT Arg;    VariantInit(&Arg);    Arg.vt = VT_BSTR; Arg.bstrVal = SysAllocString(File.c_str());
            VARIANT Result; VariantInit(&Result);
            Hr = Invoke(Operations.pdispVal, L"OpenFile", DISPATCH_METHOD, &Arg, 1, &Result);
            VariantClear(&Arg); VariantClear(&Result); VariantClear(&Operations);
            return Hr;
        }

        // One try, from any thread: the file in the Visual Studio that was started on the solution, which then comes forward. S_OK when it opened there.
        inline HRESULT TryOpenInVisualStudio(const std::filesystem::path& Solution, const std::filesystem::path& File) noexcept
        {
            const HRESULT Init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            DWORD   Process = 0;
            HRESULT Hr      = E_FAIL;
            if (HWND Window = VisualStudioWith(Solution, &Process))
                if (IDispatch* pDte = DteOfProcess(Process))
                {
                    Hr = OpenFileInDte(pDte, File.wstring());
                    pDte->Release();
                    if (SUCCEEDED(Hr)) { if (IsIconic(Window)) ShowWindow(Window, SW_RESTORE); SetForegroundWindow(Window); }
                }
            if (SUCCEEDED(Init)) CoUninitialize();
            return Hr;
        }
    }

    // Opens a file of the scripts (a system's, a component's) in the Visual Studio of this Level's game project: the one that has the solution open, or a new one that opens it when it is ready
    // (DryRun: only says which). The reply says what happened, without a name in front.
    inline std::string RequestOpenFileInVisualStudio(level_context& Ed, const std::filesystem::path& File, bool bDryRun) noexcept
    {
        const auto Solution = Ed.m_GameSolution ? Ed.m_GameSolution() : std::filesystem::path{};
        if (Solution.empty()) return "this Level has no game project (the Visual Studio solution of its scripts) to open the file in yet";
        std::error_code Ec;
        if (!std::filesystem::is_regular_file(File, Ec)) return std::format("{} is not a file", File.string());

        DWORD Process = 0;
        const bool bRunning = details::VisualStudioWith(Solution, &Process) != nullptr;
        if (bDryRun)
            return bRunning ? std::format("would open {} in the Visual Studio that has {} open (process {})", File.string(), Solution.string(), Process)
                            : std::format("would start Visual Studio on {} and open {} in it", Solution.string(), File.string());
        if (bRunning)
        {
            if (const HRESULT Hr = details::TryOpenInVisualStudio(Solution, File); SUCCEEDED(Hr))
                return std::format("opened {} in the Visual Studio that has {} open (process {})", File.string(), Solution.string(), Process);
            else
            {
                ShellExecuteW(nullptr, L"open", File.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return std::format("could not reach the Visual Studio that has {} open (error 0x{:08X}): the file was handed to Windows", Solution.string(), static_cast<unsigned>(Hr));
            }
        }
        ShellExecuteW(nullptr, L"open", Solution.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        std::thread([Solution, File]
        {
            for (int Try = 0; Try < 120; ++Try)                                       // Visual Studio takes a while to start and to load the solution
            {
                Sleep(1000);
                if (SUCCEEDED(details::TryOpenInVisualStudio(Solution, File))) return;
            }
        }).detach();
        return std::format("started Visual Studio on {}: {} opens in it when it is ready", Solution.string(), File.string());
    }

    // Opens this Level's game project in Visual Studio: brings forward the one that has it open, or starts one (DryRun: only says which). The reply says what happened.
    inline std::string RequestOpenVisualStudio(level_context& Ed, bool bDryRun) noexcept
    {
        const auto Solution = Ed.m_GameSolution ? Ed.m_GameSolution() : std::filesystem::path{};
        if (Solution.empty()) return "OpenInVisualStudio: this Level has no game project to open yet (a Level that names a Game with script modules gets one when the Game is first built)";

        DWORD Process = 0;
        if (HWND Window = details::VisualStudioWith(Solution, &Process))
        {
            if (bDryRun) return std::format("OpenInVisualStudio: would bring forward the Visual Studio that has {} open (process {})", Solution.string(), Process);
            if (IsIconic(Window)) ShowWindow(Window, SW_RESTORE);
            SetForegroundWindow(Window);
            return std::format("OpenInVisualStudio: brought forward the Visual Studio that has {} open (process {})", Solution.string(), Process);
        }
        if (bDryRun) return std::format("OpenInVisualStudio: would open {}", Solution.string());
        const auto Result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", Solution.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        return Result > 32 ? std::format("OpenInVisualStudio: opened {}", Solution.string()) : std::format("OpenInVisualStudio: Windows could not open {} (error {})", Solution.string(), Result);
    }
#else
    // Linux port: Visual Studio integration is Windows-only
    inline std::string RequestOpenFileInVisualStudio(level_context&, const std::filesystem::path&, bool) noexcept { return "Visual Studio integration is not available on this platform"; }
    inline std::string RequestOpenVisualStudio(level_context&, bool) noexcept { return "OpenInVisualStudio: not available on this platform"; }
#endif
}

#endif // XLEVEL_VISUAL_STUDIO_H
