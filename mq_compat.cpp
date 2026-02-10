/**
 * @file mq_compat.cpp
 * @brief MQ compatibility layer — utility function implementations.
 * @date 2026-02-08
 *
 * All spawn member access uses raw offset arithmetic since we cannot include
 * the full eqlib game headers (they depend on imgui, mq/base).
 *
 * Offset reference (ROF2 - May 10 2013 build):
 *   PlayerClient inherits: TListNode<PlayerClient> (0x0C) + CActorApplicationData (vtable 0x04)
 *   PlayerBase starts at offset 0x10 within PlayerClient
 *
 *   PlayerBase layout:
 *     +0x038 Lastname[0x20]
 *     +0x064 Y (float), +0x068 X (float), +0x06c Z (float)
 *     +0x0a4 Name[0x40]
 *     +0x0e4 DisplayedName[0x40]
 *     +0x125 Type (uint8_t)
 *     +0x128 Properties (CharacterPropertyHash = HashTable<int>, size 0x10)
 *     +0x13c Height (float)
 *     +0x148 SpawnID (uint32_t)
 *     +0x158 Rider (PlayerClient*)
 *
 *   PlayerZoneClient (continues from PlayerBase):
 *     +0x0208 Mercenary (bool)
 *     +0x0250 Level (uint8_t)
 *     +0x038c MasterID (uint32_t)
 *
 *   PlayerClient:
 *     +0x0ea4 mActorClient (ActorClient)
 *       ActorBase: +0x010 Race (int), +0x014 Class (uint8_t)
 *     So: GetRace() = *(int*)(pSpawn + 0x0ea4 + 0x010)  = pSpawn + 0x0eb4
 *         GetClass() = *(uint8_t*)(pSpawn + 0x0ea4 + 0x014) = pSpawn + 0x0eb8
 */

#include "pch.h"
#include "mq_compat.h"

#include <eqlib/Offsets.h>
#include <eqlib/offsets/eqgame.h>

#include <cmath>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <charconv>

// ---------------------------------------------------------------------------
// External definitions needed for function pointer resolution
// ---------------------------------------------------------------------------
extern "C" uintptr_t EQGameBaseAddress;

// ---------------------------------------------------------------------------
// Offset constants for PlayerClient member access
// ---------------------------------------------------------------------------

namespace SpawnOffsets
{
    // Layout: CActorApplicationData vtable at +0x00 (4 bytes), then
    // TListNode: m_pPrev=+0x04, m_pNext=+0x08, m_pList=+0x0C (total 0x10)
    constexpr uintptr_t Next         = 0x008;  // TListNode<PlayerClient>::m_pNext

    // PlayerBase (starts at +0x10 in PlayerClient due to TListNode + CActorApplicationData)
    constexpr uintptr_t Lastname     = 0x038;
    constexpr uintptr_t Y            = 0x064;
    constexpr uintptr_t X            = 0x068;
    constexpr uintptr_t Z            = 0x06c;
    constexpr uintptr_t Name         = 0x0a4;
    constexpr uintptr_t DisplayedName = 0x0e4;
    constexpr uintptr_t Type         = 0x125;
    constexpr uintptr_t Properties   = 0x128;  // CharacterPropertyHash
    constexpr uintptr_t Height       = 0x13c;
    constexpr uintptr_t SpawnID      = 0x148;
    constexpr uintptr_t Rider        = 0x158;

    // PlayerZoneClient
    constexpr uintptr_t Mercenary    = 0x0208;
    constexpr uintptr_t Level        = 0x0250;
    constexpr uintptr_t MasterID     = 0x038c;

    // PlayerClient -> mActorClient (ActorClient) -> ActorBase members
    constexpr uintptr_t mActorClient = 0x0ea4;
    constexpr uintptr_t ActorBase_Race  = 0x010;
    constexpr uintptr_t ActorBase_Class = 0x014;

    // Computed:
    constexpr uintptr_t Race  = mActorClient + ActorBase_Race;   // 0x0eb4
    constexpr uintptr_t Class = mActorClient + ActorBase_Class;  // 0x0eb8

    // Phase 6 additions
    constexpr uintptr_t Heading     = 0x080;
    constexpr uintptr_t SpeedY      = 0x070;
    constexpr uintptr_t SpeedX      = 0x074;
    constexpr uintptr_t SpeedRun    = 0x07c;
    constexpr uintptr_t HPCurrent   = 0x2e4;
    constexpr uintptr_t Deity       = 0x518;
}

// Helper: read a value at offset from a base pointer
template<typename T>
static inline T ReadAt(void* base, uintptr_t offset)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
}

template<typename T>
static inline T* PtrAt(void* base, uintptr_t offset)
{
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
}

// ---------------------------------------------------------------------------
// SpawnAccess namespace — raw offset accessors
// ---------------------------------------------------------------------------

