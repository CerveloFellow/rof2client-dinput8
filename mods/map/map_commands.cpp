/**
 * @file map_commands.cpp
 * @brief Map slash commands and INI loading — ported from MQ2MapCommands.cpp
 * @date 2026-02-10
 *
 * Commands: /mapfilter, /maphide, /mapshow, /highlight, /mapnames,
 *           /mapactivelayer, /mapclick
 *
 * Key substitutions from MQ source:
 *   fmt::format(...)     -> snprintf(...)
 *   pChar->X             -> SpawnAccess::GetX(pChar)
 *   GetCharInfo()        -> pLocalPlayer null check
 *   bRunNextCommand=true -> removed
 *   DWORD                -> uint32_t
 *   MapFilterOptions[i].szName != nullptr -> i < MapFilterOptions.size()
 */

#include "pch.h"
#include "map_object.h"

#include <sstream>
#include <algorithm>

// ---------------------------------------------------------------------------
// Marker type names (for FormatMarker INI writes)
// ---------------------------------------------------------------------------

static const char* szMarkType[] =
{
	"None",
	"Triangle",
	"Square",
	"Diamond",
	"Ring",
};

// ---------------------------------------------------------------------------
// MapFilterSetting — display or toggle a single filter
// ---------------------------------------------------------------------------

void MapFilterSetting(PlayerClient* pChar, MapFilter nMapFilter, const char* szValue /* = nullptr */)
{
	if (!pChar) return;

	char Buff[MAX_STRING] = { 0 };

	const char* szFilterMap[] =
	{
		"hide",
		"show",
		nullptr
	};

	MapFilterOption* pMapFilter = &MapFilterOptions[static_cast<size_t>(nMapFilter)];
	if (!RequirementsMet(nMapFilter))
	{
		MapFilterOption& option = MapFilterOptions[static_cast<size_t>(pMapFilter->RequiresOption)];
		WriteChatf("'%s' requires '%s' option.  Please enable this option first.", pMapFilter->szName, option.szName);
		return;
	}

	if (!szValue)
	{
		char szBuffer[MAX_STRING] = { 0 };

		if (pMapFilter->IsToggle())
		{
			sprintf_s(szBuffer, "%s: %s", pMapFilter->szName, szFilterMap[pMapFilter->Enabled]);
		}
		else if (nMapFilter == MapFilter::Custom)
		{
			if (IsOptionEnabled(nMapFilter) == 0)
			{
				sprintf_s(szBuffer, "%s: Off", pMapFilter->szName);
			}
			else
			{
				sprintf_s(szBuffer, "%s: %s", pMapFilter->szName, FormatSearchSpawn(Buff, sizeof(Buff), &MapFilterCustom));
			}
		}
		else if (pMapFilter->IsRadius())
		{
			sprintf_s(szBuffer, "%s: %0.2f", pMapFilter->szName, pMapFilter->Radius);
		}
		else
		{
			sprintf_s(szBuffer, "%s: %d", pMapFilter->szName, pMapFilter->Enabled);
		}

		if (pMapFilter->HasColor())
		{
			WriteChatf("%s (Color: %d %d %d)", szBuffer, pMapFilter->Color.Red,
				pMapFilter->Color.Green, pMapFilter->Color.Blue);
		}
		else
		{
			WriteChatf("%s", szBuffer);
		}
	}
	else
	{
		if (pMapFilter->IsToggle())
		{
			if (!_stricmp(szFilterMap[0], szValue))
			{
				pMapFilter->Enabled = false;
			}
			else if (!_stricmp(szFilterMap[1], szValue))
			{
				pMapFilter->Enabled = true;
			}
			else
			{
				pMapFilter->Enabled = !pMapFilter->Enabled;
			}

			WriteChatf("%s is now set to: %s", pMapFilter->szName, szFilterMap[IsOptionEnabled(nMapFilter)]);
		}
		else if (nMapFilter == MapFilter::Custom)
		{
			ClearSearchSpawn(&MapFilterCustom);
			if (szValue[0] == 0)
			{
				pMapFilter->Enabled = false;
				WriteChatf("%s is now set to: Off", pMapFilter->szName);
			}
			else
			{
				pMapFilter->Enabled = true;
				ParseSearchSpawn(szValue, &MapFilterCustom);

				WriteChatf("%s is now set to: %s", pMapFilter->szName, FormatSearchSpawn(Buff, sizeof(Buff), &MapFilterCustom));
			}
		}
		else if (nMapFilter == MapFilter::Marker)
		{
			char szBuffer2[MAX_STRING] = { 0 };
			GetArg(szBuffer2, szValue, 1);

			if (!_stricmp(szFilterMap[0], szValue))
			{
				pMapFilter->Enabled = false;
				WriteChatf("%s is now set to: %s", pMapFilter->szName, szFilterMap[IsOptionEnabled(nMapFilter)]);
			}
			else if (!_stricmp(szFilterMap[1], szValue))
			{
				pMapFilter->Enabled = true;
				WriteChatf("%s is now set to: %s", pMapFilter->szName, szFilterMap[IsOptionEnabled(nMapFilter)]);
			}
			else
			{
				pMapFilter->Enabled = true;
				WriteChatf("%s %s", pMapFilter->szName, FormatMarker(szValue, Buff, sizeof(Buff)));
			}
		}
	}

	if (szValue)
	{
		WritePrivateProfileBool("Map Filters", pMapFilter->szName, pMapFilter->Enabled, INIFileName);
	}
}

