/**
 * @file mq_compat.h
 * @brief MQ compatibility layer — exposes MQ-named APIs backed by our framework.
 * @date 2026-02-08
 *
 * Allows the MQ2Map plugin source to compile with minimal modifications.
 * We avoid including full eqlib game headers (they depend on imgui, mq/base)
 * and instead provide forward declarations, constants, and manual offset access.
 */

#pragma once

#include "game_state.h"
#include "commands.h"
#include "config.h"
#include "core.h"

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <string_view>
#include <algorithm>
#include <charconv>
#include <utility>

// ---------------------------------------------------------------------------
// A. Forward declarations, game struct definitions, and namespace
// ---------------------------------------------------------------------------

// eqlib types are forward-declared in game_state.h already:
//   PlayerClient, PlayerManagerClient, PcClient, CDisplay, CXWndManager, ZONEINFO
// We use these opaquely, accessing members via raw offsets in mq_compat.cpp.

namespace eqlib {
    class CMapViewWnd;
}

// ---------------------------------------------------------------------------
// Game struct definitions (matching exact memory layout)
// We define these ourselves since full eqlib headers can't be included.
// ---------------------------------------------------------------------------

struct CVector3
{
    CVector3() : X(0.f), Y(0.f), Z(0.f) {}
    CVector3(float x, float y, float z) : X(x), Y(y), Z(z) {}

    bool operator==(const CVector3& other) const
    {
        return X == other.X && Y == other.Y && Z == other.Z;
    }

    bool operator!=(const CVector3& other) const
    {
        return !(*this == other);
    }

    float X;
    float Y;
    float Z;
};

union ARGBCOLOR
{
    ARGBCOLOR() : ARGB(0) {}
    ARGBCOLOR(uint32_t argb) : ARGB(argb) {}
    struct
    {
        uint8_t Blue;
        uint8_t Green;
        uint8_t Red;
        uint8_t Alpha;
    };
    uint32_t ARGB;
};

// MapViewLabel — 0x38 bytes, matching eqlib UI.h layout
struct MapViewLabel
{
    uint32_t      LabelId = 0;      // 0x00
    MapViewLabel* pNext = nullptr;   // 0x04
    MapViewLabel* pPrev = nullptr;   // 0x08
    CVector3      Location;          // 0x0c (12 bytes)
    ARGBCOLOR     Color;             // 0x18 (4 bytes)
    int           Size = 0;          // 0x1c
    const char*   Label = nullptr;   // 0x20
    int           Layer = 0;         // 0x24
    int           Width = 0;         // 0x28
    int           Height = 0;        // 0x2c
    int           OffsetX = 0;       // 0x30
    int           OffsetY = 0;       // 0x34
    // total 0x38
};

// MapViewLine — 0x28 bytes, matching eqlib UI.h layout
struct MapViewLine
{
    MapViewLine*  pNext = nullptr;   // 0x00
    MapViewLine*  pPrev = nullptr;   // 0x04
    CVector3      Start;             // 0x08 (12 bytes)
    CVector3      End;               // 0x14 (12 bytes)
    ARGBCOLOR     Color;             // 0x20 (4 bytes)
    int           Layer = 0;         // 0x24
    // total 0x28
};

// EQGroundItem stub — 0x80 bytes, matching eqlib Objects.h layout
// We only define the fields we need; the rest is padding.
struct EQGroundItem
{
    /* 0x00 */ EQGroundItem* pPrev;
    /* 0x04 */ EQGroundItem* pNext;
    /* 0x08 */ uint8_t       _pad08[0x14];      // 0x08..0x1b (Item, DropID, ZoneID, DropSubID, pActor)
    /* 0x1c */ char          Name[0x40];         // 0x1c..0x5b
    /* 0x5c */ long          Expires;
    /* 0x60 */ float         Heading;
    /* 0x64 */ uint8_t       _pad64[0x0c];       // Pitch, Roll, Scale
    /* 0x70 */ float         Y;
    /* 0x74 */ float         X;
    /* 0x78 */ float         Z;
    /* 0x7c */ int           Weight;             // -1 = can't pick up
    // total 0x80
};

using GROUNDITEM  = EQGroundItem;
using PGROUNDITEM = EQGroundItem*;

// Put into eqlib namespace for compatibility with code that uses eqlib:: prefix
namespace eqlib {
    using ::MapViewLabel;
    using ::MapViewLine;
}

using namespace eqlib;