namespace SpawnAccess
{

const char* GetName(SPAWNINFO* p)         { return PtrAt<const char>(p, SpawnOffsets::Name); }
const char* GetDisplayedName(SPAWNINFO* p) { return PtrAt<const char>(p, SpawnOffsets::DisplayedName); }
const char* GetLastname(SPAWNINFO* p)     { return PtrAt<const char>(p, SpawnOffsets::Lastname); }
uint8_t     GetType(SPAWNINFO* p)         { return ReadAt<uint8_t>(p, SpawnOffsets::Type); }
uint8_t     GetLevel(SPAWNINFO* p)        { return ReadAt<uint8_t>(p, SpawnOffsets::Level); }
uint32_t    GetSpawnID(SPAWNINFO* p)      { return ReadAt<uint32_t>(p, SpawnOffsets::SpawnID); }
float       GetY(SPAWNINFO* p)            { return ReadAt<float>(p, SpawnOffsets::Y); }
float       GetX(SPAWNINFO* p)            { return ReadAt<float>(p, SpawnOffsets::X); }
float       GetZ(SPAWNINFO* p)            { return ReadAt<float>(p, SpawnOffsets::Z); }
float       GetHeight(SPAWNINFO* p)       { return ReadAt<float>(p, SpawnOffsets::Height); }
SPAWNINFO*  GetRider(SPAWNINFO* p)        { return ReadAt<SPAWNINFO*>(p, SpawnOffsets::Rider); }
uint32_t    GetMasterID(SPAWNINFO* p)     { return ReadAt<uint32_t>(p, SpawnOffsets::MasterID); }
bool        GetMercenary(SPAWNINFO* p)    { return ReadAt<bool>(p, SpawnOffsets::Mercenary); }
SPAWNINFO*  GetNext(SPAWNINFO* p)         { return ReadAt<SPAWNINFO*>(p, SpawnOffsets::Next); }
int         GetClass(SPAWNINFO* p)        { return static_cast<int>(ReadAt<uint8_t>(p, SpawnOffsets::Class)); }
int         GetRace(SPAWNINFO* p)         { return ReadAt<int>(p, SpawnOffsets::Race); }
float       GetHeading(SPAWNINFO* p)      { return ReadAt<float>(p, SpawnOffsets::Heading); }
int         GetDeity(SPAWNINFO* p)        { return ReadAt<int>(p, SpawnOffsets::Deity); }
int         GetHPCurrent(SPAWNINFO* p)    { return ReadAt<int>(p, SpawnOffsets::HPCurrent); }
float       GetSpeedRun(SPAWNINFO* p)     { return ReadAt<float>(p, SpawnOffsets::SpeedRun); }
float       GetSpeedX(SPAWNINFO* p)       { return ReadAt<float>(p, SpawnOffsets::SpeedX); }
float       GetSpeedY(SPAWNINFO* p)       { return ReadAt<float>(p, SpawnOffsets::SpeedY); }

const char* GetRaceString(SPAWNINFO* p)
{
    if (!p) return "Unknown";
    int race = GetRace(p);
    switch (race)
    {
    case 1:   return "Human";
    case 2:   return "Barbarian";
    case 3:   return "Erudite";
    case 4:   return "Wood Elf";
    case 5:   return "High Elf";
    case 6:   return "Dark Elf";
    case 7:   return "Half Elf";
    case 8:   return "Dwarf";
    case 9:   return "Troll";
    case 10:  return "Ogre";
    case 11:  return "Halfling";
    case 12:  return "Gnome";
    case 13:  return "Aviak";
    case 14:  return "Werewolf";
    case 15:  return "Brownie";
    case 128: return "Iksar";
    case 130: return "Vah Shir";
    case 330: return "Froglok";
    case 522: return "Drakkin";
    default:  return "Unknown";
    }
}

const char* GetClassString(SPAWNINFO* p)
{
    if (!p) return "Unknown";
    int cls = GetClass(p);
    switch (cls)
    {
    case 1:  return "Warrior";
    case 2:  return "Cleric";
    case 3:  return "Paladin";
    case 4:  return "Ranger";
    case 5:  return "Shadow Knight";
    case 6:  return "Druid";
    case 7:  return "Monk";
    case 8:  return "Bard";
    case 9:  return "Rogue";
    case 10: return "Shaman";
    case 11: return "Necromancer";
    case 12: return "Wizard";
    case 13: return "Magician";
    case 14: return "Enchanter";
    case 15: return "Beastlord";
    case 16: return "Berserker";
    default: return "Unknown";
    }
}

const char* GetClassThreeLetterCode(SPAWNINFO* p)
{
    if (!p) return "UNK";
    int cls = GetClass(p);
    switch (cls)
    {
    case 1:  return "WAR";
    case 2:  return "CLR";
    case 3:  return "PAL";
    case 4:  return "RNG";
    case 5:  return "SHD";
    case 6:  return "DRU";
    case 7:  return "MNK";
    case 8:  return "BRD";
    case 9:  return "ROG";
    case 10: return "SHM";
    case 11: return "NEC";
    case 12: return "WIZ";
    case 13: return "MAG";
    case 14: return "ENC";
    case 15: return "BST";
    case 16: return "BER";
    default: return "UNK";
    }
}

} // namespace SpawnAccess

