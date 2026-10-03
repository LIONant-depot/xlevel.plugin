#ifndef XLEVEL_PANEL_LEVEL_PROPERTIES_H
#define XLEVEL_PANEL_LEVEL_PROPERTIES_H
#pragma once

// The Level, selected in the Level Tree: the Inspector tab shows the Level's own descriptor (xecs::level::descriptor: its Scenes and the Game it runs under) in the xproperty inspector, the
// same way every descriptor is shown, so the Game is the resource picker and its help is the member's help. An edit the inspector makes is turned into a command (SetLevelGame, on the Level's
// own undo) and the descriptor is read again from disk, so what is shown is always what is saved: a Game that is refused simply shows the old one.
//
// Under it, read-only and from files: what the Game lists and what each scene needs from it (the same text DescribeLevel answers).
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_ComponentCompatibility.h"
#include "dependencies/xeditor/include/xeditor/hint.h"
#include "source/Tools/Editor/xeditor_inspector.h"

namespace xlevel
{
    // What the Inspector shows besides the descriptor: derived from files, never edited.
    struct level_properties_view
    {
        struct scene_row
        {
            std::string                                    m_Name;
            std::vector<std::pair<std::string, bool>>      m_Needs;         // the modules the scene needs, and whether the Game lists each
        };

        std::uint64_t              m_Level = 0;
        std::string                m_Name;
        level_game_status          m_Status;
        std::vector<scene_row>     m_Scenes;
        std::vector<std::string>   m_GameModules;   // the modules the Level's Game lists
    };

    inline level_properties_view BuildLevelView( const level_state& State ) noexcept
    {
        level_properties_view V;
        V.m_Level = State.m_CurrentLevel.m_Instance.m_Value;
        const auto LevelNames = commands::BuildAssetNameMap(xecs::level::type_guid_v);
        if (const auto It = LevelNames.find(V.m_Level); It != LevelNames.end()) V.m_Name = It->second;

        std::vector<std::uint64_t> Scenes;
        for (const auto& S : State.m_OpenScenes) Scenes.push_back(S.m_Instance.m_Value);
        V.m_Status = StatusOfLevelGame(V.m_Level, Scenes);

        const auto Modules    = commands::BuildAssetNameMap(xscript::module::type_guid_v);
        const auto SceneNames = commands::BuildAssetNameMap(xecs::scene::type_guid_v);
        auto ModuleName = [&](std::uint64_t M) { const auto It = Modules.find(M); return It == Modules.end() ? std::format("{:X}", M) : It->second; };

        const auto Game = V.m_Status.m_bNamed ? ReadGame(V.m_Status.m_Game) : project_game{};
        for (const auto& M : Game.m_Modules) V.m_GameModules.push_back(ModuleName(M.m_Instance.m_Value));

        const std::wstring Project = ProjectRoot().wstring();
        for (const auto Scene : Scenes)
        {
            level_properties_view::scene_row Row;
            const auto It = SceneNames.find(Scene);
            Row.m_Name = It == SceneNames.end() ? std::format("{:016X}", Scene) : It->second;
            scene_module_needs Needs;
            AddSceneModuleNeeds(Needs, Project, Scene, /*bTransitive*/ false);
            for (const auto& [Module, Need] : Needs.m_Modules)
            {
                const bool bHas = std::any_of(Game.m_Modules.begin(), Game.m_Modules.end(), [&](const xscript::module::module_ref& M) { return M.m_Instance.m_Value == Module; });
                Row.m_Needs.emplace_back(ModuleName(Module), bHas);
            }
            V.m_Scenes.push_back(std::move(Row));
        }
        return V;
    }

    // The same, as text: what DescribeLevel answers.
    inline std::string DescribeLevelView( const level_properties_view& V ) noexcept
    {
        std::string Out = std::format("DescribeLevel: ok\nLevel={:016X}\nName={}\nGame={}\nGameSource={}\n", V.m_Level, V.m_Name
            , V.m_Status.m_Game ? xresource_editor::commands::FormatAssetGuid(xresource::full_guid{ xresource::instance_guid{ V.m_Status.m_Game }, xgame::type_guid_v }) : std::string("(none)")
            , V.m_Status.m_bNamed ? "set" : "none");
        Out += std::format("GameName={}\nIssue={}\nGameModules={}\n", V.m_Status.m_Name, V.m_Status.m_Issue.empty() ? "none" : V.m_Status.m_Issue, V.m_GameModules.size());
        for (const auto& M : V.m_GameModules) Out += std::format("  module {}\n", M);
        Out += std::format("Scenes={}\n", V.m_Scenes.size());
        for (const auto& S : V.m_Scenes)
        {
            Out += std::format("  scene {}", S.m_Name);
            for (const auto& [Module, bHas] : S.m_Needs) Out += std::format("  needs {}{}", Module, bHas ? "" : " (MISSING)");
            Out += "\n";
        }
        return Out;
    }

    // The Inspector's content while the Level is selected: the Level's descriptor in the xproperty inspector, and the derived view under it.
    struct level_properties_panel
    {
        xeditor::inspector_panel                    m_Panel{ "LevelProperties" };
        std::unique_ptr<xecs::level::descriptor>    m_pDescriptor;          // read from the Level's Descriptor.txt; the inspector edits this copy and the edit is turned into a command
        level_properties_view                       m_View;
        level_context*                              m_pEd       = nullptr;
        std::uint64_t                               m_Bound     = 0;        // the Level the inspector is bound to, and the Game it was bound with: a different one on disk rebinds
        std::uint64_t                               m_BoundGame = 0;        // the Game the picker showed (0: none)
        bool                                        m_bRebind   = true;
        bool                                        m_bWired    = false;
        double                                      m_NextRefresh = 0.0;