// ---------------------------------------------------------------------------
// MapFilterColorSetting — set R G B color for a filter option
// ---------------------------------------------------------------------------

static void MapFilterColorSetting(MapFilter nMapFilter, const char* szValue)
{
	char szArg[MAX_STRING] = { 0 };
	MapFilterOption& option = MapFilterOptions[static_cast<size_t>(nMapFilter)];

	if (!option.HasColor())
	{
		WriteChatf("Option '%s' does not have a color.", option.szName);
	}
	else
	{
		GetArg(szArg, szValue, 2);

		MQColor& color = option.Color;

		if (szArg[0] == 0)
		{
			option.Color = option.DefaultColor;
		}
		else
		{
			uint8_t R = static_cast<uint8_t>(std::clamp(GetIntFromString(szArg, 255), 0, 255));
			uint8_t G = static_cast<uint8_t>(std::clamp(GetIntFromString(GetArg(szArg, szValue, 3), 255), 0, 255));
			uint8_t B = static_cast<uint8_t>(std::clamp(GetIntFromString(GetArg(szArg, szValue, 4), 255), 0, 255));
			color = MQColor(R, G, B);
		}

		WriteChatf("Option '%s' color set to: %d %d %d", option.szName, color.Red, color.Green, color.Blue);

		char keyBuf[128];
		snprintf(keyBuf, sizeof(keyBuf), "%s-Color", option.szName);
		WritePrivateProfileInt("Map Filters", keyBuf, option.Color.ToRGB(), INIFileName);
	}
}

// ---------------------------------------------------------------------------
// MapFilterRadiusSetting — set radius value for a filter option
// ---------------------------------------------------------------------------

static void MapFilterRadiusSetting(SPAWNINFO* pChar, MapFilter nMapFilter, const char* szValue)
{
	if (!pChar) return;
	MapFilterOption* option = &MapFilterOptions[static_cast<size_t>(nMapFilter)];
	option->Radius = GetFloatFromString(szValue, 0.0f);
	option->Enabled = option->Radius > 0.0f;

	if (option->Radius > 0.0f && !_stricmp(option->szName, "CampRadius"))
	{
		CampX = SpawnAccess::GetX(pChar);
		CampY = SpawnAccess::GetY(pChar);
	}

	if (option->Radius > 0.0f && !_stricmp(option->szName, "PullRadius"))
	{
		PullX = SpawnAccess::GetX(pChar);
		PullY = SpawnAccess::GetY(pChar);
	}

	WriteChatf("%s is now set to: %.2f", option->szName, option->Radius);
	WritePrivateProfileFloat("Map Filters", option->szName, option->Radius, INIFileName);
}

// ---------------------------------------------------------------------------
// MapFilters — /mapfilter command handler
// ---------------------------------------------------------------------------

