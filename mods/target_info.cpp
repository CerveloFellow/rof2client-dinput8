/**
 * @file target_info.cpp
 * @brief TargetInfo mod implementation — port of MQ2TargetInfo.
 * @date 2026-02-12
 *
 * Adds overlays to the EQ target window: level/race/class info, distance,
 * line-of-sight indicator, and placeholder (PH) button with web links.
 *
 * Key differences from MQ2TargetInfo:
 * - Uses raw offset access instead of eqlib class headers
 * - GameCXStr wrapper for passing strings to game UI functions
 * - HandleBuffRemoveRequest hook captures pTargetWnd pointer
 * - SEH protection on all game memory access
 */

#include "pch.h"
#include "target_info.h"
#include "../mq_compat.h"
#include "../hooks.h"

#include <eqlib/Offsets.h>
#include <eqlib/offsets/eqgame.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <shellapi.h>

// ---------------------------------------------------------------------------
// CXWnd offset constants (from eqlib CXWnd.h analysis)
// All offsets relative to CXWnd base pointer.
// ---------------------------------------------------------------------------

namespace WndOff
{
    constexpr uintptr_t ClipToParent          = 0x018;
    constexpr uintptr_t BottomAnchoredToTop   = 0x021;
    constexpr uintptr_t UseInLayoutHorizontal = 0x02C;
    constexpr uintptr_t RightAnchoredToLeft   = 0x040;
    constexpr uintptr_t Location              = 0x060; // CXRect: 4 ints (left,top,right,bottom)
    constexpr uintptr_t WindowStyle           = 0x084;
    constexpr uintptr_t Tooltip               = 0x0E8; // CXStr (4 bytes = pointer to CStrRep)
    constexpr uintptr_t UseInLayoutVertical   = 0x0F4;
    constexpr uintptr_t RightOffset           = 0x0FC;
    constexpr uintptr_t BottomOffset          = 0x100;
    constexpr uintptr_t LeftAnchoredToLeft    = 0x104;
    constexpr uintptr_t BGColor               = 0x128; // COLORREF (uint32_t)
    constexpr uintptr_t CRNormal              = 0x12C; // COLORREF (uint32_t)
    constexpr uintptr_t TopAnchoredToTop      = 0x150;
    constexpr uintptr_t LeftOffset            = 0x184;
    constexpr uintptr_t NeedsSaving           = 0x195;
    constexpr uintptr_t Visible               = 0x196; // bool dShow
    constexpr uintptr_t WindowText            = 0x1A8; // CXStr
    constexpr uintptr_t TopOffset             = 0x1D0;
    constexpr uintptr_t ParentWindow          = 0x174;
    constexpr uintptr_t ClientRectChanged     = 0x075;
}

// CLabelWnd-specific offsets (after CXWnd at 0x1D8)
namespace LabelOff
{
    constexpr uintptr_t bNoWrap      = 0x1D8;
    constexpr uintptr_t bAlignRight  = 0x1D9;
    constexpr uintptr_t bAlignCenter = 0x1DA;
}

// CButtonWnd-specific offsets (after CXWnd at 0x1D8)
namespace ButtonOff
{
    constexpr uintptr_t Checked   = 0x1E4;
    constexpr uintptr_t DecalTint = 0x1F8;
}

// CControlTemplate offsets (CScreenPieceTemplate base)
namespace TmplOff
{
    constexpr uintptr_t strName      = 0x20; // CXStr
    constexpr uintptr_t strScreenId  = 0x28; // CXStr
    constexpr uintptr_t nFont        = 0x2C; // uint32_t
    constexpr uintptr_t uStyleBits   = 0x80; // uint32_t
    constexpr uintptr_t strController = 0x90; // CXStr
}

// CSidlScreenWnd offsets
namespace SidlOff
{
    constexpr uintptr_t IniStorageName = 0x1FC; // CXStr
}

// Window style flags
constexpr uint32_t WSF_TITLEBAR       = 0x00000004;
constexpr uint32_t WSF_CLIENTMOVABLE  = 0x00000200;
constexpr uint32_t WSF_TRANSPARENT    = 0x00000010;
constexpr uint32_t WSF_SIZABLE        = 0x00000040;
constexpr uint32_t WSF_BORDER         = 0x00000100;
constexpr uint32_t WSF_AUTOSTRETCHH   = 0x00400000;
constexpr uint32_t WSF_AUTOSTRETCHV   = 0x00800000;
constexpr uint32_t WSF_RELATIVERECT   = 0x00200000;

// ---------------------------------------------------------------------------
// GameCXStr — minimal CXStr-compatible wrapper for game UI calls
//
// Allocates CStrRep via HeapAlloc with a high refcount so the game
// never tries to free our allocations. This intentionally leaks ~64 bytes
// per string creation. Since SetWindowText is called at most 2x/second
// per label, the leak is bounded (~1MB/hour). Replace with proper CXStr
// allocator integration for production use.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct GameCXStrRep
{
    volatile long refCount;  // 0x00
    uint32_t alloc;          // 0x04
    uint32_t length;         // 0x08
    int      encoding;       // 0x0C  (0 = UTF8)
    void*    freeList;       // 0x10
    // 0x14: string data follows (variable length)
};
#pragma pack(pop)

static void* AllocGameStr(const char* text)
{
    if (!text) text = "";
    uint32_t len = (uint32_t)strlen(text);
    uint32_t allocSize = sizeof(GameCXStrRep) + len + 1;
    auto* rep = (GameCXStrRep*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocSize);
    if (!rep) return nullptr;
    rep->refCount = 1000000; // High refcount — never reaches 0, never freed by game
    rep->alloc = len + 1;
    rep->length = len;
    rep->encoding = 0; // UTF8
    rep->freeList = nullptr;
    char* data = reinterpret_cast<char*>(rep) + 0x14;
    memcpy(data, text, len);
    data[len] = '\0';
    return rep;
}

// Read a CXStr at a given offset: returns the C string data, or "" if null.
static const char* ReadCXStr(void* base, uintptr_t offset)
{
    if (!base) return "";
    void* rep = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(base) + offset);
    if (!rep) return "";
    return reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(rep) + 0x14);
}

// Write a CXStr at a given offset (leaks old value — acceptable for init-time use)
static void WriteCXStr(void* base, uintptr_t offset, const char* text)
{
    if (!base) return;
    void* rep = AllocGameStr(text);
    *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(base) + offset) = rep;
}

// ---------------------------------------------------------------------------
// Offset-based CXWnd helpers
// ---------------------------------------------------------------------------

template<typename T>
static inline T WndRead(void* pWnd, uintptr_t offset)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pWnd) + offset);
}

template<typename T>
static inline void WndWrite(void* pWnd, uintptr_t offset, T value)
{
    *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(pWnd) + offset) = value;
}

