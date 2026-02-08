/**
 * @file labels.cpp
 * @brief Implementation of the LabelsOverride mod — hooks label, gauge, and stat
 *        functions to display server-authoritative values from EdgeStat packets.
 * @date 2026-02-07
 *
 * @copyright Copyright (c) 2026
 */

#include "pch.h"
#include "labels.h"
#include "multiclass_data.h"
#include "../core.h"
#include "../hooks.h"
#include "../memory.h"

#include <eqlib/Offsets.h>
#include <eqlib/offsets/eqgame.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <climits>
#include <unordered_map>
#include <functional>

// ---------------------------------------------------------------------------
// CXStr / CStrRep memory layout (headers-only — no eqlib.lib)
//
// CXStr (4 bytes): CStrRep* m_data
// CStrRep layout:
//   +0x00  int32_t  refCount
//   +0x04  uint32_t alloc (buffer capacity)
//   +0x08  uint32_t length
//   +0x0c  int32_t  encoding (0=UTF8)
//   +0x10  void*    freeList
//   +0x14  char     utf8[] (string data)
// ---------------------------------------------------------------------------

struct CStrRep
{
    int32_t  refCount;
    uint32_t alloc;
    uint32_t length;
    int32_t  encoding;
    void*    freeList;
    char     utf8[1]; // variable-length
};

static void OverwriteCXStr(void* cxstr, const char* text)
{
    if (!cxstr || !text)
        return;

    // CXStr is a pointer-to-CStrRep
    CStrRep** repPtr = reinterpret_cast<CStrRep**>(cxstr);
    CStrRep* rep = *repPtr;
    if (!rep)
        return;

    uint32_t newLen = static_cast<uint32_t>(strlen(text));

    if (rep->refCount == 1 && rep->alloc > newLen)
    {
        // Fast path: sole owner with enough buffer — overwrite in-place
        memcpy(rep->utf8, text, newLen + 1);
        rep->length = newLen;
    }
    else
    {
        // Shared or too small — allocate a new CStrRep on the process heap
        uint32_t newAlloc = newLen + 64;
        uint32_t allocSize = 0x14 + newAlloc;
        CStrRep* newRep = static_cast<CStrRep*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocSize));
        if (newRep)
        {
            newRep->refCount = 1;
            newRep->alloc    = newAlloc;
            newRep->length   = newLen;
            newRep->encoding = 0;
            newRep->freeList = nullptr;
            memcpy(newRep->utf8, text, newLen + 1);

            if (rep->refCount > 0)
                rep->refCount--;

            *repPtr = newRep;
        }
    }
}

// ---------------------------------------------------------------------------
// Class title lookup table
// ---------------------------------------------------------------------------

// Class IDs match eqlib PlayerClass enum: Warrior=1 .. Berserker=16
// Level tiers: 1-50, 51-54, 55-59, 60-64, 65+
static const char* s_classTitles[16][5] =
{
    // Warrior (1)
    { "Warrior",      "Champion",    "Myrmidon",    "Warlord",       "Overlord" },
    // Cleric (2)
    { "Cleric",       "Vicar",       "Templar",     "High Priest",   "Archon" },
    // Paladin (3)
    { "Paladin",      "Cavalier",    "Knight",      "Crusader",      "Lord Protector" },
    // Ranger (4)
    { "Ranger",       "Pathfinder",  "Outrider",    "Warder",        "Forest Stalker" },
    // Shadowknight (5)
    { "Shadowknight", "Reaver",      "Revenant",    "Grave Lord",    "Dread Lord" },
    // Druid (6)
    { "Druid",        "Wanderer",    "Preserver",   "Hierophant",    "Storm Warden" },
    // Monk (7)
    { "Monk",         "Disciple",    "Master",      "Grandmaster",   "Transcendent" },
    // Bard (8)
    { "Bard",         "Minstrel",    "Troubador",   "Virtuoso",      "Maestro" },
    // Rogue (9)
    { "Rogue",        "Rake",        "Blackguard",  "Assassin",      "Deceiver" },
    // Shaman (10)
    { "Shaman",       "Mystic",      "Luminary",    "Oracle",        "Prophet" },
    // Necromancer (11)
    { "Necromancer",  "Heretic",     "Defiler",     "Warlock",       "Arch Lich" },
    // Wizard (12)
    { "Wizard",       "Channeler",   "Evoker",      "Sorcerer",      "Arcanist" },
    // Magician (13)
    { "Magician",     "Elementalist","Conjurer",    "Arch Mage",     "Arch Convoker" },
    // Enchanter (14)
    { "Enchanter",    "Illusionist", "Beguiler",    "Phantasmist",   "Coercer" },
    // Beastlord (15)
    { "Beastlord",    "Primalist",   "Animist",     "Savage Lord",   "Feral Lord" },
    // Berserker (16)
    { "Berserker",    "Brawler",     "Vehement",    "Rager",         "Fury" },
};