void MapFilters(PlayerClient* pChar, const char* szLine)
{
	char szArg[MAX_STRING] = { 0 };
	GetArg(szArg, szLine, 1);
	const char* szRest = GetNextArg(szLine);

	if (szArg[0] == 0) // Display Settings
	{
		WriteChatColor("Map filtering settings:");
		WriteChatColor("-----------------------");

		for (size_t i = 0; i < MapFilterOptions.size(); i++)
		{
			MapFilter mf = static_cast<MapFilter>(i);

			if (RequirementsMet(mf))
				MapFilterSetting(pChar, mf);
		}
	}
	else if (!_strnicmp(szArg, "help", 4)) // Display Help
	{
		WriteChatColor("Map filtering options:");

		for (size_t i = 0; i < MapFilterOptions.size(); i++)
		{
			WriteChatf("%s%s: %s", MapFilterOptions[i].szName, (MapFilterOptions[i].IsToggle()) ? "" : " #", MapFilterOptions[i].szHelpString);
		}

		WriteChatColor("'option' color [r g b]: Set display color for 'option' (Omit to reset to default)");
	}
	else // Set Option
	{
		MapFilterOption* Found = nullptr;
		for (size_t i = 0; i < MapFilterOptions.size(); i++)
		{
			MapFilterOption& option = MapFilterOptions[i];

			if (!_stricmp(szArg, option.szName))
			{
				if (!_strnicmp(szRest, "color", 5))
				{
					MapFilterColorSetting(static_cast<MapFilter>(i), szRest);
				}
				else if (option.IsRadius())
				{
					MapFilterRadiusSetting(pChar, static_cast<MapFilter>(i), szRest);
				}
				else
				{
					MapFilterSetting(pChar, static_cast<MapFilter>(i), szRest);
				}

				Found = &option;
			}
		}

		if (!Found)
		{
			SyntaxError("Usage: /mapfilter [option|help]");
		}
		else if (Found->IsRegenerateOnChange())
		{
			MapClear();
			MapGenerate();
		}
	}
}

// ---------------------------------------------------------------------------
// MapActiveLayerCmd — /mapactivelayer command handler
// ---------------------------------------------------------------------------

void MapActiveLayerCmd(PlayerClient* pChar, const char* szLine)
{
	const int newActiveLayer = GetIntFromString(szLine, -1);
	if (szLine == nullptr || szLine[0] == 0 || newActiveLayer < 0 || newActiveLayer > 3)
	{
		SyntaxError("Usage: /mapactivelayer [0|1|2|3]");
		return;
	}

	activeLayer = newActiveLayer;

	WriteChatf("Map Active Layer: %d", activeLayer);

	WritePrivateProfileInt("Map Filters", "ActiveLayer", activeLayer, INIFileName);

	MapClear();
	MapGenerate();
}

// ---------------------------------------------------------------------------
// MapHighlightCmd — /highlight command handler
// ---------------------------------------------------------------------------

void MapHighlightCmd(PlayerClient* pChar, const char* szLine)
{
	char szArg[MAX_STRING] = { 0 };
	char red[64] = { 0 };
	char green[64] = { 0 };
	char blue[64] = { 0 };
	std::stringstream ss(szLine);

	if (szLine[0] == 0)
	{
		SyntaxError("Usage: /highlight [reset|spawnfilter|size|pulse|[color # # #]]");
		return;
	}

	ss >> szArg;

	if (!_stricmp(szArg, "color"))
	{
		char usage[MAX_STRING] = { "Usage: /highlight color [0-255] [0-255] [0-255]" };

		if (ss && !ss.eof())
		{
			ss >> red;
		}
		else
		{
			SyntaxError(usage);
			return;
		}

		if (ss && !ss.eof())
		{
			ss >> green;
		}
		else
		{
			SyntaxError(usage);
			return;
		}

		if (ss && !ss.eof())
		{
			ss >> blue;
		}
		else
		{
			SyntaxError(usage);
			return;
		}

		if (GetIntFromString(red, -1) < 0 || GetIntFromString(red, 256) > 255 || GetIntFromString(green, -1) < 0 || GetIntFromString(green, 256) > 255 || GetIntFromString(blue, -1) < 0 || GetIntFromString(blue, 256) > 255)
		{
			SyntaxError(usage);
			return;
		}

		unsigned char R = static_cast<unsigned char>(GetIntFromString(red, 255));
		unsigned char G = static_cast<unsigned char>(GetIntFromString(green, 255));
		unsigned char B = static_cast<unsigned char>(GetIntFromString(blue, 255));
		HighlightColor = MQColor(R, G, B);

		WriteChatf("Highlight color: %d %d %d", R, G, B);

		WritePrivateProfileInt("Map Filters", "High-Color", HighlightColor.ARGB, INIFileName);
		return;
	}
	else if (!_stricmp(szArg, "reset"))
	{
		MapHighlight(nullptr);
		WriteChatColor("Highlighting reset");
		return;
	}
	else if (!_stricmp(szArg, "size"))
	{
		if (ss && !ss.eof())
		{
			ss >> szArg;
		}
		else
		{
			SyntaxError("Usage: /highlight size #");
			return;
		}

		if (GetIntFromString(szArg, -1) == -1)
		{
			SyntaxError("Usage: /highlight size #");
			return;
		}

		HighlightSIDELEN = GetIntFromString(szArg, HighlightSIDELEN);
		PulseReset();

		WriteChatf("Highlight size: %d", HighlightSIDELEN);

		WritePrivateProfileInt("Map Filters", "HighSize", HighlightSIDELEN, INIFileName);
		return;
	}
	else if (!_stricmp(szArg, "pulse"))
	{
		HighlightPulse = !HighlightPulse;
		PulseReset();

		WriteChatf("Highlight pulse: %s", HighlightPulse ? "ON" : "OFF");

		WritePrivateProfileBool("Map Filters", "HighPulse", HighlightPulse, INIFileName);
		return;
	}

	if (pLocalPlayer)
	{
		MQSpawnSearch ssHighlight;
		ClearSearchSpawn(&ssHighlight);
		ParseSearchSpawn(szLine, &ssHighlight);

		WriteChatf("%d mapped spawns highlighted", MapHighlight(&ssHighlight));
	}
}

