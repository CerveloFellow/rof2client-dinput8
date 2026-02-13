/**
 * @file map_api.cpp
 * @brief Map rendering pipeline — ported from MQ2MapAPI.cpp
 * @date 2026-02-09
 *
 * Key functions: MapInit, MapClear, MapGenerate, MapUpdate, MapAttach, MapDetach,
 * AddSpawn, RemoveSpawn, AddGroundItem, RemoveGroundItem, MapHighlight, MapHide, MapShow.
 *
 * Substitutions from MQ source:
 *   pSpawn->Field        -> SpawnAccess::GetField(pSpawn)
 *   pItemList->Top       -> GameState::GetGroundItemListTop()
 *   pMapViewWnd->MapView -> raw offsets from captured MapViewMap pointer
 *   fmt::format          -> snprintf
 */

#include "pch.h"
#include "map_object.h"

#include <eqlib/Offsets.h>
#include <eqlib/offsets/eqgame.h>

#include <sstream>

// ---------------------------------------------------------------------------
// MapViewMap raw offset access
// ---------------------------------------------------------------------------

// MapViewMap offsets (within the MapViewMap object pointed to by PostDraw's this)
constexpr uintptr_t MapViewMap_pLines  = 0x250;
constexpr uintptr_t MapViewMap_pLabels = 0x254;

static void* s_mapViewMap = nullptr;

void SetMapViewMap(void* ptr)
{
	s_mapViewMap = ptr;
}

void* GetMapViewMapPtr()
{
	return s_mapViewMap;
}

// ---------------------------------------------------------------------------
// Game function pointers (resolved once at init)
// ---------------------------------------------------------------------------

using GetWorldCoordinates_t = void(__thiscall*)(void* thisPtr, CVector3& worldCoords);
static GetWorldCoordinates_t s_getWorldCoordinates = nullptr;

void InitMapFunctions()
{
	s_getWorldCoordinates = reinterpret_cast<GetWorldCoordinates_t>(
		eqlib::FixEQGameOffset(MapViewMap__GetWorldCoordinates_x));
	LogFramework("InitMapFunctions: GetWorldCoordinates = 0x%08X",
		static_cast<unsigned int>(reinterpret_cast<uintptr_t>(s_getWorldCoordinates)));
}

bool CallGetWorldCoordinates(void* mapViewMap, CVector3& worldCoords)
{
	if (!s_getWorldCoordinates || !mapViewMap)
		return false;
	s_getWorldCoordinates(mapViewMap, worldCoords);
	return true;
}

static MapViewLine** GetGameLineListHead()
{
	if (!s_mapViewMap) return nullptr;
	return reinterpret_cast<MapViewLine**>(
		reinterpret_cast<uintptr_t>(s_mapViewMap) + MapViewMap_pLines);
}