const char* GetClassTitle(int classId, int level)
{
    if (classId < 1 || classId > 16)
        return "Unknown";

    int tier;
    if (level <= 50)      tier = 0;
    else if (level <= 54) tier = 1;
    else if (level <= 59) tier = 2;
    else if (level <= 64) tier = 3;
    else                  tier = 4;

    return s_classTitles[classId - 1][tier];
}

// ---------------------------------------------------------------------------
// Forward declarations needed by FormatClassLine (defined later with other statics)
// ---------------------------------------------------------------------------
static uintptr_t s_localPlayerPtrAddr = 0; // set in Initialize()
constexpr uint32_t OFF_Level         = 0x250;  // uint8_t — PlayerZoneClient
constexpr uint32_t OFF_ActorClass    = 0x0fe0; // int — mActorClient(0xea4).Class(0x13c)

// ---------------------------------------------------------------------------
// Format function type — writes formatted text into buf, returns false to
// let the original label value stand.
// ---------------------------------------------------------------------------
using FormatFunc = std::function<bool(char* buf, size_t bufSize)>;

static std::unordered_map<int, FormatFunc> s_labelMap;

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

static bool FormatStatInt(char* buf, size_t sz, eStatEntry stat)
{
    if (!MulticlassData::HasStat(stat)) return false;
    snprintf(buf, sz, "%lld", static_cast<long long>(MulticlassData::GetStat(stat)));
    return true;
}

static bool FormatDual(char* buf, size_t sz, eStatEntry cur, eStatEntry max)
{
    if (!MulticlassData::HasStat(cur) || !MulticlassData::HasStat(max)) return false;
    snprintf(buf, sz, "%lld / %lld",
        static_cast<long long>(MulticlassData::GetStat(cur)),
        static_cast<long long>(MulticlassData::GetStat(max)));
    return true;
}

static bool FormatWeight(char* buf, size_t sz, eStatEntry stat)
{
    if (!MulticlassData::HasStat(stat)) return false;
    double val = static_cast<double>(MulticlassData::GetStat(stat)) / 10.0;
    snprintf(buf, sz, "%.1f", val);
    return true;
}

static bool FormatSpeed(char* buf, size_t sz, eStatEntry stat)
{
    if (!MulticlassData::HasStat(stat)) return false;
    double val = static_cast<double>(MulticlassData::GetStat(stat)) / 100000.0;
    snprintf(buf, sz, "%.3f", val);
    return true;
}

static bool FormatDualWeight(char* buf, size_t sz, eStatEntry cur, eStatEntry max)
{
    if (!MulticlassData::HasStat(cur) || !MulticlassData::HasStat(max)) return false;
    double c = static_cast<double>(MulticlassData::GetStat(cur)) / 10.0;
    double m = static_cast<double>(MulticlassData::GetStat(max)) / 10.0;
    snprintf(buf, sz, "%.1f / %.1f", c, m);
    return true;
}

static bool FormatSynergy(char* buf, size_t sz, eStatEntry stat)
{
    if (!MulticlassData::HasStat(stat)) return false;
    if (!MulticlassData::IsClassless())
    {
        buf[0] = '\0';
        return true;
    }
    snprintf(buf, sz, "%lld", static_cast<long long>(MulticlassData::GetStat(stat)));
    return true;
}

// classNum: 1, 2, or 3
// Class1 ALWAYS overrides — uses EdgeStat data if available, otherwise falls back
// to the character's level/class from the spawn struct.
// Class2/3 show blank " " when ClassCount < 2/3.
static bool FormatClassLine(char* buf, size_t sz, int classNum)
{
    // One-time diagnostic: dump class-related stat values
    static bool s_diagLogged = false;
    if (!s_diagLogged && MulticlassData::HasData())
    {
        LogFramework("FormatClassLine diag: ClassCount=%d HasClass1=%d Class1=%lld Class1Level=%lld "
            "HasClass2=%d Class2=%lld Class2Level=%lld HasClass3=%d Class3=%lld Class3Level=%lld",
            MulticlassData::GetClassCount(),
            MulticlassData::HasStat(eStatEntry::Class1),
            static_cast<long long>(MulticlassData::GetStat(eStatEntry::Class1)),
            static_cast<long long>(MulticlassData::GetStat(eStatEntry::Class1Level)),
            MulticlassData::HasStat(eStatEntry::Class2),
            static_cast<long long>(MulticlassData::GetStat(eStatEntry::Class2)),
            static_cast<long long>(MulticlassData::GetStat(eStatEntry::Class2Level)),
            MulticlassData::HasStat(eStatEntry::Class3),
            static_cast<long long>(MulticlassData::GetStat(eStatEntry::Class3)),
            static_cast<long long>(MulticlassData::GetStat(eStatEntry::Class3Level)));
        s_diagLogged = true;
    }

    // Read local player for fallback data
    uintptr_t pLocalPlayer = Memory::ReadMemory<uintptr_t>(s_localPlayerPtrAddr);

    if (classNum == 1)
    {
        // Class1: always show something. Prefer EdgeStat, fall back to spawn data.
        if (MulticlassData::HasStat(eStatEntry::ClassCount)
            && MulticlassData::GetClassCount() >= 1
            && MulticlassData::HasStat(eStatEntry::Class1))
        {
            int classId = static_cast<int>(MulticlassData::GetStat(eStatEntry::Class1));
            int level   = static_cast<int>(MulticlassData::GetStat(eStatEntry::Class1Level));
            if (classId > 0)
            {
                snprintf(buf, sz, "%d %s", level, GetClassTitle(classId, level));
                return true;
            }
        }

        // Fallback: read from spawn data
        if (pLocalPlayer)
        {
            int classId = Memory::ReadMemory<int>(pLocalPlayer + OFF_ActorClass);
            int level   = static_cast<int>(Memory::ReadMemory<uint8_t>(pLocalPlayer + OFF_Level));
            if (classId > 0 && classId <= 16 && level > 0)
            {
                snprintf(buf, sz, "%d %s", level, GetClassTitle(classId, level));
                return true;
            }
        }
        return false;
    }

    // Class2/3: need EdgeStat class data
    if (!MulticlassData::HasStat(eStatEntry::ClassCount))
    {
        snprintf(buf, sz, " ");
        return true;
    }

    int classCount = MulticlassData::GetClassCount();
    eStatEntry classStat, levelStat;
    int requiredCount;

    switch (classNum)
    {
    case 2:
        classStat = eStatEntry::Class2;
        levelStat = eStatEntry::Class2Level;
        requiredCount = 2;
        break;
    case 3:
        classStat = eStatEntry::Class3;
        levelStat = eStatEntry::Class3Level;
        requiredCount = 3;
        break;
    default:
        return false;
    }

    if (classCount < requiredCount)
    {
        snprintf(buf, sz, " ");
        return true;
    }

    int classId = static_cast<int>(MulticlassData::GetStat(classStat));
    int level   = static_cast<int>(MulticlassData::GetStat(levelStat));

    if (classId == 0)
    {
        snprintf(buf, sz, " ");
        return true;
    }

    snprintf(buf, sz, "%d %s", level, GetClassTitle(classId, level));
    return true;
}