// ---------------------------------------------------------------------------
// PulseReset — reset highlight pulse animation state
// ---------------------------------------------------------------------------

void PulseReset()
{
	HighlightPulseIncreasing = true;
	HighlightPulseIndex = 0;
	HighlightPulseDiff = HighlightSIDELEN / 10;
}

// ---------------------------------------------------------------------------
// MapHideCmd — /maphide command handler
// ---------------------------------------------------------------------------

void MapHideCmd(PlayerClient* pChar, const char* szLine)
{
	char szArg[MAX_STRING] = { 0 };

	if (szLine == nullptr || szLine[0] == 0)
	{
		SyntaxError("Usage: /maphide [spawnfilter|reset|repeat]");
		return;
	}

	GetArg(szArg, szLine, 1);
	if (!_stricmp(szArg, "reset"))
	{
		MapClear();
		MapGenerate();
		WriteChatColor("Map spawns regenerated");
		return;
	}

	if (!_stricmp(szArg, "repeat"))
	{
		repeatMaphide = !repeatMaphide;

		WritePrivateProfileInt("Map Filters", "Maphide-Repeat", repeatMaphide, INIFileName);

		WriteChatf("maphide repeat set to: %s", (repeatMaphide ? "on" : "off"));
		return;
	}

	if (pLocalPlayer)
	{
		MQSpawnSearch ssHide;
		ClearSearchSpawn(&ssHide);
		ParseSearchSpawn(szLine, &ssHide);

		WriteChatf("%d mapped spawns hidden", MapHide(ssHide));
	}
}

// ---------------------------------------------------------------------------
// MapShowCmd — /mapshow command handler
// ---------------------------------------------------------------------------

void MapShowCmd(PlayerClient* pChar, const char* szLine)
{
	char szArg[MAX_STRING] = { 0 };

	if (szLine == nullptr || szLine[0] == 0)
	{
		SyntaxError("Usage: /mapshow [spawnfilter|reset|repeat]");
		return;
	}

	GetArg(szArg, szLine, 1);
	if (!_stricmp(szArg, "reset"))
	{
		MapClear();
		MapGenerate();

		WriteChatColor("Map spawns regenerated");
		return;
	}

	if (!_stricmp(szArg, "repeat"))
	{
		repeatMapshow = !repeatMapshow;

		WritePrivateProfileBool("Map Filters", "Mapshow-Repeat", repeatMapshow, INIFileName);

		WriteChatf("mapshow repeat set to: %s", (repeatMapshow ? "on" : "off"));
		return;
	}

	if (pLocalPlayer)
	{
		MQSpawnSearch ssShow;
		ClearSearchSpawn(&ssShow);
		ParseSearchSpawn(szLine, &ssShow);

		WriteChatf("%d previously hidden spawns shown", MapShow(ssShow));
	}
}

// ---------------------------------------------------------------------------
// MapNames — /mapnames command handler
// ---------------------------------------------------------------------------

