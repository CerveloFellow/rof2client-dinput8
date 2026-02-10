/**
 * @file map.h
 * @brief Map plugin data structures — ported from MQ2Map.h
 * @date 2026-02-09
 *
 * MapFilter enum, MapFilterOption struct, extern globals, inline helpers,
 * and forward declarations for the map API.
 */

#pragma once

#include "../../mq_compat.h"

#include <map>
#include <vector>
#include <memory>

// ---------------------------------------------------------------------------
// MapFilter enum
// ---------------------------------------------------------------------------

enum class MapFilter {
	Invalid = -1,
	All = 0,
	PC = 1,
	PCConColor = 2,
	Group = 3,
	Mount = 4,
	NPC = 5,
	NPCConColor = 6,
	Untargetable = 7,
	Pet = 8,
	Corpse = 9,
	Chest = 10,
	Trigger = 11,
	Trap = 12,
	Timer = 13,
	Ground = 14,
	Target = 15,
	TargetLine = 16,
	TargetRadius = 17,
	TargetMelee = 18,
	Vector = 19,
	Custom = 20,
	CastRadius = 21,
	NormalLabels = 22,
	ContextMenu = 23,
	SpellRadius = 24,
	Aura = 25,
	Object = 26,
	Banner = 27,
	Campfire = 28,
	PCCorpse = 29,
	NPCCorpse = 30,
	Mercenary = 31,
	Named = 32,
	TargetPath = 33,
	Marker = 34,
	CampRadius = 35,
	PullRadius = 36,

	Last,
};

// ---------------------------------------------------------------------------
// MapObject forward declaration and alias
// ---------------------------------------------------------------------------

class MapObject;
using MAPSPAWN = MapObject;

// ---------------------------------------------------------------------------
// MarkerType enum
// ---------------------------------------------------------------------------

enum class MarkerType
{
	None = 0,
	Triangle,
	Square,
	Diamond,
	Ring,
	Unknown,
};

// ---------------------------------------------------------------------------
// MapFilterOption struct
// ---------------------------------------------------------------------------

struct MapFilterOption
{
	enum Flags {
		Toggle       = 0x01,
		NoColor      = 0x02,
		Regenerate   = 0x04,
		UsesRadius   = 0x08,
		Object       = 0x10,
	};

	const char*      szName = nullptr;
	bool             Default = false;
	MapFilter        ThisFilter = MapFilter::Invalid;
	MQColor          DefaultColor;
	MapFilter        RequiresOption = MapFilter::Invalid;
	uint32_t         Flags = 0;
	const char*      szHelpString = nullptr;

	MarkerType       Marker = MarkerType::None;
	int              MarkerSize = 0;
	bool             Enabled = false;
	float            Radius = 0;
	MQColor          Color;

	bool IsToggle() const { return Flags & Toggle; }
	bool IsRegenerateOnChange() const { return Flags & Regenerate; }
	bool IsRadius() const { return Flags & UsesRadius; }
	bool HasColor() const { return !(Flags & NoColor); }
	bool IsObject() const { return Flags & Object; }
};

// ---------------------------------------------------------------------------
// Extern globals
// ---------------------------------------------------------------------------

extern uint32_t bmMapRefresh;
extern int activeLayer;
extern float CampX;
extern float CampY;
extern float PullX;
extern float PullY;

extern MQColor HighlightColor;
extern int HighlightSIDELEN;
extern bool HighlightPulse;
extern bool HighlightPulseIncreasing;
extern int HighlightPulseIndex;
extern int HighlightPulseDiff;

extern char MapNameString[MAX_STRING];
extern char MapTargetNameString[MAX_STRING];
extern char mapshowStr[MAX_STRING];
extern char maphideStr[MAX_STRING];
extern MQSpawnSearch MapFilterCustom;
extern MQSpawnSearch MapFilterNamed;