// ---------------------------------------------------------------------------
// Hook typedefs and originals
// ---------------------------------------------------------------------------

// GetLabelFromEQ / GetGaugeValueFromEQ — cdecl free functions
using GetLabelFromEQ_t = int(__cdecl*)(int EQType, void* cxstrOut, bool*, unsigned long*);
static GetLabelFromEQ_t GetLabelFromEQ_Original = nullptr;

using GetGaugeValueFromEQ_t = int(__cdecl*)(int EQType, void* cxstrOut, bool*, unsigned long*);
static GetGaugeValueFromEQ_t GetGaugeValueFromEQ_Original = nullptr;

// CharacterZoneClient stat overrides — thiscall via __fastcall trick
using CurHP_t   = int32_t(__fastcall*)(void* thisPtr, void* edx, int spawn, unsigned char flag);
using CurMana_t = int32_t(__fastcall*)(void* thisPtr, void* edx, int spawn);
using MaxHP_t   = int32_t(__fastcall*)(void* thisPtr, void* edx, int spawn, int flag);
using MaxMana_t = int32_t(__fastcall*)(void* thisPtr, void* edx, int spawn);
using MaxEnd_t  = int32_t(__fastcall*)(void* thisPtr, void* edx, int spawn);

static CurHP_t   CurHP_Original   = nullptr;
static CurMana_t CurMana_Original = nullptr;
static MaxHP_t   MaxHP_Original   = nullptr;
static MaxMana_t MaxMana_Original = nullptr;
static MaxEnd_t  MaxEnd_Original  = nullptr;

// Manual ASLR hooks (not in eqlib offsets or need manual calc)
using CurEnd_t        = int32_t(__fastcall*)(void* thisPtr, void* edx, int spawn);
using CalculateWeight_t = double(__fastcall*)(void* thisPtr, void* edx);

static CurEnd_t         CurEnd_Original         = nullptr;
static CalculateWeight_t CalculateWeight_Original = nullptr;

// RunWalkState resolved address
static uintptr_t s_runWalkStateAddr = 0;

// Inventory window title support
// s_localPlayerPtrAddr declared earlier (needed by FormatClassLine)
static uintptr_t s_wndMgrPtrAddr = 0;     // address of pinstCXWndManager pointer
static uintptr_t s_cachedInvWnd = 0;       // cached CInventoryWnd pointer

// Spawn struct offsets (from eqlib PlayerClient base)
constexpr uint32_t OFF_DisplayedName = 0x0e4;  // char[0x40] — PlayerBase
constexpr uint32_t OFF_Deity         = 0x518;  // int — PlayerZoneClient
// OFF_Level and OFF_ActorClass declared earlier (needed by FormatClassLine)

// CXWnd offsets
constexpr uint32_t OFF_WindowText = 0x1a8; // CXStr
constexpr uint32_t OFF_dShow      = 0x196; // bool — CXWnd::dShow (IsVisible)

// CSidlScreenWnd offsets
constexpr uint32_t OFF_SidlText   = 0x1dc; // CXStr — CSidlScreenWnd::SidlText (SIDL name)