        // The inspector has already applied the edit to the copy. Only the Game is editable: it becomes SetLevelGame (undoable, refused when the Game lacks what the scenes need), and the
        // descriptor is read again next frame whatever happened, so a refused edit shows the Game that is really there.
        void OnChange( xproperty::inspector&, const xproperty::ui::undo::cmd& ) noexcept
        {
            m_bRebind = true;
            if (!m_pEd || !m_pDescriptor) return;
            const auto Game = m_pDescriptor->m_Game.m_Instance.m_Value;
            if (Game == m_BoundGame) return;                  // not the Game (the rest is read-only): nothing to do, the copy is read again
            xeditor::Run(m_pEd->m_Undo, Game ? std::format("SetLevelGame -Level {:016X} -Game {:016X}{:016X}", m_Bound, Game, xgame::type_guid_v.m_Value)
                                             : std::format("SetLevelGame -Level {:016X}", m_Bound));
        }

        void Bind( const level_state& State ) noexcept
        {
            m_Bound = State.m_CurrentLevel.m_Instance.m_Value;
            m_pDescriptor = std::make_unique<xecs::level::descriptor>();
            xproperty::settings::context Context{};
            const std::wstring File = std::format(L"{}/Descriptors/Level/{:02X}/{:02X}/{:X}.desc/Descriptor.txt", ProjectRoot().wstring(), m_Bound & 0xFF, (m_Bound >> 8) & 0xFF, m_Bound);
            std::error_code Ec;
            if (std::filesystem::exists(File, Ec)) m_pDescriptor->Serialize(true, File, Context);      // a Level that was never saved has none: it shows the empty one
            m_BoundGame = m_pDescriptor->m_Game.m_Instance.m_Value;       // empty: the Level names no Game, and has no scripts, components or systems of any module

            m_Panel.Clear();
            m_Panel.m_Inspector.AppendEntity();
            m_Panel.AppendComponent(*m_pDescriptor->getProperties(), m_pDescriptor.get());
            m_Panel.m_Inspector.m_OnChangeEvent.m_Delegates.clear();
            m_Panel.m_Inspector.m_OnChangeEvent.Register<&level_properties_panel::OnChange>(*this);
            m_bRebind = false;
        }

        void Render( level_context& Ed, const char* pWindowName ) noexcept
        {
            m_pEd = &Ed;
            if (!m_bWired)
            {
                xeditor::BindInspectorHints(m_Panel.m_Inspector);                       // member help is the editors' hint window
                xresource_editor::WireResourcePickerCallbacks(m_Panel.m_Inspector);     // the Game is a resource reference: the picker
                m_bWired = true;
            }

            auto& State = Ed.State();
            const double Now = ImGui::GetTime();
            if (Now >= m_NextRefresh)
            {
                m_NextRefresh = Now + 1.0;
                m_View = BuildLevelView(State);
                // the file changed under us (an undo, a command, another editor): show it
                if (xlevel::ReadLevelGame(ProjectRoot().wstring(), m_Bound) != m_BoundGame) m_bRebind = true;
            }
            if (m_bRebind || m_Bound != State.m_CurrentLevel.m_Instance.m_Value || !m_pDescriptor)
            {
                Bind(State);
                m_View = BuildLevelView(State);
            }

            ImGui::SetNextWindowPos(ImVec2(18, 18), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(480, 500), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(pWindowName))
            {
                ImGui::Text("Level  %s", m_View.m_Name.empty() ? "(unnamed)" : m_View.m_Name.c_str());
                ImGui::TextDisabled("%016llX   %s", static_cast<unsigned long long>(m_View.m_Level), State.isPlaying() ? "Stop to change the Game" : "");

                ImGui::BeginDisabled(State.isPlaying());
                m_Panel.Show();
                ImGui::EndDisabled();

                if (!m_View.m_Status.m_Issue.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.45f, 0.42f, 1.0f));
                    ImGui::TextWrapped("%s", m_View.m_Status.m_Issue.c_str());
                    ImGui::PopStyleColor();
                }
                else if (m_View.m_Status.m_bNamed) ImGui::TextDisabled("Runs under the Game '%s'.", m_View.m_Status.m_Name.c_str());
                else ImGui::TextDisabled("No Game: this Level has no scripts, components or systems of any module. Drag a Game resource onto its Game.");

                ImGui::SeparatorText("Modules of the Game");
                if (m_View.m_GameModules.empty()) ImGui::TextDisabled("The Game lists no script module.");
                for (const auto& M : m_View.m_GameModules) ImGui::BulletText("%s", M.c_str());

                ImGui::SeparatorText("What the scenes need");
                if (m_View.m_Scenes.empty()) ImGui::TextDisabled("No scene is open.");
                for (const auto& S : m_View.m_Scenes)
                {
                    ImGui::BulletText("%s", S.m_Name.c_str());
                    for (const auto& [Module, bHas] : S.m_Needs)
                    {
                        ImGui::SameLine();
                        if (bHas) ImGui::TextDisabled("needs %s", Module.c_str());
                        else ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.42f, 1.0f), "needs %s (the Game does not list it)", Module.c_str());
                    }
                }
            }
            ImGui::End();
        }
    };
}

#endif // XLEVEL_PANEL_LEVEL_PROPERTIES_H
