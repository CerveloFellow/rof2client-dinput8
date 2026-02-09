/**
 * @file commands.h
 * @brief Command registry — allows mods to register custom slash commands.
 * @date 2026-02-08
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

// Command handler — void* pChar is an opaque PlayerClient pointer for now.
using CommandHandler = void(*)(void* pChar, const char* szLine);

namespace Commands
{

// Register a slash command. Leading '/' is optional and will be stripped.
void AddCommand(const char* command, CommandHandler handler);

// Unregister a slash command. Leading '/' is optional and will be stripped.
void RemoveCommand(const char* command);

// Called by InterpretCmd detour. Returns true if command was handled.
bool Dispatch(void* pChar, const char* szFullLine);

// Clear the registry (called during Core::Shutdown).
void Shutdown();

} // namespace Commands
