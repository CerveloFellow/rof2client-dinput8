/**
 * @file spellbook_unlock.cpp
 * @brief Implementation of SpellbookUnlock mod — hooks IsSpellcaster, GetSpellLevelNeeded,
 *        CanStartMemming, and GetUsableClasses to bypass class restrictions.
 * @date 2026-02-07
 *
 * @copyright Copyright (c) 2026
 */

#include "pch.h"
#include "spellbook_unlock.h"
#include "multiclass_data.h"
#include "../core.h"
#include "../hooks.h"

#include <eqlib/Offsets.h>

#include <cstdint>

// ---------------------------------------------------------------------------
// EQGameBaseAddress — defined in core.cpp (extern "C" linkage)
// ---------------------------------------------------------------------------
extern "C" uintptr_t EQGameBaseAddress;

// ---------------------------------------------------------------------------
// Raw offsets (not in eqlib offsets file — manual ASLR calculation needed)
// ---------------------------------------------------------------------------
#define EQ_Character__IsSpellcaster_x       0x443F50
#define EQ_Character__IsSpellcaster_2_x     0x4288E0
#define EQ_Character__IsSpellcaster_3_x     0x59FB90
#define CSpellBookWnd__CanStartMemming_x    0x75BD40
#define EQ_Item__GetUsableClasses_x         0x7B4CE0

// ---------------------------------------------------------------------------
// Original function typedefs and pointers (thiscall via fastcall trick)
// ---------------------------------------------------------------------------

// IsSpellcaster: int __thiscall()
using IsSpellcaster_t = int (__fastcall*)(void* thisPtr, void* edx);
static IsSpellcaster_t IsSpellcaster_Original  = nullptr;
static IsSpellcaster_t IsSpellcaster2_Original = nullptr;
static IsSpellcaster_t IsSpellcaster3_Original = nullptr;

// GetSpellLevelNeeded: int __thiscall(int spellid)
using GetSpellLevelNeeded_t = int (__fastcall*)(void* thisPtr, void* edx, int spellid);
static GetSpellLevelNeeded_t GetSpellLevelNeeded_Original = nullptr;

// CanStartMemming: int __thiscall(int spellid)
using CanStartMemming_t = int (__fastcall*)(void* thisPtr, void* edx, int spellid);
static CanStartMemming_t CanStartMemming_Original = nullptr;

// GetUsableClasses: int __thiscall(int a1, DWORD a2)
using GetUsableClasses_t = int (__fastcall*)(void* thisPtr, void* edx, int a1, DWORD a2);
static GetUsableClasses_t GetUsableClasses_Original = nullptr;

// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

// IsSpellcaster — return 1 to enable spell gems for all classes
static int __fastcall IsSpellcaster_Detour(void* thisPtr, void* edx)
{
    return 1;
}

// IsSpellcaster_2 — return 1 for spellcaster checks
static int __fastcall IsSpellcaster2_Detour(void* thisPtr, void* edx, int a1, int a2, int a3, int a4)
{
    return 1;
}

// IsSpellcaster_3 — return 1 for spellcaster checks
static int __fastcall IsSpellcaster3_Detour(void* thisPtr, void* edx)
{
    return 1;
}

// GetSpellLevelNeeded — return 1 if classless (all spells usable at level 1)
static int __fastcall GetSpellLevelNeeded_Detour(void* thisPtr, void* edx, int spellid)
{
    if (MulticlassData::HasData())
        return 1;
    return GetSpellLevelNeeded_Original(thisPtr, edx, spellid);
}

// CanStartMemming — always allow spell memorization
static int __fastcall CanStartMemming_Detour(void* thisPtr, void* edx, int spellid)
{
    return 1;
}

// GetUsableClasses — return -1 if classless (bypass equip class restrictions)
static int __fastcall GetUsableClasses_Detour(void* thisPtr, void* edx, int a1, DWORD a2)
{
    if (MulticlassData::IsClassless())
        return -1;
    return GetUsableClasses_Original(thisPtr, edx, a1, a2);
}

// ---------------------------------------------------------------------------
// IMod interface
// ---------------------------------------------------------------------------

const char* SpellbookUnlock::GetName() const
{
    return "SpellbookUnlock";
}

