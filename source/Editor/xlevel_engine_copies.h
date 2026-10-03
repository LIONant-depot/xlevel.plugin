#ifndef XLEVEL_ENGINE_COPIES_H
#define XLEVEL_ENGINE_COPIES_H
#pragma once

// The copies of the engine DLLs that one Level runs on. xECS is mostly header-only: the registry lives in LIONCore.dll, so two Levels that share the one LIONCore.dll share one registry (one component bit
// space, one set of systems). To run several Levels at once, each completely independent of the others, each gets its OWN copy of the core and of the render DLL, loaded under their own names:
//
//      LIONCore.dll      -> LC000001.dll        (a byte copy: the core imports nothing of ours)
//      LIONRender.dll    -> LR000001.dll        (its import of LIONCore.dll is renamed to LC000001.dll, so that it binds to ITS copy of the core)
//      Game.dll          -> its import of LIONCore.dll is renamed the same way (see PatchedCopy, used by the Game.dll loader)
//
// The new names are never longer than the ones they replace (the import table is patched in place, no section changes) and the PE checksum is fixed. The copies are found by the loader by the name of the
// module that is already loaded, so the core copy is always loaded first and by its full path. PDBs are not copied: the debug directory of a copy still names the original PDB, which is where the symbols are.
//
// The manager is the app's (Services()): it makes a set when a Level opens and the set goes away (modules freed, files deleted) with the last one that holds it. Many DLLs loaded at once is accepted.
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xlevel::engine
{
    //---------------------------------------------------------------------------
    // PE patching: rename what a module imports, fix the checksum
    //---------------------------------------------------------------------------
    // The offsets (in the file) of the names of the imported DLLs of a 64-bit PE image; empty when the bytes are not one.
    inline std::vector<std::size_t> ImportNameOffsets(const std::vector<std::uint8_t>& Data) noexcept
    {
        std::vector<std::size_t> Out;
        if (Data.size() < sizeof(IMAGE_DOS_HEADER)) return Out;
        const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Data.data());
        if (pDos->e_magic != IMAGE_DOS_SIGNATURE || pDos->e_lfanew <= 0 || static_cast<std::size_t>(pDos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > Data.size()) return Out;
        const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Data.data() + pDos->e_lfanew);
        if (pNt->Signature != IMAGE_NT_SIGNATURE || pNt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return Out;

        const auto* pSections = IMAGE_FIRST_SECTION(pNt);
        const auto  ToOffset = [&](std::uint32_t Rva) noexcept -> std::size_t
        {
            for (unsigned i = 0; i < pNt->FileHeader.NumberOfSections; ++i)
            {
                const auto& S = pSections[i];
                if (Rva >= S.VirtualAddress && Rva < S.VirtualAddress + (std::max)(S.Misc.VirtualSize, S.SizeOfRawData)) return S.PointerToRawData + (Rva - S.VirtualAddress);
            }
            return 0;
        };

        const auto& Dir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (Dir.VirtualAddress == 0) return Out;
        auto Offset = ToOffset(Dir.VirtualAddress);
        while (Offset && Offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= Data.size())
        {
            const auto* pDesc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(Data.data() + Offset);
            if (pDesc->Name == 0) break;
            if (const auto NameOffset = ToOffset(pDesc->Name); NameOffset && NameOffset < Data.size()) Out.push_back(NameOffset);
            Offset += sizeof(IMAGE_IMPORT_DESCRIPTOR);
        }
        return Out;
    }

    // The names (as written) of the DLLs a PE image imports, for the log and the tests.
    inline std::vector<std::string> ImportsOf(const std::vector<std::uint8_t>& Data) noexcept
    {
        std::vector<std::string> Out;
        for (const auto Offset : ImportNameOffsets(Data)) Out.emplace_back(reinterpret_cast<const char*>(Data.data() + Offset));
        return Out;
    }

    // Renames every import of From (any case) to To, which has to fit in the room of the old name. Returns how many were renamed.
    inline int RenameImport(std::vector<std::uint8_t>& Data, std::string_view From, std::string_view To) noexcept
    {
        if (To.size() > From.size()) return 0;
        int Count = 0;
        for (const auto Offset : ImportNameOffsets(Data))
        {
            auto* pName = reinterpret_cast<char*>(Data.data() + Offset);
            const std::string_view Name(pName);
            if (Name.size() != From.size() || _strnicmp(Name.data(), From.data(), From.size()) != 0) continue;
            std::memset(pName, 0, Name.size());
            std::memcpy(pName, To.data(), To.size());
            ++Count;
        }
        return Count;
    }

    // The checksum the loader and the debuggers check (what imagehlp's CheckSumMappedFile computes), written into the header.
    inline void FixChecksum(std::vector<std::uint8_t>& Data) noexcept
    {
        if (Data.size() < sizeof(IMAGE_DOS_HEADER)) return;
        const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Data.data());
        if (pDos->e_magic != IMAGE_DOS_SIGNATURE || static_cast<std::size_t>(pDos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > Data.size()) return;
        auto*       pNt        = reinterpret_cast<IMAGE_NT_HEADERS64*>(Data.data() + pDos->e_lfanew);
        pNt->OptionalHeader.CheckSum = 0;

        std::uint64_t Sum = 0;
        const auto    nWords = Data.size() / 2;
        for (std::size_t i = 0; i < nWords; ++i)
        {
            Sum += static_cast<std::uint64_t>(Data[2 * i]) | (static_cast<std::uint64_t>(Data[2 * i + 1]) << 8);
            Sum = (Sum & 0xFFFF) + (Sum >> 16);
        }
        if (Data.size() & 1) { Sum += Data.back(); Sum = (Sum & 0xFFFF) + (Sum >> 16); }
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
        pNt->OptionalHeader.CheckSum = static_cast<DWORD>(Sum + Data.size());
    }

    // Whether the checksum in the header of the file is the one the system computes (imagehlp, loaded on demand): what a debugger or a signing tool checks.
    inline bool ChecksumIsRight(const std::filesystem::path& Path) noexcept
    {
        using pfn_map_and_sum = DWORD(WINAPI*)(PCWSTR, PDWORD, PDWORD);
        HMODULE hImageHlp  = LoadLibraryW(L"imagehlp.dll");
        auto*   pMapAndSum = hImageHlp ? reinterpret_cast<pfn_map_and_sum>(GetProcAddress(hImageHlp, "MapFileAndCheckSumW")) : nullptr;
        DWORD   Header = 0, Computed = 0;
        const bool bOk = pMapAndSum && pMapAndSum(Path.c_str(), &Header, &Computed) == 0 && Header == Computed;
        if (hImageHlp) FreeLibrary(hImageHlp);
        return bOk;
    }

    inline std::vector<std::uint8_t> ReadFile(const std::filesystem::path& Path) noexcept
    {
        std::ifstream In(Path, std::ios::binary | std::ios::ate);
        if (!In) return {};
        std::vector<std::uint8_t> Data(static_cast<std::size_t>(In.tellg()));
        In.seekg(0);
        In.read(reinterpret_cast<char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
        return In ? Data : std::vector<std::uint8_t>{};
    }

    // Src copied to Dst with its imports of From renamed to To (when To is empty it is a plain copy). False when the copy could not be made or Src does not import From.
    inline bool PatchedCopy(const std::filesystem::path& Src, const std::filesystem::path& Dst, std::string_view From = {}, std::string_view To = {}) noexcept
    {
        auto Data = ReadFile(Src);
        if (Data.empty()) return false;
        if (!To.empty())
        {
            if (RenameImport(Data, From, To) == 0) return false;
            FixChecksum(Data);
        }
        std::error_code Ec;
        std::filesystem::create_directories(Dst.parent_path(), Ec);
        std::ofstream Out(Dst, std::ios::binary | std::ios::trunc);
        Out.write(reinterpret_cast<const char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
        return static_cast<bool>(Out);
    }

    //---------------------------------------------------------------------------
    // The set of copies of one Level
    //---------------------------------------------------------------------------
    inline constexpr const char*    kCoreName   = "LIONCore.dll";
    inline constexpr const wchar_t* kCoreNameW  = L"LIONCore.dll";
    inline constexpr const wchar_t* kRenderNameW = L"LIONRender.dll";

    struct engine_set
    {
        std::uint32_t         m_Id = 0;
        std::wstring          m_Core;           // the module names (the loader finds the copies by them once they are loaded)
        std::wstring          m_Render;         // empty when there is no render DLL (a build without it)
        std::filesystem::path m_CorePath, m_RenderPath;
        HMODULE               m_hCore = nullptr, m_hRender = nullptr;

        std::string CoreName() const { return std::filesystem::path(m_Core).string(); }

        engine_set() = default;
        engine_set(const engine_set&) = delete;
        engine_set& operator=(const engine_set&) = delete;

        // Freed in the order they were loaded, backwards: whatever imports the core goes first. A module that cannot be freed (something still holds it) just stays; its file is then left for the next run to delete.
        ~engine_set() noexcept
        {
            if (m_hRender) FreeLibrary(m_hRender);
            if (m_hCore)   FreeLibrary(m_hCore);
            if (!m_RenderPath.empty()) RemoveFile(m_RenderPath);
            if (!m_CorePath.empty())   RemoveFile(m_CorePath);
        }

        // A file that was just unloaded can stay locked for a moment (a scanner, the loader finishing): tried again a few times, then left for the next set to sweep (manager::CleanLeftovers).
        static void RemoveFile(const std::filesystem::path& Path) noexcept
        {
            for (int Try = 0; Try < 20; ++Try)
            {
                std::error_code Ec;
                std::filesystem::remove(Path, Ec);
                if (!Ec || !std::filesystem::exists(Path, Ec)) return;
                Sleep(25);
            }
        }
    };

    // The manager: makes the sets. One per app (Services()); the originals are looked for next to the executable (where the build puts them).
    struct manager
    {
        std::filesystem::path m_OriginalsDir;       // LIONCore.dll and LIONRender.dll are here
        std::filesystem::path m_CopiesDir;          // the copies are made and loaded from here
        std::atomic<std::uint32_t> m_NextId{ 1 };

        manager() noexcept
        {
            wchar_t Buffer[MAX_PATH * 2]{};
            GetModuleFileNameW(nullptr, Buffer, static_cast<DWORD>(std::size(Buffer)));
            m_OriginalsDir = std::filesystem::path(Buffer).parent_path();
            m_CopiesDir    = m_OriginalsDir / L"EngineCopies";
        }

        // What an earlier run or an earlier set left (it crashed, or a file stayed locked): deleted when it can be. A file that is loaded (by a set that is alive here or by another instance of the editor) cannot be deleted, so it is just skipped.
        void CleanLeftovers() noexcept
        {
            std::error_code Ec;
            for (std::filesystem::directory_iterator It(m_CopiesDir, Ec), End; !Ec && It != End; It.increment(Ec))
            {
                const auto Name = It->path().filename().wstring();
                if (Name.size() == 12 && (Name.starts_with(L"LC") || Name.starts_with(L"LR")) && Name.ends_with(L".dll"))
                {
                    std::error_code RemoveEc;
                    std::filesystem::remove(It->path(), RemoveEc);
                }
            }
        }

        // A new set: the core copied and loaded first, then the render DLL copied with its import patched to the core copy. Null (and nothing left behind) when a step fails; Why says which.
        std::shared_ptr<engine_set> Make(std::string& Why) noexcept
        {
            CleanLeftovers();
            const auto OriginalCore = m_OriginalsDir / kCoreNameW;
            if (!std::filesystem::exists(OriginalCore)) { Why = std::format("{} is not next to the editor", std::filesystem::path(kCoreNameW).string()); return {}; }

            auto pSet = std::make_shared<engine_set>();
            std::error_code Ec;
            // An id whose files do not exist (another editor instance may be running with its own).
            for (int Try = 0; Try < 1000; ++Try)
            {
                pSet->m_Id   = m_NextId++;
                pSet->m_Core = std::format(L"LC{:06}.dll", pSet->m_Id % 1000000);
                pSet->m_CorePath = m_CopiesDir / pSet->m_Core;
                if (!std::filesystem::exists(pSet->m_CorePath, Ec)) break;
            }
            pSet->m_Render = std::format(L"LR{:06}.dll", pSet->m_Id % 1000000);
            pSet->m_RenderPath = m_CopiesDir / pSet->m_Render;

            if (!PatchedCopy(OriginalCore, pSet->m_CorePath)) { Why = "the core could not be copied"; pSet->m_CorePath.clear(); pSet->m_RenderPath.clear(); return {}; }
            pSet->m_hCore = LoadLibraryExW(pSet->m_CorePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!pSet->m_hCore) { Why = std::format("the core copy did not load (error {})", GetLastError()); pSet->m_RenderPath.clear(); return {}; }

            const auto OriginalRender = m_OriginalsDir / kRenderNameW;
            if (std::filesystem::exists(OriginalRender, Ec))
            {
                if (!PatchedCopy(OriginalRender, pSet->m_RenderPath, kCoreName, pSet->CoreName())) { Why = "the render DLL could not be copied and patched"; pSet->m_RenderPath.clear(); return {}; }
                pSet->m_hRender = LoadLibraryExW(pSet->m_RenderPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (!pSet->m_hRender) { Why = std::format("the render copy did not load (error {})", GetLastError()); return {}; }
            }
            else
            {
                pSet->m_Render.clear();
                pSet->m_RenderPath.clear();
            }
            return pSet;
        }
    };
}

#endif // XLEVEL_ENGINE_COPIES_H
