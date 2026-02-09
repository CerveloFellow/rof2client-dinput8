/**
 * @file commands.cpp
 * @brief Command registry implementation — dispatch table for custom slash commands.
 * @date 2026-02-08
 *
 * @copyright Copyright (c) 2026
 */

#include "pch.h"
#include "commands.h"
#include "core.h"

#include <unordered_map>
#include <string>
#include <algorithm>
#include <cctype>

static std::unordered_map<std::string, CommandHandler> s_commands;

// Strip leading '/' if present and lowercase the name.
static std::string NormalizeCommand(const char* cmd)
{
    const char* p = cmd;
    if (*p == '/')
        ++p;
    std::string name(p);
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

namespace Commands
{

void AddCommand(const char* command, CommandHandler handler)
{
    std::string name = NormalizeCommand(command);
    s_commands[name] = handler;
    LogFramework("Command registered: /%s", name.c_str());
}

void RemoveCommand(const char* command)
{
    std::string name = NormalizeCommand(command);
    s_commands.erase(name);
    LogFramework("Command removed: /%s", name.c_str());
}

bool Dispatch(void* pChar, const char* szFullLine)
{
    if (!szFullLine)
        return false;

    // Skip leading whitespace
    const char* p = szFullLine;
    while (*p == ' ' || *p == '\t')
        ++p;

    if (*p == '\0')
        return false;

    // Extract first token (the command name)
    const char* tokenStart = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
        ++p;

    std::string token(tokenStart, p);

    // Normalize: strip leading '/' and lowercase
    std::string name = NormalizeCommand(token.c_str());

    auto it = s_commands.find(name);
    if (it == s_commands.end())
        return false;

    // Skip whitespace after the command name to get the rest of the line
    while (*p == ' ' || *p == '\t')
        ++p;

    it->second(pChar, p);
    return true;
}

void Shutdown()
{
    s_commands.clear();
    LogFramework("Command registry cleared");
}

} // namespace Commands
