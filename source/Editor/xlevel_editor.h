#ifndef XLEVEL_EDITOR_H
#define XLEVEL_EDITOR_H
#pragma once

// The Level editor: a level is a list of scenes; this adds opening, saving and closing one, the Level tree panel, and Play
// (Play, Pause, Step and Stop with keeping the tweaks made while playing). Built on the scene editor (xscene.plugin). The
// headers are meant to be included in this order in one translation unit.
#include "plugins/xlevel.plugin/source/Editor/xlevel_context.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_command_context.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_ops.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_save.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_document_session.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_document.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_play_session.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_panel_play_transport.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_commands_level.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_commands_scene_dependency.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_commands_workspace.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_commands_play_session.h"
#include "plugins/xlevel.plugin/source/Editor/xlevel_panel_level_tree.h"

#endif // XLEVEL_EDITOR_H