static MapViewLabel** GetGameLabelListHead()
{
	if (!s_mapViewMap) return nullptr;
	return reinterpret_cast<MapViewLabel**>(
		reinterpret_cast<uintptr_t>(s_mapViewMap) + MapViewMap_pLabels);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static MapViewLine*  pTargetLine = nullptr;
static MapViewLabel* pActualLabelList = nullptr;
static bool s_labelsAttached = false;
static bool s_linesAttached = false;

static MapCircle CastCircle;
static MapCircle SpellCircle;
static MapCircle TargetCircle;
static MapCircle TargetMeleeCircle;
static MapCircle CampCircle;
static MapCircle PullCircle;

// ---------------------------------------------------------------------------
// RemoveMapObject helper
// ---------------------------------------------------------------------------

static void RemoveMapObject(MapObject* pMapSpawn)
{
	delete pMapSpawn;
}

// ---------------------------------------------------------------------------
// GetCurrentMapObject
// ---------------------------------------------------------------------------

static MapObject* GetCurrentMapObject()
{
	if (!pCurrentMapLabel)
		return nullptr;

	return GetMapObjectForLabel(pCurrentMapLabel);
}

// ---------------------------------------------------------------------------
// MapInit / MapClear
// ---------------------------------------------------------------------------

void MapInit()
{
	CastCircle.Clear();
	SpellCircle.Clear();
	TargetCircle.Clear();
	TargetMeleeCircle.Clear();
	CampCircle.Clear();
	PullCircle.Clear();
}

void MapClear()
{
	ClearBodyTypeCache();
	MapObjects_Clear();

	pLastTarget = nullptr;

	if (pTargetLine)
	{
		DeleteLine(pTargetLine);
		pTargetLine = nullptr;
	}

	CastCircle.Clear();
	SpellCircle.Clear();
	TargetCircle.Clear();
	TargetMeleeCircle.Clear();
	CampCircle.Clear();
	PullCircle.Clear();
}

// ---------------------------------------------------------------------------
// AddSpawn / RemoveSpawn / AddGroundItem / RemoveGroundItem
// ---------------------------------------------------------------------------

MapObject* AddSpawn(SPAWNINFO* pNewSpawn, bool ExplicitAllow)
{
	return MakeMapObject(pNewSpawn, ExplicitAllow);
}

bool RemoveSpawn(SPAWNINFO* pSpawn)
{
	MapObject* pMapObject = FindMapObject(pSpawn);
	if (pMapObject)
	{
		RemoveMapObject(pMapObject);  // handles detach/attach internally
		return true;
	}
	return false;
}

MapObject* AddGroundItem(EQGroundItem* pGroundItem)
{
	return MakeMapObject(pGroundItem);
}

void RemoveGroundItem(EQGroundItem* pGroundItem)
{
	if (MapObject* mapObject = FindMapObject(pGroundItem))
	{
		RemoveMapObject(mapObject);  // handles detach/attach internally
	}
}

// ---------------------------------------------------------------------------
// MapGenerate
// ---------------------------------------------------------------------------

void MapGenerate()
{
	if (!IsOptionEnabled(MapFilter::All))
	{
		LogFramework("MapGenerate: All filter disabled, skipping");
		return;
	}

	// Diagnostic: log filter state
	LogFramework("MapGenerate: filter state — All=%d PC=%d NPC=%d Named=%d Target=%d Corpse=%d NPCCorpse=%d PCCorpse=%d Pet=%d Mount=%d Untargetable=%d",
		IsOptionEnabled(MapFilter::All) ? 1 : 0,
		IsOptionEnabled(MapFilter::PC) ? 1 : 0,
		IsOptionEnabled(MapFilter::NPC) ? 1 : 0,
		IsOptionEnabled(MapFilter::Named) ? 1 : 0,
		IsOptionEnabled(MapFilter::Target) ? 1 : 0,
		IsOptionEnabled(MapFilter::Corpse) ? 1 : 0,
		IsOptionEnabled(MapFilter::NPCCorpse) ? 1 : 0,
		IsOptionEnabled(MapFilter::PCCorpse) ? 1 : 0,
		IsOptionEnabled(MapFilter::Pet) ? 1 : 0,
		IsOptionEnabled(MapFilter::Mount) ? 1 : 0,
		IsOptionEnabled(MapFilter::Untargetable) ? 1 : 0);

	int spawnCount = 0;
	int spawnObjectCount = 0;
	int groundCount = 0;
	int typeCountPC = 0, typeCountNPC = 0, typeCountMount = 0, typeCountPet = 0;
	int typeCountCorpse = 0, typeCountUntarget = 0, typeCountOther = 0;
	int rejectedCount = 0;

	SPAWNINFO* pSpawn = pSpawnList;
	LogFramework("MapGenerate: pSpawnList=0x%p", pSpawn);

	// Diagnostic: dump first spawn's TListNode and manager to diagnose list traversal
	if (pSpawn)
	{
		uintptr_t base = reinterpret_cast<uintptr_t>(pSpawn);
		__try
		{
			// Dump first 0x14 bytes to see TListNode + CActorApplicationData vtable
			LogFramework("  First spawn raw dump (0x%p):", pSpawn);
			for (int i = 0; i < 5; i++)
			{
				uint32_t val = *reinterpret_cast<uint32_t*>(base + i * 4);
				LogFramework("    [+0x%02X] = 0x%08X", i * 4, val);
			}
			uint8_t  type    = *reinterpret_cast<uint8_t*>(base + 0x125);
			uint16_t spawnID = *reinterpret_cast<uint16_t*>(base + 0x148);
			LogFramework("    type=%d spawnID=%d name='%.30s'", type, spawnID, SpawnAccess::GetName(pSpawn));

			// Also dump the spawn manager to check first/last pointers
			void* mgr = reinterpret_cast<void*>(GameState::GetSpawnManager());
			if (mgr)
			{
				uintptr_t mgrBase = reinterpret_cast<uintptr_t>(mgr);
				LogFramework("  SpawnManager (0x%p) raw dump:", mgr);
				for (int i = 0; i < 6; i++)
				{
					uint32_t val = *reinterpret_cast<uint32_t*>(mgrBase + i * 4);
					LogFramework("    [+0x%02X] = 0x%08X", i * 4, val);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			LogFramework("!!! Cannot read first spawn at 0x%p — code=0x%08X", pSpawn, GetExceptionCode());
			pSpawn = nullptr;
		}
	}

	__try
	{
		while (pSpawn)
		{
			spawnCount++;
			if (spawnCount <= 5)
				LogFramework("  Spawn %d: 0x%p name='%.20s'", spawnCount, pSpawn, SpawnAccess::GetName(pSpawn));

			eSpawnType sType = GetSpawnType(pSpawn);
			switch (sType) {
			case PC: typeCountPC++; break;
			case NPC: typeCountNPC++; break;
			case MOUNT: typeCountMount++; break;
			case PET: case PCPET: case NPCPET: typeCountPet++; break;
			case CORPSE: case NPCCORPSE: case PCCORPSE: typeCountCorpse++; break;
			case UNTARGETABLE: typeCountUntarget++; break;
			default: typeCountOther++; break;
			}

			if (AddSpawn(pSpawn))
				spawnObjectCount++;
			else
				rejectedCount++;

			pSpawn = SpawnAccess::GetNext(pSpawn);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LogFramework("!!! MapGenerate EXCEPTION in spawn walk after %d spawns, code=0x%08X, lastSpawn=0x%p",
			spawnCount, GetExceptionCode(), pSpawn);
	}

	LogFramework("MapGenerate: spawn walk done — %d spawns, %d objects, %d rejected", spawnCount, spawnObjectCount, rejectedCount);
	LogFramework("MapGenerate: types — PC=%d NPC=%d Mount=%d Pet=%d Corpse=%d Untarget=%d Other=%d",
		typeCountPC, typeCountNPC, typeCountMount, typeCountPet, typeCountCorpse, typeCountUntarget, typeCountOther);

	if (IsOptionEnabled(MapFilter::Ground))
	{
		EQGroundItem* pItem = GameState::GetGroundItemListTop();
		LogFramework("MapGenerate: ground items top=0x%p", pItem);

		__try
		{
			while (pItem)
			{
				AddGroundItem(pItem);
				pItem = pItem->pNext;
				groundCount++;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			LogFramework("!!! MapGenerate EXCEPTION in ground item walk after %d items, code=0x%08X",
				groundCount, GetExceptionCode());
		}
	}

	CreateAllMapLocs();

	LogFramework("MapGenerate: complete — %d spawns walked, %d map objects, %d ground items",
		spawnCount, spawnObjectCount, groundCount);

	LogFramework("MapGenerate: ready (gpLabelList=0x%p gpLineList=0x%p)",
		gpLabelList, gpLineList);
}

// ---------------------------------------------------------------------------
// MapUpdate
// ---------------------------------------------------------------------------

void MapUpdate()
{
	static int s_updateCount = 0;

	if (!pLocalPC)
	{
		if (s_updateCount == 0)
			LogFramework("MapUpdate: pLocalPC is NULL — skipping");
		return;
	}
	EnterMQ2Benchmark(bmMapRefresh);

	SPAWNINFO* localPlayer = pLocalPlayer;
	SPAWNINFO* target = pTarget;

	bool bTargetChanged = false;
	MapObject* pOldLastTarget = pLastTarget;

	// Check if current target is obsolete
	if (pLastTarget && pLastTarget->GetSpawn() != target)
	{
		if (!pLastTarget->CanDisplayObject())
		{
			RemoveMapObject(pLastTarget);
		}

		pLastTarget = nullptr;
		bTargetChanged = true;
	}

	// Update target with new
	if (target && IsOptionEnabled(MapFilter::Target))
	{
		if (MapObject* pMapObject = FindMapObject(target))
		{
			pLastTarget = pMapObject;
		}
		else
		{
			pLastTarget = nullptr;
		}

		if (!pLastTarget)
		{
			pLastTarget = AddSpawn(target);
		}
	}

	int totalObjects = 0, removedObjects = 0;
	MapObject* mapObject = gpActiveMapObjects;
	while (mapObject)
	{
		totalObjects++;
		bool forced = (mapObject == pOldLastTarget) && bTargetChanged;
		mapObject->Update(forced);

		if (!mapObject->CanDisplayObject())
		{
			removedObjects++;
			MapObject* pNext = mapObject->GetNext();
			RemoveMapObject(mapObject);
			mapObject = pNext;
		}
		else
		{
			mapObject = mapObject->GetNext();
		}
	}

	s_updateCount++;
	if (s_updateCount <= 5 || (removedObjects > 0 && s_updateCount % 300 == 0))
	{
		LogFramework("MapUpdate #%d: pLocalPC=0x%p total=%d removed=%d remaining=%d target=0x%p",
			s_updateCount, (void*)pLocalPC, totalObjects, removedObjects,
			totalObjects - removedObjects, (void*)target);
	}

	// Cast radius circle
	if (localPlayer && IsOptionEnabled(MapFilter::CastRadius))
	{
		const MapFilterOption& option = GetMapFilterOption(MapFilter::CastRadius);
		CastCircle.UpdateCircle(option.Color, option.Radius,
			SpawnAccess::GetX(localPlayer), SpawnAccess::GetY(localPlayer), SpawnAccess::GetZ(localPlayer));
	}
	else
	{
		CastCircle.Clear();
	}

	// Camp radius circle
	if (localPlayer && IsOptionEnabled(MapFilter::CampRadius))
	{
		const MapFilterOption& option = GetMapFilterOption(MapFilter::CampRadius);
		CampCircle.UpdateCircle(option.Color, option.Radius,
			CampX, CampY, SpawnAccess::GetZ(localPlayer));
	}
	else
	{
		CampCircle.Clear();
	}

	// Pull radius circle
	if (localPlayer && IsOptionEnabled(MapFilter::PullRadius))
	{
		const MapFilterOption& option = GetMapFilterOption(MapFilter::PullRadius);
		PullCircle.UpdateCircle(option.Color, option.Radius,
			PullX, PullY, SpawnAccess::GetZ(localPlayer));
	}
	else
	{
		PullCircle.Clear();
	}

	// Spell radius circle
	if (localPlayer && IsOptionEnabled(MapFilter::SpellRadius))
	{
		const MapFilterOption& option = GetMapFilterOption(MapFilter::SpellRadius);
		SpellCircle.UpdateCircle(option.Color, option.Radius,
			SpawnAccess::GetX(localPlayer), SpawnAccess::GetY(localPlayer), SpawnAccess::GetZ(localPlayer));
	}
	else
	{
		SpellCircle.Clear();
	}

	// Group coloring — skipped for now (needs pLocalPC->Group access)

	// Target line
	if (pLastTarget && localPlayer && target)
	{
		if (IsOptionEnabled(MapFilter::TargetLine))
		{
			if (!pTargetLine)
			{
				pTargetLine = InitLine();
				pTargetLine->Layer = activeLayer;
			}

			pTargetLine->Color.ARGB = GetMapFilterOption(MapFilter::TargetLine).Color.ToARGB();
			pTargetLine->Start.X = -SpawnAccess::GetX(localPlayer);
			pTargetLine->Start.Y = -SpawnAccess::GetY(localPlayer);
			pTargetLine->Start.Z = SpawnAccess::GetZ(localPlayer);
			pTargetLine->End.X = -SpawnAccess::GetX(target);
			pTargetLine->End.Y = -SpawnAccess::GetY(target);
			pTargetLine->End.Z = SpawnAccess::GetZ(target);
		}
		else if (pTargetLine)
		{
			DeleteLine(pTargetLine);
			pTargetLine = nullptr;
		}

		// Target radius circle
		if (IsOptionEnabled(MapFilter::TargetRadius))
		{
			const MapFilterOption& option = GetMapFilterOption(MapFilter::TargetRadius);
			TargetCircle.UpdateCircle(option.Color, option.Radius,
				SpawnAccess::GetX(target), SpawnAccess::GetY(target), SpawnAccess::GetZ(target));
		}
		else
		{
			TargetCircle.Clear();
		}

		// Target melee range circle
		if (IsOptionEnabled(MapFilter::TargetMelee))
		{
			const MapFilterOption& option = GetMapFilterOption(MapFilter::TargetMelee);
			SPAWNINFO* refSpawn = (static_cast<int>(option.Radius) <= 1) ? pControlledPlayer : target;
			float radius = get_melee_range(refSpawn, target);
			TargetMeleeCircle.UpdateCircle(option.Color, radius,
				SpawnAccess::GetX(target), SpawnAccess::GetY(target), SpawnAccess::GetZ(target));
		}
		else
		{
			TargetMeleeCircle.Clear();
		}
	}
	else
	{
		TargetCircle.Clear();
		TargetMeleeCircle.Clear();

		if (pTargetLine)
		{
			DeleteLine(pTargetLine);
			pTargetLine = nullptr;
		}
	}

	ExitMQ2Benchmark(bmMapRefresh);
}

// ---------------------------------------------------------------------------
// MapAttach / MapDetach
// ---------------------------------------------------------------------------

void MapAttach()
{
	MapViewLabel** ppGameLabels = GetGameLabelListHead();
	MapViewLine**  ppGameLines  = GetGameLineListHead();

	if (!ppGameLabels || !ppGameLines)
		return;

	if (gpLabelList)
	{
		// Save game's head FIRST, then flag as attached, then modify.
		// If any step crashes, MapDetach knows whether to restore.
		pActualLabelList = *ppGameLabels;
		s_labelsAttached = true;

		if (IsOptionEnabled(MapFilter::NormalLabels))
			gpLabelListTail->pNext = *ppGameLabels;

		*ppGameLabels = gpLabelList;
	}

	if (gpLineList)
	{
		s_linesAttached = true;
		gpLineListTail->pNext = *ppGameLines;
		*ppGameLines = gpLineList;
	}
}

void MapDetach()
{
	MapViewLabel** ppGameLabels = GetGameLabelListHead();
	MapViewLine**  ppGameLines  = GetGameLineListHead();

	if (!ppGameLabels || !ppGameLines)
		return;

	// Only restore what was actually attached — prevents corruption on partial MapAttach
	if (s_labelsAttached && gpLabelList)
	{
		*ppGameLabels = pActualLabelList;
		gpLabelListTail->pNext = nullptr;
		s_labelsAttached = false;
	}

	if (s_linesAttached && gpLineList)
	{
		*ppGameLines = gpLineListTail->pNext;
		gpLineListTail->pNext = nullptr;
		s_linesAttached = false;
	}
}

// ---------------------------------------------------------------------------
// MapHighlight / MapHide / MapShow
// ---------------------------------------------------------------------------

int MapHighlight(MQSpawnSearch* pSearch)
{
	if (!pSearch)
	{
		MapObject* pMapSpawn = gpActiveMapObjects;
		while (pMapSpawn)
		{
			pMapSpawn->SetHighlight(false);
			pMapSpawn = pMapSpawn->GetNext();
		}
		return 0;
	}

	uint32_t Count = 0;
	MAPSPAWN* pMapSpawn = gpActiveMapObjects;

	while (pMapSpawn)
	{
		SPAWNINFO* pSpawn = pMapSpawn->GetSpawn();
		if (pSpawn && SpawnMatchesSearch(pSearch, pLocalPlayer, pSpawn))
		{
			pMapSpawn->SetHighlight(true);
			Count++;
		}
		pMapSpawn = pMapSpawn->GetNext();
	}

	return Count;
}

int MapHide(MQSpawnSearch& Search)
{
	MapObject* pMapSpawn = gpActiveMapObjects;
	uint32_t Count = 0;

	while (pMapSpawn)
	{
		SPAWNINFO* pSpawn = pMapSpawn->GetSpawn();
		if (pSpawn && SpawnMatchesSearch(&Search, pLocalPlayer, pSpawn))
		{
			MapObject* pNext = pMapSpawn->GetNext();
			RemoveMapObject(pMapSpawn);
			pMapSpawn = pNext;
			Count++;
		}
		else
		{
			pMapSpawn = pMapSpawn->GetNext();
		}
	}

	return Count;
}

int MapShow(MQSpawnSearch& Search)
{
	SPAWNINFO* pSpawn = pSpawnList;
	uint32_t Count = 0;

	while (pSpawn)
	{
		if (FindMapObject(pSpawn) == nullptr
			&& SpawnMatchesSearch(&Search, pLocalPlayer, pSpawn))
		{
			AddSpawn(pSpawn, true);
			Count++;
		}
		pSpawn = SpawnAccess::GetNext(pSpawn);
	}

	return Count;
}

// ---------------------------------------------------------------------------
// MapClickLocation — handle left-click on map
// ---------------------------------------------------------------------------

void MapClickLocation(float x, float y, float z)
{
	int modKeys = GetModifierKeys();
	if (modKeys < 0 || modKeys >= MAX_CLICK_STRINGS)
		return;

	if (MapLeftClickString[modKeys][0] == 0)
		return;

	char szCommand[MAX_STRING];
	strcpy_s(szCommand, MapLeftClickString[modKeys]);

	// Substitute %x, %y, %z placeholders with world coordinates
	char xBuf[64], yBuf[64], zBuf[64];
	snprintf(xBuf, sizeof(xBuf), "%.2f", x);
	snprintf(yBuf, sizeof(yBuf), "%.2f", y);
	snprintf(zBuf, sizeof(zBuf), "%.2f", z);

	char szOutput[MAX_STRING] = { 0 };
	char* pOut = szOutput;
	char* pEnd = szOutput + sizeof(szOutput) - 1;
	for (const char* p = szCommand; *p && pOut < pEnd; )
	{
		if (*p == '%' && *(p + 1))
		{
			const char* sub = nullptr;
			switch (*(p + 1))
			{
			case 'x': sub = xBuf; break;
			case 'y': sub = yBuf; break;
			case 'z': sub = zBuf; break;
			}
			if (sub)
			{
				size_t len = strlen(sub);
				if (pOut + len < pEnd)
				{
					memcpy(pOut, sub, len);
					pOut += len;
				}
				p += 2;
				continue;
			}
		}
		*pOut++ = *p++;
	}
	*pOut = 0;

	EzCommand(szOutput);
}

// ---------------------------------------------------------------------------
// MapSelectTarget — handle right-click on hovered map label
// ---------------------------------------------------------------------------

bool MapSelectTarget()
{
	MAPSPAWN* pMapSpawn = GetCurrentMapObject();
	if (!pMapSpawn)
		return false;

	SPAWNINFO* pSpawn = pMapSpawn->GetSpawn();
	if (!pSpawn)
		return false;

	int modKeys = GetModifierKeys();

	if (modKeys == 0)
	{
		// No modifiers — directly set target
		GameState::SetTarget(pSpawn);
		LogFramework("MapSelectTarget: targeted '%s' (id=%u)", SpawnAccess::GetName(pSpawn), SpawnAccess::GetSpawnID(pSpawn));
	}
	else if (modKeys > 0 && modKeys < MAX_CLICK_STRINGS
		&& MapSpecialClickString[modKeys][0] != 0)
	{
		// With modifiers — substitute placeholders and execute command
		char szCommand[MAX_STRING];
		strcpy_s(szCommand, MapSpecialClickString[modKeys]);

		const char* name = SpawnAccess::GetName(pSpawn);
		char idBuf[32];
		snprintf(idBuf, sizeof(idBuf), "%u", SpawnAccess::GetSpawnID(pSpawn));
		char xBuf[64], yBuf[64], zBuf[64];
		snprintf(xBuf, sizeof(xBuf), "%.2f", SpawnAccess::GetX(pSpawn));
		snprintf(yBuf, sizeof(yBuf), "%.2f", SpawnAccess::GetY(pSpawn));
		snprintf(zBuf, sizeof(zBuf), "%.2f", SpawnAccess::GetZ(pSpawn));

		char szOutput[MAX_STRING] = { 0 };
		char* pOut = szOutput;
		char* pOutEnd = szOutput + sizeof(szOutput) - 1;
		for (const char* p = szCommand; *p && pOut < pOutEnd; )
		{
			if (*p == '%' && *(p + 1))
			{
				const char* sub = nullptr;
				switch (*(p + 1))
				{
				case 'n': sub = name; break;
				case 'i': sub = idBuf; break;
				case 'x': sub = xBuf; break;
				case 'y': sub = yBuf; break;
				case 'z': sub = zBuf; break;
				}
				if (sub)
				{
					size_t len = strlen(sub);
					if (pOut + len < pOutEnd)
					{
						memcpy(pOut, sub, len);
						pOut += len;
					}
					p += 2;
					continue;
				}
			}
			*pOut++ = *p++;
		}
		*pOut = 0;

		EzCommand(szOutput);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

MarkerType FindMarker(std::string_view szMark, MarkerType fallback)
{
	if (ci_equals(szMark, "none"))
		return MarkerType::None;
	if (ci_equals(szMark, "triangle"))
		return MarkerType::Triangle;
	if (ci_equals(szMark, "square"))
		return MarkerType::Square;
	if (ci_equals(szMark, "diamond"))
		return MarkerType::Diamond;
	if (ci_equals(szMark, "ring"))
		return MarkerType::Ring;

	return fallback;
}

bool IsFloat(const std::string& in)
{
	std::stringstream sstr(in);
	float f;
	return !((sstr >> std::noskipws >> f).rdstate() ^ std::ios_base::eofbit);
}

void MapLocSyntaxOutput()
{
	SyntaxError("Usage: /maploc [[size 10-200] | [width 1-10] | [color r g b] | [radius <distance>] | [rcolor r g b] | [yloc xloc (zloc) | target]] | [label text]");
	SyntaxError(" -- Omit locs to set defaults");
	SyntaxError(" -- Add label to loc by putting 'label <my text here>' only at end of command");
	SyntaxError("Remove maplocs: /maploc remove [index | [yloc xloc (zloc)]]");
	WriteChatf("MapLoc Defaults: Width:%.0f, Size:%.0f, Color:%d,%d,%d, Radius:%.0f, Radius Color:%d,%d,%d",
		gDefaultMapLocParams.width,
		gDefaultMapLocParams.lineSize,
		(int)gDefaultMapLocParams.color.Red, (int)gDefaultMapLocParams.color.Green, (int)gDefaultMapLocParams.color.Blue,
		gDefaultMapLocParams.circleRadius,
		(int)gDefaultMapLocParams.circleColor.Red, (int)gDefaultMapLocParams.circleColor.Green, (int)gDefaultMapLocParams.circleColor.Blue);
}

void MapRemoveLocation(const char* szLine)
{
	char arg[MAX_STRING];
	std::stringstream ss(szLine);
	ss >> arg; // reparse initial "remove"

	if (!ss || ss.eof())
	{
		int count = (int)gMapLocTemplates.size();
		DeleteAllMapLocs();
		WriteChatf("%d MapLoc(s) removed", count);
		return;
	}

	char yloc[MAX_STRING] = { "not set" };
	char xloc[MAX_STRING] = { "not set" };
	char zloc[MAX_STRING] = { "0" };
	char tag[MAX_STRING] = { 0 };

	ss >> arg;

	if (!IsFloat(arg))
	{
		MapLocSyntaxOutput();
		return;
	}

	strcpy_s(yloc, arg);
	MapLocTemplate* loc = nullptr;

	if (ss && !ss.eof())
	{
		ss >> arg;

		if (!IsFloat(arg))
		{
			MapLocSyntaxOutput();
			return;
		}

		strcpy_s(xloc, arg);

		if (ss && !ss.eof())
		{
			ss >> arg;
			if (!IsFloat(arg))
			{
				MapLocSyntaxOutput();
				return;
			}
			strcpy_s(zloc, arg);
		}

		// Truncate to integer locs
		std::string delim = ".";
		std::string temp = yloc;
		temp.erase(std::remove(temp.begin(), temp.end(), '+'), temp.end());
		strcpy_s(yloc, temp.substr(0, temp.find(delim)).c_str());
		temp = xloc;
		temp.erase(std::remove(temp.begin(), temp.end(), '+'), temp.end());
		strcpy_s(xloc, temp.substr(0, temp.find(delim)).c_str());
		temp = zloc;
		temp.erase(std::remove(temp.begin(), temp.end(), '+'), temp.end());
		strcpy_s(zloc, temp.substr(0, temp.find(delim)).c_str());

		sprintf_s(tag, "%s,%s,%s", yloc, xloc, zloc);

		auto maploc = GetMapLocTemplateByTag(tag);
		if (maploc == nullptr)
		{
			SyntaxError("Could not find MapLoc: %s", tag);
			return;
		}
		loc = maploc;
	}
	else // remove by index
	{
		size_t index;
		try
		{
			index = static_cast<size_t>(std::stoul(yloc));
		}
		catch (const std::exception&)
		{
			SyntaxError("Could not parse index: %s", yloc);
			return;
		}

		loc = GetMapLocByIndex((int)index);
		if (!loc)
		{
			WriteChatf("\arRemove loc by index out of bounds: %s", yloc);
			return;
		}

		strcpy_s(tag, loc->GetTag().c_str());
	}

	if (!loc)
	{
		SyntaxError("Could not find MapLoc: %s", tag);
		return;
	}

	int locIndex = loc->GetIndex();
	DeleteMapLoc(loc);
	WriteChatf("MapLoc removed: Index:%d, loc:%s", locIndex, tag);
}
