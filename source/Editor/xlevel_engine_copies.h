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
//
// Linux (ELF): the same scheme on shared objects. The module names keep their Windows spelling ("LC000001.dll"; the platform layer maps "X.dll" to "libX.so"), the files are the
// ELF names (libLC000001.so):
//
//      libLIONCore.so    -> libLC000001.so      (its DT_SONAME renamed to libLC000001.so: a later DT_NEEDED of that name binds to this copy, the way the loader finds a loaded DLL by name)
//      libLIONRender.so  -> libLR000001.so      (its DT_NEEDED libLIONCore.so renamed to libLC000001.so, and its own DT_SONAME to libLR000001.so)
//      Game .so          -> its DT_NEEDED libLIONCore.so renamed the same way
//
// The names are patched in place in .dynstr (every NUL-bounded copy of the name, so .gnu.version_r's file name follows), never longer than the old one. ELF looks a symbol up in the global
// scope first (the executable and what it links, the ORIGINAL libLIONCore.so among them), so the copies are dlopen'ed with RTLD_DEEPBIND: a copy's own dependencies (its core copy) come
// first, which is what binding an import to a named DLL does on Windows. libxlion_pathcompat.so (the Windows-path layer at the libc boundary) is linked by the engine libraries for that
// reason: a deep-bound copy still finds it ahead of libc. Each process makes its copies under EngineCopies/<pid>; a folder whose process is gone is swept by the next one.
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
#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <dlfcn.h>
#include <elf.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace xlevel::engine
{
#if !defined(_WIN32)
    //---------------------------------------------------------------------------
    // ELF patching: rename what a shared object needs (DT_NEEDED) and what it is called (DT_SONAME)
    //---------------------------------------------------------------------------
    // "LIONCore.dll" -> "libLIONCore.so" (a name already in ELF spelling is kept)
    inline std::string ElfName(std::string_view Name) noexcept
    {
        if (Name.size() > 4 && (Name.ends_with(".dll") || Name.ends_with(".DLL"))) return std::format("lib{}.so", Name.substr(0, Name.size() - 4));
        return std::string(Name);
    }

    struct elf_dynamic
    {
        std::size_t              m_StrTab = 0, m_StrSize = 0;     // .dynstr, in the file
        std::vector<std::size_t> m_Needed;                        // offsets of the DT_NEEDED names in .dynstr
        std::size_t              m_SoName = SIZE_MAX;             // offset of the DT_SONAME name in .dynstr
    };

    // The dynamic section of a 64-bit little-endian ELF image; false when the bytes are not one.
    inline bool ParseDynamic(const std::vector<std::uint8_t>& Data, elf_dynamic& Out) noexcept
    {
        if (Data.size() < sizeof(Elf64_Ehdr)) return false;
        Elf64_Ehdr Eh; std::memcpy(&Eh, Data.data(), sizeof(Eh));
        if (std::memcmp(Eh.e_ident, ELFMAG, SELFMAG) != 0 || Eh.e_ident[EI_CLASS] != ELFCLASS64 || Eh.e_ident[EI_DATA] != ELFDATA2LSB) return false;
        if (Eh.e_phentsize != sizeof(Elf64_Phdr) || Eh.e_phoff + std::size_t(Eh.e_phnum) * sizeof(Elf64_Phdr) > Data.size()) return false;

        std::vector<Elf64_Phdr> Ph(Eh.e_phnum);
        std::memcpy(Ph.data(), Data.data() + Eh.e_phoff, Ph.size() * sizeof(Elf64_Phdr));
        const auto ToOffset = [&](std::uint64_t VAddr) noexcept -> std::size_t
        {
            for (auto& P : Ph) if (P.p_type == PT_LOAD && VAddr >= P.p_vaddr && VAddr < P.p_vaddr + P.p_filesz) return static_cast<std::size_t>(P.p_offset + (VAddr - P.p_vaddr));
            return 0;
        };

        std::uint64_t StrTabAddr = 0;
        for (auto& P : Ph)
        {
            if (P.p_type != PT_DYNAMIC) continue;
            for (std::size_t o = P.p_offset; o + sizeof(Elf64_Dyn) <= P.p_offset + P.p_filesz && o + sizeof(Elf64_Dyn) <= Data.size(); o += sizeof(Elf64_Dyn))
            {
                Elf64_Dyn D; std::memcpy(&D, Data.data() + o, sizeof(D));
                if (D.d_tag == DT_NULL) break;
                if      (D.d_tag == DT_STRTAB) StrTabAddr    = D.d_un.d_ptr;
                else if (D.d_tag == DT_STRSZ)  Out.m_StrSize = static_cast<std::size_t>(D.d_un.d_val);
                else if (D.d_tag == DT_NEEDED) Out.m_Needed.push_back(static_cast<std::size_t>(D.d_un.d_val));
                else if (D.d_tag == DT_SONAME) Out.m_SoName = static_cast<std::size_t>(D.d_un.d_val);
            }
        }
        Out.m_StrTab = StrTabAddr ? ToOffset(StrTabAddr) : 0;
        return Out.m_StrTab != 0 && Out.m_StrSize != 0 && Out.m_StrTab + Out.m_StrSize <= Data.size();
    }

    inline std::string DynString(const std::vector<std::uint8_t>& Data, const elf_dynamic& Dyn, std::size_t Offset) noexcept
    {
        if (Offset >= Dyn.m_StrSize) return {};
        const char* p = reinterpret_cast<const char*>(Data.data() + Dyn.m_StrTab + Offset);
        return std::string(p, ::strnlen(p, Dyn.m_StrSize - Offset));
    }

    // The names (as written) of the shared objects an ELF image needs, for the log and the tests.
    inline std::vector<std::string> ImportsOf(const std::vector<std::uint8_t>& Data) noexcept
    {
        std::vector<std::string> Out;
        elf_dynamic Dyn;
        if (ParseDynamic(Data, Dyn)) for (auto o : Dyn.m_Needed) Out.push_back(DynString(Data, Dyn, o));
        return Out;
    }

    // Every NUL-bounded copy of From in .dynstr becomes To (padded with NULs; To has to fit). Returns how many there were.
    inline int RenameDynString(std::vector<std::uint8_t>& Data, const elf_dynamic& Dyn, std::string_view From, std::string_view To) noexcept
    {
        if (From.empty() || To.size() > From.size()) return 0;
        int   Count = 0;
        char* pBase = reinterpret_cast<char*>(Data.data() + Dyn.m_StrTab);
        for (std::size_t i = 0; i + From.size() < Dyn.m_StrSize; ++i)
        {
            if ((i == 0 || pBase[i - 1] == 0) && pBase[i + From.size()] == 0 && std::memcmp(pBase + i, From.data(), From.size()) == 0)
            {
                std::memset(pBase + i, 0, From.size());
                std::memcpy(pBase + i, To.data(), To.size());
                ++Count;
                i += From.size();
            }
        }
        return Count;
    }

    // Renames what the image needs: From (Windows or ELF spelling) becomes To. Returns how many DT_NEEDED entries named it (0: the image does not need From, nothing changed).
    inline int RenameImport(std::vector<std::uint8_t>& Data, std::string_view From, std::string_view To) noexcept
    {
        elf_dynamic Dyn;
        if (!ParseDynamic(Data, Dyn)) return 0;
        const auto ElfFrom = ElfName(From), ElfTo = ElfName(To);
        int nNeeded = 0;
        for (auto o : Dyn.m_Needed) if (DynString(Data, Dyn, o) == ElfFrom) ++nNeeded;
        if (nNeeded == 0 || RenameDynString(Data, Dyn, ElfFrom, ElfTo) == 0) return 0;
        return nNeeded;
    }

    // The image's own name (DT_SONAME) becomes To when it fits. False when there is none or it does not fit.
    inline bool RenameSoName(std::vector<std::uint8_t>& Data, std::string_view To) noexcept
    {
        elf_dynamic Dyn;
        if (!ParseDynamic(Data, Dyn) || Dyn.m_SoName == SIZE_MAX) return false;
        const auto Old = DynString(Data, Dyn, Dyn.m_SoName);
        return !Old.empty() && Old != To && RenameDynString(Data, Dyn, Old, To) > 0;
    }

    inline std::string SoNameOf(const std::vector<std::uint8_t>& Data) noexcept
    {
        elf_dynamic Dyn;
        return ParseDynamic(Data, Dyn) && Dyn.m_SoName != SIZE_MAX ? DynString(Data, Dyn, Dyn.m_SoName) : std::string{};
    }

    inline void FixChecksum(std::vector<std::uint8_t>&) noexcept {}                         // ELF has no image checksum
    inline bool ChecksumIsRight(const std::filesystem::path&) noexcept { return true; }
#else
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

#endif
    // LoadLibrary of a file that was just written: a scanner can hold it for a moment (a sharing violation), so it is tried again for a short while.
    // Linux: dlopen with RTLD_DEEPBIND - the copy's own dependencies (its copy of the core) come before the global scope, which holds the original core (see the top comment).
    inline HMODULE LoadCopy(const std::filesystem::path& Path, DWORD& Error) noexcept
    {
    #if defined(_WIN32)
        for (int Try = 0; Try < 40; ++Try)
        {
            if (HMODULE hModule = LoadLibraryExW(Path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return hModule;
            Error = GetLastError();
            if (Error != ERROR_SHARING_VIOLATION && Error != ERROR_ACCESS_DENIED) break;
            Sleep(25);
        }
        return nullptr;
    #else
        if (void* h = ::dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND)) return reinterpret_cast<HMODULE>(h);
        const char* pWhy = ::dlerror();
        std::printf("[EngineCopies] dlopen %s failed: %s\n", Path.c_str(), pWhy ? pWhy : "?");
        std::fflush(stdout);
        Error = ERROR_FILE_NOT_FOUND;
        return nullptr;
    #endif
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
    #if !defined(_WIN32)
        // The copy is called by its own file name, so that a DT_NEEDED of that name binds to it once it is loaded (best effort: a name that does not fit stays as it was)
        RenameSoName(Data, Dst.filename().string());
    #endif
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

    // The file a module name is in: the name itself on Windows, its ELF spelling elsewhere ("LC000001.dll" -> "libLC000001.so")
    inline std::filesystem::path FileOf(std::wstring_view ModuleName) noexcept
    {
    #if defined(_WIN32)
        return std::filesystem::path(ModuleName);
    #else
        return ElfName(std::filesystem::path(ModuleName).string());
    #endif
    }

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
        #if !defined(_WIN32)
            m_CopiesDir   /= std::to_string(::getpid());        // this process's own folder (see CleanLeftovers)
        #endif
        }

        // What an earlier run or an earlier set left (it crashed, or a file stayed locked): deleted when it can be. A file that is loaded (by a set that is alive here or by another instance of the editor) cannot be deleted, so it is just skipped.
        void CleanLeftovers() noexcept
        {
            std::error_code Ec;
        #if defined(_WIN32)
            for (std::filesystem::directory_iterator It(m_CopiesDir, Ec), End; !Ec && It != End; It.increment(Ec))
            {
                const auto Name = It->path().filename().wstring();
                if (Name.size() == 12 && (Name.starts_with(L"LC") || Name.starts_with(L"LR")) && Name.ends_with(L".dll"))
                {
                    std::error_code RemoveEc;
                    std::filesystem::remove(It->path(), RemoveEc);
                }
            }
        #else
            // A loaded .so can be deleted on Linux (it stays mapped), so "cannot be deleted" does not protect the copies of a running editor: each process has its own folder, and only the
            // folders of processes that are gone are swept.
            for (std::filesystem::directory_iterator It(m_CopiesDir.parent_path(), Ec), End; !Ec && It != End; It.increment(Ec))
            {
                const auto Name = It->path().filename().string();
                if (Name.empty() || Name.find_first_not_of("0123456789") != std::string::npos) continue;
                const auto Pid = static_cast<pid_t>(std::strtol(Name.c_str(), nullptr, 10));
                if (Pid != ::getpid() && ::kill(Pid, 0) != 0 && errno == ESRCH)
                {
                    std::error_code RemoveEc;
                    std::filesystem::remove_all(It->path(), RemoveEc);
                }
            }
        #endif
        }

        // A new set: the core copied and loaded first, then the render DLL copied with its import patched to the core copy. Null (and nothing left behind) when a step fails; Why says which.
        std::shared_ptr<engine_set> Make(std::string& Why) noexcept
        {
            CleanLeftovers();
            const auto OriginalCore = m_OriginalsDir / FileOf(kCoreNameW);
            if (!std::filesystem::exists(OriginalCore)) { Why = std::format("{} is not next to the editor", OriginalCore.filename().string()); return {}; }

            auto pSet = std::make_shared<engine_set>();
            std::error_code Ec;
            // An id whose files do not exist (another editor instance may be running with its own).
            for (int Try = 0; Try < 1000; ++Try)
            {
                pSet->m_Id   = m_NextId++;
                pSet->m_Core = std::format(L"LC{:06}.dll", pSet->m_Id % 1000000);
                pSet->m_CorePath = m_CopiesDir / FileOf(pSet->m_Core);
                if (!std::filesystem::exists(pSet->m_CorePath, Ec)) break;
            }
            pSet->m_Render = std::format(L"LR{:06}.dll", pSet->m_Id % 1000000);
            pSet->m_RenderPath = m_CopiesDir / FileOf(pSet->m_Render);

            if (!PatchedCopy(OriginalCore, pSet->m_CorePath)) { Why = "the core could not be copied"; pSet->m_CorePath.clear(); pSet->m_RenderPath.clear(); return {}; }
            DWORD Error = 0;
            pSet->m_hCore = LoadCopy(pSet->m_CorePath, Error);
            if (!pSet->m_hCore) { Why = std::format("the core copy did not load (error {})", Error); pSet->m_RenderPath.clear(); return {}; }

            const auto OriginalRender = m_OriginalsDir / FileOf(kRenderNameW);
            if (std::filesystem::exists(OriginalRender, Ec))
            {
                if (!PatchedCopy(OriginalRender, pSet->m_RenderPath, kCoreName, pSet->CoreName())) { Why = "the render DLL could not be copied and patched"; pSet->m_RenderPath.clear(); return {}; }
                pSet->m_hRender = LoadCopy(pSet->m_RenderPath, Error);
                if (!pSet->m_hRender) { Why = std::format("the render copy did not load (error {})", Error); return {}; }
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