// CXWndManager offsets
// pWindows (ArrayClass<CXWnd*>) at CXWndManager + 0x004
//   ArrayClass layout: +0x00 m_length (int), +0x04 m_array (T*)
constexpr uint32_t OFF_WndMgr_pWindows = 0x004;

static const char* GetDeityName(int deity)
{
    switch (deity)
    {
    case 201: return "Bertoxxulous";
    case 202: return "Brell Serilis";
    case 203: return "Cazic Thule";
    case 204: return "Erollisi Marr";
    case 205: return "Bristlebane";
    case 206: return "Innoruuk";
    case 207: return "Karana";
    case 208: return "Mithaniel Marr";
    case 209: return "Prexus";
    case 210: return "Quellious";
    case 211: return "Rallos Zek";
    case 212: return "Rodcet Nife";
    case 213: return "Solusek Ro";
    case 214: return "The Tribunal";
    case 215: return "Tunare";
    case 216: return "Veeshan";
    case 396: return "Agnostic";
    default:  return "Unknown";
    }
}

// Read the CXStr utf8 buffer at a given CXStr address (in our process).
// Returns nullptr if the CXStr is null/empty.
static const char* ReadCXStrText(uintptr_t cxstrAddr)
{
    uintptr_t repAddr = *reinterpret_cast<uintptr_t*>(cxstrAddr);
    if (!repAddr)
        return nullptr;
    return reinterpret_cast<const char*>(repAddr + 0x14); // CStrRep::utf8
}

// Walk CXWndManager->pWindows to find a window by its SidlText name.
// SidlText is the fixed SIDL (XML) identifier — e.g. "InventoryWindow".
// Not all windows in pWindows are CSidlScreenWnd — plain CXWnd objects don't
// have SidlText at 0x1dc, so we use __try/__except to safely skip them.
static uintptr_t FindWindowBySidlName(const char* sidlName)
{
    uintptr_t pWndMgr = Memory::ReadMemory<uintptr_t>(s_wndMgrPtrAddr);
    if (!pWndMgr)
        return 0;

    // ArrayClass<CXWnd*> at CXWndManager + OFF_WndMgr_pWindows
    uintptr_t arrayBase = pWndMgr + OFF_WndMgr_pWindows;
    int count = *reinterpret_cast<int*>(arrayBase + 0x00);          // m_length
    uintptr_t* arr = *reinterpret_cast<uintptr_t**>(arrayBase + 0x04); // m_array
    if (!arr || count <= 0 || count > 10000)
        return 0;

    for (int i = 0; i < count; ++i)
    {
        uintptr_t wnd = arr[i];
        if (!wnd)
            continue;

        __try
        {
            const char* text = ReadCXStrText(wnd + OFF_SidlText);
            if (text && strcmp(text, sidlName) == 0)
                return wnd;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Not a CSidlScreenWnd or invalid memory — skip
        }
    }

    return 0;
}

static bool s_invWndSearchLogged = false;

