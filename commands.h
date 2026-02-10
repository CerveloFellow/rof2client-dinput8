/**
 * @file commands.h
 * @brief Command registry — allows mods to register custom slash commands.
 * @date 2026-02-08
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace eqlib { class PlayerClient; }

using CommandHandler = void(*)(eqlib::PlayerClient* pChar, const char* szLine);

namespace Commands
{

// Register a slash command. Leading '/' is optional and will be stripped.
void AddCommand(const char* command, CommandHandler handler);

// Unregister a slash command. Leading '/' is optional and will be stripped.
void RemoveCommand(const char* command);

// Called by InterpretCmd detour. Returns true if command was handled.
bool Dispatch(eqlib::PlayerClient* pChar, const char* szFullLine);

// Clear the registry (called during Core::Shutdown).
void Shutdown();

} // namespace Commands
