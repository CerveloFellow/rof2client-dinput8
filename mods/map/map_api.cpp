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

	int spawnCount = 0;
	int spawnObjectCount = 0;
	int groundCount = 0;

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
			if (AddSpawn(pSpawn))
				spawnObjectCount++;
			pSpawn = SpawnAccess::GetNext(pSpawn);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LogFramework("!!! MapGenerate EXCEPTION in spawn walk after %d spawns, code=0x%08X, lastSpawn=0x%p",
			spawnCount, GetExceptionCode(), pSpawn);
	}

	LogFramework("MapGenerate: spawn walk done — %d spawns, %d objects", spawnCount, spawnObjectCount);

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
	if (!pLocalPC) return;
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

	MapObject* mapObject = gpActiveMapObjects;
	while (mapObject)
	{
		bool forced = (mapObject == pOldLastTarget) && bTargetChanged;
		mapObject->Update(forced);

		if (!mapObject->CanDisplayObject())
		{
			MapObject* pNext = mapObject->GetNext();
			RemoveMapObject(mapObject);
			mapObject = pNext;
		}
		else
		{
			mapObject = mapObject->GetNext();
		}
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
// MapSelectTarget (simplified — no keyboard modifier support)
// ---------------------------------------------------------------------------

bool MapSelectTarget()
{
	MAPSPAWN* pMapSpawn = GetCurrentMapObject();
	if (!pMapSpawn)
		return true;

	// TODO: ground item targeting not implemented (needs MQGroundSpawn)

	// For spawns, just set the target
	// (pTarget is a macro for GameState::GetTarget(), can't assign through it)
	// TODO: implement target setting via game function pointer

	return IsOptionEnabled(MapFilter::TargetPath);
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