// ---------------------------------------------------------------------------
// B. Type aliases (MQ names -> eqlib names)
// ---------------------------------------------------------------------------

using SPAWNINFO  = eqlib::PlayerClient;
using PSPAWNINFO = eqlib::PlayerClient*;
using MAPLABEL   = eqlib::MapViewLabel;
using MAPLINE    = eqlib::MapViewLine;

// ---------------------------------------------------------------------------
// C. Global macros (map to GameState:: getters)
// ---------------------------------------------------------------------------

#define pLocalPlayer      GameState::GetLocalPlayer()
#define pTarget           GameState::GetTarget()
#define pControlledPlayer GameState::GetControlledPlayer()
#define pSpawnList        GameState::GetSpawnList()
#define pLocalPC          GameState::GetLocalPC()
#define pDisplay          GameState::GetDisplay()
#define pZoneInfo         GameState::GetZoneInfo()
#define pWndMgr           GameState::GetWndManager()
#define pMapViewWnd       ((eqlib::CMapViewWnd*)nullptr)

// ---------------------------------------------------------------------------
// Constants from eqlib that we need (avoids including full headers)
// ---------------------------------------------------------------------------

// Spawn types
#ifndef SPAWN_PLAYER
#define SPAWN_PLAYER  0
#define SPAWN_NPC     1
#define SPAWN_CORPSE  2
#endif

// Con colors (from ChatFilters.h — can't include due to mq/base/Color.h dep)
#ifndef CONCOLOR_GREY
#define CONCOLOR_GREY       0x06
#define CONCOLOR_GREEN      0x02
#define CONCOLOR_LIGHTBLUE  0x12
#define CONCOLOR_BLUE       0x04
#define CONCOLOR_BLACK      0x14
#define CONCOLOR_WHITE      0x0a
#define CONCOLOR_YELLOW     0x0f
#define CONCOLOR_RED        0x0d
#define COLOR_PURPLE        0x05
#endif

// NPC Level cap
#ifndef MAX_NPC_LEVEL
constexpr int MAX_NPC_LEVEL = 200;
#endif

// Max spawn name length
#ifndef EQ_MAX_NAME
constexpr int EQ_MAX_NAME = 0x40;
#endif

// Class constants
constexpr int MQ_Class_Object = 62;

// Race constants (for GetSpawnType body type classification)
constexpr int MQ_EQR_INVISIBLE_MAN   = 127;
constexpr int MQ_EQR_BANNER          = 500;
constexpr int MQ_EQR_SPIKE_TRAP      = 513;
constexpr int MQ_EQR_TOTEM           = 514;
constexpr int MQ_EQR_BANNER0         = 553;
constexpr int MQ_EQR_BANNER4         = 557;
constexpr int MQ_EQR_CAMPSITE        = 567;
constexpr int MQ_EQR_TCGBANNER       = 586;

// Character body type properties
enum {
    MQ_CharProp_None           = 0,
    MQ_CharProp_Construct      = 5,
    MQ_CharProp_Magical        = 7,
    MQ_CharProp_Untargetable   = 11,
    MQ_CharProp_Cursed         = 33,
    MQ_CharProp_Utility        = 100,
    MQ_CharProp_Trap           = 101,
    MQ_CharProp_Companion      = 102,
    MQ_CharProp_Suicide        = 103,
};

// String buffer size (used throughout)
constexpr int MAX_STRING = 2048;

// Game state constants
constexpr int GAMESTATE_POSTCHARSELECT = 3;
constexpr int GAMESTATE_INGAME = 5;

// Math constant
constexpr float PI = 3.14159265358979f;

// PLUGIN_API — empty in our proxy build
#define PLUGIN_API

// ---------------------------------------------------------------------------
// D1. test_and_set utility template (replaces mq::test_and_set)
// ---------------------------------------------------------------------------

template <typename T, typename U>
bool test_and_set(T& target, U&& value)
{
    if (target == value)
        return false;
    target = std::forward<U>(value);
    return true;
}

// ---------------------------------------------------------------------------
// D. eSpawnType enum (MQ-specific, not in eqlib)
// ---------------------------------------------------------------------------

enum eSpawnType
{
    NONE = 0,
    PC,
    MOUNT,
    PET,
    PCPET,
    NPCPET,
    XTARHATER,
    NPC,
    CORPSE,
    TRIGGER,
    TRAP,
    TIMER,
    UNTARGETABLE,
    CHEST,
    ITEM,
    AURA,
    OBJECT,
    BANNER,
    CAMPFIRE,
    MERCENARY,
    FLYER,
    NPCCORPSE = 2000,
    PCCORPSE,
};

