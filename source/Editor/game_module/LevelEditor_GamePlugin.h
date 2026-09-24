#ifndef XLVL_NEW_LevelEditor_GAME_PLUGIN_H
#define XLVL_NEW_LevelEditor_GAME_PLUGIN_H
#pragma once

// Phase 8 of the xECSV2 type-registration architecture plan: LevelEditor's own host-side half of the
// hot-reloadable "Game.dll" - loading/unloading the plugin and the full destroy-and-recreate-world
// reload sequence (Phase 8A - "full-world reconstruction", the smallest unload-safety surface;
// see the plan's own Phase 8 section for the entity-preserving 8B milestone this deliberately does
// NOT attempt). Mirrors E27_NodeOS's own ReloadPlugin as closely as this project's much simpler
// registration model allows - see xecs_plugin_api.h's own comment for why no virtual node/factory
// interfaces are needed here at all.
//
// Must be included after xecs.h (the umbrella that pulls this in - xlevel_session.h - already has it).
#include <Windows.h>
#include <filesystem>
#include <future>
#include <mutex>
#include "dependencies/xECSV2/src/xecs_plugin_api.h"

// This file is now a thin umbrella - phase 3 of the kit split (same external review as
// source/Editors/LevelEditor/kit/'s own phase 1/2 comments; direct user go-ahead to do
// the whole thing phase by phase). Split along the review's own proposed seam: build/load
// mechanics (does the DLL need rebuilding, how it gets copied/loaded/unloaded) vs. play-session
// orchestration (V1/Vn snapshot, ReloadGameModule + the app's CreateWorld/RestoreWorld, Start/Poll/Stop) - "clarifies the DLL story" per the
// review's own framing. Included here, in the same order they used to appear inline in this file,
// so this remains the one header LevelEditor_Main.cpp includes - no external-facing change.
// Mechanical move only - no behavior change; see each file's own top comment.
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_ProjectScriptConfig.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GamePluginLog.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GamePluginBuild.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GamePluginLoad.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GameModuleReload.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_play_session.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_GameReloadSession.h"
#include "plugins/xlevel.plugin/source/Editor/game_module/LevelEditor_Panel_SystemRegistry.h"

#endif
