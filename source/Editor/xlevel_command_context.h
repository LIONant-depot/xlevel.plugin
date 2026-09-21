#ifndef XLEVEL_COMMAND_CONTEXT_H
#define XLEVEL_COMMAND_CONTEXT_H
#pragma once

// The Level editor's commands (levels, Play, the game module, ...) get the editor context through level_command.
// Which pointer a command receives is decided where the command set is built (where the command set is built).
//
// Meant to be included after the contexts are defined, via the kit umbrella (xlevel_editor.h).
#include "dependencies/xundo/source/xundo_system.h"
#include "dependencies/xeditor/include/xeditor/commands.h"
#include "dependencies/xeditor/include/xeditor/serialize.h"

namespace xlevel::commands
{
    // The same for the commands of the Level editor: World(), State() (with the Level fields), LevelContext().
    template<typename T_BASE>
    struct level_command_mixin : T_BASE
    {
        using T_BASE::T_BASE;
        xecs::game_mgr::instance& World() noexcept { return this->template get<level_context>().World(); }
        level_state&             State() noexcept { return this->template get<level_context>().State(); }
        level_context&      LevelContext() noexcept { return this->template get<level_context>(); }
    };
    using level_command       = level_command_mixin<xundo::command_base>;
    using level_query_command = level_command_mixin<xundo::query_command_base>;
}

#endif // XLEVEL_COMMAND_CONTEXT_H
