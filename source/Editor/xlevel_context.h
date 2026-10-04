#pragma once
#include "dependencies/xLIONCore/src/game/xlioncore_game.h"
#include <filesystem>
#include <functional>
#include <string>

// level_state: the scene state plus what is specific to a Level editor: which Level is open, whether it is playing, and
// where its document stands (saved or not).
namespace xlevel
{
    struct level_state : xscene::scene_state
    {
        xecs::level::guid   m_CurrentLevel  = {};

        std::string m_TreeSearchString;

        // The Scene rows of the Level tree that are expanded right now (kept by the tree as it draws): the first Scene of a Level
        // opens by itself when the Level does, and a person can collapse it. ListScenes -Tree true reports it.
        std::vector<xecs::scene::guid> m_TreeExpandedScenes;
        bool                           m_bExpandFirstScene = false; // set by every load of a Level (xlevel::OpenLevel): the tree expands the first Scene once, when it is open

        // Scenes another Level editor just saved while this one has them open (read-only here, since the other one was
        // editing them): reloaded from disk at the next clean point of the frame, so this editor never works on a stale copy.
        std::vector<xecs::scene::guid> m_ScenesToReload;

        // Level Tree inline rename (Windows Explorer style): the entity being renamed and its edit text.
        xecs::scene::guid           m_RenameScene    = {};
        xecs::scene::permanent_id   m_RenameId       = xecs::scene::invalid_permanent_id_v;
        std::array<char, 256>       m_RenameText     = {};
        bool                        m_bRenameFocus   = false;      // focus the edit box on its first frame
        bool                        m_bRenameRequested = false;    // the Rename action (F2) asks the tree to start renaming the selected entity; the tree row takes it the same frame

        // Stopped: editing normally, the world is never ticked. Playing: ticking every frame. Paused: a live play session
        // (the world stays exactly as it is and Stop still reverts it) that is not ticked this frame, which is also what lets
        // a code-edit reload happen "at rest" mid-session (PollGameReload treats Playing and Paused alike).
        enum class play_state : std::uint8_t { Stopped, Playing, Paused };

        // Also what the System Registry panel checks to tell a permanent, persisted edit from a transient one that Stop
        // will discard.
        play_state m_PlayState = play_state::Stopped;
        bool isPlaying() const noexcept { return m_PlayState != play_state::Stopped; }

        // Set by the toolbar's Step button and consumed at the tick gate that runs the world, whichever way Playing was
        // reached: it waits for the next real tick. From Stopped, Step starts play and that first tick immediately
        // re-pauses; from Paused, the gate runs ONE tick without leaving Paused (this flag alone authorizes a tick then).
        bool m_bStepOneFrame = false;

        // Why this Level cannot be played (its Game does not list the modules its scenes need), "" when it can: read from files about once a second by the session, for the Play button.
        // RequestPlay asks the play_gate itself, so a Play that comes before the next refresh is still refused.
        std::string m_WhyNotPlay;

        bool m_bLevelEditorOpen = true;   // peer Level root tab (Texture-shaped close)
        // Set by Play (Stopped -> Playing only; Paused -> Playing is a plain resume) and consumed by PollGameReload once the
        // recompile-check it starts resolves: Play must never tick against a DLL that might still be rebuilding.
        bool m_bPlayRequested = false;

        // Set by Stop and consumed at the same clean top-of-frame point as PollGameReload, never at the click itself:
        // stopping destroys and recreates the world, and the click happens inside an active ImGui menu-bar scope.
        bool m_bStopRequested = false;

        // Set by EnterPlaying once V1 is saved, consumed at that same top-of-frame point: Play rebuilds the world from V1
        // with builder systems on (doc/xecs_builder_components.md), exactly as the game would load it, then starts Playing.
        bool m_bPlayWorldRebuildRequested = false;

