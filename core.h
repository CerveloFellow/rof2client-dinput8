/**
 * @file core.h
 * @brief Framework core — mod registry, initialization, shutdown, and logging.
 * @date 2026-02-07
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "mods/mod_interface.h"
#include <memory>

// Logging function used by core and hooks modules.
// Writes timestamped lines to dinput8_proxy.log.
void LogFramework(const char* fmt, ...);

namespace Core
{

// Register a mod to be managed by the framework.
// Call before Initialize().
void RegisterMod(std::unique_ptr<IMod> mod);

// Called from the init thread once the game window is ready.
// Initializes all mods, then installs hooks.
void Initialize();

// Called from DLL_PROCESS_DETACH.
// Removes all hooks, then shuts down all mods.
void Shutdown();

// Execute a slash command as if the player typed it. Uses InterpretCmd internally.
void ExecuteCommand(const char* szCommand);

} // namespace Core

// Write a message to the EQ chat window. Falls back to LogFramework if CEverQuest is unavailable.
void WriteChatf(const char* fmt, ...);
void WriteChatColor(const char* line, int color = 273);

// Init thread entry point — polls for game window, then calls Core::Initialize().
DWORD WINAPI InitThread(LPVOID lpParam);
