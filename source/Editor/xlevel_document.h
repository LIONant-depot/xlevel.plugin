#ifndef XLEVEL_LEVEL_DOCUMENT_H
#define XLEVEL_LEVEL_DOCUMENT_H
#pragma once

// Level resource as xeditor::IDocument. Session (not the document) owns undo.
// Guid/display name track State.m_CurrentLevel; Save wires to SaveEverything.

#include "dependencies/xeditor/include/xeditor/types.h"
#include "dependencies/xeditor/include/xeditor/registry.h"
#include "dependencies/xeditor/include/xeditor/session.h"
#include "dependencies/xeditor/include/xeditor/host.h"

namespace xlevel
{


    // Forward (defined below with Ensure/IsLevelWritable).
    inline void ReleaseLevelEditAccess(xeditor::host& Host, xeditor::session& Sess, level_state& State) noexcept;

    struct LevelDocument final : xeditor::IDocument
    {
        level_context* m_pEd = nullptr;    // the editor this document belongs to; its undo holds the dirty watermark

        void Bind(level_context& Ed) noexcept { m_pEd = &Ed; }

        xresource::full_guid CurrentGuid() const noexcept
        {
            if (!m_pEd) return {};
            if (m_pEd->State().isPrefabEditor()) return PrefabResourceGuid(m_pEd->State());         // a Prefab Editor's document is the prefab
            if (m_pEd->State().m_CurrentLevel.empty()) return {};
            return xresource::full_guid{ m_pEd->State().m_CurrentLevel.m_Instance, xecs::level::type_guid_v };
        }

        xresource::full_guid getGuid() const noexcept override { return CurrentGuid(); }

        std::string getDisplayName() const noexcept override
        {
            if (!m_pEd || !m_pEd->State().HasDocument()) return {};
            std::string Name;
            xresource_editor::RemapGUIDToString(Name, CurrentGuid());
            return Name.empty() ? std::string(m_pEd->State().isPrefabEditor() ? "Prefab" : "Level") : Name;
        }

        bool Load() noexcept override
        {
            return m_pEd && m_pEd->State().HasDocument();
        }

        std::string Save() noexcept override
        {
            if (!m_pEd) return "LevelDocument: not bound";
            auto& State = m_pEd->State();
            if (State.m_CurrentLevel.empty() && State.m_OpenScenes.empty())
                return "LevelDocument: nothing open";
            if (!SaveEverything(m_pEd->World(), State) && State.isPrefabEditor()) return "the prefab was not saved (it breaks a rule of a prefab: one root, references inside)";
            MarkDocumentClean(State, m_pEd->m_Undo);
            // Save restores just-loaded: drop write locks so peers can edit again.
            if (auto* pHost = xeditor::host::current())
            {
                for (auto& S : pHost->m_Sessions)
                {
                    if (!S || S->m_Document.get() != this) continue;
                    ReleaseLevelEditAccess(*pHost, *S, State);
                    break;
                }
            }
            return {};
        }

        bool isDirty() const noexcept override
        {
            return m_pEd && HasUnsavedDocumentChanges(m_pEd->State(), m_pEd->m_Undo);
        }
    };


    // Edit vs view (DESIGN 4.2):
    // - Clean / just-loaded: no locks; every session can view and may start editing.
    // - First mutation claims Level + selected scene(s) (no dialog); other open scenes stay free.
    // - Successful save (clean undo): release locks - back to just-loaded.
    // - Sync checks every frame: release when clean; acquire only via mutation gate.

    // True if this Level session may mutate (no other writer holds Level/scene locks).
    inline bool IsLevelWritable(xeditor::host* pHost, xeditor::session* pSess, const level_state& State) noexcept
    {
        if (pHost == nullptr || pSess == nullptr) return true;
        if (!State.m_CurrentLevel.empty())
        {
            const xresource::full_guid LevelGuid{ State.m_CurrentLevel.m_Instance, xecs::level::type_guid_v };
            if (!pHost->can_write(LevelGuid, pSess)) return false;
        }
        for (const auto& SceneInst : State.m_OpenScenes)
        {
            const xresource::full_guid SceneGuid{ SceneInst.m_Instance, xecs::scene::type_guid_v };
            if (!pHost->can_write(SceneGuid, pSess)) return false;
        }
        return true;
    }