// ---------------------------------------------------------------------------
// E. MQColor class (stripped of ImGui deps, layout matches eqlib ARGBCOLOR)
// ---------------------------------------------------------------------------

class MQColor
{
public:
    constexpr MQColor()
        : Blue(0), Green(0), Red(0), Alpha(255) {}

    constexpr MQColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
        : Blue(blue), Green(green), Red(red), Alpha(alpha) {}

    constexpr MQColor(const MQColor& other)
        : ARGB(other.ARGB) {}

    struct argb_t {};
    static const inline argb_t format_argb;

    constexpr MQColor(argb_t, uint32_t argbcolor)
        : ARGB(argbcolor) {}

    constexpr MQColor(uint32_t argbcolor)
        : ARGB(argbcolor) {}

    constexpr MQColor& operator=(uint32_t argbcolor) { ARGB = argbcolor; return *this; }
    constexpr MQColor& operator=(const MQColor& other) { ARGB = other.ARGB; return *this; }

    constexpr explicit operator uint32_t() const { return ToARGB(); }

    constexpr uint32_t ToARGB() const { return ARGB; }
    constexpr void SetARGB(uint32_t value) { ARGB = value; }
    constexpr uint32_t ToRGB() const { return ARGB & 0xFFFFFF; }

    constexpr void Invert()
    {
        ARGB = (0xFFFFFF - (ARGB & 0xFFFFFF)) | (ARGB & 0xFF000000);
    }

    constexpr MQColor GetInverted() const
    {
        return MQColor((0xFFFFFF - (ARGB & 0xFFFFFF)) | (ARGB & 0xFF000000));
    }

    union
    {
        struct
        {
            uint8_t Blue;
            uint8_t Green;
            uint8_t Red;
            uint8_t Alpha;
        };
        uint32_t ARGB = 0;
    };
};

inline bool operator==(const MQColor& l, const MQColor& r) { return l.ARGB == r.ARGB; }
inline bool operator!=(const MQColor& l, const MQColor& r) { return l.ARGB != r.ARGB; }

// ---------------------------------------------------------------------------
// F. MQSpawnSearch struct (simplified — only fields the map uses)
// ---------------------------------------------------------------------------

enum class SearchSortBy { Level = 0, Name, Race, Class, Distance, Guild, Id };

struct MQSpawnSearch
{
    int          MinLevel   = 0;
    int          MaxLevel   = MAX_NPC_LEVEL;
    eSpawnType   SpawnType  = NONE;
    uint32_t     SpawnID    = 0;
    uint32_t     FromSpawnID = 0;
    float        Radius     = 0;
    char         szName[2048] = { 0 };
    char         szBodyType[2048] = { 0 };
    char         szRace[2048] = { 0 };
    char         szClass[2048] = { 0 };
    bool         bSpawnID   = false;
    bool         bNotNearAlert = false;
    bool         bNearAlert = false;
    bool         bNoAlert   = false;
    bool         bAlert     = false;
    bool         bLFG       = false;
    bool         bTrader    = false;
    bool         bTargNext  = false;
    bool         bTargPrev  = false;
    bool         bGroup     = false;
    bool         bNoGroup   = false;
    bool         bRaid      = false;
    bool         bGM        = false;
    bool         bNamed     = false;
    bool         bMerchant  = false;
    bool         bBanker    = false;
    bool         bTank      = false;
    bool         bHealer    = false;
    bool         bDps       = false;
    bool         bSlower    = false;
    bool         bAura      = false;
    bool         bBanner    = false;
    bool         bCampfire  = false;
    bool         bXTarHater = false;
    bool         bNoPet     = false;
    bool         bExactName = false;
    bool         bTargetable = false;
    bool         bKnownLocation = false;
    bool         bLoS       = false;
    bool         bNoGuild   = false;
    uint32_t     NotID      = 0;
    uint32_t     NotNearAlertList = 0;
    uint32_t     NearAlertList = 0;
    uint32_t     NoAlertList = 0;
    uint32_t     AlertList  = 0;
    double       ZRadius    = 10000.0;
    double       FRadius    = 10000.0;
    float        xLoc       = 0;
    float        yLoc       = 0;
    float        zLoc       = 0;
    uint32_t     PlayerState = 0;
    SearchSortBy SortBy     = SearchSortBy::Level;
};