extern std::vector<MapFilterOption> MapFilterOptions;
extern MapFilterOption MapFilterInvalidOption;

constexpr int MAX_CLICK_STRINGS = 16;
extern char MapSpecialClickString[MAX_CLICK_STRINGS][MAX_STRING];
extern char MapLeftClickString[MAX_CLICK_STRINGS][MAX_STRING];
extern bool repeatMapshow;
extern bool repeatMaphide;

extern std::vector<MapFilterOption*> mapFilterObjectOptions;
extern std::vector<MapFilterOption*> mapFilterGeneralOptions;

// Linked list globals (defined in map_object.cpp, used by map_api.cpp)
extern MapObject* gpActiveMapObjects;
extern MapViewLabel* gpLabelList;
extern MapViewLabel* gpLabelListTail;
extern MapViewLine* gpLineList;
extern MapViewLine* gpLineListTail;
extern MapObject* pLastTarget;

// ---------------------------------------------------------------------------
// Inline helpers
// ---------------------------------------------------------------------------

inline MapFilterOption& GetMapFilterOption(MapFilter Option)
{
	if (Option < MapFilter::All || Option >= MapFilter::Last)
		return MapFilterInvalidOption;

	return MapFilterOptions[static_cast<size_t>(Option)];
}

inline bool IsOptionEnabled(MapFilter Option)
{
	if (Option == MapFilter::Invalid)
		return true;

	MapFilterOption& option = GetMapFilterOption(Option);
	return option.Enabled && IsOptionEnabled(option.RequiresOption);
}

inline bool RequirementsMet(MapFilter Option)
{
	if (Option == MapFilter::Invalid)
		return true;

	MapFilterOption& option = GetMapFilterOption(Option);
	return IsOptionEnabled(option.RequiresOption);
}

// ---------------------------------------------------------------------------
// Function declarations
// ---------------------------------------------------------------------------

MarkerType FindMarker(std::string_view szMark, MarkerType fallback = MarkerType::Unknown);

PLUGIN_API MapViewLine* InitLine();
PLUGIN_API void DeleteLine(MapViewLine* pLine);

// Commands (Phase 7+)
void MapFilters(PlayerClient* pChar, const char* szLine);
void MapFilterSetting(PlayerClient* pChar, MapFilter nMapFilter, const char* szValue = nullptr);
void MapHighlightCmd(PlayerClient* pChar, const char* szLine);
void PulseReset();
void MapHideCmd(PlayerClient* pChar, const char* szLine);
void MapShowCmd(PlayerClient* pChar, const char* szLine);
void MapNames(PlayerClient* pChar, const char* szLine);
void MapClickCommand(PlayerClient* pChar, const char* szLine);
void MapActiveLayerCmd(PlayerClient* pChar, const char* szLine);
void MapSetLocationCmd(PlayerClient* pChar, const char* szLine);
char* FormatMarker(const char* szLine, char* szDest, size_t BufferSize);
bool IsFloat(const std::string& in);

// Current map label (game global — the label the user is hovering over)
#define pCurrentMapLabel GameState::GetCurrentMapLabel()

// MapViewMap pointer management (set from PostDraw detour)
void SetMapViewMap(void* ptr);

// API
void MapInit();
void MapClear();
void MapGenerate();
int MapHighlight(MQSpawnSearch* pSearch);
int MapHide(MQSpawnSearch& Search);
int MapShow(MQSpawnSearch& Search);
void MapUpdate();
void MapAttach();
void MapDetach();

void MapLocSyntaxOutput();
void MapRemoveLocation(const char* szLine);

bool MapSelectTarget();

MapObject* AddSpawn(SPAWNINFO* pNewSpawn, bool ExplicitAllow = false);
bool RemoveSpawn(SPAWNINFO* pSpawn);
MapObject* AddGroundItem(GROUNDITEM* pGroundItem);
void RemoveGroundItem(GROUNDITEM* pGroundItem);