void MapNames(PlayerClient* pChar, const char* szLine)
{
	if (!szLine[0])
	{
		WriteChatf("Normal naming string: %s", MapNameString);
		WriteChatf("Target naming string: %s", MapTargetNameString);
		return;
	}

	char szArg[MAX_STRING] = { 0 };
	GetArg(szArg, szLine, 1);
	const char* szRest = GetNextArg(szLine);

	if (!_stricmp(szArg, "target"))
	{
		if (!_stricmp(szRest, "reset"))
			strcpy_s(MapTargetNameString, "%N");
		else
			strcpy_s(MapTargetNameString, szRest);

		WriteChatf("Target naming string: %s", MapTargetNameString);

		WritePrivateProfileString("Naming Schemes", "Target", MapTargetNameString, INIFileName);
		MapClear();
		MapGenerate();
	}
	else if (!_stricmp(szArg, "normal"))
	{
		if (!_stricmp(szRest, "reset"))
			strcpy_s(MapNameString, "%N");
		else
			strcpy_s(MapNameString, szRest);

		WriteChatf("Normal naming string: %s", MapNameString);

		WritePrivateProfileString("Naming Schemes", "Normal", MapNameString, INIFileName);
		MapClear();
		MapGenerate();
	}
	else
	{
		SyntaxError("Usage: /mapnames <target|normal> [value|reset]");
	}
}

// ---------------------------------------------------------------------------
// MapClickCommand — /mapclick command handler
// ---------------------------------------------------------------------------

void MapClickCommand(PlayerClient* pChar, const char* szLine)
{
	if (!szLine[0])
	{
		SyntaxError("Usage: /mapclick [left] <list|<key[+key[...]]> <clear|command>>");
		return;
	}

	auto f = [](char szArg[MAX_STRING], const char* szRest, char(&command_array)[16][MAX_STRING], const char* szSection)
	{
		if (!_stricmp(szArg, "list"))
		{
			int Count = 0;
			for (int i = 1; i < 16; i++)
			{
				if (command_array[i][0])
				{
					WriteChatf("%d: %s", i, command_array[i]);
					Count++;
				}
			}

			WriteChatf("%d special click commands", Count);
			return;
		}

		int combo = GetIntFromString(szArg, 0);
		if (combo <= 0 || combo >= 16)
		{
			WriteChatf("Invalid combo '%s'", szArg);
			return;
		}

		if (!szRest[0])
		{
			WriteChatf("%d: %s", combo, command_array[combo]);
			return;
		}

		if (!_stricmp(szRest, "clear"))
		{
			command_array[combo][0] = 0;
			char keyBuf[32];
			snprintf(keyBuf, sizeof(keyBuf), "KeyCombo%d", combo);
			WritePrivateProfileString(szSection, keyBuf, command_array[combo], INIFileName);

			WriteChatf("%s -- %d cleared", szSection, combo);
			return;
		}

		strcpy_s(command_array[combo], szRest);
		char keyBuf[32];
		snprintf(keyBuf, sizeof(keyBuf), "KeyCombo%d", combo);
		WritePrivateProfileString(szSection, keyBuf, command_array[combo], INIFileName);

		WriteChatf("%s -- %d: %s", szSection, combo, command_array[combo]);
	};

	char szArg[MAX_STRING] = { 0 };
	GetArg(szArg, szLine, 1);
	const char* szRest = GetNextArg(szLine);

	if (!_stricmp(szArg, "left"))
	{
		GetArg(szArg, szRest, 1);
		szRest = GetNextArg(szRest);
		f(szArg, szRest, MapLeftClickString, "Left Click");
	}
	else
	{
		f(szArg, szRest, MapSpecialClickString, "Right Click");
	}
}

// ---------------------------------------------------------------------------
// FormatMarker — parse marker type/shape/size and persist to INI
// ---------------------------------------------------------------------------