static void UpdateInventoryTitle()
{
    __try
    {
        // Must be in-game (pLocalPlayer valid) before searching for windows
        uintptr_t pLocalPlayer = Memory::ReadMemory<uintptr_t>(s_localPlayerPtrAddr);
        if (!pLocalPlayer)
            return;

        // Find and cache the inventory window by its SIDL name
        if (!s_cachedInvWnd)
        {
            s_cachedInvWnd = FindWindowBySidlName("InventoryWindow");
            if (s_cachedInvWnd)
            {
                LogFramework("LabelsOverride: Found InventoryWindow at 0x%08X",
                    static_cast<unsigned int>(s_cachedInvWnd));
            }
            else
            {
                if (!s_invWndSearchLogged)
                {
                    LogFramework("LabelsOverride: InventoryWindow not yet found — will retry each pulse");
                    s_invWndSearchLogged = true;
                }
                return;
            }
        }

        // Read character name and deity
        const char* name = reinterpret_cast<const char*>(pLocalPlayer + OFF_DisplayedName);
        int deity = Memory::ReadMemory<int>(pLocalPlayer + OFF_Deity);
        const char* deityName = GetDeityName(deity);

        // Format title — max 73 chars (tested safe limit for inventory window title)
        constexpr size_t MAX_TITLE_LEN = 73;
        char title[MAX_TITLE_LEN + 1];
        snprintf(title, sizeof(title), "%s - %s", name, deityName);

        uint32_t newLen = static_cast<uint32_t>(strlen(title));

        // Write the title into the CXStr, handling shared (refCount > 1) reps
        CStrRep** repPtr = reinterpret_cast<CStrRep**>(s_cachedInvWnd + OFF_WindowText);
        CStrRep* rep = *repPtr;

        if (rep && rep->refCount == 1 && rep->alloc > newLen)
        {
            // Fast path: sole owner with enough buffer — overwrite in-place
            memcpy(rep->utf8, title, newLen + 1);
            rep->length = newLen;
        }
        else
        {
            // Shared or too small — allocate a new CStrRep on the process heap
            // (game uses HeapAlloc/HeapFree for CXStr memory)
            uint32_t newAlloc = newLen + 64; // extra space for future in-place overwrites
            uint32_t allocSize = 0x14 + newAlloc; // CStrRep header + utf8 data
            CStrRep* newRep = static_cast<CStrRep*>(
                HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocSize));
            if (newRep)
            {
                newRep->refCount = 1;
                newRep->alloc    = newAlloc;
                newRep->length   = newLen;
                newRep->encoding = 0;
                newRep->freeList = nullptr;
                memcpy(newRep->utf8, title, newLen + 1);

                // Release our reference to the old rep (decrement refCount)
                if (rep && rep->refCount > 0)
                    rep->refCount--;

                // Swap in our new rep
                *repPtr = newRep;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Log once that we're hitting crashes
        static bool s_exceptLogged = false;
        if (!s_exceptLogged)
        {
            uintptr_t pWndMgr = 0;
            __try { pWndMgr = Memory::ReadMemory<uintptr_t>(s_wndMgrPtrAddr); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            LogFramework("LabelsOverride: EXCEPTION in UpdateInventoryTitle — pWndMgr=0x%08X",
                static_cast<unsigned int>(pWndMgr));
            s_exceptLogged = true;
        }
        s_cachedInvWnd = 0;
    }
}

// EQGameBaseAddress — defined in core.cpp, declared with C linkage
extern "C" uintptr_t EQGameBaseAddress;

// Raw addresses for hooks not in eqlib offsets
#define CharacterZoneClient__Cur_Endurance_x  0x444170
#define CharacterZoneClient__CalculateWeight_x 0x44CDD0

// ---------------------------------------------------------------------------
// Detour implementations
// ---------------------------------------------------------------------------

static int __cdecl GetLabelFromEQ_Detour(int EQType, void* cxstrOut, bool* p3, unsigned long* p4)
{
    int result = GetLabelFromEQ_Original(EQType, cxstrOut, p3, p4);

    if (MulticlassData::HasData())
    {
        auto it = s_labelMap.find(EQType);
        if (it != s_labelMap.end())
        {
            char buf[256];
            if (it->second(buf, sizeof(buf)))
            {
                OverwriteCXStr(cxstrOut, buf);

                // One-time diagnostic logging for class label EQTypes
                static bool s_logged1 = false, s_logged3 = false, s_logged4 = false;
                if ((EQType == 1 && !s_logged1) || (EQType == 3 && !s_logged3) || (EQType == 4 && !s_logged4))
                {
                    LogFramework("LabelsOverride: EQType %d overwritten -> \"%s\"", EQType, buf);
                    if (EQType == 1) s_logged1 = true;
                    if (EQType == 3) s_logged3 = true;
                    if (EQType == 4) s_logged4 = true;
                }
            }
        }
    }
    else
    {
        // Log once that HasData() is false when class labels are requested
        static bool s_noDataLogged = false;
        if (!s_noDataLogged && (EQType == 1 || EQType == 3 || EQType == 4))
        {
            LogFramework("LabelsOverride: EQType %d requested but HasData()=false", EQType);
            s_noDataLogged = true;
        }
    }

    return result;
}

static int __cdecl GetGaugeValueFromEQ_Detour(int EQType, void* cxstrOut, bool* p3, unsigned long* p4)
{
    int result = GetGaugeValueFromEQ_Original(EQType, cxstrOut, p3, p4);

    if (!MulticlassData::HasData())
        return result;

    int64_t cur = 0, max = 0;
    bool override = false;

    switch (EQType)
    {
    case 1: // HP gauge (player)
    case 6: // HP gauge (alt form)
        if (MulticlassData::HasStat(eStatEntry::CurHP) && MulticlassData::HasStat(eStatEntry::MaxHP))
        {
            cur = MulticlassData::GetStat(eStatEntry::CurHP);
            max = MulticlassData::GetStat(eStatEntry::MaxHP);
            override = true;
        }
        break;
    case 2: // Mana gauge
        if (MulticlassData::HasStat(eStatEntry::CurMana) && MulticlassData::HasStat(eStatEntry::MaxMana))
        {
            cur = MulticlassData::GetStat(eStatEntry::CurMana);
            max = MulticlassData::GetStat(eStatEntry::MaxMana);
            override = true;
        }
        break;
    case 3: // Endurance gauge
        if (MulticlassData::HasStat(eStatEntry::CurEndur) && MulticlassData::HasStat(eStatEntry::MaxEndur))
        {
            cur = MulticlassData::GetStat(eStatEntry::CurEndur);
            max = MulticlassData::GetStat(eStatEntry::MaxEndur);
            override = true;
        }
        break;
    }

    if (override && max > 0)
    {
        // Gauge value is cur*1000/max (permille)
        int gaugeVal = static_cast<int>((cur * 1000) / max);

        // Overwrite the return via CXStr — gauge functions write the numeric
        // value into the CXStr output
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", gaugeVal);
        OverwriteCXStr(cxstrOut, buf);
    }

    return result;
}

// Stat override detours — return server value if HasData(), else call original

static int32_t __fastcall CurHP_Detour(void* thisPtr, void* edx, int spawn, unsigned char flag)
{
    if (MulticlassData::HasStat(eStatEntry::CurHP))
    {
        int64_t val = MulticlassData::GetStat(eStatEntry::CurHP);
        return (val > INT32_MAX - 1) ? INT32_MAX - 1 : static_cast<int32_t>(val);
    }
    return CurHP_Original(thisPtr, edx, spawn, flag);
}

static int32_t __fastcall CurMana_Detour(void* thisPtr, void* edx, int spawn)
{
    if (MulticlassData::HasStat(eStatEntry::CurMana))
    {
        int64_t val = MulticlassData::GetStat(eStatEntry::CurMana);
        return (val > INT32_MAX - 1) ? INT32_MAX - 1 : static_cast<int32_t>(val);
    }
    return CurMana_Original(thisPtr, edx, spawn);
}

static int32_t __fastcall MaxHP_Detour(void* thisPtr, void* edx, int spawn, int flag)
{
    if (MulticlassData::HasStat(eStatEntry::MaxHP))
    {
        int64_t val = MulticlassData::GetStat(eStatEntry::MaxHP);
        return (val > INT32_MAX - 1) ? INT32_MAX - 1 : static_cast<int32_t>(val);
    }
    return MaxHP_Original(thisPtr, edx, spawn, flag);
}

static int32_t __fastcall MaxMana_Detour(void* thisPtr, void* edx, int spawn)
{
    if (MulticlassData::HasStat(eStatEntry::MaxMana))
    {
        int64_t val = MulticlassData::GetStat(eStatEntry::MaxMana);
        return (val > INT32_MAX - 1) ? INT32_MAX - 1 : static_cast<int32_t>(val);
    }
    return MaxMana_Original(thisPtr, edx, spawn);
}

static int32_t __fastcall MaxEnd_Detour(void* thisPtr, void* edx, int spawn)
{
    if (MulticlassData::HasStat(eStatEntry::MaxEndur))
    {
        int64_t val = MulticlassData::GetStat(eStatEntry::MaxEndur);
        return (val > INT32_MAX - 1) ? INT32_MAX - 1 : static_cast<int32_t>(val);
    }
    return MaxEnd_Original(thisPtr, edx, spawn);
}

static int32_t __fastcall CurEnd_Detour(void* thisPtr, void* edx, int spawn)
{
    if (MulticlassData::HasStat(eStatEntry::CurEndur))
    {
        int64_t val = MulticlassData::GetStat(eStatEntry::CurEndur);
        return (val > INT32_MAX - 1) ? INT32_MAX - 1 : static_cast<int32_t>(val);
    }
    return CurEnd_Original(thisPtr, edx, spawn);
}

static double __fastcall CalculateWeight_Detour(void* thisPtr, void* edx)
{
    if (MulticlassData::HasStat(eStatEntry::Runspeed) && MulticlassData::HasStat(eStatEntry::Walkspeed))
    {
        uint8_t runState = Memory::ReadMemory<uint8_t>(s_runWalkStateAddr);
        if (runState)
            return static_cast<double>(MulticlassData::GetStat(eStatEntry::Runspeed)) / 100000.0;
        else
            return static_cast<double>(MulticlassData::GetStat(eStatEntry::Walkspeed)) / 100000.0;
    }
    return CalculateWeight_Original(thisPtr, edx);
}

// ---------------------------------------------------------------------------
// IMod implementation
// ---------------------------------------------------------------------------

const char* LabelsOverride::GetName() const
{
    return "LabelsOverride";
}

bool LabelsOverride::Initialize()
{
    LogFramework("LabelsOverride: Building label mapping table...");

    // --- Class lines (override Name/Class/Deity globally) ---
    // EQType 1 (Name) -> Class1 line (passthrough if no class data)
    s_labelMap[1] = [](char* buf, size_t sz) { return FormatClassLine(buf, sz, 1); };
    // EQType 3 (Class) -> Class2 line (blank if ClassCount < 2)
    s_labelMap[3] = [](char* buf, size_t sz) { return FormatClassLine(buf, sz, 2); };
    // EQType 4 (Deity) -> Class3 line (blank if ClassCount < 3)
    s_labelMap[4] = [](char* buf, size_t sz) { return FormatClassLine(buf, sz, 3); };

    // --- Base stats ---
    s_labelMap[5]  = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::STR); };
    s_labelMap[6]  = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::STA); };
    s_labelMap[7]  = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::AGI); };
    s_labelMap[8]  = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::DEX); };
    s_labelMap[9]  = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::WIS); };
    s_labelMap[10] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::INT); };
    s_labelMap[11] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::CHA); };

    // --- Resists ---
    s_labelMap[12] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::PR); };
    s_labelMap[13] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::DR); };
    s_labelMap[14] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::FR); };
    s_labelMap[15] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::CR); };
    s_labelMap[16] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::MR); };

    // --- HP/Mana/Endur labels ---
    s_labelMap[17] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::CurHP); };
    s_labelMap[18] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::MaxHP); };
    s_labelMap[19] = [](char* buf, size_t sz) { return FormatDual(buf, sz, eStatEntry::CurHP, eStatEntry::MaxHP); };
    s_labelMap[20] = [](char* buf, size_t sz) { return FormatDual(buf, sz, eStatEntry::CurMana, eStatEntry::MaxMana); };
    s_labelMap[21] = [](char* buf, size_t sz) { return FormatDual(buf, sz, eStatEntry::CurEndur, eStatEntry::MaxEndur); };

    // --- AC/ATK ---
    s_labelMap[22] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::AC); };
    s_labelMap[23] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::ATK); };

    // --- Weight ---
    s_labelMap[24] = [](char* buf, size_t sz) { return FormatWeight(buf, sz, eStatEntry::Weight); };
    s_labelMap[25] = [](char* buf, size_t sz) { return FormatWeight(buf, sz, eStatEntry::MaxWeight); };

    // --- Alternate HP label ---
    s_labelMap[70] = [](char* buf, size_t sz) { return FormatDual(buf, sz, eStatEntry::CurHP, eStatEntry::MaxHP); };

    // --- Mana/Endur individual + dual ---
    s_labelMap[124] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::CurMana); };
    s_labelMap[125] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::MaxMana); };
    s_labelMap[126] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::CurEndur); };
    s_labelMap[127] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::MaxEndur); };
    s_labelMap[128] = [](char* buf, size_t sz) { return FormatDual(buf, sz, eStatEntry::CurMana, eStatEntry::MaxMana); };
    s_labelMap[129] = [](char* buf, size_t sz) { return FormatDual(buf, sz, eStatEntry::CurEndur, eStatEntry::MaxEndur); };

    // --- Weight dual ---
    s_labelMap[237] = [](char* buf, size_t sz) { return FormatDualWeight(buf, sz, eStatEntry::Weight, eStatEntry::MaxWeight); };

    // --- Custom labels (1002+) ---
    s_labelMap[1002] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::MeleePower); };
    s_labelMap[1003] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::SpellPower); };
    s_labelMap[1004] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::HealingPower); };
    s_labelMap[1005] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::TotalPower); };
    s_labelMap[1006] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel); };

    s_labelMap[1009] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::MeleeHaste); };
    s_labelMap[1010] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::SpellHaste); };
    s_labelMap[1011] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::HealingHaste); };
    s_labelMap[1012] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::MeleeCrit); };
    s_labelMap[1013] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::SpellCrit); };
    s_labelMap[1014] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::HealingCrit); };

    s_labelMap[1015] = [](char* buf, size_t sz) { return FormatSpeed(buf, sz, eStatEntry::Walkspeed); };
    s_labelMap[1016] = [](char* buf, size_t sz) { return FormatSpeed(buf, sz, eStatEntry::Runspeed); };
    s_labelMap[1017] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::Classless); };
    s_labelMap[1018] = [](char* buf, size_t sz) { return FormatSpeed(buf, sz, eStatEntry::Mitigation); };
    s_labelMap[1019] = [](char* buf, size_t sz) { return FormatStatInt(buf, sz, eStatEntry::AAPoints); };

    // --- Synergy 1-12 ---
    s_labelMap[1020] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel1); };
    s_labelMap[1021] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel2); };
    s_labelMap[1022] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel3); };
    s_labelMap[1023] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel4); };
    s_labelMap[1024] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel5); };
    s_labelMap[1025] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel6); };
    s_labelMap[1026] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel7); };
    s_labelMap[1027] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel8); };
    s_labelMap[1028] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel9); };
    s_labelMap[1029] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel10); };
    s_labelMap[1030] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel11); };
    s_labelMap[1031] = [](char* buf, size_t sz) { return FormatSynergy(buf, sz, eStatEntry::SynergyLevel12); };

    LogFramework("LabelsOverride: %zu label mappings registered", s_labelMap.size());

    // --- Resolve hook addresses ---

    // Label/gauge hooks (in eqlib offsets)
    uintptr_t labelAddr = eqlib::FixEQGameOffset(__GetLabelFromEQ_x);
    GetLabelFromEQ_Original = reinterpret_cast<GetLabelFromEQ_t>(labelAddr);
    LogFramework("LabelsOverride: GetLabelFromEQ = 0x%08X", static_cast<unsigned int>(labelAddr));

    uintptr_t gaugeAddr = eqlib::FixEQGameOffset(__GetGaugeValueFromEQ_x);
    GetGaugeValueFromEQ_Original = reinterpret_cast<GetGaugeValueFromEQ_t>(gaugeAddr);
    LogFramework("LabelsOverride: GetGaugeValueFromEQ = 0x%08X", static_cast<unsigned int>(gaugeAddr));

    // Stat override hooks (in eqlib offsets)
    uintptr_t curHPAddr = eqlib::FixEQGameOffset(CharacterZoneClient__Cur_HP_x);
    CurHP_Original = reinterpret_cast<CurHP_t>(curHPAddr);
    LogFramework("LabelsOverride: Cur_HP = 0x%08X", static_cast<unsigned int>(curHPAddr));

    uintptr_t curManaAddr = eqlib::FixEQGameOffset(CharacterZoneClient__Cur_Mana_x);
    CurMana_Original = reinterpret_cast<CurMana_t>(curManaAddr);
    LogFramework("LabelsOverride: Cur_Mana = 0x%08X", static_cast<unsigned int>(curManaAddr));

    uintptr_t maxHPAddr = eqlib::FixEQGameOffset(CharacterZoneClient__Max_HP_x);
    MaxHP_Original = reinterpret_cast<MaxHP_t>(maxHPAddr);
    LogFramework("LabelsOverride: Max_HP = 0x%08X", static_cast<unsigned int>(maxHPAddr));

    uintptr_t maxManaAddr = eqlib::FixEQGameOffset(CharacterZoneClient__Max_Mana_x);
    MaxMana_Original = reinterpret_cast<MaxMana_t>(maxManaAddr);
    LogFramework("LabelsOverride: Max_Mana = 0x%08X", static_cast<unsigned int>(maxManaAddr));

    uintptr_t maxEndAddr = eqlib::FixEQGameOffset(CharacterZoneClient__Max_Endurance_x);
    MaxEnd_Original = reinterpret_cast<MaxEnd_t>(maxEndAddr);
    LogFramework("LabelsOverride: Max_Endurance = 0x%08X", static_cast<unsigned int>(maxEndAddr));

    // Manual ASLR hooks (raw - 0x400000 + EQGameBaseAddress)
    uintptr_t curEndAddr = static_cast<uintptr_t>(CharacterZoneClient__Cur_Endurance_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    CurEnd_Original = reinterpret_cast<CurEnd_t>(curEndAddr);
    LogFramework("LabelsOverride: Cur_Endurance = 0x%08X", static_cast<unsigned int>(curEndAddr));

    uintptr_t calcWeightAddr = static_cast<uintptr_t>(CharacterZoneClient__CalculateWeight_x)
        - eqlib::EQGamePreferredAddress + EQGameBaseAddress;
    CalculateWeight_Original = reinterpret_cast<CalculateWeight_t>(calcWeightAddr);
    LogFramework("LabelsOverride: CalculateWeight = 0x%08X", static_cast<unsigned int>(calcWeightAddr));

    // RunWalkState global address
    s_runWalkStateAddr = eqlib::FixEQGameOffset(__RunWalkState_x);
    LogFramework("LabelsOverride: RunWalkState = 0x%08X", static_cast<unsigned int>(s_runWalkStateAddr));

    // Inventory window title support — resolve global pointer addresses
    s_localPlayerPtrAddr = eqlib::FixEQGameOffset(pinstLocalPlayer_x);
    LogFramework("LabelsOverride: pLocalPlayer @ 0x%08X", static_cast<unsigned int>(s_localPlayerPtrAddr));

    s_wndMgrPtrAddr = eqlib::FixEQGameOffset(pinstCXWndManager_x);
    LogFramework("LabelsOverride: pCXWndManager @ 0x%08X", static_cast<unsigned int>(s_wndMgrPtrAddr));

    // --- Install hooks ---
    Hooks::Install("GetLabelFromEQ",
        reinterpret_cast<void**>(&GetLabelFromEQ_Original),
        reinterpret_cast<void*>(&GetLabelFromEQ_Detour));

    Hooks::Install("GetGaugeValueFromEQ",
        reinterpret_cast<void**>(&GetGaugeValueFromEQ_Original),
        reinterpret_cast<void*>(&GetGaugeValueFromEQ_Detour));

    Hooks::Install("Cur_HP",
        reinterpret_cast<void**>(&CurHP_Original),
        reinterpret_cast<void*>(&CurHP_Detour));

    Hooks::Install("Cur_Mana",
        reinterpret_cast<void**>(&CurMana_Original),
        reinterpret_cast<void*>(&CurMana_Detour));

    Hooks::Install("Max_HP",
        reinterpret_cast<void**>(&MaxHP_Original),
        reinterpret_cast<void*>(&MaxHP_Detour));

    Hooks::Install("Max_Mana",
        reinterpret_cast<void**>(&MaxMana_Original),
        reinterpret_cast<void*>(&MaxMana_Detour));

    Hooks::Install("Max_Endurance",
        reinterpret_cast<void**>(&MaxEnd_Original),
        reinterpret_cast<void*>(&MaxEnd_Detour));

    Hooks::Install("Cur_Endurance",
        reinterpret_cast<void**>(&CurEnd_Original),
        reinterpret_cast<void*>(&CurEnd_Detour));

    Hooks::Install("CalculateWeight",
        reinterpret_cast<void**>(&CalculateWeight_Original),
        reinterpret_cast<void*>(&CalculateWeight_Detour));

    LogFramework("LabelsOverride: Initialized — 9 hooks installed");
    return true;
}

void LabelsOverride::Shutdown()
{
    s_labelMap.clear();
    s_cachedInvWnd = 0;
    s_invWndSearchLogged = false;
    LogFramework("LabelsOverride: Shutdown");
}

void LabelsOverride::OnPulse()
{
    UpdateInventoryTitle();
}

bool LabelsOverride::OnIncomingMessage(uint32_t opcode, const void* buffer, uint32_t size)
{
    // No message processing — stat data comes from MulticlassData mod
    return true;
}
