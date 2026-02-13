/**
 * @file map_mod.cpp
 * @brief MapMod implementation — PostDraw hook and mod lifecycle.
 * @date 2026-02-09
 *
 * Hooks MapViewMap::PostDraw (vtable slot 4, offset 0x10) to:
 *   1. Run MapUpdate (position/color updates)
 *   2. Splice our label/line lists into the game's map for rendering
 *
 * PostDraw is MapViewMap-specific (only MapViewMap overrides it), so no
 * thisPtr filtering is needed. The base-class Draw (slot 3) renders generic
 * window chrome, NOT map content — label rendering happens in PostDraw.
 *
 * The vtable address is at MapViewMap__vftable_x (0x9EE580 before ASLR).
 */

#include "pch.h"
#include "map_mod.h"
#include "map.h"
#include "map_object.h"
#include "../../hooks.h"

#include <eqlib/Offsets.h>
#include <eqlib/offsets/eqgame.h>
#include <ctime>

extern "C" uintptr_t EQGameBaseAddress;

// ---------------------------------------------------------------------------
// Vtable hook typedefs
// ---------------------------------------------------------------------------

using PostDraw_t = int(__fastcall*)(void* thisPtr, void* edx);
using HandleLButtonDown_t = int(__fastcall*)(void* thisPtr, void* edx, const CXPoint& pos, uint32_t flags);
using HandleRButtonDown_t = int(__fastcall*)(void* thisPtr, void* edx, const CXPoint& pos, uint32_t flags);

static PostDraw_t PostDraw_Original = nullptr;
static HandleLButtonDown_t HandleLButtonDown_Original = nullptr;
static HandleRButtonDown_t HandleRButtonDown_Original = nullptr;

// ---------------------------------------------------------------------------
// PostDraw hook — MapUpdate + label/line splice for rendering
// ---------------------------------------------------------------------------

static bool s_mapRenderEnabled = false;
static int s_postDrawFrameCount = 0;
static int s_postDrawFaultCooldown = 0;
static bool s_firstRenderLogged = false;
static bool s_needsRegenerate = false;  // set when map cleared due to zone transition
static bool s_hadMapObjects = false;    // tracks populated→empty transition for zone detection