// ---------------------------------------------------------------------------
// Function pointers for game functions (resolved once)
// ---------------------------------------------------------------------------

// PcClient::GetConLevel(const PlayerClient*) — thiscall
using GetConLevel_t = unsigned long(__fastcall*)(void* thisPtr, void* edx, void* pSpawn);
static GetConLevel_t s_GetConLevel = nullptr;

// PlayerManagerClient::GetSpawnByID(int) — thiscall
using GetSpawnByID_t = eqlib::PlayerClient*(__fastcall*)(void* thisPtr, void* edx, int id);
static GetSpawnByID_t s_GetSpawnByID = nullptr;

static bool s_funcPtrsResolved = false;

static void ResolveFuncPtrs()
{
    if (s_funcPtrsResolved)
        return;
    s_funcPtrsResolved = true;

    s_GetConLevel = reinterpret_cast<GetConLevel_t>(
        eqlib::FixEQGameOffset(PcClient__GetConLevel_x));
    s_GetSpawnByID = reinterpret_cast<GetSpawnByID_t>(
        eqlib::FixEQGameOffset(PlayerManagerClient__GetSpawnByID_x));

    LogFramework("mq_compat resolved: GetConLevel=0x%08X, GetSpawnByID=0x%08X",
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(s_GetConLevel)),
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(s_GetSpawnByID)));
}

// ---------------------------------------------------------------------------
// Body type access via CharacterPropertyHash
//
// CharacterPropertyHash is a HashTable<int> at spawn offset 0x128.
// HashTable layout: +0x00 pHashData (HashNode<int>**), +0x04 TableSize (int)
// We walk the table to find the minimum property value.
// ---------------------------------------------------------------------------

// HashTable<int> layout:
//   +0x00: HashNode<int>** pHashData  (array of bucket pointers)
//   +0x04: int TableSize              (number of buckets)
// HashNode<int> layout:
//   +0x00: int value
//   +0x04: HashNode<int>* pNext
//   +0x08: int key (hash key)

// SEH-protected body type lookup — the Properties hash table offset (0x128) is
// unverified for this build and may crash if incorrect.
static int GetBodyType_Inner(SPAWNINFO* pSpawn)
{
    uintptr_t propsAddr = reinterpret_cast<uintptr_t>(pSpawn) + SpawnOffsets::Properties;

    // HashTable<int>: first member is pointer to bucket array, second is table size
    void** pHashData = *reinterpret_cast<void***>(propsAddr);
    int tableSize = *reinterpret_cast<int*>(propsAddr + 0x04);

    if (!pHashData || tableSize <= 0 || tableSize > 256)
        return 0;

    int minProperty = 0;

    for (int i = 0; i < tableSize; i++)
    {
        uintptr_t node = reinterpret_cast<uintptr_t>(pHashData[i]);
        while (node)
        {
            int value = *reinterpret_cast<int*>(node);
            if (minProperty == 0 || value < minProperty)
                minProperty = value;
            // pNext is at offset +0x04 in HashNode
            node = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(node + 0x04));
        }
    }

    // If Utility, check for sub-types
    if (minProperty == MQ_CharProp_Utility)
    {
        for (int i = 0; i < tableSize; i++)
        {
            uintptr_t node = reinterpret_cast<uintptr_t>(pHashData[i]);
            while (node)
            {
                int value = *reinterpret_cast<int*>(node);
                if (value == MQ_CharProp_Trap) return MQ_CharProp_Trap;
                if (value == MQ_CharProp_Companion) return MQ_CharProp_Companion;
                if (value == MQ_CharProp_Suicide) return MQ_CharProp_Suicide;
                node = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(node + 0x04));
            }
        }
    }

    return minProperty;
}

int GetBodyType(SPAWNINFO* pSpawn)
{
    if (!pSpawn)
        return 0;

    __try
    {
        return GetBodyType_Inner(pSpawn);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static bool s_logged = false;
        if (!s_logged)
        {
            LogFramework("!!! GetBodyType EXCEPTION on spawn 0x%p — Properties offset may be wrong", pSpawn);
            s_logged = true;
        }
        return 0;
    }
}

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

