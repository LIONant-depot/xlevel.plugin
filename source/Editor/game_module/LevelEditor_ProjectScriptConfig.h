#ifndef XLVL_NEW_LevelEditor_PROJECT_SCRIPT_CONFIG_H
#define XLVL_NEW_LevelEditor_PROJECT_SCRIPT_CONFIG_H
#pragma once

// Project-level Scripting settings - {ProjectPath}\Project.config\Script.config.txt, same fixed settings-file convention as
// Library.config.txt/SystemOrder.config.txt (plain xtextfile + xproperty::sprop::serializer::Stream against an ordinary
// XPROPERTY_DEF'd struct, no descriptor/factory/resource-pipeline visibility).
//
// The project's code is a Game resource (xgame.plugin): the list of its script modules lives in that resource, and the resource pipeline
// makes the CMake project of the game from it. This file only says WHICH Game resource the project builds. The struct and its load/save
// live with the Game (xgame_descriptor.h): the compiler of the Game reads the file too, to know whether it is compiling the project's Game.
#include "plugins/xgame.plugin/source/Module/xgame_descriptor.h"

namespace xlevel
{
    using script_config = xgame::script_config;
    using xgame::SaveScriptConfig;
    using xgame::LoadScriptConfig;

    inline script_config g_ScriptConfig; // one per process - one instance per process
}

#endif // XLVL_NEW_LevelEditor_PROJECT_SCRIPT_CONFIG_H