static void WndSetVisible(void* pWnd, bool vis)          { if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::Visible, vis ? 1 : 0); }
static bool WndIsVisible(void* pWnd)                     { return pWnd && WndRead<uint8_t>(pWnd, WndOff::Visible) != 0; }
static void WndSetTopOffset(void* pWnd, int v)           { if (pWnd) WndWrite<int>(pWnd, WndOff::TopOffset, v); }
static int  WndGetTopOffset(void* pWnd)                  { return pWnd ? WndRead<int>(pWnd, WndOff::TopOffset) : 0; }
static void WndSetBottomOffset(void* pWnd, int v)        { if (pWnd) WndWrite<int>(pWnd, WndOff::BottomOffset, v); }
static int  WndGetBottomOffset(void* pWnd)               { return pWnd ? WndRead<int>(pWnd, WndOff::BottomOffset) : 0; }
static void WndSetLeftOffset(void* pWnd, int v)          { if (pWnd) WndWrite<int>(pWnd, WndOff::LeftOffset, v); }
static void WndSetRightOffset(void* pWnd, int v)         { if (pWnd) WndWrite<int>(pWnd, WndOff::RightOffset, v); }
static void WndSetCRNormal(void* pWnd, uint32_t c)       { if (pWnd) WndWrite<uint32_t>(pWnd, WndOff::CRNormal, c); }
static void WndSetBGColor(void* pWnd, uint32_t c)        { if (pWnd) WndWrite<uint32_t>(pWnd, WndOff::BGColor, c); }
static uint32_t WndGetWindowStyle(void* pWnd)             { return pWnd ? WndRead<uint32_t>(pWnd, WndOff::WindowStyle) : 0; }
static void WndSetWindowStyle(void* pWnd, uint32_t s)    { if (pWnd) WndWrite<uint32_t>(pWnd, WndOff::WindowStyle, s); }
static void WndAddStyle(void* pWnd, uint32_t s)          { if (pWnd) WndWrite<uint32_t>(pWnd, WndOff::WindowStyle, WndGetWindowStyle(pWnd) | s); }
static void WndSetLeftAnchoredToLeft(void* pWnd, bool v) { if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::LeftAnchoredToLeft, v ? 1 : 0); }
static void WndSetRightAnchoredToLeft(void* pWnd, bool v){ if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::RightAnchoredToLeft, v ? 1 : 0); }
static void WndSetTopAnchoredToTop(void* pWnd, bool v)   { if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::TopAnchoredToTop, v ? 1 : 0); }
static void WndSetBottomAnchoredToTop(void* pWnd, bool v){ if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::BottomAnchoredToTop, v ? 1 : 0); }
static void WndSetClipToParent(void* pWnd, bool v)       { if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::ClipToParent, v ? 1 : 0); }
static void WndSetUseInLayoutV(void* pWnd, bool v)       { if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::UseInLayoutVertical, v ? 1 : 0); }
static void WndSetUseInLayoutH(void* pWnd, bool v)       { if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::UseInLayoutHorizontal, v ? 1 : 0); }
static void WndSetNeedsSaving(void* pWnd, bool v)        { if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::NeedsSaving, v ? 1 : 0); }
static void WndSetClientRectChanged(void* pWnd, bool v)  { if (pWnd) WndWrite<uint8_t>(pWnd, WndOff::ClientRectChanged, v ? 1 : 0); }

// Set Location CXRect (left, top, right, bottom) at offset 0x060
static void WndSetLocation(void* pWnd, int left, int top, int right, int bottom)
{
    if (!pWnd) return;
    uintptr_t base = reinterpret_cast<uintptr_t>(pWnd) + WndOff::Location;
    reinterpret_cast<int*>(base)[0] = left;
    reinterpret_cast<int*>(base)[1] = top;
    reinterpret_cast<int*>(base)[2] = right;
    reinterpret_cast<int*>(base)[3] = bottom;
}

// CLabelWnd helpers
static void LabelSetNoWrap(void* pWnd, bool v)      { if (pWnd) WndWrite<uint8_t>(pWnd, LabelOff::bNoWrap, v ? 1 : 0); }
static void LabelSetAlignRight(void* pWnd, bool v)   { if (pWnd) WndWrite<uint8_t>(pWnd, LabelOff::bAlignRight, v ? 1 : 0); }
static void LabelSetAlignCenter(void* pWnd, bool v)  { if (pWnd) WndWrite<uint8_t>(pWnd, LabelOff::bAlignCenter, v ? 1 : 0); }

// ---------------------------------------------------------------------------
// Game function pointers (resolved lazily)
// ---------------------------------------------------------------------------

// CSidlManagerBase::FindScreenPieceTemplate(const CXStr& name) — thiscall
using FindScreenPieceTemplate_t = void*(__fastcall*)(void* thisPtr, void* edx, const void* pCXStr);
static FindScreenPieceTemplate_t s_FindScreenPieceTemplate = nullptr;

// CSidlManagerBase::CreateXWndFromTemplate(CXWnd* parent, CControlTemplate* tmpl) — thiscall
using CreateXWndFromTemplate_t = void*(__fastcall*)(void* thisPtr, void* edx, void* parent, void* tmpl);
static CreateXWndFromTemplate_t s_CreateXWndFromTemplate = nullptr;

// CSidlScreenWnd::GetChildItem(const CXStr& name) — thiscall
using GetChildItem_t = void*(__fastcall*)(void* thisPtr, void* edx, const void* pCXStr);
static GetChildItem_t s_GetChildItem = nullptr;

// CXWnd::Destroy() — thiscall, returns int
using DestroyWnd_t = int(__fastcall*)(void* thisPtr, void* edx);
static DestroyWnd_t s_DestroyWnd = nullptr;