const char* GetNextArg(const char* szLine, int dwNumber, bool CSV, char Separator)
{
    if (!szLine)
        return "";

    const char* szNext = szLine;
    bool InQuotes = false;
    bool CustomSep = Separator != 0;

    while ((!CustomSep && *szNext == ' ')
        || (!CustomSep && *szNext == '\t')
        || (CustomSep && *szNext == Separator)
        || (!CustomSep && CSV && *szNext == ','))
    {
        szNext++;
    }

    if (dwNumber < 1)
        return szNext;

    for (; dwNumber > 0; dwNumber--)
    {
        while (((CustomSep || *szNext != ' ')
            && (CustomSep || *szNext != '\t')
            && (!CustomSep || *szNext != Separator)
            && (CustomSep || !CSV || *szNext != ',')
            && *szNext != '\0')
            || InQuotes)
        {
            if (*szNext == '\0' && InQuotes)
                return szNext;
            if (*szNext == '"')
                InQuotes = !InQuotes;
            szNext++;
        }

        while ((!CustomSep && *szNext == ' ')
            || (!CustomSep && *szNext == '\t')
            || (CustomSep && *szNext == Separator)
            || (!CustomSep && CSV && *szNext == ','))
        {
            szNext++;
        }
    }

    return szNext;
}

const char* GetArg(char* szDest, const char* szSrc, int dwNumber,
    bool LeaveQuotes, bool ToParen, bool CSV, char Separator, bool AnyNonAlphaNum)
{
    if (!szSrc || !szDest)
        return nullptr;

    bool CustomSep = Separator != 0;
    bool InQuotes = false;

    const char* szTemp = GetNextArg(szSrc, dwNumber - 1, CSV, Separator);
    int i = 0;
    int j = 0;

    while ((
        (CustomSep || szTemp[i] != ' ')
        && (CustomSep || szTemp[i] != '\t')
        && (CustomSep || !CSV || szTemp[i] != ',')
        && (!CustomSep || szTemp[i] != Separator)
        && (!AnyNonAlphaNum || ((szTemp[i] >= '0' && szTemp[i] <= '9')
            || (szTemp[i] >= 'a' && szTemp[i] <= 'z')
            || (szTemp[i] >= 'A' && szTemp[i] <= 'Z')
            || szTemp[i] == '_'))
        && (szTemp[i] != '\0')
        && (!ToParen || szTemp[i] != ')'))
        || InQuotes)
    {
        if (szTemp[i] == '\0' && InQuotes)
        {
            szDest[j] = '\0';
            return szDest;
        }

        if (szTemp[i] == '"')
        {
            InQuotes = !InQuotes;
            if (LeaveQuotes)
            {
                szDest[j] = szTemp[i];
                j++;
            }
        }
        else
        {
            szDest[j] = szTemp[i];
            j++;
        }
        i++;
    }

    if (ToParen && szTemp[i] == ')')
        szDest[j++] = ')';

    szDest[j] = '\0';
    return szDest;
}

int GetIntFromString(const char* str, int defaultVal)
{
    if (!str || !*str)
        return defaultVal;

    while (*str == ' ' || *str == '\t')
        str++;

    int result = defaultVal;
    std::from_chars(str, str + strlen(str), result);
    return result;
}

float GetFloatFromString(const char* str, float defaultVal)
{
    if (!str || !*str)
        return defaultVal;

    while (*str == ' ' || *str == '\t')
        str++;

    float result = defaultVal;
    std::from_chars(str, str + strlen(str), result);
    return result;
}