void ClearSearchSpawn(MQSpawnSearch* pSearchSpawn);
void ParseSearchSpawn(const char* Buffer, MQSpawnSearch* pSearchSpawn);
bool SpawnMatchesSearch(MQSpawnSearch* pSearchSpawn, SPAWNINFO* pSpawn);
bool SpawnMatchesSearch(MQSpawnSearch* pSearchSpawn, SPAWNINFO* pChar, SPAWNINFO* pSpawn);
char* FormatSearchSpawn(char* Buffer, size_t BufferSize, MQSpawnSearch* pSearchSpawn);

// ---------------------------------------------------------------------------
// G. Config wrappers (bare function names -> Config:: namespace)
// ---------------------------------------------------------------------------

// Win32 defines GetPrivateProfileInt/String as A/W macros — must undef
#undef GetPrivateProfileInt
#undef GetPrivateProfileString
#undef WritePrivateProfileInt
#undef WritePrivateProfileString

inline bool GetPrivateProfileBool(const char* s, const char* k, bool d, const char* f)
{
    return Config::GetBool(s, k, d, f);
}

inline int GetPrivateProfileInt(const char* s, const char* k, int d, const char* f)
{
    return Config::GetInt(s, k, d, f);
}

inline float GetPrivateProfileFloat(const char* s, const char* k, float d, const char* f)
{
    return Config::GetFloat(s, k, d, f);
}

inline std::string GetPrivateProfileString(const char* s, const char* k, const char* d, const char* f)
{
    return Config::GetString(s, k, d, f);
}

inline void WritePrivateProfileBool(const char* s, const char* k, bool v, const char* f)
{
    Config::WriteBool(s, k, v, f);
}

inline void WritePrivateProfileInt(const char* s, const char* k, int v, const char* f)
{
    Config::WriteInt(s, k, v, f);
}

inline void WritePrivateProfileFloat(const char* s, const char* k, float v, const char* f)
{
    Config::WriteFloat(s, k, v, f);
}

inline void WritePrivateProfileString(const char* s, const char* k, const char* v, const char* f)
{
    Config::WriteString(s, k, v, f);
}

// ---------------------------------------------------------------------------
// H. Command wrappers
// ---------------------------------------------------------------------------

inline void AddCommand(const char* cmd, CommandHandler handler, bool = false, bool = true, bool = false)
{
    Commands::AddCommand(cmd, handler);
}

inline void RemoveCommand(const char* cmd)
{
    Commands::RemoveCommand(cmd);
}

// ---------------------------------------------------------------------------
// Spawn raw offset access helpers (declared here, implemented in mq_compat.cpp)
//
// Since we can't include eqlib game headers, we access PlayerClient members
// via raw pointer arithmetic using known ROF2 offsets.
// ---------------------------------------------------------------------------

namespace SpawnAccess
{
    // PlayerBase offsets (PlayerBase inherits TListNode<PlayerClient> + CActorApplicationData)
    // TListNode<T>: +0x00 m_pPrev, +0x04 m_pNext, +0x08 m_pList => 0x0C bytes
    // CActorApplicationData: vtable => 0x04 bytes
    // Total prefix = 0x10, then PlayerBase members start

    // PlayerBase layout (from eqlib PlayerClient.h):
    //   +0x038 Lastname[0x20]
    //   +0x064 Y, +0x068 X, +0x06c Z
    //   +0x0a4 Name[EQ_MAX_NAME]     (0x40 bytes)
    //   +0x0e4 DisplayedName[EQ_MAX_NAME]
    //   +0x125 Type (uint8_t)
    //   +0x128 Properties (CharacterPropertyHash)
    //   +0x13c Height
    //   +0x148 SpawnID (uint32_t)
    //   +0x158 Rider (PlayerClient*)

    // PlayerZoneClient additional (offsets continue from PlayerBase):
    //   +0x0208 Mercenary (bool)
    //   +0x0250 Level (uint8_t)
    //   +0x038c MasterID (uint32_t)

