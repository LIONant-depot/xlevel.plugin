#ifndef XLEVEL_VISUAL_STUDIO_H
#define XLEVEL_VISUAL_STUDIO_H
#pragma once

// The way back to Visual Studio, where the scripts are written: the button at the right of the transport (Play, Step, Pause, Stop) and the OpenInVisualStudio command do the same thing.
// The game project of a Game is a CMake project that Visual Studio opens as the solution CMake wrote into its Build folder when the Game was first built. If a Visual Studio that was started on
// that solution is running, its window comes forward; otherwise the solution is handed to Windows, which starts one.
//
// A running Visual Studio is recognised by its command line (Windows starts it with the path of the solution), so one in which the solution was opened afterwards (File > Open) is not found: another
// is started then.
#include "plugins/xlevel.plugin/source/Editor/xlevel_context.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#undef ERROR
#pragma comment(lib, "shell32.lib")

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
}

#endif // XLEVEL_VISUAL_STUDIO_H