static bool nocase_char_equals(char a, char b)
{
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

bool ci_equals(std::string_view sv1, std::string_view sv2)
{
    return sv1.size() == sv2.size()
        && std::equal(sv1.begin(), sv1.end(), sv2.begin(), nocase_char_equals);
}

bool ci_equals(std::string_view haystack, std::string_view needle, bool isExact)
{
    if (isExact)
        return ci_equals(haystack, needle);
    return ci_find_substr(haystack, needle) != -1;
}

int ci_find_substr(std::string_view haystack, std::string_view needle)
{
    auto iter = std::search(haystack.begin(), haystack.end(),
        needle.begin(), needle.end(), nocase_char_equals);
    if (iter == haystack.end())
        return -1;
    return static_cast<int>(iter - haystack.begin());
}

// ---------------------------------------------------------------------------
// Spawn utilities
// ---------------------------------------------------------------------------

eSpawnType GetSpawnType(SPAWNINFO* pSpawn)
{
    if (!pSpawn)
        return NONE;

    uint8_t type = SpawnAccess::GetType(pSpawn);

    switch (type)
    {
    case SPAWN_PLAYER:
        return PC;

    case SPAWN_NPC:
    {
        // Mount check
        if (SpawnAccess::GetRider(pSpawn))
            return MOUNT;
        const char* dispName = SpawnAccess::GetDisplayedName(pSpawn);
        const char* suffix = "`s Mount";
        size_t dispLen = strlen(dispName);
        size_t suffLen = strlen(suffix);
        if (dispLen >= suffLen && strcmp(dispName + dispLen - suffLen, suffix) == 0)
            return MOUNT;

        if (SpawnAccess::GetMasterID(pSpawn))
            return PET;
        if (SpawnAccess::GetMercenary(pSpawn))
            return MERCENARY;

        // Flyer check
        float y = SpawnAccess::GetY(pSpawn);
        float x = SpawnAccess::GetX(pSpawn);
        float z = SpawnAccess::GetZ(pSpawn);
        if (std::isnan(y) && std::isnan(x) && std::isnan(z))
            return FLYER;

        int bodyType = GetBodyType(pSpawn);
        int spawnClass = SpawnAccess::GetClass(pSpawn);
        int spawnRace = SpawnAccess::GetRace(pSpawn);
        const char* name = SpawnAccess::GetName(pSpawn);

        switch (bodyType)
        {
        case MQ_CharProp_None:
            if (spawnClass == MQ_Class_Object)
                return OBJECT;
            return NPC;

        case MQ_CharProp_Construct:
            if ((spawnRace == MQ_EQR_INVISIBLE_MAN) &&
                (strstr(name, "Aura") || strstr(name, "Circle_of") ||
                 strstr(name, "Guardian_Circle") || strstr(name, "Earthen_Strength") ||
                 strstr(name, "Pact_of_the_Wolf")))
                return AURA;
            if ((spawnRace == MQ_EQR_SPIKE_TRAP) &&
                (strstr(name, "poison") || strstr(name, "Poison")))
                return AURA;
            if (strstr(name, "Rune"))
                return AURA;
            if (spawnClass == MQ_Class_Object)
                return OBJECT;
            return NPC;

        case MQ_CharProp_Magical:
            if (spawnRace == MQ_EQR_CAMPSITE)
                return CAMPFIRE;
            if (spawnRace == MQ_EQR_BANNER ||
                (spawnRace >= MQ_EQR_BANNER0 && spawnRace <= MQ_EQR_BANNER4) ||
                spawnRace == MQ_EQR_TCGBANNER)
                return BANNER;
            if ((spawnRace == MQ_EQR_TOTEM) && strstr(name, "Idol"))
                return AURA;
            if (spawnClass == MQ_Class_Object)
                return OBJECT;
            return NPC;

        case MQ_CharProp_Untargetable:
            return UNTARGETABLE;

        case MQ_CharProp_Cursed:
            return CHEST;

        case MQ_CharProp_Utility:
            return UNTARGETABLE;

        case MQ_CharProp_Trap:
            return TRAP;

        case MQ_CharProp_Companion:
            return TIMER;

        case MQ_CharProp_Suicide:
            return TRIGGER;

        default:
            break;
        }
        return NPC;
    }

    case SPAWN_CORPSE:
        return CORPSE;

    default:
        break;
    }

    return ITEM;
}

int ConColor(SPAWNINFO* pSpawn)
{
    ResolveFuncPtrs();

    eqlib::PlayerClient* localPlayer = pLocalPlayer;
    eqlib::PcClient* localPC = pLocalPC;

    if (!localPlayer || !localPC || !pSpawn || !s_GetConLevel)
        return CONCOLOR_WHITE;

    unsigned long conLevel = s_GetConLevel(localPC, nullptr, pSpawn);

    switch (conLevel)
    {
    case 0:
    case 1:  return CONCOLOR_GREY;
    case 2:  return CONCOLOR_GREEN;
    case 3:  return CONCOLOR_LIGHTBLUE;
    case 4:  return CONCOLOR_BLUE;
    case 5:  return CONCOLOR_WHITE;
    case 6:  return CONCOLOR_YELLOW;
    case 7:  return CONCOLOR_RED;
    default: return COLOR_PURPLE;
    }
}

uint32_t ConColorToARGB(int conColor)
{
    switch (conColor)
    {
    case CONCOLOR_GREY:      return 0xFF808080;
    case CONCOLOR_GREEN:     return 0xFF00FF00;
    case CONCOLOR_LIGHTBLUE: return 0xFF00FFFF;
    case CONCOLOR_BLUE:      return 0xFF0000FF;
    case CONCOLOR_WHITE:     return 0xFFFFFFFF;
    case CONCOLOR_YELLOW:    return 0xFFFFFF00;
    case CONCOLOR_RED:       return 0xFFFF0000;
    default:                 return 0xFFFFFFFF;
    }
}

bool IsNamed(SPAWNINFO* pSpawn)
{
    if (!pSpawn)
        return false;

    if (GetSpawnType(pSpawn) != NPC)
        return false;

    if (SpawnAccess::GetClass(pSpawn) == MQ_Class_Object)
        return false;

    const char* name = SpawnAccess::GetName(pSpawn);
    if (!name[0])
        return false;

    // "A_" or "An_" prefix => common mob, not named
    if (name[0] == 'A')
    {
        if (name[1] == '_')
            return false;
        if (name[1] == 'n' && name[2] == '_')
            return false;
    }

    if (name[0] == '#')
        return true;
    if (isupper(static_cast<unsigned char>(name[0])))
        return true;

    return false;
}

SPAWNINFO* GetSpawnByID(uint32_t spawnID)
{
    ResolveFuncPtrs();

    eqlib::PlayerManagerClient* mgr = GameState::GetSpawnManager();
    if (!mgr || !s_GetSpawnByID)
        return nullptr;
    return s_GetSpawnByID(mgr, nullptr, static_cast<int>(spawnID));
}

float DistanceToSpawn(SPAWNINFO* pFrom, SPAWNINFO* pTo)
{
    if (!pFrom || !pTo)
        return 0.0f;

    float dX = SpawnAccess::GetX(pFrom) - SpawnAccess::GetX(pTo);
    float dY = SpawnAccess::GetY(pFrom) - SpawnAccess::GetY(pTo);
    return sqrtf(dX * dX + dY * dY);
}

float get_melee_range(SPAWNINFO* pSpawn1, SPAWNINFO* pSpawn2)
{
    if (!pSpawn1 || !pSpawn2)
        return 14.0f;

    float range = SpawnAccess::GetHeight(pSpawn1) + SpawnAccess::GetHeight(pSpawn2);
    if (range < 14.0f) range = 14.0f;
    if (range > 75.0f) range = 75.0f;
    return range;
}

// ---------------------------------------------------------------------------
// MQSpawnSearch helpers
// ---------------------------------------------------------------------------

void ClearSearchSpawn(MQSpawnSearch* pSearchSpawn)
{
    if (!pSearchSpawn)
        return;
    *pSearchSpawn = MQSpawnSearch{};
}

void ParseSearchSpawn(const char* Buffer, MQSpawnSearch* pSearchSpawn)
{
    if (!Buffer || !pSearchSpawn)
        return;

    ClearSearchSpawn(pSearchSpawn);

    char szArg[MAX_STRING] = { 0 };
    const char* szRest = Buffer;

    while (true)
    {
        GetArg(szArg, szRest, 1);
        szRest = GetNextArg(szRest, 1);

        if (szArg[0] == '\0')
            break;

        if (!_stricmp(szArg, "pc"))
            pSearchSpawn->SpawnType = PC;
        else if (!_stricmp(szArg, "npc"))
            pSearchSpawn->SpawnType = NPC;
        else if (!_stricmp(szArg, "mount"))
            pSearchSpawn->SpawnType = MOUNT;
        else if (!_stricmp(szArg, "pet"))
            pSearchSpawn->SpawnType = PET;
        else if (!_stricmp(szArg, "pcpet"))
            pSearchSpawn->SpawnType = PCPET;
        else if (!_stricmp(szArg, "npcpet"))
            pSearchSpawn->SpawnType = NPCPET;
        else if (!_stricmp(szArg, "xtarhater"))
            pSearchSpawn->bXTarHater = true;
        else if (!_stricmp(szArg, "nopet"))
            pSearchSpawn->bNoPet = true;
        else if (!_stricmp(szArg, "corpse"))
            pSearchSpawn->SpawnType = CORPSE;
        else if (!_stricmp(szArg, "npccorpse"))
            pSearchSpawn->SpawnType = NPCCORPSE;
        else if (!_stricmp(szArg, "pccorpse"))
            pSearchSpawn->SpawnType = PCCORPSE;
        else if (!_stricmp(szArg, "trigger"))
            pSearchSpawn->SpawnType = TRIGGER;
        else if (!_stricmp(szArg, "untargetable"))
            pSearchSpawn->SpawnType = UNTARGETABLE;
        else if (!_stricmp(szArg, "trap"))
            pSearchSpawn->SpawnType = TRAP;
        else if (!_stricmp(szArg, "chest"))
            pSearchSpawn->SpawnType = CHEST;
        else if (!_stricmp(szArg, "timer"))
            pSearchSpawn->SpawnType = TIMER;
        else if (!_stricmp(szArg, "aura"))
            pSearchSpawn->SpawnType = AURA;
        else if (!_stricmp(szArg, "object"))
            pSearchSpawn->SpawnType = OBJECT;
        else if (!_stricmp(szArg, "banner"))
            pSearchSpawn->SpawnType = BANNER;
        else if (!_stricmp(szArg, "campfire"))
            pSearchSpawn->SpawnType = CAMPFIRE;
        else if (!_stricmp(szArg, "mercenary"))
            pSearchSpawn->SpawnType = MERCENARY;
        else if (!_stricmp(szArg, "flyer"))
            pSearchSpawn->SpawnType = FLYER;
        else if (!_stricmp(szArg, "any"))
            pSearchSpawn->SpawnType = NONE;
        else if (!_stricmp(szArg, "next"))
            pSearchSpawn->bTargNext = true;
        else if (!_stricmp(szArg, "prev"))
            pSearchSpawn->bTargPrev = true;
        else if (!_stricmp(szArg, "lfg"))
            pSearchSpawn->bLFG = true;
        else if (!_stricmp(szArg, "gm"))
            pSearchSpawn->bGM = true;
        else if (!_stricmp(szArg, "group"))
            pSearchSpawn->bGroup = true;
        else if (!_stricmp(szArg, "nogroup"))
            pSearchSpawn->bNoGroup = true;
        else if (!_stricmp(szArg, "raid"))
            pSearchSpawn->bRaid = true;
        else if (!_stricmp(szArg, "noguild"))
            pSearchSpawn->bNoGuild = true;
        else if (!_stricmp(szArg, "trader"))
            pSearchSpawn->bTrader = true;
        else if (!_stricmp(szArg, "named"))
            pSearchSpawn->bNamed = true;
        else if (!_stricmp(szArg, "merchant"))
            pSearchSpawn->bMerchant = true;
        else if (!_stricmp(szArg, "banker"))
            pSearchSpawn->bBanker = true;
        else if (!_stricmp(szArg, "tank"))
            pSearchSpawn->bTank = true;
        else if (!_stricmp(szArg, "healer"))
            pSearchSpawn->bHealer = true;
        else if (!_stricmp(szArg, "dps"))
            pSearchSpawn->bDps = true;
        else if (!_stricmp(szArg, "slower"))
            pSearchSpawn->bSlower = true;
        else if (!_stricmp(szArg, "los"))
            pSearchSpawn->bLoS = true;
        else if (!_stricmp(szArg, "targetable"))
            pSearchSpawn->bTargetable = true;
        else if (!_stricmp(szArg, "range"))
        {
            GetArg(szArg, szRest, 1);
            szRest = GetNextArg(szRest, 1);
            pSearchSpawn->MinLevel = GetIntFromString(szArg, 0);

            GetArg(szArg, szRest, 1);
            szRest = GetNextArg(szRest, 1);
            pSearchSpawn->MaxLevel = GetIntFromString(szArg, MAX_NPC_LEVEL);
        }
        else if (!_stricmp(szArg, "loc"))
        {
            pSearchSpawn->bKnownLocation = true;

            GetArg(szArg, szRest, 1);
            szRest = GetNextArg(szRest, 1);
            pSearchSpawn->yLoc = GetFloatFromString(szArg, 0.0f);

            GetArg(szArg, szRest, 1);
            szRest = GetNextArg(szRest, 1);
            pSearchSpawn->xLoc = GetFloatFromString(szArg, 0.0f);
        }
        else if (!_stricmp(szArg, "id"))
        {
            GetArg(szArg, szRest, 1);
            szRest = GetNextArg(szRest, 1);
            pSearchSpawn->SpawnID = static_cast<uint32_t>(GetIntFromString(szArg, 0));
            pSearchSpawn->bSpawnID = true;
        }
        else if (!_stricmp(szArg, "radius"))
        {
            GetArg(szArg, szRest, 1);
            szRest = GetNextArg(szRest, 1);
            pSearchSpawn->FRadius = GetFloatFromString(szArg, 10000.0f);
        }
        else if (!_stricmp(szArg, "zradius"))
        {
            GetArg(szArg, szRest, 1);
            szRest = GetNextArg(szRest, 1);
            pSearchSpawn->ZRadius = GetFloatFromString(szArg, 10000.0f);
        }
        else if (!_stricmp(szArg, "notid"))
        {
            GetArg(szArg, szRest, 1);
            szRest = GetNextArg(szRest, 1);
            pSearchSpawn->NotID = static_cast<uint32_t>(GetIntFromString(szArg, 0));
        }
        else
        {
            if (pSearchSpawn->szName[0])
            {
                strcat_s(pSearchSpawn->szName, " ");
                strcat_s(pSearchSpawn->szName, szArg);
            }
            else
            {
                strcpy_s(pSearchSpawn->szName, szArg);
            }
        }
    }
}

bool SpawnMatchesSearch(MQSpawnSearch* pSearchSpawn, SPAWNINFO* pSpawn)
{
    if (!pSearchSpawn || !pSpawn)
        return false;

    eSpawnType spawnType = GetSpawnType(pSpawn);

    if (pSearchSpawn->SpawnType != NONE && pSearchSpawn->SpawnType != spawnType)
        return false;

    uint8_t level = SpawnAccess::GetLevel(pSpawn);
    if (level < pSearchSpawn->MinLevel)
        return false;
    if (level > pSearchSpawn->MaxLevel)
        return false;

    uint32_t spawnID = SpawnAccess::GetSpawnID(pSpawn);

    if (pSearchSpawn->bSpawnID && spawnID != pSearchSpawn->SpawnID)
        return false;

    if (pSearchSpawn->NotID && spawnID == pSearchSpawn->NotID)
        return false;

    if (pSearchSpawn->szName[0])
    {
        const char* name = SpawnAccess::GetName(pSpawn);
        if (name[0])
        {
            if (pSearchSpawn->bExactName)
            {
                if (!ci_equals(name, pSearchSpawn->szName))
                    return false;
            }
            else
            {
                if (ci_find_substr(name, pSearchSpawn->szName) == -1)
                {
                    const char* dispName = SpawnAccess::GetDisplayedName(pSpawn);
                    if (ci_find_substr(dispName, pSearchSpawn->szName) == -1)
                        return false;
                }
            }
        }
    }

    if (pSearchSpawn->bNamed && !IsNamed(pSpawn))
        return false;

    if (pSearchSpawn->bNoPet && (spawnType == PET || spawnType == MERCENARY))
        return false;

    if (pSearchSpawn->bKnownLocation)
    {
        float dX = SpawnAccess::GetX(pSpawn) - pSearchSpawn->xLoc;
        float dY = SpawnAccess::GetY(pSpawn) - pSearchSpawn->yLoc;
        float dist = sqrtf(dX * dX + dY * dY);
        if (dist > static_cast<float>(pSearchSpawn->FRadius))
            return false;
    }
    else if (pSearchSpawn->FRadius < 9999.0)
    {
        eqlib::PlayerClient* local = pLocalPlayer;
        if (local)
        {
            float dist = DistanceToSpawn(local, pSpawn);
            if (dist > static_cast<float>(pSearchSpawn->FRadius))
                return false;
        }
    }

    if (pSearchSpawn->ZRadius < 9999.0)
    {
        eqlib::PlayerClient* local = pLocalPlayer;
        if (local)
        {
            float dZ = fabsf(SpawnAccess::GetZ(local) - SpawnAccess::GetZ(pSpawn));
            if (dZ > static_cast<float>(pSearchSpawn->ZRadius))
                return false;
        }
    }

    return true;
}

bool SpawnMatchesSearch(MQSpawnSearch* pSearchSpawn, SPAWNINFO* /*pChar*/, SPAWNINFO* pSpawn)
{
    return SpawnMatchesSearch(pSearchSpawn, pSpawn);
}

char* FormatSearchSpawn(char* Buffer, size_t BufferSize, MQSpawnSearch* pSearchSpawn)
{
    if (!Buffer || !pSearchSpawn)
        return Buffer;

    Buffer[0] = '\0';

    if (pSearchSpawn->SpawnType != NONE)
    {
        const char* typeName = "any";
        switch (pSearchSpawn->SpawnType)
        {
        case PC:           typeName = "pc"; break;
        case NPC:          typeName = "npc"; break;
        case MOUNT:        typeName = "mount"; break;
        case PET:          typeName = "pet"; break;
        case CORPSE:       typeName = "corpse"; break;
        case TRIGGER:      typeName = "trigger"; break;
        case TRAP:         typeName = "trap"; break;
        case TIMER:        typeName = "timer"; break;
        case UNTARGETABLE: typeName = "untargetable"; break;
        case CHEST:        typeName = "chest"; break;
        case AURA:         typeName = "aura"; break;
        case OBJECT:       typeName = "object"; break;
        case BANNER:       typeName = "banner"; break;
        case CAMPFIRE:     typeName = "campfire"; break;
        case MERCENARY:    typeName = "mercenary"; break;
        case FLYER:        typeName = "flyer"; break;
        case NPCCORPSE:    typeName = "npccorpse"; break;
        case PCCORPSE:     typeName = "pccorpse"; break;
        default: break;
        }
        snprintf(Buffer, BufferSize, "%s", typeName);
    }

    if (pSearchSpawn->szName[0])
    {
        size_t len = strlen(Buffer);
        snprintf(Buffer + len, BufferSize - len, "%s%s",
            len > 0 ? " " : "", pSearchSpawn->szName);
    }

    if (pSearchSpawn->MinLevel > 0 || pSearchSpawn->MaxLevel < MAX_NPC_LEVEL)
    {
        size_t len = strlen(Buffer);
        snprintf(Buffer + len, BufferSize - len, "%srange %d %d",
            len > 0 ? " " : "", pSearchSpawn->MinLevel, pSearchSpawn->MaxLevel);
    }

    return Buffer;
}

// ---------------------------------------------------------------------------
// Ground item utilities
// ---------------------------------------------------------------------------

const char* GetFriendlyNameForGroundItem(EQGroundItem* pItem)
{
    if (!pItem)
        return "Unknown";
    return pItem->Name;
}

// ---------------------------------------------------------------------------
// Time utility
// ---------------------------------------------------------------------------

int MakeTime()
{
    return static_cast<int>(clock());
}

// ---------------------------------------------------------------------------
// INI file name (default for map plugin)
// ---------------------------------------------------------------------------

char INIFileName[MAX_STRING] = "MQ2Map.ini";