// CXWnd::SetWindowText(const CXStr&) — virtual at vtable index 73 (offset 0x124)
// We call via vtable to ensure correct dispatch for CLabelWnd override.
static void CallSetWindowText(void* pWnd, const char* text)
{
    if (!pWnd) return;
    __try
    {
        void* rep = AllocGameStr(text);
        // CXStr is 4 bytes (pointer to CStrRep). Build one on the stack.
        void* cxstr = rep;
        void** vtable = *reinterpret_cast<void***>(pWnd);
        using Fn = void(__fastcall*)(void*, void*, const void*);
        auto fn = reinterpret_cast<Fn>(vtable[73]); // SetWindowText is vtable slot 73
        fn(pWnd, nullptr, &cxstr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogFramework("TargetInfo: SetWindowText EXCEPTION on wnd 0x%p", pWnd);
    }
}

// CXWnd::UpdateLayout(bool finish) — virtual at vtable index 88 (offset 0x160)
static void CallUpdateLayout(void* pWnd)
{
    if (!pWnd) return;
    __try
    {
        void** vtable = *reinterpret_cast<void***>(pWnd);
        using Fn = void(__fastcall*)(void*, void*, int);
        auto fn = reinterpret_cast<Fn>(vtable[88]);
        fn(pWnd, nullptr, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// PlayerBase::CanSee(const PlayerBase&) — thiscall
using CanSee_t = bool(__fastcall*)(void* thisPtr, void* edx, void* pOther);
static CanSee_t s_CanSee = nullptr;

// CTargetWnd::HandleBuffRemoveRequest(CXWnd*) — thiscall
using HandleBuffRemoveRequest_t = void(__fastcall*)(void* thisPtr, void* edx, void* pWnd);
static HandleBuffRemoveRequest_t s_HandleBuffRemoveRequest_Original = nullptr;

static bool s_funcPtrsResolved = false;

static void ResolveTargetInfoFuncPtrs()
{
    if (s_funcPtrsResolved) return;
    s_funcPtrsResolved = true;

    s_FindScreenPieceTemplate = reinterpret_cast<FindScreenPieceTemplate_t>(
        eqlib::FixEQGameOffset(CSidlManagerBase__FindScreenPieceTemplate_x));
    s_CreateXWndFromTemplate = reinterpret_cast<CreateXWndFromTemplate_t>(
        eqlib::FixEQGameOffset(CSidlManagerBase__CreateXWndFromTemplate_x));
    s_GetChildItem = reinterpret_cast<GetChildItem_t>(
        eqlib::FixEQGameOffset(CSidlScreenWnd__GetChildItem_x));
    s_DestroyWnd = reinterpret_cast<DestroyWnd_t>(
        eqlib::FixEQGameOffset(CXWnd__Destroy_x));
    s_CanSee = reinterpret_cast<CanSee_t>(
        eqlib::FixEQGameOffset(PlayerBase__CanSee_x));
    s_HandleBuffRemoveRequest_Original = reinterpret_cast<HandleBuffRemoveRequest_t>(
        eqlib::FixEQGameOffset(CTargetWnd__HandleBuffRemoveRequest_x));

    LogFramework("TargetInfo func ptrs resolved:");
    LogFramework("  FindScreenPieceTemplate = 0x%08X", (unsigned)(uintptr_t)s_FindScreenPieceTemplate);
    LogFramework("  CreateXWndFromTemplate  = 0x%08X", (unsigned)(uintptr_t)s_CreateXWndFromTemplate);
    LogFramework("  GetChildItem            = 0x%08X", (unsigned)(uintptr_t)s_GetChildItem);
    LogFramework("  DestroyWnd              = 0x%08X", (unsigned)(uintptr_t)s_DestroyWnd);
    LogFramework("  CanSee                  = 0x%08X", (unsigned)(uintptr_t)s_CanSee);
    LogFramework("  HandleBuffRemoveRequest = 0x%08X", (unsigned)(uintptr_t)s_HandleBuffRemoveRequest_Original);
}

// Wrappers for resolved function pointers
static void* CallFindTemplate(const char* name)
{
    void* pSidlMgr = GameState::GetSidlManager();
    if (!pSidlMgr || !s_FindScreenPieceTemplate) return nullptr;
    __try
    {
        void* rep = AllocGameStr(name);
        void* cxstr = rep;
        return s_FindScreenPieceTemplate(pSidlMgr, nullptr, &cxstr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void* CallCreateWndFromTemplate(void* parent, void* tmpl)
{
    void* pSidlMgr = GameState::GetSidlManager();
    if (!pSidlMgr || !s_CreateXWndFromTemplate || !parent || !tmpl) return nullptr;
    __try
    {
        return s_CreateXWndFromTemplate(pSidlMgr, nullptr, parent, tmpl);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void* CallGetChildItem(void* pWnd, const char* name)
{
    if (!pWnd || !s_GetChildItem) return nullptr;
    __try
    {
        void* rep = AllocGameStr(name);
        void* cxstr = rep;
        return s_GetChildItem(pWnd, nullptr, &cxstr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void CallDestroyWnd(void* pWnd)
{
    if (!pWnd || !s_DestroyWnd) return;
    __try { s_DestroyWnd(pWnd, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Module state (file-scope)
// ---------------------------------------------------------------------------

static void* s_pTargetWnd = nullptr;      // CTargetWnd* — captured from hook or scan
static bool  s_initialized = false;
static bool  s_disabledBadUI = false;

// UI overlay elements
static void* s_pInfoLabel = nullptr;      // CLabelWnd*
static void* s_pDistanceLabel = nullptr;  // CLabelWnd*
static void* s_pCanSeeLabel = nullptr;    // CLabelWnd*
static void* s_pPHButton = nullptr;       // CButtonWnd*

// Saved original child window state
static void* s_pBuffWindow = nullptr;
static void* s_pAggroPctPlayerLabel = nullptr;
static void* s_pAggroNameSecLabel = nullptr;
static void* s_pAggroPctSecLabel = nullptr;

static int s_buffWndTopOffsetOld = 50;
static int s_aggroPctPlayerTopOld = 33, s_aggroPctPlayerBottomOld = 47;
static int s_aggroNameSecTopOld = 33, s_aggroNameSecBottomOld = 47;
static int s_aggroPctSecTopOld = 33, s_aggroPctSecBottomOld = 47;
static uint32_t s_orgTargetWindStyle = 0;
static void* s_oldSpawn = nullptr;

// Config settings
static bool s_showDistance = true;
static bool s_showTargetInfo = true;
static bool s_showPlaceholder = true;
static bool s_showAnon = true;
static bool s_showSight = true;
static bool s_usePerCharSettings = false;

static int s_buffWndTopOffset = 76;
static int s_dTopOffset = 60;
static int s_dBottomOffset = 74;
static int s_dLeftOffset = 50;
static int s_canSeeTopOffset = 47;
static int s_canSeeBottomOffset = 61;
static int s_targetInfoWindowStyle = 0;
static int s_targetInfoAnchoredToRight = 0;
static std::string s_manaLabelName = "Player_ManaLabel";
static std::string s_fatigueLabelName = "Player_FatigueLabel";
static std::string s_targetInfoLoc = "34,48,0,40";
static std::string s_targetDistanceLoc = "34,48,90,0";

// PH database
static std::recursive_mutex s_phMutex;
static std::map<std::string, struct PHInfo> s_phMap;

struct PHInfo
{
    std::string Expansion;
    std::string Zone;
    std::string Named;
    std::string Link;
};

static char s_INIFileName[MAX_STRING] = "TargetInfo.ini";

// ---------------------------------------------------------------------------
// SpawnAccess additions for TargetInfo
// ---------------------------------------------------------------------------

namespace SpawnOffsets_TI
{
    constexpr uintptr_t Anon = 0x02B8; // int — 0=normal, 1=anon, 2=roleplay
}

static int GetSpawnAnon(void* pSpawn)
{
    if (!pSpawn) return 0;
    return *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pSpawn) + SpawnOffsets_TI::Anon);
}

static float Distance3DToSpawn(void* pFrom, void* pTo)
{
    if (!pFrom || !pTo) return 0.0f;
    float dX = SpawnAccess::GetX((SPAWNINFO*)pFrom) - SpawnAccess::GetX((SPAWNINFO*)pTo);
    float dY = SpawnAccess::GetY((SPAWNINFO*)pFrom) - SpawnAccess::GetY((SPAWNINFO*)pTo);
    float dZ = SpawnAccess::GetZ((SPAWNINFO*)pFrom) - SpawnAccess::GetZ((SPAWNINFO*)pTo);
    return sqrtf(dX * dX + dY * dY + dZ * dZ);
}

static bool SpawnCanSee(void* pFrom, void* pTo)
{
    if (!pFrom || !pTo || !s_CanSee) return false;
    __try
    {
        return s_CanSee(pFrom, nullptr, pTo);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// PH database loading
// ---------------------------------------------------------------------------

static void LoadPHs(const char* filePath)
{
    std::scoped_lock lock(s_phMutex);
    s_phMap.clear();

    FILE* fp = _fsopen(filePath, "rb", _SH_DENYNO);
    if (!fp)
    {
        LogFramework("TargetInfo: Could not open PH file: %s", filePath);
        return;
    }

    PHInfo phinf;
    char szBuffer[MAX_STRING] = { 0 };
    while (fgets(szBuffer, MAX_STRING, fp) != nullptr)
    {
        std::string phs;
        char* pDest = strchr(szBuffer, '^');
        if (!pDest) continue;
        *pDest++ = '\0';
        phinf.Named = szBuffer;

        char* pDest2 = strchr(pDest, '^');
        if (!pDest2) continue;
        *pDest2++ = '\0';
        phs = pDest;

        pDest = strchr(pDest2, '^');
        if (!pDest) continue;
        *pDest++ = '\0';
        phinf.Expansion = pDest2;

        pDest2 = strchr(pDest, '^');
        if (!pDest2) continue;
        *pDest2++ = '\0';
        phinf.Zone = pDest;

        // Trim trailing whitespace
        char* end = pDest2 + strlen(pDest2) - 1;
        while (end > pDest2 && (*end == '\r' || *end == '\n')) { *end-- = '\0'; }
        phinf.Link = pDest2;

        // Split comma-separated PH names (except known multi-word names)
        if (phs.find(",") != std::string::npos
            && phs.find("Yikkarvi,") == std::string::npos
            && phs.find("Furg,") == std::string::npos
            && phs.find("Tykronar,") == std::string::npos
            && phs.find("Ejarld,") == std::string::npos
            && phs.find("Grald,") == std::string::npos
            && phs.find("Graluk,") == std::string::npos)
        {
            size_t commapos;
            while ((commapos = phs.find_last_of(",")) != std::string::npos)
            {
                std::string temp = phs.substr(commapos + 2);
                phs.erase(commapos);
                s_phMap[temp] = phinf;
            }
            s_phMap[phs] = phinf;
        }
        else
        {
            s_phMap[phs] = phinf;
        }
    }
    fclose(fp);

    LogFramework("TargetInfo: Loaded %u PH entries", (unsigned)s_phMap.size());
}

static bool GetPhMap(void* pSpawn, PHInfo* pInfo)
{
    if (!pSpawn) return false;
    std::scoped_lock lock(s_phMutex);
    const char* name = SpawnAccess::GetDisplayedName((SPAWNINFO*)pSpawn);
    auto it = s_phMap.find(name);
    if (it != s_phMap.end())
    {
        *pInfo = it->second;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// HandleBuffRemoveRequest detour — captures pTargetWnd and handles PH clicks
// ---------------------------------------------------------------------------

static void __fastcall HandleBuffRemoveRequest_Detour(void* thisPtr, void* edx, void* pWnd)
{
    // Capture the target window pointer
    if (!s_pTargetWnd)
    {
        s_pTargetWnd = thisPtr;
        LogFramework("TargetInfo: Captured pTargetWnd = 0x%p from HandleBuffRemoveRequest", thisPtr);
    }

    // Check if click was on our PH button
    if (s_pPHButton && pWnd == s_pPHButton)
    {
        void* pTarg = (void*)pTarget;
        if (pTarg)
        {
            PHInfo pinf;
            if (GetPhMap(pTarg, &pinf) && !pinf.Link.empty())
            {
                ShellExecuteA(nullptr, "open", pinf.Link.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    }

    // Call original
    if (s_HandleBuffRemoveRequest_Original)
        s_HandleBuffRemoveRequest_Original(thisPtr, edx, pWnd);
}

// ---------------------------------------------------------------------------
// INI config
// ---------------------------------------------------------------------------

static void HandleINI(bool read, bool write)
{
    const char* section = "Default";

    if (read)
    {
        s_showDistance    = GetPrivateProfileBool(section, "ShowDistance", s_showDistance, s_INIFileName);
        s_showTargetInfo = GetPrivateProfileBool(section, "ShowTargetInfo", s_showTargetInfo, s_INIFileName);
        s_showPlaceholder= GetPrivateProfileBool(section, "ShowPlaceholder", s_showPlaceholder, s_INIFileName);
        s_showAnon       = GetPrivateProfileBool(section, "ShowAnon", s_showAnon, s_INIFileName);
        s_showSight      = GetPrivateProfileBool(section, "ShowSight", s_showSight, s_INIFileName);

        s_buffWndTopOffset    = GetPrivateProfileInt("UI", "Target_BuffWindow_TopOffset", 76, s_INIFileName);
        s_dTopOffset          = GetPrivateProfileInt("UI", "dTopOffset", 60, s_INIFileName);
        s_dBottomOffset       = GetPrivateProfileInt("UI", "dBottomOffset", 74, s_INIFileName);
        s_dLeftOffset         = GetPrivateProfileInt("UI", "dLeftOffset", 50, s_INIFileName);
        s_canSeeTopOffset     = GetPrivateProfileInt("UI", "CanSeeTopOffset", 47, s_INIFileName);
        s_canSeeBottomOffset  = GetPrivateProfileInt("UI", "CanSeeBottomOffset", 61, s_INIFileName);
        s_targetInfoWindowStyle    = GetPrivateProfileInt("UI", "TargetInfoWindowStyle", 0, s_INIFileName);
        s_targetInfoAnchoredToRight= GetPrivateProfileInt("UI", "TargetInfoAnchoredToRight", 0, s_INIFileName);

        s_manaLabelName    = GetPrivateProfileString("UI", "Label1", "Player_ManaLabel", s_INIFileName);
        s_fatigueLabelName = GetPrivateProfileString("UI", "Label2", "Player_FatigueLabel", s_INIFileName);
        s_targetDistanceLoc= GetPrivateProfileString("UI", "TargetDistanceLoc", "34,48,90,0", s_INIFileName);
        s_targetInfoLoc    = GetPrivateProfileString("UI", "TargetInfoLoc", "34,48,0,40", s_INIFileName);
    }

    if (write)
    {
        WritePrivateProfileBool(section, "ShowDistance", s_showDistance, s_INIFileName);
        WritePrivateProfileBool(section, "ShowTargetInfo", s_showTargetInfo, s_INIFileName);
        WritePrivateProfileBool(section, "ShowPlaceholder", s_showPlaceholder, s_INIFileName);
        WritePrivateProfileBool(section, "ShowAnon", s_showAnon, s_INIFileName);
        WritePrivateProfileBool(section, "ShowSight", s_showSight, s_INIFileName);
    }
}

// Parse "top,bottom,left,right" rect string
struct Rect4 { int top, bottom, left, right; };
static Rect4 ParseRect(const std::string& s, int dT, int dB, int dL, int dR)
{
    Rect4 r = { dT, dB, dL, dR };
    // Simple comma-delimited parser
    int vals[4] = { dT, dB, dL, dR };
    int idx = 0;
    size_t start = 0;
    for (size_t i = 0; i <= s.size() && idx < 4; i++)
    {
        if (i == s.size() || s[i] == ',')
        {
            if (i > start)
                vals[idx] = GetIntFromString(s.substr(start, i - start).c_str(), vals[idx]);
            idx++;
            start = i + 1;
        }
    }
    r.top = vals[0]; r.bottom = vals[1]; r.left = vals[2]; r.right = vals[3];
    return r;
}

// ---------------------------------------------------------------------------
// GetBoolFromString — parse on/off/true/false/1/0
// ---------------------------------------------------------------------------

static bool GetBoolFromString(const char* str, bool defaultVal)
{
    if (!str || !*str) return defaultVal;
    if (ci_equals(str, "on") || ci_equals(str, "true") || ci_equals(str, "1")) return true;
    if (ci_equals(str, "off") || ci_equals(str, "false") || ci_equals(str, "0")) return false;
    return defaultVal;
}

// ---------------------------------------------------------------------------
// Slash command handler
// ---------------------------------------------------------------------------

static void CMD_TargetInfo(SPAWNINFO* /*pChar*/, const char* szLine)
{
    char szArg[MAX_STRING] = { 0 };
    GetArg(szArg, szLine, 1);
    bool writeIni = false;

    if (ci_equals(szArg, "distance"))
    {
        GetArg(szArg, szLine, 2);
        s_showDistance = GetBoolFromString(szArg, !s_showDistance);
        writeIni = true;
    }
    else if (ci_equals(szArg, "info"))
    {
        GetArg(szArg, szLine, 2);
        s_showTargetInfo = GetBoolFromString(szArg, !s_showTargetInfo);
        writeIni = true;
    }
    else if (ci_equals(szArg, "placeholder"))
    {
        GetArg(szArg, szLine, 2);
        s_showPlaceholder = GetBoolFromString(szArg, !s_showPlaceholder);
        writeIni = true;
    }
    else if (ci_equals(szArg, "anon"))
    {
        GetArg(szArg, szLine, 2);
        s_showAnon = GetBoolFromString(szArg, !s_showAnon);
        writeIni = true;
    }
    else if (ci_equals(szArg, "sight"))
    {
        GetArg(szArg, szLine, 2);
        s_showSight = GetBoolFromString(szArg, !s_showSight);
        writeIni = true;
    }
    else if (ci_equals(szArg, "reload"))
    {
        // Will re-init on next pulse
        s_initialized = false;
    }
    else
    {
        WriteChatf("TargetInfo Usage:");
        WriteChatf("  /targetinfo distance [on|off]");
        WriteChatf("  /targetinfo info [on|off]");
        WriteChatf("  /targetinfo placeholder [on|off]");
        WriteChatf("  /targetinfo anon [on|off]");
        WriteChatf("  /targetinfo sight [on|off]");
        WriteChatf("  /targetinfo reload");
    }

    if (writeIni)
        HandleINI(false, true);
}

// ---------------------------------------------------------------------------
// UI cleanup — destroy overlays, restore original window state
// ---------------------------------------------------------------------------

static void CleanUpUI()
{
    s_disabledBadUI = false;

    if (s_pTargetWnd && s_orgTargetWindStyle)
    {
        WndSetWindowStyle(s_pTargetWnd, s_orgTargetWindStyle);
        WndSetNeedsSaving(s_pTargetWnd, true);
        WndSetClientRectChanged(s_pTargetWnd, true);
        s_orgTargetWindStyle = 0;
    }

    if (s_pInfoLabel)     { CallDestroyWnd(s_pInfoLabel);     s_pInfoLabel = nullptr; }
    if (s_pDistanceLabel) { CallDestroyWnd(s_pDistanceLabel); s_pDistanceLabel = nullptr; }
    if (s_pCanSeeLabel)   { CallDestroyWnd(s_pCanSeeLabel);   s_pCanSeeLabel = nullptr; }
    if (s_pPHButton)      { CallDestroyWnd(s_pPHButton);      s_pPHButton = nullptr; }

    if (GameState::GetGameState() == GAMESTATE_INGAME && s_pTargetWnd)
    {
        __try
        {
            if (s_pBuffWindow)
                WndSetTopOffset(s_pBuffWindow, s_buffWndTopOffsetOld);
            if (s_pAggroPctPlayerLabel)
            {
                WndSetTopOffset(s_pAggroPctPlayerLabel, s_aggroPctPlayerTopOld);
                WndSetBottomOffset(s_pAggroPctPlayerLabel, s_aggroPctPlayerBottomOld);
            }
            if (s_pAggroNameSecLabel)
            {
                WndSetTopOffset(s_pAggroNameSecLabel, s_aggroNameSecTopOld);
                WndSetBottomOffset(s_pAggroNameSecLabel, s_aggroNameSecBottomOld);
            }
            if (s_pAggroPctSecLabel)
            {
                WndSetTopOffset(s_pAggroPctSecLabel, s_aggroPctSecTopOld);
                WndSetBottomOffset(s_pAggroPctSecLabel, s_aggroPctSecBottomOld);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            LogFramework("TargetInfo: EXCEPTION restoring child window offsets");
        }
    }

    s_pBuffWindow = nullptr;
    s_pAggroPctPlayerLabel = nullptr;
    s_pAggroNameSecLabel = nullptr;
    s_pAggroPctSecLabel = nullptr;

    if (s_pTargetWnd)
        CallUpdateLayout(s_pTargetWnd);
}

// ---------------------------------------------------------------------------
// UI initialization — create overlays on the target window
// ---------------------------------------------------------------------------

static void InitUI()
{
    if (s_initialized) return;
    if (GameState::GetGameState() != GAMESTATE_INGAME) return;
    if (s_disabledBadUI || !s_pTargetWnd) return;
    if (!pLocalPlayer) return;

    HandleINI(true, true);

    __try
    {
        // Modify target window style
        s_orgTargetWindStyle = WndGetWindowStyle(s_pTargetWnd);
        if (s_orgTargetWindStyle & WSF_TITLEBAR)
            WndAddStyle(s_pTargetWnd, WSF_SIZABLE | WSF_BORDER);
        else if (s_targetInfoWindowStyle == 0)
            WndAddStyle(s_pTargetWnd, WSF_CLIENTMOVABLE | WSF_SIZABLE | WSF_BORDER);
        else
            WndSetWindowStyle(s_pTargetWnd, (uint32_t)s_targetInfoWindowStyle);

        // Move aggro labels down to make room for our overlays
        s_pAggroPctPlayerLabel = CallGetChildItem(s_pTargetWnd, "Target_AggroPctPlayerLabel");
        if (s_pAggroPctPlayerLabel)
        {
            WndSetBGColor(s_pAggroPctPlayerLabel, 0xFF000000);
            s_aggroPctPlayerTopOld = WndGetTopOffset(s_pAggroPctPlayerLabel);
            WndSetTopOffset(s_pAggroPctPlayerLabel, s_dTopOffset);
            s_aggroPctPlayerBottomOld = WndGetBottomOffset(s_pAggroPctPlayerLabel);
            WndSetBottomOffset(s_pAggroPctPlayerLabel, s_dBottomOffset);
        }

        s_pAggroNameSecLabel = CallGetChildItem(s_pTargetWnd, "Target_AggroNameSecondaryLabel");
        if (s_pAggroNameSecLabel)
        {
            WndSetBGColor(s_pAggroNameSecLabel, 0xFF000000);
            s_aggroNameSecTopOld = WndGetTopOffset(s_pAggroNameSecLabel);
            WndSetTopOffset(s_pAggroNameSecLabel, s_dTopOffset);
            s_aggroNameSecBottomOld = WndGetBottomOffset(s_pAggroNameSecLabel);
            WndSetBottomOffset(s_pAggroNameSecLabel, s_dBottomOffset);
        }

        s_pAggroPctSecLabel = CallGetChildItem(s_pTargetWnd, "Target_AggroPctSecondaryLabel");
        if (s_pAggroPctSecLabel)
        {
            WndSetBGColor(s_pAggroPctSecLabel, 0xFF000000);
            s_aggroPctSecTopOld = WndGetTopOffset(s_pAggroPctSecLabel);
            WndSetTopOffset(s_pAggroPctSecLabel, s_dTopOffset);
            s_aggroPctSecBottomOld = WndGetBottomOffset(s_pAggroPctSecLabel);
            WndSetBottomOffset(s_pAggroPctSecLabel, s_dBottomOffset);
        }

        s_pBuffWindow = CallGetChildItem(s_pTargetWnd, "Target_BuffWindow");
        if (s_pBuffWindow)
        {
            WndSetBGColor(s_pBuffWindow, 0xFF000000);
            s_buffWndTopOffsetOld = WndGetTopOffset(s_pBuffWindow);
            WndSetTopOffset(s_pBuffWindow, s_buffWndTopOffset);
        }

        // Find UI templates
        void* distTmpl = CallFindTemplate(s_manaLabelName.c_str());
        void* canSeeTmpl = CallFindTemplate(s_fatigueLabelName.c_str());
        void* phBtnTmpl = CallFindTemplate("IDW_ModButton");

        if (!distTmpl || !canSeeTmpl || !phBtnTmpl)
        {
            s_disabledBadUI = true;
            WriteChatf("TargetInfo: Disabled due to incompatible UI (missing templates).");
            LogFramework("TargetInfo: Missing templates - distTmpl=0x%p canSeeTmpl=0x%p phBtnTmpl=0x%p",
                distTmpl, canSeeTmpl, phBtnTmpl);
            return;
        }

        // Save original template values
        uint32_t oldDistFont = WndRead<uint32_t>(distTmpl, TmplOff::nFont);
        uint32_t oldDistStyle = WndRead<uint32_t>(distTmpl, TmplOff::uStyleBits);
        void* oldDistName = WndRead<void*>(distTmpl, TmplOff::strName);
        void* oldDistScreenId = WndRead<void*>(distTmpl, TmplOff::strScreenId);
        void* oldDistController = WndRead<void*>(distTmpl, TmplOff::strController);

        uint32_t oldCanSeeFont = WndRead<uint32_t>(canSeeTmpl, TmplOff::nFont);
        void* oldCanSeeName = WndRead<void*>(canSeeTmpl, TmplOff::strName);
        void* oldCanSeeScreenId = WndRead<void*>(canSeeTmpl, TmplOff::strScreenId);
        void* oldCanSeeController = WndRead<void*>(canSeeTmpl, TmplOff::strController);

        uint32_t oldPHFont = WndRead<uint32_t>(phBtnTmpl, TmplOff::nFont);

        // Modify templates for our labels
        WndWrite<uint32_t>(distTmpl, TmplOff::nFont, 2);
        WriteCXStr(distTmpl, TmplOff::strController, "0");
        WriteCXStr(canSeeTmpl, TmplOff::strController, "0");

        // --- Create InfoLabel ---
        WriteCXStr(distTmpl, TmplOff::strName, "Target_InfoLabel");
        WriteCXStr(distTmpl, TmplOff::strScreenId, "Target_InfoLabel");

        s_pInfoLabel = CallCreateWndFromTemplate(s_pTargetWnd, distTmpl);
        if (s_pInfoLabel)
        {
            if (s_targetInfoAnchoredToRight)
            {
                WndSetRightAnchoredToLeft(s_pInfoLabel, true);
                WndSetLeftAnchoredToLeft(s_pInfoLabel, false);
            }
            else
            {
                WndSetRightAnchoredToLeft(s_pInfoLabel, false);
                WndSetLeftAnchoredToLeft(s_pInfoLabel, true);
            }
            WndSetVisible(s_pInfoLabel, true);
            WndSetUseInLayoutV(s_pInfoLabel, true);
            WndSetWindowStyle(s_pInfoLabel, WSF_AUTOSTRETCHH | WSF_TRANSPARENT | WSF_AUTOSTRETCHV | WSF_RELATIVERECT);
            WndSetClipToParent(s_pInfoLabel, true);
            WndSetUseInLayoutH(s_pInfoLabel, true);
            LabelSetAlignCenter(s_pInfoLabel, false);
            LabelSetAlignRight(s_pInfoLabel, false);

            Rect4 r = ParseRect(s_targetInfoLoc, 34, 48, 0, 40);
            WndSetTopOffset(s_pInfoLabel, r.top);
            WndSetBottomOffset(s_pInfoLabel, r.bottom);
            WndSetLeftOffset(s_pInfoLabel, r.left);
            WndSetRightOffset(s_pInfoLabel, r.right);

            WndSetCRNormal(s_pInfoLabel, 0xFF00FF00); // green
            WndSetBGColor(s_pInfoLabel, 0xFFFFFFFF);
            WriteCXStr(s_pInfoLabel, WndOff::Tooltip, "Target Info");
        }

        // --- Create DistanceLabel ---
        WriteCXStr(distTmpl, TmplOff::strName, "Target_DistLabel");
        WriteCXStr(distTmpl, TmplOff::strScreenId, "Target_DistLabel");
        WndWrite<uint32_t>(distTmpl, TmplOff::uStyleBits, WSF_AUTOSTRETCHH | WSF_AUTOSTRETCHV | WSF_RELATIVERECT);

        s_pDistanceLabel = CallCreateWndFromTemplate(s_pTargetWnd, distTmpl);
        if (s_pDistanceLabel)
        {
            Rect4 r = ParseRect(s_targetDistanceLoc, 34, 48, 90, 0);
            WndSetTopOffset(s_pDistanceLabel, r.top);
            WndSetBottomOffset(s_pDistanceLabel, r.bottom);
            WndSetLeftOffset(s_pDistanceLabel, r.left);
            WndSetRightOffset(s_pDistanceLabel, r.right);
            WndSetCRNormal(s_pDistanceLabel, 0xFF00FF00);
            WndSetBGColor(s_pDistanceLabel, 0xFFFFFFFF);
            WriteCXStr(s_pDistanceLabel, WndOff::Tooltip, "Target Distance");
            WndSetVisible(s_pDistanceLabel, s_showDistance);
            LabelSetNoWrap(s_pDistanceLabel, true);
            WndSetLeftAnchoredToLeft(s_pDistanceLabel, true);
            LabelSetAlignRight(s_pDistanceLabel, true);
            LabelSetAlignCenter(s_pDistanceLabel, false);
        }

        // --- Create CanSeeLabel ---
        WndWrite<uint32_t>(canSeeTmpl, TmplOff::nFont, 2);
        WriteCXStr(canSeeTmpl, TmplOff::strName, "Target_CanSeeLabel");
        WriteCXStr(canSeeTmpl, TmplOff::strScreenId, "Target_CanSeeLabel");

        s_pCanSeeLabel = CallCreateWndFromTemplate(s_pTargetWnd, canSeeTmpl);
        if (s_pCanSeeLabel)
        {
            WndSetVisible(s_pCanSeeLabel, true);
            LabelSetNoWrap(s_pCanSeeLabel, true);
            WndSetWindowStyle(s_pCanSeeLabel, WSF_AUTOSTRETCHH | WSF_TRANSPARENT | WSF_AUTOSTRETCHV | WSF_RELATIVERECT);
            WndSetLeftAnchoredToLeft(s_pCanSeeLabel, true);
            WndSetRightAnchoredToLeft(s_pCanSeeLabel, false);
            WndSetBottomAnchoredToTop(s_pCanSeeLabel, true);
            WndSetTopAnchoredToTop(s_pCanSeeLabel, true);
            LabelSetAlignCenter(s_pCanSeeLabel, true);
            LabelSetAlignRight(s_pCanSeeLabel, false);
            WndSetTopOffset(s_pCanSeeLabel, s_canSeeTopOffset + 10);
            WndSetBottomOffset(s_pCanSeeLabel, s_canSeeBottomOffset + 10);
            WndSetLeftOffset(s_pCanSeeLabel, s_dLeftOffset);
            WndSetRightOffset(s_pCanSeeLabel, s_dLeftOffset);
            WndSetCRNormal(s_pCanSeeLabel, 0xFF00FF00);
            WndSetBGColor(s_pCanSeeLabel, 0xFFFFFFFF);
            WriteCXStr(s_pCanSeeLabel, WndOff::Tooltip, "Can See Target");
        }

        // --- Create PHButton ---
        WndWrite<uint32_t>(phBtnTmpl, TmplOff::nFont, 0);

        s_pPHButton = CallCreateWndFromTemplate(s_pTargetWnd, phBtnTmpl);
        if (s_pPHButton)
        {
            WndSetVisible(s_pPHButton, false);
            WndSetBottomAnchoredToTop(s_pPHButton, true);
            WndSetLeftAnchoredToLeft(s_pPHButton, true);
            WndSetRightAnchoredToLeft(s_pPHButton, false);
            WndSetTopAnchoredToTop(s_pPHButton, true);
            WndSetTopOffset(s_pPHButton, s_canSeeTopOffset + 1);
            WndSetBottomOffset(s_pPHButton, s_dTopOffset - 1);
            WndSetLeftOffset(s_pPHButton, 0);
            WndSetRightOffset(s_pPHButton, 0);
            WndSetLocation(s_pPHButton, 2, s_canSeeTopOffset + 1, 20, WndGetBottomOffset(s_pPHButton));
            WndSetCRNormal(s_pPHButton, 0xFF00FFFF); // cyan
            WndSetBGColor(s_pPHButton, 0xFFFFFFFF);
            WriteCXStr(s_pPHButton, WndOff::Tooltip, "Target is a Place Holder");
            CallSetWindowText(s_pPHButton, "PH");
        }

        // Restore template values
        WndWrite<uint32_t>(distTmpl, TmplOff::nFont, oldDistFont);
        WndWrite<uint32_t>(distTmpl, TmplOff::uStyleBits, oldDistStyle);
        WndWrite<void*>(distTmpl, TmplOff::strName, oldDistName);
        WndWrite<void*>(distTmpl, TmplOff::strScreenId, oldDistScreenId);
        WndWrite<void*>(distTmpl, TmplOff::strController, oldDistController);
        WndWrite<uint32_t>(canSeeTmpl, TmplOff::nFont, oldCanSeeFont);
        WndWrite<void*>(canSeeTmpl, TmplOff::strName, oldCanSeeName);
        WndWrite<void*>(canSeeTmpl, TmplOff::strScreenId, oldCanSeeScreenId);
        WndWrite<void*>(canSeeTmpl, TmplOff::strController, oldCanSeeController);
        WndWrite<uint32_t>(phBtnTmpl, TmplOff::nFont, oldPHFont);

        if (!(s_pInfoLabel && s_pDistanceLabel && s_pCanSeeLabel && s_pPHButton))
        {
            WriteChatf("TargetInfo: Some UI elements failed to create. Try /targetinfo reload.");
            LogFramework("TargetInfo: Partial init - Info=%p Dist=%p CanSee=%p PH=%p",
                s_pInfoLabel, s_pDistanceLabel, s_pCanSeeLabel, s_pPHButton);
        }

        s_initialized = true;
        LogFramework("TargetInfo: UI initialized successfully");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogFramework("TargetInfo: EXCEPTION during InitUI!");
        s_disabledBadUI = true;
    }
}

// ---------------------------------------------------------------------------
// IMod implementation
// ---------------------------------------------------------------------------

bool TargetInfoMod::Initialize()
{
    ResolveTargetInfoFuncPtrs();

    AddCommand("/targetinfo", CMD_TargetInfo);

    // Load PH database from game directory
    char phPath[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, phPath, MAX_PATH);
    // Strip exe name, append our file
    char* lastSlash = strrchr(phPath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    strcat_s(phPath, "TargetInfoPHs.txt");
    LoadPHs(phPath);

    // Install HandleBuffRemoveRequest hook
    Hooks::Install("CTargetWnd_HandleBuffRemoveRequest",
        (void**)&s_HandleBuffRemoveRequest_Original,
        (void*)&HandleBuffRemoveRequest_Detour);

    LogFramework("TargetInfo: Initialized");
    return true;
}

void TargetInfoMod::Shutdown()
{
    CleanUpUI();
    RemoveCommand("/targetinfo");
    Hooks::Remove("CTargetWnd_HandleBuffRemoveRequest");
    s_pTargetWnd = nullptr;
    s_initialized = false;
    LogFramework("TargetInfo: Shutdown");
}

void TargetInfoMod::OnCleanUI()
{
    CleanUpUI();
}

void TargetInfoMod::OnReloadUI()
{
    s_initialized = false;
}

void TargetInfoMod::OnSetGameState(int gameState)
{
    if (gameState == GAMESTATE_INGAME)
        s_initialized = false; // Will re-init on next pulse
}

// Extracted update logic — contains C++ objects (PHInfo has std::string members)
// so it cannot live inside a __try block directly.
static void UpdateTargetOverlays(void* pTarg)
{
    // --- Placeholder button ---
    if (s_showPlaceholder)
    {
        if (s_oldSpawn != pTarg)
        {
            s_oldSpawn = pTarg;
            PHInfo pinf;
            if (GetPhMap(pTarg, &pinf))
            {
                WriteCXStr(s_pPHButton, WndOff::Tooltip, pinf.Named.c_str());
                WndSetVisible(s_pPHButton, true);
            }
            else
            {
                WndSetVisible(s_pPHButton, false);
            }
        }
    }
    else
    {
        WndSetVisible(s_pPHButton, false);
    }

    // --- Target info label (level/race/class or anon) ---
    char szText[128] = { 0 };
    if (s_showTargetInfo)
    {
        int anon = GetSpawnAnon(pTarg);
        if (anon == 1 && s_showAnon)
        {
            strcpy_s(szText, "Anonymous");
        }
        else if (anon == 2 && s_showAnon)
        {
            strcpy_s(szText, "Roleplaying");
        }
        else
        {
            uint8_t type = SpawnAccess::GetType((SPAWNINFO*)pTarg);
            uint8_t level = SpawnAccess::GetLevel((SPAWNINFO*)pTarg);
            const char* race = SpawnAccess::GetRaceString((SPAWNINFO*)pTarg);

            if (type == SPAWN_PLAYER)
            {
                const char* cls = SpawnAccess::GetClassThreeLetterCode((SPAWNINFO*)pTarg);
                sprintf_s(szText, "%d %s %s", level, race, cls);
            }
            else
            {
                const char* cls = SpawnAccess::GetClassString((SPAWNINFO*)pTarg);
                sprintf_s(szText, "%d %s %s", level, race, cls);
            }
        }
        CallSetWindowText(s_pInfoLabel, szText);
    }
    WndSetVisible(s_pInfoLabel, s_showTargetInfo);

    // --- Distance label ---
    if (s_showDistance)
    {
        float dist = Distance3DToSpawn((void*)pLocalPlayer, pTarg);
        sprintf_s(szText, "%.2f", dist);

        if (dist < 250.0f)
            WndSetCRNormal(s_pDistanceLabel, 0xFF00FF00); // green
        else
            WndSetCRNormal(s_pDistanceLabel, 0xFFFF0000); // red

        CallSetWindowText(s_pDistanceLabel, szText);
    }
    WndSetVisible(s_pDistanceLabel, s_showDistance);

    // --- Line of sight label ---
    if (s_showSight)
    {
        if (SpawnCanSee((void*)pLocalPlayer, pTarg))
        {
            CallSetWindowText(s_pCanSeeLabel, "O");
            WndSetCRNormal(s_pCanSeeLabel, 0xFF00FF00); // green
        }
        else
        {
            CallSetWindowText(s_pCanSeeLabel, "X");
            WndSetCRNormal(s_pCanSeeLabel, 0xFFFF0000); // red
        }
    }
    WndSetVisible(s_pCanSeeLabel, s_showSight);
}

// SEH wrapper — no C++ objects, safe for __try
static void UpdateTargetOverlaysSafe(void* pTarg)
{
    __try
    {
        UpdateTargetOverlays(pTarg);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogFramework("TargetInfo: EXCEPTION in OnPulse update");
    }
}

void TargetInfoMod::OnPulse()
{
    if (GameState::GetGameState() != GAMESTATE_INGAME || !pLocalPlayer)
        return;

    // Throttle to ~500ms
    static ULONGLONG lastUpdate = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastUpdate < 500)
        return;
    lastUpdate = now;

    // Try to find pTargetWnd if we don't have it yet
    // (HandleBuffRemoveRequest hook will capture it on first click,
    //  but we also try scanning here)
    if (!s_pTargetWnd)
    {
        // TODO: Implement window manager scan as fallback.
        // For now, we rely on the HandleBuffRemoveRequest hook.
        // The user must click the target window once to activate the mod.
        return;
    }

    if (!WndIsVisible(s_pTargetWnd))
        return;

    // Lazy-init UI on first visible pulse
    InitUI();

    if (!s_pInfoLabel || !s_pDistanceLabel || !s_pCanSeeLabel || !s_pPHButton)
        return;

    void* pTarg = (void*)pTarget;

    if (pTarg)
    {
        UpdateTargetOverlaysSafe(pTarg);
    }
    else
    {
        // No target — clear all labels
        CallSetWindowText(s_pInfoLabel, "");
        CallSetWindowText(s_pDistanceLabel, "");
        CallSetWindowText(s_pCanSeeLabel, "");
        WndSetVisible(s_pPHButton, false);
    }
}