bool SpellbookUnlock::Initialize()
{
    LogFramework("SpellbookUnlock: Initializing...");

    // All addresses need manual ASLR calculation: (raw - 0x400000 + base)
    uintptr_t isSpellcasterAddr = static_cast<uintptr_t>(EQ_Character__IsSpellcaster_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    IsSpellcaster_Original = reinterpret_cast<IsSpellcaster_t>(isSpellcasterAddr);
    LogFramework("SpellbookUnlock: IsSpellcaster = 0x%08X", static_cast<unsigned int>(isSpellcasterAddr));

    uintptr_t isSpellcaster2Addr = static_cast<uintptr_t>(EQ_Character__IsSpellcaster_2_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    IsSpellcaster2_Original = reinterpret_cast<IsSpellcaster_t>(isSpellcaster2Addr);
    LogFramework("SpellbookUnlock: IsSpellcaster_2 = 0x%08X", static_cast<unsigned int>(isSpellcaster2Addr));

    uintptr_t isSpellcaster3Addr = static_cast<uintptr_t>(EQ_Character__IsSpellcaster_3_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    IsSpellcaster3_Original = reinterpret_cast<IsSpellcaster_t>(isSpellcaster3Addr);
    LogFramework("SpellbookUnlock: IsSpellcaster_3 = 0x%08X", static_cast<unsigned int>(isSpellcaster3Addr));

    uintptr_t getSpellLevelAddr = static_cast<uintptr_t>(EQ_Spell__GetSpellLevelNeeded_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    GetSpellLevelNeeded_Original = reinterpret_cast<GetSpellLevelNeeded_t>(getSpellLevelAddr);
    LogFramework("SpellbookUnlock: GetSpellLevelNeeded = 0x%08X", static_cast<unsigned int>(getSpellLevelAddr));

    uintptr_t canStartMemmingAddr = static_cast<uintptr_t>(CSpellBookWnd__CanStartMemming_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    CanStartMemming_Original = reinterpret_cast<CanStartMemming_t>(canStartMemmingAddr);
    LogFramework("SpellbookUnlock: CanStartMemming = 0x%08X", static_cast<unsigned int>(canStartMemmingAddr));

    uintptr_t getUsableClassesAddr = static_cast<uintptr_t>(EQ_Item__GetUsableClasses_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    GetUsableClasses_Original = reinterpret_cast<GetUsableClasses_t>(getUsableClassesAddr);
    LogFramework("SpellbookUnlock: GetUsableClasses = 0x%08X", static_cast<unsigned int>(getUsableClassesAddr));

    // --- Install hooks ---
    Hooks::Install("IsSpellcaster",
        reinterpret_cast<void**>(&IsSpellcaster_Original),
        reinterpret_cast<void*>(&IsSpellcaster_Detour));

    Hooks::Install("IsSpellcaster_2",
        reinterpret_cast<void**>(&IsSpellcaster2_Original),
        reinterpret_cast<void*>(&IsSpellcaster2_Detour));

    Hooks::Install("IsSpellcaster_3",
        reinterpret_cast<void**>(&IsSpellcaster3_Original),
        reinterpret_cast<void*>(&IsSpellcaster3_Detour));

    Hooks::Install("GetSpellLevelNeeded",
        reinterpret_cast<void**>(&GetSpellLevelNeeded_Original),
        reinterpret_cast<void*>(&GetSpellLevelNeeded_Detour));

    Hooks::Install("CanStartMemming",
        reinterpret_cast<void**>(&CanStartMemming_Original),
        reinterpret_cast<void*>(&CanStartMemming_Detour));

    Hooks::Install("GetUsableClasses",
        reinterpret_cast<void**>(&GetUsableClasses_Original),
        reinterpret_cast<void*>(&GetUsableClasses_Detour));

    LogFramework("SpellbookUnlock: Initialized — 6 hooks installed");
    return true;
}

void SpellbookUnlock::Shutdown()
{
    LogFramework("SpellbookUnlock: Shutdown");
}

void SpellbookUnlock::OnPulse()
{
    // No per-frame work needed
}

bool SpellbookUnlock::OnIncomingMessage(uint32_t opcode, const void* buffer, uint32_t size)
{
    // No packet interception needed
    return true;
}