    // Claim Level + scenes the user is actually editing (selection), not every open scene.
    inline bool EnsureLevelEditAccess(xeditor::host& Host, xeditor::session& Sess, level_state& State) noexcept
    {
        if (!State.m_CurrentLevel.empty())
        {
            const xresource::full_guid LevelGuid{ State.m_CurrentLevel.m_Instance, xecs::level::type_guid_v };
            if (!Host.try_acquire_write(LevelGuid, &Sess))
                return false;
        }
        auto TryScene = [&](const xecs::scene::guid& SceneInst) noexcept -> bool
        {
            if (SceneInst.empty()) return true;
            const xresource::full_guid SceneGuid{ SceneInst.m_Instance, xecs::scene::type_guid_v };
            return Host.try_acquire_write(SceneGuid, &Sess);
        };
        if (!TryScene(State.m_SelectedEntityScene)) return false;
        if (!TryScene(State.m_MultiSelectScene)) return false;
        return true;
    }

    inline void ReleaseLevelEditAccess(xeditor::host& Host, xeditor::session& Sess, level_state& State) noexcept
    {
        if (!State.m_CurrentLevel.empty())
        {
            const xresource::full_guid LevelGuid{ State.m_CurrentLevel.m_Instance, xecs::level::type_guid_v };
            Host.release_write(LevelGuid, &Sess);
        }
        for (const auto& SceneInst : State.m_OpenScenes)
        {
            const xresource::full_guid SceneGuid{ SceneInst.m_Instance, xecs::scene::type_guid_v };
            Host.release_write(SceneGuid, &Sess);
        }
    }

    inline void RegisterLevelEditorDescriptor() noexcept
    {
        xeditor::editor_descriptor Desc;
        Desc.m_TypeGuid          = xecs::level::type_guid_v;
        Desc.m_TypeName          = "Level";
        Desc.m_bSupportsHeadless = true;
        Desc.m_CreateDocument    = [](xresource::full_guid) -> std::unique_ptr<xeditor::IDocument>
        {
            return std::make_unique<LevelDocument>();
        };
        xeditor::registry::Get().Register(std::move(Desc));
    }

    // Long-lived Level session: owned here when closed; in Host.m_Sessions while a Level is open. The session object
    // never moves (only its owning pointer does), so its undo is a stable address for the editor context to refer to.
    struct level_host_session
    {
        std::unique_ptr<xeditor::session> Owned;
        xeditor::session*                 pLive = nullptr; // Owned.get() or entry inside Host
        bool                              bInHost = false;

        level_host_session() noexcept
        {
            Owned = std::make_unique<xeditor::session>();
            Owned->m_Document = std::make_unique<LevelDocument>();
            if (auto Err = Owned->m_Undo.Init({}, false); !Err.empty())
                xeditor::NotifyModal(std::format("Level session xundo Init failed: {}", Err));
            pLive = Owned.get();
        }

        xundo::system& Undo() noexcept { return pLive->m_Undo; }

        void Bind(level_context& Ed) noexcept { static_cast<LevelDocument*>(pLive->m_Document.get())->Bind(Ed); }

        void Sync(xeditor::host& Host, level_state& State) noexcept
        {
            const bool bWant = State.HasDocument();

            if (bWant && !bInHost)
            {
                Host.m_Sessions.push_back(std::move(Owned));
                pLive   = Host.m_Sessions.back().get();
                bInHost = true;
            }
            else if (!bWant && bInHost)
            {
                if (pLive) ReleaseLevelEditAccess(Host, *pLive, State);
                for (auto It = Host.m_Sessions.begin(); It != Host.m_Sessions.end(); ++It)
                {
                    if (It->get() != pLive) continue;
                    Owned   = std::move(*It);
                    Host.m_Sessions.erase(It);
                    pLive   = Owned.get();
                    bInHost = false;
                    break;
                }
            }
        
            // Every frame (DESIGN 4.2): clean => unlocked (same as just-loaded). Dirty locks
            // are claimed only by TryGateLevelMutation on first edit - never by Sync.
            if (bWant && pLive && !HasUnsavedDocumentChanges(State, pLive->m_Undo))
            {
                ReleaseLevelEditAccess(Host, *pLive, State);
            }
        }
    };