        // The undo index at the moment Play starts. Property edits made while Playing are carried back into the persistent
        // scene on Stop (see RequestStop): it replays every SetProperty pushed since this index against the restored scene
        // and discards the rest of the play session's history.
        int m_PlayHistoryBoundary = 0;

        // The SetProperty commands Stop will replay if the answer to "keep these Play-mode tweaks?" is yes: filled by
        // RequestStop right away (an explicit -Keep, or the modal's button) or left pending for the modal, and cleared once
        // the deferred Stop consumes them.
        std::vector<std::string> m_PendingKeepTweaksCommands;

        // True while "keep them?" is open and the real Stop is on hold, with the world frozen (Paused) so nothing else can
        // happen mid-question. Never set when there is nothing to ask about.
        bool m_bAwaitingKeepTweaksAnswer = false;

        // The undo index at the last Save, successful Level open or Close. File>Save greys out when equal; Close and
        // opening another Level prompt when unequal (HasUnsavedDocumentChanges).
        int m_CleanUndoIndex = 0;

        // The save-before-Close / save-before-OpenLevel modal (RenderSaveBeforeCloseModal).
        bool                 m_bAwaitingSaveBeforeClose = false;
        // Empty = Close only; otherwise open this Level after Save / Don't Save.
        xresource::full_guid m_PendingOpenLevelAfterClose = {};
        // The intent while that modal is open. NOT the one-shot consume flag below, so the editor cannot start a game reload
        // before the person has answered.
        bool                 m_bPendingOpenWantsGameReload = false;
        // One-shot: set by FinishPendingDocumentAction after a Level really opened; the editor frame consumes it.
        bool                 m_bPendingStartGameReloadAfterOpen = false;
    };

    struct game_plugin_state;

    // A Level editor's context: the scene context plus access to its Level state. The scene code only ever sees the base.
    struct level_context : xscene::scene_context
    {
        level_context(level_state& State, std::unique_ptr<xecs::game_mgr::instance>& pWorld, xundo::system& Undo) noexcept
            : xscene::scene_context{ State, pWorld, Undo } {}

        level_state& State() noexcept { return static_cast<level_state&>(m_State); }

        xlioncore::game* m_pGame = nullptr;   // this editor's game (the game manager and the time), set by its session

        void* m_pToolEditor = nullptr;   // this editor's viewport_tools::editor (the "Edit Collider" tools), set by its session
        game_plugin_state* m_pGamePlugin = nullptr;   // the game module of this editor's Level (its own: every Level loads the one of its Game), set by its session
        std::function<std::string(const xmath::fvec3&, const xmath::fvec3&)> m_PickRay;   // which entity (scene and id) a ray hits, as the render module of this Level picks (what a click in the viewport does)
        std::function<std::string()> m_DescribeTextDraw;            // what the last draw of the Texts of this Level did (the render module says it)
        std::function<std::string(std::uint64_t)> m_DescribeText;   // the layout of the Text of an entity (raw runtime entity value), as the render module of this Level says it, set by its session
        std::function<std::filesystem::path()> m_GameSolution;   // the Visual Studio solution of this Level's game project (empty: not made yet), set by its session
    };

    // Every Level editor that is open right now (one per Level, plus the session that stands in when none is), and the one the
    // user touched last. Commands that are not addressed to one Level by name act on the active one.
    inline std::vector<level_context*> g_LevelContexts;
    inline level_context*              g_pActiveLevelContext = nullptr;

    inline level_context* FindLevelContext() noexcept { return g_pActiveLevelContext; }

    // The game module of the active Level (null when there is none): what the commands that name no Level, and the Play gate, act on. Set with the active context.
    inline game_plugin_state* g_pGamePlugin = nullptr;
    inline void SetActiveLevelContext(level_context* pContext) noexcept
    {
        g_pActiveLevelContext = pContext;
        g_pGamePlugin         = pContext ? pContext->m_pGamePlugin : nullptr;
    }
}