char* FormatMarker(const char* szLine, char* szDest, size_t BufferSize)
{
	memset(szDest, 0, BufferSize);

	char MarkType[MAX_STRING] = { 0 };
	char MarkShape[MAX_STRING] = { 0 };
	char MarkSize[MAX_STRING] = { 0 };

	GetArg(MarkType, szLine, 1);
	GetArg(MarkShape, szLine, 2);
	GetArg(MarkSize, szLine, 3);

	if (!strlen(MarkType))
	{
		sprintf_s(szDest, BufferSize, "unchanged, no spawn type given.");
		return szDest;
	}

	if (!strlen(MarkShape))
	{
		sprintf_s(szDest, BufferSize, "unchanged, no shape given.");
		return szDest;
	}

	for (size_t i = 0; i < MapFilterOptions.size(); i++)
	{
		if (!_stricmp(MarkType, MapFilterOptions[i].szName))
		{
			MarkerType Marker = FindMarker(MarkShape);
			if (Marker == MarkerType::Unknown)
			{
				sprintf_s(szDest, BufferSize, "unchanged, unknown shape: '%s'", MarkShape);
				return szDest;
			}

			int Size = 6;
			if (strlen(MarkSize))
			{
				Size = GetIntFromString(MarkSize, 0);
				if (!Size)
				{
					sprintf_s(szDest, BufferSize, "unchanged, invalid size: '%s'", MarkSize);
					return szDest;
				}
			}

			WritePrivateProfileString("Marker Filters", MapFilterOptions[i].szName, szMarkType[static_cast<size_t>(Marker)], INIFileName);

			char sizeBuf[128];
			snprintf(sizeBuf, sizeof(sizeBuf), "%s-Size", MapFilterOptions[i].szName);
			WritePrivateProfileInt("Marker Filters", sizeBuf, Size, INIFileName);

			MapFilterOptions[i].Marker = Marker;
			MapFilterOptions[i].MarkerSize = Size;

			sprintf_s(szDest, BufferSize, "'%s' is now set to '%s' with size %d.", MapFilterOptions[i].szName, szMarkType[static_cast<size_t>(Marker)], Size);
			return szDest;
		}
	}

	sprintf_s(szDest, BufferSize, "unchanged, unknown spawn type: %s", MarkType);
	return szDest;
}

// ---------------------------------------------------------------------------
// MapSetLocationCmd — /maploc command handler
// ---------------------------------------------------------------------------