static int PostDraw_MapLogic(void* thisPtr, void* edx)
{
	int phase = 0;
	__try
	{
		phase = 1;  // SetMapViewMap
		SetMapViewMap(thisPtr);

		phase = 2;  // MapUpdate (position/color/text updates)
		MapUpdate();

		phase = 3;  // MapAttach (splice our labels into game's list)
		MapAttach();

		phase = 4;  // PostDraw_Original (game renders map content with our labels)
		int result = PostDraw_Original(thisPtr, edx);

		phase = 5;  // MapDetach (remove our labels from game's list)
		MapDetach();

		if (!s_firstRenderLogged && gpLabelList)
		{
			s_firstRenderLogged = true;
			MapViewLabel* lbl = gpLabelList;
			LogFramework("First successful render! label=0x%p text='%.30s' "
				"color=0x%08X size=%d layer=%d loc=(%.1f,%.1f,%.1f)",
				lbl, lbl->Label ? lbl->Label : "(null)",
				lbl->Color.ARGB, lbl->Size, lbl->Layer,
				lbl->Location.X, lbl->Location.Y, lbl->Location.Z);
		}

		return result;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LogFramework("!!! PostDraw EXCEPTION code=0x%08X at frame=%d phase=%d "
			"(1=SetMap 2=Update 3=Attach 4=PostDrawOrig 5=Detach) labels=0x%p tail=0x%p",
			GetExceptionCode(), s_postDrawFrameCount, phase,
			gpLabelList, gpLabelListTail);

		// If crash was in MapUpdate (phase 2), stale spawn pointers are the likely
		// cause — clear all map objects so the next retry doesn't hit the same data.
		if (phase <= 2)
		{
			__try { MapClear(); s_needsRegenerate = true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }
		}

		// Cooldown: skip 60 frames (~1 second) then retry
		s_postDrawFaultCooldown = 60;

		// Try to detach safely — MapAttach may have partially completed
		__try { MapDetach(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { /* best effort */ }

		return 0;
	}
}

static int __fastcall PostDraw_Detour(void* thisPtr, void* edx)
{
	// PostDraw only fires on MapViewMap — no thisPtr filtering needed
	s_postDrawFrameCount++;

	if (s_postDrawFrameCount <= 10 || s_postDrawFrameCount % 300 == 0)
		LogFramework("PostDraw frame %d: thisPtr=0x%p render=%d cooldown=%d regen=%d labels=0x%p",
			s_postDrawFrameCount, thisPtr, s_mapRenderEnabled ? 1 : 0,
			s_postDrawFaultCooldown, s_needsRegenerate ? 1 : 0, gpLabelList);

	// Fault cooldown — count down then allow retry
	if (s_postDrawFaultCooldown > 0)
	{
		s_postDrawFaultCooldown--;
		if (s_postDrawFaultCooldown == 0)
			LogFramework("PostDraw fault cooldown expired at frame %d — retrying",
				s_postDrawFrameCount);
		return PostDraw_Original(thisPtr, edx);
	}

	if (s_mapRenderEnabled)
	{
		// Check actual game state directly — s_mapRenderEnabled may be stale
		// during zone transitions before OnSetGameState fires
		if (GameState::GetGameState() == GAMESTATE_INGAME)
		{
			// Zone transition detection: pLocalPlayer goes null during zone-to-zone
			// while game state stays INGAME. Clear the map to avoid accessing freed spawns.
			SPAWNINFO* localPlayer = pLocalPlayer;
			if (!localPlayer)
			{
				if (gpActiveMapObjects || gpLabelList || gpLineList)
				{
					LogFramework("PostDraw: pLocalPlayer null at frame %d — zone transition, clearing map",
						s_postDrawFrameCount);
					MapClear();
					s_needsRegenerate = true;
				}
				return PostDraw_Original(thisPtr, edx);
			}

			// Regenerate after zone transition completes (pLocalPlayer restored)
			if (s_needsRegenerate)
			{
				LogFramework("PostDraw: pLocalPlayer restored at frame %d — regenerating map",
					s_postDrawFrameCount);
				MapClear();  // clear any objects added by OnAddSpawn during transition
				MapGenerate();
				s_needsRegenerate = false;
				s_hadMapObjects = (gpActiveMapObjects != nullptr);
				s_firstRenderLogged = false;
			}

			// Zone transition detection (alternate path): spawns may be removed
			// one-by-one via PrepForDestroyPlayer BEFORE pLocalPlayer goes null.
			// In that case the null-player check above never fires. Detect
			// populated→empty transition and schedule a full regenerate.
			if (gpActiveMapObjects)
			{
				s_hadMapObjects = true;
			}
			else if (s_hadMapObjects)
			{
				LogFramework("PostDraw: map depleted at frame %d — scheduling regenerate",
					s_postDrawFrameCount);
				s_needsRegenerate = true;
				s_hadMapObjects = false;
				// Don't regenerate this frame — spawns may still be loading.
				// The s_needsRegenerate check above will fire next frame.
				return PostDraw_Original(thisPtr, edx);
			}

			return PostDraw_MapLogic(thisPtr, edx);
		}
	}

	return PostDraw_Original(thisPtr, edx);
}

// ---------------------------------------------------------------------------
// HandleLButtonDown hook — left-click on map for location commands
// ---------------------------------------------------------------------------

static int __fastcall HandleLButtonDown_Detour(void* thisPtr, void* edx, const CXPoint& pos, uint32_t flags)
{
	__try
	{
		if (s_mapRenderEnabled && pLocalPlayer && GameState::GetGameState() == GAMESTATE_INGAME)
		{
			CVector3 worldCoords;
			if (CallGetWorldCoordinates(thisPtr, worldCoords))
			{
				// Use player's Z since map click has no Z information
				float playerZ = SpawnAccess::GetZ(pLocalPlayer);
				MapClickLocation(worldCoords.X, worldCoords.Y, playerZ);
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LogFramework("!!! HandleLButtonDown EXCEPTION code=0x%08X", GetExceptionCode());
	}

	return HandleLButtonDown_Original(thisPtr, edx, pos, flags);
}

// ---------------------------------------------------------------------------
// HandleRButtonDown hook — right-click on map for targeting
// ---------------------------------------------------------------------------

static int __fastcall HandleRButtonDown_Detour(void* thisPtr, void* edx, const CXPoint& pos, uint32_t flags)
{
	// Guard: HandleRButtonDown may be inherited from CSidlScreenWnd base class,
	// shared by many windows. Only process for our MapViewMap instance.
	if (thisPtr != GetMapViewMapPtr())
		return HandleRButtonDown_Original(thisPtr, edx, pos, flags);

	__try
	{
		if (s_mapRenderEnabled && pLocalPlayer && GameState::GetGameState() == GAMESTATE_INGAME)
		{
			if (pCurrentMapLabel)
			{
				if (MapSelectTarget())
					return 0;  // Handled — suppress default context menu
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LogFramework("!!! HandleRButtonDown EXCEPTION code=0x%08X", GetExceptionCode());
	}

	return HandleRButtonDown_Original(thisPtr, edx, pos, flags);
}

// ---------------------------------------------------------------------------
// MapMod implementation
// ---------------------------------------------------------------------------

bool MapMod::Initialize()
{
	LogFramework("MapMod::Initialize — setting up vtable hooks");

	// Resolve game function pointers for map interactions
	InitMapFunctions();

	uintptr_t vtableAddr = eqlib::FixEQGameOffset(MapViewMap__vftable_x);

	// PostDraw is vtable slot 4 (offset 0x10) — MapViewMap-specific
	uintptr_t postDrawAddr = *reinterpret_cast<uintptr_t*>(vtableAddr + 0x10);
	PostDraw_Original = reinterpret_cast<PostDraw_t>(postDrawAddr);

	LogFramework("  MapViewMap vtable = 0x%08X", static_cast<unsigned int>(vtableAddr));
	LogFramework("  PostDraw function = 0x%08X", static_cast<unsigned int>(postDrawAddr));

	Hooks::Install("MapViewMap_PostDraw",
		reinterpret_cast<void**>(&PostDraw_Original),
		reinterpret_cast<void*>(&PostDraw_Detour));

	// HandleLButtonDown is vtable slot 14 (offset 0x38) — MapViewMap-specific
	uintptr_t lbtnDownAddr = *reinterpret_cast<uintptr_t*>(vtableAddr + 0x38);
	HandleLButtonDown_Original = reinterpret_cast<HandleLButtonDown_t>(lbtnDownAddr);
	LogFramework("  HandleLButtonDown function = 0x%08X", static_cast<unsigned int>(lbtnDownAddr));

	Hooks::Install("MapViewMap_HandleLButtonDown",
		reinterpret_cast<void**>(&HandleLButtonDown_Original),
		reinterpret_cast<void*>(&HandleLButtonDown_Detour));

	// HandleRButtonDown is vtable slot 18 (offset 0x48) — may be inherited from
	// CSidlScreenWnd, so the detour has a thisPtr guard.
	uintptr_t rbtnDownAddr = *reinterpret_cast<uintptr_t*>(vtableAddr + 0x48);
	HandleRButtonDown_Original = reinterpret_cast<HandleRButtonDown_t>(rbtnDownAddr);
	LogFramework("  HandleRButtonDown function = 0x%08X", static_cast<unsigned int>(rbtnDownAddr));

	Hooks::Install("MapViewMap_HandleRButtonDown",
		reinterpret_cast<void**>(&HandleRButtonDown_Original),
		reinterpret_cast<void*>(&HandleRButtonDown_Detour));

	// Initialize map state (clears all circles)
	MapInit();

	// Enable default filters so dots appear on the map
	MapFilterOptions[static_cast<size_t>(MapFilter::All)].Enabled = true;
	MapFilterOptions[static_cast<size_t>(MapFilter::PC)].Enabled = true;
	MapFilterOptions[static_cast<size_t>(MapFilter::NPC)].Enabled = true;
	MapFilterOptions[static_cast<size_t>(MapFilter::Target)].Enabled = true;
	MapFilterOptions[static_cast<size_t>(MapFilter::TargetLine)].Enabled = true;
	MapFilterOptions[static_cast<size_t>(MapFilter::NormalLabels)].Enabled = true;
	MapFilterOptions[static_cast<size_t>(MapFilter::Corpse)].Enabled = true;
	MapFilterOptions[static_cast<size_t>(MapFilter::NPCCorpse)].Enabled = true;
	MapFilterOptions[static_cast<size_t>(MapFilter::PCCorpse)].Enabled = true;

	// Set default colors from the table's DefaultColor
	for (auto& opt : MapFilterOptions)
	{
		if (opt.HasColor())
			opt.Color = opt.DefaultColor;
	}

	// Load persistent settings from INI (overrides defaults above)
	LoadMapSettings();

	// Register slash commands
	Commands::AddCommand("/mapfilter", MapFilters);
	Commands::AddCommand("/maphide", MapHideCmd);
	Commands::AddCommand("/mapshow", MapShowCmd);
	Commands::AddCommand("/highlight", MapHighlightCmd);
	Commands::AddCommand("/mapnames", MapNames);
	Commands::AddCommand("/mapclick", MapClickCommand);
	Commands::AddCommand("/mapactivelayer", MapActiveLayerCmd);
	Commands::AddCommand("/maploc", MapSetLocationCmd);

	LogFramework("MapMod initialized (14 hooks + 8 commands)");
	return true;
}

void MapMod::Shutdown()
{
	LogFramework("MapMod::Shutdown");

	// Remove slash commands
	Commands::RemoveCommand("/mapfilter");
	Commands::RemoveCommand("/maphide");
	Commands::RemoveCommand("/mapshow");
	Commands::RemoveCommand("/highlight");
	Commands::RemoveCommand("/mapnames");
	Commands::RemoveCommand("/mapclick");
	Commands::RemoveCommand("/mapactivelayer");
	Commands::RemoveCommand("/maploc");

	s_mapRenderEnabled = false;
	MapClear();
	m_mapActive = false;
}

void MapMod::OnPulse()
{
	// MapUpdate is called from PostDraw detour, not OnPulse.
	// But highlight pulse animation runs here on a timer.
	if (HighlightPulse)
	{
		static clock_t s_lastPulse = clock();
		clock_t now = clock();
		if (now > s_lastPulse + 50)  // ~50ms interval
		{
			if (HighlightPulseIndex == 5 || HighlightPulseIndex == -5)
				HighlightPulseIncreasing = !HighlightPulseIncreasing;

			if (HighlightPulseIncreasing)
				HighlightPulseIndex++;
			else
				HighlightPulseIndex--;

			s_lastPulse = now;
		}
	}
}

bool MapMod::OnIncomingMessage(uint32_t /*opcode*/, const void* /*buffer*/, uint32_t /*size*/)
{
	return true;  // pass-through
}

void MapMod::OnAddSpawn(void* pSpawn)
{
	if (m_mapActive)
		AddSpawn(static_cast<SPAWNINFO*>(pSpawn));
}

void MapMod::OnRemoveSpawn(void* pSpawn)
{
	if (m_mapActive)
		RemoveSpawn(static_cast<SPAWNINFO*>(pSpawn));
}

void MapMod::OnAddGroundItem(void* pItem)
{
	if (m_mapActive)
		AddGroundItem(static_cast<EQGroundItem*>(pItem));
}

void MapMod::OnRemoveGroundItem(void* pItem)
{
	if (m_mapActive)
		RemoveGroundItem(static_cast<EQGroundItem*>(pItem));
}

void MapMod::OnSetGameState(int gameState)
{
	if (gameState == GAMESTATE_INGAME)
	{
		LogFramework("MapMod: game state INGAME — generating map");
		MapClear();
		MapGenerate();
		m_mapActive = true;
		s_mapRenderEnabled = true;
		s_postDrawFaultCooldown = 0;
		s_needsRegenerate = false;
		s_hadMapObjects = (gpActiveMapObjects != nullptr);
		s_firstRenderLogged = false;
	}
	else
	{
		LogFramework("MapMod: game state %d — clearing map", gameState);
		s_mapRenderEnabled = false;
		MapClear();
		m_mapActive = false;
		s_hadMapObjects = false;
	}
}

void MapMod::OnCleanUI()
{
	LogFramework("MapMod::OnCleanUI — clearing map");
	s_mapRenderEnabled = false;
	MapClear();
	m_mapActive = false;
	s_hadMapObjects = false;
}

void MapMod::OnReloadUI()
{
	if (GameState::GetGameState() == GAMESTATE_INGAME)
	{
		LogFramework("MapMod::OnReloadUI — regenerating map");
		MapClear();
		MapGenerate();
		m_mapActive = true;
		s_mapRenderEnabled = true;
	}
}
