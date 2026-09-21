# source/Editor - the Level editor

A level is a list of scenes. This is what a level editor adds to the scene editor (`xscene.plugin`): opening, saving and
closing a level, the Level tree panel, and Play. It compiles into the editor application and knows nothing about the game
module: the build Play waits for is reached through `xlevel::play_gate`.

An editor includes `plugins/xlevel.plugin/source/Editor/xlevel_editor.h` after `xscene_editor.h`. The headers are not
standalone: they need xECSV2, `xeditor`, `xresource_pipeline_v2/source/editor` and the scene editor.

## The context

`xlevel::level_state` is the scene state plus the open level, Play state and where the document stands (saved or not).
`xlevel::level_context` derives from `xscene::scene_context`; its `State()` gives the `level_state`. The editor provides
the context to `xeditor::host` (`provide<xlevel::level_context>` and `provide<xscene::scene_context>`).

The level's undo lives in its `xeditor::session` (`level_host_session`): the document is bound to the context once, and
`TryGateLevelMutation` claims the level and the selected scenes for the session before its first edit.

## Commands

The commands derive from `xlevel::commands::level_command` / `level_query_command`. The editor that builds the command set
must give them a `level_context*` as their database.

| Header | Commands |
|---|---|
| `xlevel_commands_level.h` | `OpenLevel`, `CloseScene`, `AddScene`, `RemoveScene`, `ListLevels`, `ListScenes`, `ListEntities`, `ListFolders`, `AuditComponentUsage` |
| `xlevel_commands_scene_dependency.h` | `AddSceneDependency`, `RemoveSceneDependency` |
| `xlevel_commands_workspace.h` | `Undo`, `Redo`, `Save`, `Close`, `DescribeEntity`, `ListComponentTypes` |
| `xlevel_commands_play_session.h` | `Play`, `Pause`, `Step`, `Stop`, `GetPlayState` |

## Play

`xlevel_play_session.h` is the transport state machine. Play writes the level to disk (V1) and remembers the undo index;
Stop discards the play session's world and reopens V1, and property edits made while playing are replayed on the restored
scene if the person chooses to keep them (`Stop -Keep true|false`).

Before it starts, Play asks the host for an `xlevel::play_gate`: whether a build is running, and a way to start the
recompile-check Play waits for. An editor with no game module provides none and enters Playing at once. Whoever provides a
gate finishes the request with `EnterPlaying` (or `CancelPlayRequest` when the build fails).

## The rest

| Header | What it is |
|---|---|
| `xlevel_context.h` | `level_state`, `level_context` |
| `xlevel_command_context.h` | `level_command`: gives a command `World()`, `State()` and `LevelContext()` |
| `xlevel_ops.h` | opening a level, and the Level tree's icons |
| `xlevel_save.h` | `SaveEverything`: levels, scenes and prefabs to disk |
| `xlevel_document.h` | the level as a `xeditor` document and its long-lived session |
| `xlevel_document_session.h` | dirty tracking, Close, save-before-open and its modal |
| `xlevel_panel_play_transport.h` | the Play / Step / Pause / Stop buttons |
| `xlevel_panel_level_tree.h` | the Level tree panel |