void MapSetLocationCmd(PlayerClient* pChar, const char* szLine)
{
	char szArg[MAX_STRING] = { 0 };
	if (szLine && szLine[0])
		GetArg(szArg, szLine, 1);

	// /maploc help
	if (!_stricmp(szArg, "help"))
	{
		MapLocSyntaxOutput();
		return;
	}

	// /maploc remove [...]
	if (!_stricmp(szArg, "remove"))
	{
		MapRemoveLocation(szLine);
		return;
	}

	// Parse optional keywords: size, width, color, radius, rcolor, label, target, coords
	MapLocParams params = gDefaultMapLocParams;
	bool isCreatedFromDefaults = true;
	bool useTarget = false;
	bool hasCoords = false;
	float locY = 0.f, locX = 0.f, locZ = 0.f;
	std::string labelText;

	int argIdx = 1;
	const char* currentLine = szLine;

	while (currentLine && *currentLine)
	{
		GetArg(szArg, currentLine, 1);
		if (szArg[0] == 0) break;

		if (!_stricmp(szArg, "size"))
		{
			currentLine = GetNextArg(currentLine);
			GetArg(szArg, currentLine, 1);
			float val = GetFloatFromString(szArg, params.lineSize);
			if (val >= 1.f && val <= 200.f)
			{
				params.lineSize = val;
				isCreatedFromDefaults = false;
			}
			currentLine = GetNextArg(currentLine);
		}
		else if (!_stricmp(szArg, "width"))
		{
			currentLine = GetNextArg(currentLine);
			GetArg(szArg, currentLine, 1);
			float val = GetFloatFromString(szArg, params.width);
			if (val >= 1.f && val <= 10.f)
			{
				params.width = val;
				isCreatedFromDefaults = false;
			}
			currentLine = GetNextArg(currentLine);
		}
		else if (!_stricmp(szArg, "color"))
		{
			currentLine = GetNextArg(currentLine);
			char r[64], g[64], b[64];
			GetArg(r, currentLine, 1);
			GetArg(g, currentLine, 2);
			GetArg(b, currentLine, 3);
			params.color = MQColor(
				static_cast<uint8_t>(std::clamp(GetIntFromString(r, 255), 0, 255)),
				static_cast<uint8_t>(std::clamp(GetIntFromString(g, 0), 0, 255)),
				static_cast<uint8_t>(std::clamp(GetIntFromString(b, 0), 0, 255)));
			isCreatedFromDefaults = false;
			currentLine = GetNextArg(currentLine, 3);
		}
		else if (!_stricmp(szArg, "radius"))
		{
			currentLine = GetNextArg(currentLine);
			GetArg(szArg, currentLine, 1);
			params.circleRadius = GetFloatFromString(szArg, 0.f);
			isCreatedFromDefaults = false;
			currentLine = GetNextArg(currentLine);
		}
		else if (!_stricmp(szArg, "rcolor"))
		{
			currentLine = GetNextArg(currentLine);
			char r[64], g[64], b[64];
			GetArg(r, currentLine, 1);
			GetArg(g, currentLine, 2);
			GetArg(b, currentLine, 3);
			params.circleColor = MQColor(
				static_cast<uint8_t>(std::clamp(GetIntFromString(r, 0), 0, 255)),
				static_cast<uint8_t>(std::clamp(GetIntFromString(g, 0), 0, 255)),
				static_cast<uint8_t>(std::clamp(GetIntFromString(b, 255), 0, 255)));
			isCreatedFromDefaults = false;
			currentLine = GetNextArg(currentLine, 3);
		}
		else if (!_stricmp(szArg, "label"))
		{
			// Everything after "label" is the label text
			currentLine = GetNextArg(currentLine);
			if (currentLine && *currentLine)
				labelText = currentLine;
			break;  // label consumes the rest of the line
		}
		else if (!_stricmp(szArg, "target"))
		{
			useTarget = true;
			currentLine = GetNextArg(currentLine);
		}
		else if (IsFloat(szArg))
		{
			// Coordinates: yloc xloc [zloc]
			locY = GetFloatFromString(szArg, 0.f);
			currentLine = GetNextArg(currentLine);
			GetArg(szArg, currentLine, 1);
			if (IsFloat(szArg))
			{
				locX = GetFloatFromString(szArg, 0.f);
				currentLine = GetNextArg(currentLine);
				GetArg(szArg, currentLine, 1);
				if (szArg[0] && IsFloat(szArg))
				{
					locZ = GetFloatFromString(szArg, 0.f);
					currentLine = GetNextArg(currentLine);
				}
			}
			hasCoords = true;
		}
		else
		{
			// Unrecognized argument
			MapLocSyntaxOutput();
			return;
		}
	}

	// Determine position
	CVector3 pos;
	if (useTarget)
	{
		SPAWNINFO* targetSpawn = pTarget;
		if (!targetSpawn)
		{
			WriteChatColor("No target selected.");
			return;
		}
		pos.X = SpawnAccess::GetX(targetSpawn);
		pos.Y = SpawnAccess::GetY(targetSpawn);
		pos.Z = SpawnAccess::GetZ(targetSpawn);
	}
	else if (hasCoords)
	{
		pos.Y = locY;
		pos.X = locX;
		pos.Z = locZ;
	}
	else
	{
		// Default to player position
		SPAWNINFO* localSpawn = pLocalPlayer;
		if (!localSpawn)
		{
			WriteChatColor("Not in game.");
			return;
		}
		pos.X = SpawnAccess::GetX(localSpawn);
		pos.Y = SpawnAccess::GetY(localSpawn);
		pos.Z = SpawnAccess::GetZ(localSpawn);
	}

	// Build tag from truncated integer coords
	char tag[MAX_STRING];
	snprintf(tag, sizeof(tag), "%d,%d,%d",
		static_cast<int>(pos.Y), static_cast<int>(pos.X), static_cast<int>(pos.Z));

	// Create and add the map loc
	auto mapLoc = std::make_unique<MapLocTemplate>(
		params, labelText, tag, pos, isCreatedFromDefaults);

	int index = static_cast<int>(gMapLocTemplates.size());
	WriteChatf("MapLoc %d added at %s: size=%.0f, width=%.0f, color=%d,%d,%d, radius=%.0f%s",
		index, tag,
		params.lineSize, params.width,
		(int)params.color.Red, (int)params.color.Green, (int)params.color.Blue,
		params.circleRadius,
		labelText.empty() ? "" : (std::string(", label=") + labelText).c_str());

	AddMapLoc(std::move(mapLoc));
}

// ---------------------------------------------------------------------------
// LoadMapSettings — load all persistent settings from INI
// ---------------------------------------------------------------------------