    const char* GetName(SPAWNINFO* pSpawn);
    const char* GetDisplayedName(SPAWNINFO* pSpawn);
    const char* GetLastname(SPAWNINFO* pSpawn);
    uint8_t     GetType(SPAWNINFO* pSpawn);
    uint8_t     GetLevel(SPAWNINFO* pSpawn);
    uint32_t    GetSpawnID(SPAWNINFO* pSpawn);
    float       GetY(SPAWNINFO* pSpawn);
    float       GetX(SPAWNINFO* pSpawn);
    float       GetZ(SPAWNINFO* pSpawn);
    float       GetHeight(SPAWNINFO* pSpawn);
    SPAWNINFO*  GetRider(SPAWNINFO* pSpawn);
    uint32_t    GetMasterID(SPAWNINFO* pSpawn);
    bool        GetMercenary(SPAWNINFO* pSpawn);
    SPAWNINFO*  GetNext(SPAWNINFO* pSpawn);
    int         GetClass(SPAWNINFO* pSpawn);
    int         GetRace(SPAWNINFO* pSpawn);
    float       GetHeading(SPAWNINFO* pSpawn);
    int         GetDeity(SPAWNINFO* pSpawn);
    int         GetHPCurrent(SPAWNINFO* pSpawn);
    float       GetSpeedRun(SPAWNINFO* pSpawn);
    float       GetSpeedX(SPAWNINFO* pSpawn);
    float       GetSpeedY(SPAWNINFO* pSpawn);
    const char* GetRaceString(SPAWNINFO* pSpawn);
    const char* GetClassString(SPAWNINFO* pSpawn);
    const char* GetClassThreeLetterCode(SPAWNINFO* pSpawn);
}

// ---------------------------------------------------------------------------
// I. Spawn utility function declarations (implemented in mq_compat.cpp)
// ---------------------------------------------------------------------------

eSpawnType GetSpawnType(SPAWNINFO* pSpawn);
int GetBodyType(SPAWNINFO* pSpawn);
int ConColor(SPAWNINFO* pSpawn);
uint32_t ConColorToARGB(int conColor);
bool IsNamed(SPAWNINFO* pSpawn);
SPAWNINFO* GetSpawnByID(uint32_t spawnID);
float DistanceToSpawn(SPAWNINFO* pFrom, SPAWNINFO* pTo);
float get_melee_range(SPAWNINFO* pSpawn1, SPAWNINFO* pSpawn2);

const char* GetFriendlyNameForGroundItem(EQGroundItem* pItem);
int MakeTime();

extern char INIFileName[MAX_STRING];

// ---------------------------------------------------------------------------
// J. String utility function declarations (implemented in mq_compat.cpp)
// ---------------------------------------------------------------------------

const char* GetArg(char* szDest, const char* szSrc, int dwNumber,
    bool LeaveQuotes = false, bool ToParen = false, bool CSV = false,
    char Separator = 0, bool AnyNonAlphaNum = false);
const char* GetNextArg(const char* szLine, int dwNumber = 1,
    bool CSV = false, char Separator = 0);
int GetIntFromString(const char* str, int defaultVal);
float GetFloatFromString(const char* str, float defaultVal);
bool ci_equals(std::string_view sv1, std::string_view sv2);
bool ci_equals(std::string_view haystack, std::string_view needle, bool isExact);
int ci_find_substr(std::string_view haystack, std::string_view needle);

// ---------------------------------------------------------------------------
// K. Chat/command forwarding (inline)
// ---------------------------------------------------------------------------

inline void SyntaxError(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    WriteChatColor(buf, CONCOLOR_YELLOW);
}

inline void MacroError(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    WriteChatColor(buf, CONCOLOR_RED);
}

inline void EzCommand(const char* szCommand)
{
    Core::ExecuteCommand(szCommand);
}

inline void DoCommand(SPAWNINFO* /*pChar*/, const char* szCommand)
{
    Core::ExecuteCommand(szCommand);
}

// ---------------------------------------------------------------------------
// L. No-op stubs (inline)
// ---------------------------------------------------------------------------

inline uint32_t AddMQ2Benchmark(const char*) { return 0; }
inline void RemoveMQ2Benchmark(uint32_t) {}
inline void EnterMQ2Benchmark(uint32_t) {}
inline void ExitMQ2Benchmark(uint32_t) {}

inline bool AddMQ2Data(const char*, ...) { return false; }
inline bool RemoveMQ2Data(const char*) { return false; }

inline void AddSettingsPanel(const char*, ...) {}
inline void RemoveSettingsPanel(const char*) {}

// ---------------------------------------------------------------------------
// M. (MAX_STRING moved to top of file, before first use)
// ---------------------------------------------------------------------------