    // The host runs this before every edit, from the UI or the CLI (DESIGN 4.2). The first editor to edit a Scene takes its
    // write lock; an edit of a Scene another editor holds is refused. The Scene is the command's -Scene argument; an edit
    // with none (Level membership, ...) locks the Level itself. Selecting is not editing: it never locks anything.
    inline bool TryGateLevelMutation(xundo::system& System, std::string_view Cmd) noexcept
    {
        auto* pHost = xeditor::host::current();
        level_context* pEd = nullptr;
        for (auto* p : g_LevelContexts) if (&System == &p->m_Undo) { pEd = p; break; }
        if (pHost == nullptr || pEd == nullptr) return true;
        auto* pMe = FindHostSession(System);
        if (pMe == nullptr) return true;

        const auto Name = Cmd.substr(0, Cmd.find(' '));

        // A context scene of a Prefab Editor (a scene brought in to test the prefab against) is never edited or picked: its entities are there to play with, not part of the document.
        if (!pEd->State().m_ContextScenes.empty())
        {
            const auto Tokens = xcmdline::parser::Tokenize(Cmd);
            for (std::size_t i = 0; i + 1 < Tokens.size(); ++i)
            {
                if (Tokens[i].m_bQuoted || Tokens[i].m_Text != "-Scene") continue;
                const auto Scene = xscene::commands::ParseSceneGuid(Tokens[i + 1].m_Text);
                const auto& Contexts = pEd->State().m_ContextScenes;
                if (!Scene.empty() && std::find(Contexts.begin(), Contexts.end(), Scene) != Contexts.end())
                {
                    xeditor::NotifyToast("Read-only: this scene is a context of the Prefab Editor (it plays with the prefab; it is never edited, picked or saved)");
                    return false;
                }
                break;
            }
        }

        if (Name == "Select" || Name == "SelectLevel" || Name == "ToggleMultiSelect" || Name == "ClearSelection") return true;

        // Adding/removing a Scene names it but changes the Level's membership list, not the Scene.
        const bool bLevelMembershipEdit = Name == "AddScene" || Name == "RemoveScene";
        if (!bLevelMembershipEdit)
        {
            // the value of -Scene as the parser reads it (a "-Scene " inside some quoted text is text, not the argument)
            const auto Tokens = xcmdline::parser::Tokenize(Cmd);
            for (std::size_t i = 0; i + 1 < Tokens.size(); ++i)
            {
                if (Tokens[i].m_bQuoted || Tokens[i].m_Text != "-Scene") continue;
                const auto Scene = xscene::commands::ParseSceneGuid(Tokens[i + 1].m_Text);
                if (!Scene.empty()) return pHost->try_acquire_write(SceneResourceGuid(Scene), pMe);
                break;
            }
        }

        auto& State = pEd->State();
        if (State.m_CurrentLevel.empty()) return true;
        return pHost->try_acquire_write(xresource::full_guid{ State.m_CurrentLevel.m_Instance, xecs::level::type_guid_v }, pMe);
    }

    // Undo/Redo change a Scene too, so they pass the same gate: the step about to be undone (or redone) - every command of
    // a grouped step - must not touch a Scene another Level editor owns. False (after telling the person) when it would.
    inline bool MayUndoRedo(xundo::system& Undo, bool bRedo) noexcept
    {
        const int Index = bRedo ? Undo.GetUndoIndex() : Undo.GetUndoIndex() - 1;
        if (Index < 0 || static_cast<std::size_t>(Index) >= Undo.GetHistoryCount()) return true;

        auto Check = [&](const std::string& Cmd) noexcept { return TryGateLevelMutation(Undo, Cmd); };
        bool bOk = true;
        if (const auto nSub = Undo.GetHistorySubCommandCount(Index); nSub > 0)
            for (std::size_t i = 0; i < nSub && bOk; ++i) bOk = Check(Undo.GetHistorySubCommandString(Index, i));
        else
            bOk = Check(Undo.GetHistoryCommandString(Index));

        if (!bOk) xeditor::NotifyToast(bRedo ? "Redo refused: it changes a Scene another Level is editing" : "Undo refused: it changes a Scene another Level is editing");
        return bOk;
    }
}

#endif // XLEVEL_LEVEL_DOCUMENT_H