void LoadMapSettings()
{
	char szBuffer[MAX_STRING] = { 0 };

	for (size_t i = 0; i < MapFilterOptions.size(); i++)
	{
		MapFilterOption& option = MapFilterOptions[i];

		option.Enabled = GetPrivateProfileBool("Map Filters", option.szName, option.Default, INIFileName);

		if (option.IsRadius())
			option.Radius = GetPrivateProfileFloat("Map Filters", option.szName, option.Default ? 1.0f : 0.0f, INIFileName);

		// If CampRadius or PullRadius, set the center to player position
		if (!_stricmp(option.szName, "CampRadius"))
		{
			if (pLocalPlayer && option.Radius > 0.0f)
			{
				CampX = SpawnAccess::GetX(pLocalPlayer);
				CampY = SpawnAccess::GetY(pLocalPlayer);
			}
		}
		if (!_stricmp(option.szName, "PullRadius"))
		{
			if (pLocalPlayer && option.Radius > 0.0f)
			{
				PullX = SpawnAccess::GetX(pLocalPlayer);
				PullY = SpawnAccess::GetY(pLocalPlayer);
			}
		}

		if (option.HasColor())
		{
			char colorKey[128];
			snprintf(colorKey, sizeof(colorKey), "%s-Color", option.szName);
			option.Color.SetARGB(GetPrivateProfileInt("Map Filters", colorKey, option.DefaultColor.ToARGB(), INIFileName));
			option.Color.Alpha = 255; // always enforce 255 alpha channel
		}

		char sizeKey[128];
		snprintf(sizeKey, sizeof(sizeKey), "%s-Size", option.szName);
		option.MarkerSize = GetPrivateProfileInt("Marker Filters", sizeKey, 0, INIFileName);
		std::string markerString = GetPrivateProfileString("Marker Filters", option.szName, "None", INIFileName);
		option.Marker = FindMarker(markerString, MarkerType::None);
	}

	activeLayer = GetPrivateProfileInt("Map Filters", "ActiveLayer", activeLayer, INIFileName);

	InitDefaultMapLocParams();
	ResetMapLocOverrides();

	repeatMapshow = GetPrivateProfileBool("Map Filters", "Mapshow-Repeat", false, INIFileName);
	repeatMaphide = GetPrivateProfileBool("Map Filters", "Maphide-Repeat", false, INIFileName);

	HighlightSIDELEN = GetPrivateProfileInt("Map Filters", "HighSize", HighlightSIDELEN, INIFileName);
	HighlightPulse = GetPrivateProfileBool("Map Filters", "HighPulse", HighlightPulse, INIFileName);
	HighlightColor.SetARGB(GetPrivateProfileInt("Map Filters", "High-Color", MQColor(112, 0, 112).ToARGB(), INIFileName));
	HighlightPulseIncreasing = true;
	HighlightPulseIndex = 0;
	HighlightPulseDiff = HighlightSIDELEN / 10;

	// Load mapshow/maphide filter strings
	std::string mapshowINI = GetPrivateProfileString("Map Filters", "Mapshow", "", INIFileName);
	strcpy_s(mapshowStr, mapshowINI.c_str());
	std::string maphideINI = GetPrivateProfileString("Map Filters", "Maphide", "", INIFileName);
	strcpy_s(maphideStr, maphideINI.c_str());

	// Load naming schemes
	std::string normalName = GetPrivateProfileString("Naming Schemes", "Normal", "%N", INIFileName);
	strcpy_s(MapNameString, normalName.c_str());
	std::string targetName = GetPrivateProfileString("Naming Schemes", "Target", "%N", INIFileName);
	strcpy_s(MapTargetNameString, targetName.c_str());

	// Load click commands
	for (int i = 1; i < MAX_CLICK_STRINGS; i++)
	{
		char keyBuf[32];
		snprintf(keyBuf, sizeof(keyBuf), "KeyCombo%d", i);
		std::string rightClick = GetPrivateProfileString("Right Click", keyBuf, MapSpecialClickString[i], INIFileName);
		strcpy_s(MapSpecialClickString[i], rightClick.c_str());
		std::string leftClick = GetPrivateProfileString("Left Click", keyBuf, MapLeftClickString[i], INIFileName);
		strcpy_s(MapLeftClickString[i], leftClick.c_str());
	}

	// Custom filter: do not use since the string isn't stored
	MapFilterOptions[static_cast<size_t>(MapFilter::Custom)].Enabled = false;

	// Named filter setup
	ClearSearchSpawn(&MapFilterNamed);
	ParseSearchSpawn("#", &MapFilterNamed);

	LogFramework("LoadMapSettings: complete (layer=%d, naming='%s'/'%s')", activeLayer, MapNameString, MapTargetNameString);
}
