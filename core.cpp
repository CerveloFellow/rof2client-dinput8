/**
 * @file core.cpp
 * @brief Implementation of the framework core — mod lifecycle, hooks, eqlib glue, and logging.
 * @date 2026-02-07
 *
 * @copyright Copyright (c) 2026
 */

#include "pch.h"
#include "core.h"
#include "hooks.h"
#include "memory.h"
#include "game_state.h"
#include "commands.h"

#include <eqlib/Offsets.h>
#include <eqlib/offsets/eqgame.h>

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <vector>
#include <memory>

// ---------------------------------------------------------------------------
// eqlib extern definitions
//
// eqlib headers declare these via EQLIB_VAR (extern "C") and EQLIB_OBJECT
// but we don't link eqlib.lib, so we provide the storage here.
//
// EQGameBaseAddress: declared as `extern "C" uintptr_t` in namespace eqlib
//   — extern "C" means the symbol has C linkage regardless of namespace
// InitBaseAddress:   declared as plain `void InitBaseAddress()` in namespace eqlib
//   — EQLIB_OBJECT is empty with EQLIB_STATIC, so it's C++ linkage in namespace eqlib
// ---------------------------------------------------------------------------
extern "C" uintptr_t EQGameBaseAddress = 0;

namespace eqlib
{
    void InitBaseAddress()
    {
        EQGameBaseAddress = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    }
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static FILE* g_frameworkLog = nullptr;

static void OpenLog()
{
    if (!g_frameworkLog)
    {
        // Force-delete any stale file from a previous crash, then create fresh
        DeleteFileA("dinput8_proxy.log");
        fopen_s(&g_frameworkLog, "dinput8_proxy.log", "w");
    }
}

void LogFramework(const char* fmt, ...)
{
    OpenLog();
    if (!g_frameworkLog)
        return;

    time_t now = time(nullptr);
    struct tm local;
    localtime_s(&local, &now);
    fprintf(g_frameworkLog, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
        local.tm_hour, local.tm_min, local.tm_sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_frameworkLog, fmt, args);
    va_end(args);

    fprintf(g_frameworkLog, "\n");
    fflush(g_frameworkLog);
}

// ---------------------------------------------------------------------------
// Mod registry
// ---------------------------------------------------------------------------
static std::vector<std::unique_ptr<IMod>> s_mods;
static bool s_initialized = false;

// ---------------------------------------------------------------------------
// Hook addresses and originals
// ---------------------------------------------------------------------------

// ProcessGameEvents — free function: int __cdecl ProcessGameEvents()
using ProcessGameEvents_t = int(__cdecl*)();
static ProcessGameEvents_t ProcessGameEvents_Original = nullptr;

// HandleWorldMessage — member of CEverQuest (thiscall on x86)
// Signature: unsigned char CEverQuest::HandleWorldMessage(UdpConnection*, uint32_t opcode, char* buffer, uint32_t size)
// We use the __fastcall trick: first param = this (ecx), second = edx (unused)
using HandleWorldMessage_t = unsigned char(__fastcall*)(void* thisPtr, void* edx,
    void* connection, uint32_t opcode, char* buffer, uint32_t size);
static HandleWorldMessage_t HandleWorldMessage_Original = nullptr;

// CreatePlayer — PlayerManagerClient::CreatePlayer (thiscall, 8 params)
// Called when a new spawn enters the world. We treat all params as opaque void*.
using CreatePlayer_t = void*(__fastcall*)(void* thisPtr, void* edx,
    void* buf, void* a, void* b, void* c, void* d, void* e, void* f, void* g);
static CreatePlayer_t CreatePlayer_Original = nullptr;

// PrepForDestroyPlayer — PlayerManagerBase::PrepForDestroyPlayer (thiscall, 1 param)
// Called just before a spawn is removed from the world.
using PrepForDestroyPlayer_t = void*(__fastcall*)(void* thisPtr, void* edx, void* spawn);
static PrepForDestroyPlayer_t PrepForDestroyPlayer_Original = nullptr;

// GroundItemAdd — EQGroundItemListManager::Add (thiscall, 1 param)
// Called when a ground item is added to the world.
using GroundItemAdd_t = void(__fastcall*)(void* thisPtr, void* edx, void* pItem);
static GroundItemAdd_t GroundItemAdd_Original = nullptr;

// GroundItemDelete — EQGroundItemListManager::Delete (thiscall, 1 param)
// Called when a ground item is removed from the world.
using GroundItemDelete_t = void(__fastcall*)(void* thisPtr, void* edx, void* pItem);
static GroundItemDelete_t GroundItemDelete_Original = nullptr;

// GroundItemClear — EQGroundItemListManager::Clear (thiscall, no params)
// Called on zone change to remove all ground items.
using GroundItemClear_t = void(__fastcall*)(void* thisPtr, void* edx);
static GroundItemClear_t GroundItemClear_Original = nullptr;

// InterpretCmd — CEverQuest::InterpretCmd (thiscall, 2 params)
// Called when the player enters a slash command.
using InterpretCmd_t = void(__fastcall*)(void* thisPtr, void* edx,
    void* pChar, const char* szFullLine);
static InterpretCmd_t InterpretCmd_Original = nullptr;

// ---------------------------------------------------------------------------
// Detour implementations
// ---------------------------------------------------------------------------

static int __cdecl ProcessGameEvents_Detour()
{
    int result = ProcessGameEvents_Original();

    for (auto& mod : s_mods)
        mod->OnPulse();

    return result;
}

static unsigned char __fastcall HandleWorldMessage_Detour(
    void* thisPtr, void* edx,
    void* connection, uint32_t opcode, char* buffer, uint32_t size)
{
    for (auto& mod : s_mods)
    {
        if (!mod->OnIncomingMessage(opcode, buffer, size))
            return 0;
    }

    return HandleWorldMessage_Original(thisPtr, edx, connection, opcode, buffer, size);
}

static void* __fastcall CreatePlayer_Detour(
    void* thisPtr, void* edx,
    void* buf, void* a, void* b, void* c, void* d, void* e, void* f, void* g)
{
    void* result = CreatePlayer_Original(thisPtr, edx, buf, a, b, c, d, e, f, g);
    if (result)
    {
        for (auto& mod : s_mods)
            mod->OnAddSpawn(result);
    }
    return result;
}

static void* __fastcall PrepForDestroyPlayer_Detour(
    void* thisPtr, void* edx, void* spawn)
{
    for (auto& mod : s_mods)
        mod->OnRemoveSpawn(spawn);

    return PrepForDestroyPlayer_Original(thisPtr, edx, spawn);
}

static void __fastcall GroundItemAdd_Detour(
    void* thisPtr, void* edx, void* pItem)
{
    GroundItemAdd_Original(thisPtr, edx, pItem);

    for (auto& mod : s_mods)
        mod->OnAddGroundItem(pItem);
}

static void __fastcall GroundItemDelete_Detour(
    void* thisPtr, void* edx, void* pItem)
{
    for (auto& mod : s_mods)
        mod->OnRemoveGroundItem(pItem);

    GroundItemDelete_Original(thisPtr, edx, pItem);
}

static void __fastcall GroundItemClear_Detour(
    void* thisPtr, void* edx)
{
    // Walk the linked list before clearing: Top at offset 0x00, pNext at offset 0x04
    void* current = *reinterpret_cast<void**>(thisPtr);
    while (current)
    {
        void* next = *reinterpret_cast<void**>(
            reinterpret_cast<uintptr_t>(current) + 0x04);
        for (auto& mod : s_mods)
            mod->OnRemoveGroundItem(current);
        current = next;
    }

    GroundItemClear_Original(thisPtr, edx);
}

static void __fastcall InterpretCmd_Detour(
    void* thisPtr, void* edx, void* pChar, const char* szFullLine)
{
    if (Commands::Dispatch(pChar, szFullLine))
        return;  // Command handled by a registered handler
    InterpretCmd_Original(thisPtr, edx, pChar, szFullLine);
}

// ---------------------------------------------------------------------------
// Address resolution
// ---------------------------------------------------------------------------

// HandleWorldMessage raw offset (not in eqlib offsets file for ROF2)
#define CEverQuest__HandleWorldMessage_x  0x004C3250

// ---------------------------------------------------------------------------
// Core implementation
// ---------------------------------------------------------------------------

namespace Core
{

void RegisterMod(std::unique_ptr<IMod> mod)
{
    LogFramework("Registered mod: %s", mod->GetName());
    s_mods.push_back(std::move(mod));
}

void Initialize()
{
    if (s_initialized)
        return;
    s_initialized = true;

    LogFramework("=== Framework initializing ===");
    LogFramework("EQGameBaseAddress = 0x%08X", static_cast<unsigned int>(EQGameBaseAddress));

    // Resolve game global pointers (must come after InitBaseAddress)
    GameState::ResolveGlobals();

    // Resolve ProcessGameEvents address using eqlib's FixEQGameOffset
    uintptr_t pgeAddr = eqlib::FixEQGameOffset(__ProcessGameEvents_x);
    ProcessGameEvents_Original = reinterpret_cast<ProcessGameEvents_t>(pgeAddr);
    LogFramework("ProcessGameEvents = 0x%08X", static_cast<unsigned int>(pgeAddr));

    // Resolve HandleWorldMessage address (manual calculation — not in eqlib offsets)
    uintptr_t hwmAddr = static_cast<uintptr_t>(CEverQuest__HandleWorldMessage_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    HandleWorldMessage_Original = reinterpret_cast<HandleWorldMessage_t>(hwmAddr);
    LogFramework("HandleWorldMessage = 0x%08X", static_cast<unsigned int>(hwmAddr));

    // Resolve spawn tracking hooks
    uintptr_t cpAddr = eqlib::FixEQGameOffset(PlayerManagerClient__CreatePlayer_x);
    CreatePlayer_Original = reinterpret_cast<CreatePlayer_t>(cpAddr);
    LogFramework("CreatePlayer = 0x%08X", static_cast<unsigned int>(cpAddr));

    uintptr_t pdpAddr = eqlib::FixEQGameOffset(PlayerManagerBase__PrepForDestroyPlayer_x);
    PrepForDestroyPlayer_Original = reinterpret_cast<PrepForDestroyPlayer_t>(pdpAddr);
    LogFramework("PrepForDestroyPlayer = 0x%08X", static_cast<unsigned int>(pdpAddr));

    // Resolve ground item tracking hooks
    uintptr_t giAddAddr = eqlib::FixEQGameOffset(EQGroundItemListManager__Add_x);
    GroundItemAdd_Original = reinterpret_cast<GroundItemAdd_t>(giAddAddr);
    LogFramework("GroundItemAdd = 0x%08X", static_cast<unsigned int>(giAddAddr));

    uintptr_t giDelAddr = eqlib::FixEQGameOffset(EQGroundItemListManager__Delete_x);
    GroundItemDelete_Original = reinterpret_cast<GroundItemDelete_t>(giDelAddr);
    LogFramework("GroundItemDelete = 0x%08X", static_cast<unsigned int>(giDelAddr));

    uintptr_t giClrAddr = eqlib::FixEQGameOffset(EQGroundItemListManager__Clear_x);
    GroundItemClear_Original = reinterpret_cast<GroundItemClear_t>(giClrAddr);
    LogFramework("GroundItemClear = 0x%08X", static_cast<unsigned int>(giClrAddr));

    // Resolve InterpretCmd address (slash command interpreter)
    uintptr_t icAddr = eqlib::FixEQGameOffset(CEverQuest__InterpretCmd_x);
    InterpretCmd_Original = reinterpret_cast<InterpretCmd_t>(icAddr);
    LogFramework("InterpretCmd = 0x%08X", static_cast<unsigned int>(icAddr));

    // Initialize all mods before installing hooks
    for (auto& mod : s_mods)
    {
        LogFramework("Initializing mod: %s", mod->GetName());
        if (!mod->Initialize())
            LogFramework("  WARNING: mod '%s' failed to initialize", mod->GetName());
    }

    // Install hooks
    Hooks::Install("ProcessGameEvents",
        reinterpret_cast<void**>(&ProcessGameEvents_Original),
        reinterpret_cast<void*>(&ProcessGameEvents_Detour));

    Hooks::Install("HandleWorldMessage",
        reinterpret_cast<void**>(&HandleWorldMessage_Original),
        reinterpret_cast<void*>(&HandleWorldMessage_Detour));

    Hooks::Install("CreatePlayer",
        reinterpret_cast<void**>(&CreatePlayer_Original),
        reinterpret_cast<void*>(&CreatePlayer_Detour));

    Hooks::Install("PrepForDestroyPlayer",
        reinterpret_cast<void**>(&PrepForDestroyPlayer_Original),
        reinterpret_cast<void*>(&PrepForDestroyPlayer_Detour));

    Hooks::Install("GroundItemAdd",
        reinterpret_cast<void**>(&GroundItemAdd_Original),
        reinterpret_cast<void*>(&GroundItemAdd_Detour));

    Hooks::Install("GroundItemDelete",
        reinterpret_cast<void**>(&GroundItemDelete_Original),
        reinterpret_cast<void*>(&GroundItemDelete_Detour));

    Hooks::Install("GroundItemClear",
        reinterpret_cast<void**>(&GroundItemClear_Original),
        reinterpret_cast<void*>(&GroundItemClear_Detour));

    Hooks::Install("InterpretCmd",
        reinterpret_cast<void**>(&InterpretCmd_Original),
        reinterpret_cast<void*>(&InterpretCmd_Detour));

    LogFramework("=== Framework initialized — 8 hooks installed ===");
}

void Shutdown()
{
    if (!s_initialized)
        return;
    s_initialized = false;

    LogFramework("=== Framework shutting down ===");

    // Remove hooks before shutting down mods
    Hooks::RemoveAll();

    // Clear command registry
    Commands::Shutdown();

    // Shutdown all mods
    for (auto& mod : s_mods)
    {
        LogFramework("Shutting down mod: %s", mod->GetName());
        mod->Shutdown();
    }
    s_mods.clear();

    LogFramework("=== Framework shutdown complete ===");
}

} // namespace Core

// ---------------------------------------------------------------------------
// Init thread — waits for game window, then initializes the framework
// ---------------------------------------------------------------------------
DWORD WINAPI InitThread(LPVOID lpParam)
{
    // Resolve base address early — FixEQGameOffset needs EQGameBaseAddress set.
    eqlib::InitBaseAddress();

    // Poll until the game window handle is valid.
    // __HWnd_x (0xE678A0) is a fixed offset holding the HWND.
    // We resolve it with ASLR and read the pointer from game memory.
    uintptr_t hwndAddr = eqlib::FixEQGameOffset(__HWnd_x);

    LogFramework("Init thread started — waiting for game window (HWnd @ 0x%08X)",
        static_cast<unsigned int>(hwndAddr));

    while (Memory::ReadMemory<uintptr_t>(hwndAddr) == 0)
    {
        Sleep(100);
    }

    LogFramework("Game window detected — beginning initialization");
    Core::Initialize();

    return 0;
}
