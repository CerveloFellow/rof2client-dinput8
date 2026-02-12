/**
 * @file map_object.cpp
 * @brief MapObject implementations — ported from MapObject.cpp
 * @date 2026-02-09
 *
 * Key substitutions from MQ source:
 *   CXStr -> std::string
 *   m_spawn->Field -> SpawnAccess::GetField(m_spawn)
 *   mq::test_and_set -> test_and_set
 *   fmt::format -> snprintf
 *   DWORD -> uint32_t
 */

#include "pch.h"
#include "map_object.h"

// ---------------------------------------------------------------------------
// Global state definitions (from both MapObject.cpp and MQ2Map.cpp)
// ---------------------------------------------------------------------------

extern MapObject* pLastTarget;
MapObject* gpActiveMapObjects = nullptr;

std::vector<std::unique_ptr<MapLocTemplate>> gMapLocTemplates;
MapLocParams gDefaultMapLocParams;
MapLocParams gOverrideMapLocParams;

// ---------------------------------------------------------------------------
// MapFilterOptions table (38 entries, matching MapFilter enum)
// ---------------------------------------------------------------------------

// Shorthand for flags
using MFO = MapFilterOption;

MapFilterOption MapFilterInvalidOption = {
	"Invalid", false, MapFilter::Invalid, MQColor(), MapFilter::Invalid, 0, "Invalid filter"
};

std::vector<MapFilterOption> MapFilterOptions = {
	// All
	{ "All",           true,  MapFilter::All,          MQColor(),             MapFilter::Invalid,     MFO::Toggle | MFO::NoColor,
	  "Enables/disables map functions" },
	// PC
	{ "PC",            true,  MapFilter::PC,           MQColor(255, 0, 255), MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays PCs" },
	// PCConColor
	{ "PCConColor",    false, MapFilter::PCConColor,   MQColor(),             MapFilter::PC,          MFO::Toggle | MFO::NoColor | MFO::Regenerate,
	  "Displays PCs in consider colors" },
	// Group
	{ "Group",         false, MapFilter::Group,        MQColor(0, 128, 192), MapFilter::PC,          MFO::Toggle | MFO::Object,
	  "Displays group members in a specific color" },
	// Mount
	{ "Mount",         false, MapFilter::Mount,        MQColor(112, 112, 112), MapFilter::All,       MFO::Toggle | MFO::Object,
	  "Displays mounts" },
	// NPC
	{ "NPC",           true,  MapFilter::NPC,          MQColor(64, 64, 64),  MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays NPCs" },
	// NPCConColor
	{ "NPCConColor",   false, MapFilter::NPCConColor,  MQColor(),             MapFilter::NPC,         MFO::Toggle | MFO::NoColor | MFO::Regenerate,
	  "Displays NPCs in consider colors" },
	// Untargetable
	{ "Untargetable",  false, MapFilter::Untargetable, MQColor(128, 128, 128), MapFilter::All,       MFO::Toggle | MFO::Object,
	  "Displays untargetable spawns" },
	// Pet
	{ "Pet",           false, MapFilter::Pet,          MQColor(128, 0, 128), MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays pets" },
	// Corpse
	{ "Corpse",        false, MapFilter::Corpse,       MQColor(0, 0, 128),   MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays corpses" },
	// Chest
	{ "Chest",         false, MapFilter::Chest,        MQColor(192, 128, 0), MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays chests" },
	// Trigger
	{ "Trigger",       false, MapFilter::Trigger,      MQColor(192, 128, 0), MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays triggers" },
	// Trap
	{ "Trap",          false, MapFilter::Trap,         MQColor(192, 128, 0), MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays traps" },
	// Timer
	{ "Timer",         false, MapFilter::Timer,        MQColor(192, 128, 0), MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays timers" },
	// Ground
	{ "Ground",        false, MapFilter::Ground,       MQColor(192, 128, 0), MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays ground items" },
	// Target
	{ "Target",        true,  MapFilter::Target,       MQColor(192, 0, 0),   MapFilter::All,         MFO::Toggle | MFO::Regenerate,
	  "Displays your target" },
	// TargetLine
	{ "TargetLine",    true,  MapFilter::TargetLine,   MQColor(128, 0, 0),   MapFilter::Target,      MFO::Toggle,
	  "Displays a line to your target" },
	// TargetRadius
	{ "TargetRadius",  false, MapFilter::TargetRadius, MQColor(128, 128, 0), MapFilter::Target,      MFO::Toggle | MFO::UsesRadius,
	  "Sets radius of target circle" },
	// TargetMelee
	{ "TargetMelee",   false, MapFilter::TargetMelee,  MQColor(255, 128, 0), MapFilter::Target,      MFO::Toggle | MFO::UsesRadius,
	  "Displays melee range for target" },
	// Vector
	{ "Vector",        false, MapFilter::Vector,       MQColor(),             MapFilter::All,         MFO::Toggle | MFO::NoColor | MFO::Regenerate,
	  "Displays heading vectors" },
	// Custom
	{ "Custom",        false, MapFilter::Custom,       MQColor(),             MapFilter::All,         MFO::Toggle | MFO::NoColor | MFO::Regenerate,
	  "Sets custom filter" },
	// CastRadius
	{ "CastRadius",    false, MapFilter::CastRadius,   MQColor(128, 128, 0), MapFilter::All,         MFO::Toggle | MFO::UsesRadius,
	  "Sets radius of cast circle" },
	// NormalLabels
	{ "NormalLabels",  true,  MapFilter::NormalLabels, MQColor(),             MapFilter::All,         MFO::Toggle | MFO::NoColor,
	  "Displays normal EQ labels" },
	// ContextMenu
	{ "ContextMenu",   true,  MapFilter::ContextMenu,  MQColor(),             MapFilter::All,         MFO::Toggle | MFO::NoColor,
	  "Displays context menu" },
	// SpellRadius
	{ "SpellRadius",   false, MapFilter::SpellRadius,  MQColor(128, 128, 0), MapFilter::All,         MFO::Toggle | MFO::UsesRadius,
	  "Sets radius of spell circle" },
	// Aura
	{ "Aura",          false, MapFilter::Aura,         MQColor(64, 64, 64),  MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays auras" },
	// Object
	{ "Object",        false, MapFilter::Object,       MQColor(64, 64, 64),  MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays objects" },
	// Banner
	{ "Banner",        false, MapFilter::Banner,       MQColor(64, 64, 64),  MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays banners" },
	// Campfire
	{ "Campfire",      false, MapFilter::Campfire,     MQColor(64, 64, 64),  MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays campfires" },
	// PCCorpse
	{ "PCCorpse",      false, MapFilter::PCCorpse,     MQColor(0, 0, 128),   MapFilter::Corpse,      MFO::Toggle | MFO::Object,
	  "Displays PC corpses" },
	// NPCCorpse
	{ "NPCCorpse",     false, MapFilter::NPCCorpse,    MQColor(0, 0, 128),   MapFilter::Corpse,      MFO::Toggle | MFO::Object,
	  "Displays NPC corpses" },
	// Mercenary
	{ "Mercenary",     false, MapFilter::Mercenary,    MQColor(128, 0, 128), MapFilter::All,         MFO::Toggle | MFO::Object,
	  "Displays mercenaries" },
	// Named
	{ "Named",         false, MapFilter::Named,        MQColor(64, 64, 64),  MapFilter::NPC,         MFO::Toggle | MFO::Regenerate,
	  "Displays named NPCs" },
	// TargetPath
	{ "TargetPath",    false, MapFilter::TargetPath,   MQColor(128, 0, 0),   MapFilter::Target,      MFO::Toggle,
	  "Displays a path to your target" },
	// Marker
	{ "Marker",        false, MapFilter::Marker,       MQColor(),             MapFilter::All,         MFO::Toggle | MFO::NoColor | MFO::Regenerate,
	  "Displays markers on spawns" },
	// CampRadius
	{ "CampRadius",    false, MapFilter::CampRadius,   MQColor(128, 128, 0), MapFilter::All,         MFO::Toggle | MFO::UsesRadius,
	  "Sets radius of camp circle" },
	// PullRadius
	{ "PullRadius",    false, MapFilter::PullRadius,   MQColor(128, 128, 0), MapFilter::All,         MFO::Toggle | MFO::UsesRadius,
	  "Sets radius of pull circle" },
};

// ---------------------------------------------------------------------------
// More globals
// ---------------------------------------------------------------------------

uint32_t bmMapRefresh = 0;
int activeLayer = 3;
float CampX = 0.0f;
float CampY = 0.0f;
float PullX = 0.0f;
float PullY = 0.0f;

MQColor HighlightColor;
int HighlightSIDELEN = 10;
bool HighlightPulse = false;
bool HighlightPulseIncreasing = true;
int HighlightPulseIndex = 0;
int HighlightPulseDiff = 1;

char MapNameString[MAX_STRING] = "%N";
char MapTargetNameString[MAX_STRING] = "%N";
char mapshowStr[MAX_STRING] = { 0 };
char maphideStr[MAX_STRING] = { 0 };
MQSpawnSearch MapFilterCustom;
MQSpawnSearch MapFilterNamed;

char MapSpecialClickString[MAX_CLICK_STRINGS][MAX_STRING] = { 0 };
char MapLeftClickString[MAX_CLICK_STRINGS][MAX_STRING] = { 0 };
bool repeatMapshow = false;
bool repeatMaphide = false;

std::vector<MapFilterOption*> mapFilterObjectOptions;
std::vector<MapFilterOption*> mapFilterGeneralOptions;

MapObject* pLastTarget = nullptr;

// ---------------------------------------------------------------------------
// Label list management
// ---------------------------------------------------------------------------

MAPLABEL* gpLabelList = nullptr;
MAPLABEL* gpLabelListTail = nullptr;

std::map<MAPLABEL*, MapObject*> LabelMap;

static MAPLABEL* InitLabel()
{
	MAPLABEL* pLabel = new MAPLABEL;
	pLabel->pPrev = nullptr;
	pLabel->pNext = gpLabelList;

	if (gpLabelList)
		gpLabelList->pPrev = pLabel;
	else
		gpLabelListTail = pLabel;

	gpLabelList = pLabel;
	return pLabel;
}

static void DeleteLabel(MAPLABEL* pLabel)
{
	if (pLabel->pNext)
		pLabel->pNext->pPrev = pLabel->pPrev;
	else
		gpLabelListTail = pLabel->pPrev;

	if (pLabel->pPrev)
		pLabel->pPrev->pNext = pLabel->pNext;
	else
		gpLabelList = pLabel->pNext;

	delete pLabel;
}

MapObject* GetMapObjectForLabel(MAPLABEL* pLabel)
{
	auto iter = LabelMap.find(pLabel);
	return iter == LabelMap.end() ? nullptr : iter->second;
}

// ---------------------------------------------------------------------------
// Line list management
// ---------------------------------------------------------------------------

MapViewLine* gpLineList = nullptr;
MapViewLine* gpLineListTail = nullptr;

MapViewLine* InitLine()
{
	MapViewLine* pLine = new MapViewLine;
	pLine->pPrev = nullptr;
	pLine->pNext = gpLineList;

	if (gpLineList)
		gpLineList->pPrev = pLine;
	else
		gpLineListTail = pLine;

	gpLineList = pLine;
	return pLine;
}

void DeleteLine(MapViewLine* pLine)
{
	if (!pLine)
		return;

	if (pLine->pNext)
		pLine->pNext->pPrev = pLine->pPrev;
	else
		gpLineListTail = pLine->pPrev;

	if (pLine->pPrev)
		pLine->pPrev->pNext = pLine->pNext;
	else
		gpLineList = pLine->pNext;

	delete pLine;
}

//============================================================================
// MapObject base class
//============================================================================

MapObject::MapObject()
{
	m_pNext = gpActiveMapObjects;
	if (gpActiveMapObjects)
		gpActiveMapObjects->m_pLast = this;
	gpActiveMapObjects = this;
}

void MapObject::PostInit()
{
	GenerateMarker();
	Update(true);
}

MapObject::~MapObject()
{
	if (m_label)
	{
		DeleteLabel(m_label);
		LabelMap.erase(m_label);
		m_label = nullptr;
	}

	if (m_vector)
	{
		DeleteLine(m_vector);
		m_vector = nullptr;
	}

	RemoveMarker();

	if (m_pNext)
		m_pNext->m_pLast = m_pLast;

	if (m_pLast)
		m_pLast->m_pNext = m_pNext;
	else
		gpActiveMapObjects = m_pNext;
}

void MapObject::Update(bool forced)
{
	if (m_label)
	{
		m_label->Location.X = -m_pos.X;
		m_label->Location.Y = -m_pos.Y;
		m_label->Location.Z = m_pos.Z;
	}

	if (m_highlight)
	{
		SetColor(HighlightColor);
	}

	if (IsOptionEnabled(MapFilter::Marker))
		UpdateMarker();
	else
		RemoveMarker();
}

bool MapObject::CanDisplayObject() const
{
	return false;
}

std::string MapObject::FormatString(const char* formatString)
{
	std::string sOutput;

	for (int n = 0; formatString[n]; n++)
	{
		if (formatString[n] != '%')
		{
			sOutput.append(1, formatString[n]);
			continue;
		}

		char spec = formatString[++n];
		HandleFormatSpecifier(spec, sOutput);
	}

	return sOutput;
}

void MapObject::HandleFormatSpecifier(char spec, std::string& sOutput)
{
	switch (spec)
	{
	case 'N':
	case 'n':
		sOutput.append(m_text);
		return;

	case 'h':
		sOutput.append(1, '1');
		return;

	case 'i':
	case 'l':
		sOutput.append(1, '0');
		return;

	case 'x':
		sOutput.append(std::to_string(m_pos.X));
		return;
	case 'y':
		sOutput.append(std::to_string(m_pos.Y));
		return;
	case 'z':
		sOutput.append(std::to_string(m_pos.Z));
		return;

	case '%':
		sOutput.append(1, '%');
		return;

	default:
		sOutput.append(1, '%');
		sOutput.append(1, spec);
		return;
	}
}

MapFilter MapObject::GetMapFilter() const
{
	return MapFilter::Invalid;
}

void MapObject::GenerateLabel()
{
	m_label = InitLabel();
	m_label->Location.X = -m_pos.X;
	m_label->Location.Y = -m_pos.Y;
	m_label->Location.Z = m_pos.Z;
	m_label->Layer = activeLayer;
	m_label->Size = 3;
	m_label->Color.ARGB = m_color.ToARGB();
	m_label->Width = 20;
	m_label->Height = 14;
	m_label->OffsetX = 0;
	m_label->OffsetY = 0;
	m_label->Label = "";

	LabelMap[m_label] = this;
}

void MapObject::SetText(std::string_view text)
{
	if (test_and_set(m_text, std::string(text)))
	{
		if (m_label)
		{
			if (m_text.empty())
				m_label->Label = "";
			else
				m_label->Label = &m_text[0];
		}
	}
}

void MapObject::SetColor(MQColor color)
{
	if (test_and_set(m_color, color))
	{
		if (m_label)
		{
			m_label->Color.ARGB = color.ToARGB();
		}
	}
}

void MapObject::SetPosition(const CVector3& pos)
{
	if (test_and_set(m_pos, pos))
	{
		Update(true);
	}
}

#pragma region Markers

constexpr int MarkerSides_Triangle = 3;
constexpr int MarkerSides_Square = 4;
constexpr int MarkerSides_Diamond = 4;
constexpr int MarkerSides_Ring = 8;

static constexpr int GetNumMarkerSides(MarkerType type)
{
	switch (type)
	{
	case MarkerType::Triangle: return MarkerSides_Triangle;
	case MarkerType::Square: return MarkerSides_Square;
	case MarkerType::Diamond: return MarkerSides_Diamond;
	case MarkerType::Ring: return MarkerSides_Ring;
	default: return 0;
	}
}

void MapObject::GenerateMarker()
{
	if (!IsOptionEnabled(MapFilter::Marker))
		return;

	const MapFilterOption& option = GetMapFilterOption(GetMapFilter());
	m_marker = option.Marker;
	m_markerSize = option.MarkerSize;

	if (m_marker == MarkerType::None)
		return;

	int sides = GetNumMarkerSides(m_marker);

	m_markerLines.clear();
	m_markerLines.reserve(sides);

	for (int i = 0; i < sides; i++)
	{
		MapViewLine* pNewLine = InitLine();
		pNewLine->Start.X = 0;
		pNewLine->Start.Y = 0;
		pNewLine->Start.Z = m_pos.Z;
		pNewLine->End.X = 0;
		pNewLine->End.Y = 0;
		pNewLine->End.Z = m_pos.Z;
		pNewLine->Layer = activeLayer;
		pNewLine->Color = m_label->Color;

		m_markerLines.push_back(pNewLine);
	}

	UpdateMarker();
}

void MapObject::RemoveMarker()
{
	if (m_marker == MarkerType::None)
		return;

	for (MapViewLine* line : m_markerLines)
		DeleteLine(line);

	m_markerLines.clear();
	m_marker = MarkerType::None;
}

void MapObject::UpdateMarker()
{
	switch (m_marker)
	{
	case MarkerType::Triangle:
		MakeTriangleMarker();
		return;
	case MarkerType::Square:
		MakeSquareMarker();
		return;
	case MarkerType::Diamond:
		MakeDiamondMarker();
		return;
	case MarkerType::Ring:
		MakeRingMarker();
		return;

	default: break;
	}
}

void MapObject::MakeSquareMarker()
{
	uint32_t markerSideLen = m_markerSize;
	float x[2], y[2], X[2], Y[2];

	if (m_highlight)
	{
		if (HighlightPulse)
		{
			markerSideLen = HighlightSIDELEN + (HighlightPulseIndex * HighlightPulseDiff);
		}
		else
		{
			markerSideLen = HighlightSIDELEN;
		}
	}

	x[0] = -m_pos.X - markerSideLen / 2;
	x[1] = -m_pos.X + markerSideLen / 2;
	y[0] = -m_pos.Y - markerSideLen / 2;
	y[1] = -m_pos.Y + markerSideLen / 2;

	for (int i = 0; i < GetNumMarkerSides(MarkerType::Square); i++)
	{
		switch (i)
		{
		case 0: X[0] = x[0]; X[1] = x[1]; Y[0] = y[0]; Y[1] = y[0]; break;
		case 1: X[0] = x[1]; X[1] = x[1]; Y[0] = y[0]; Y[1] = y[1]; break;
		case 2: X[0] = x[1]; X[1] = x[0]; Y[0] = y[1]; Y[1] = y[1]; break;
		case 3: X[0] = x[0]; X[1] = x[0]; Y[0] = y[1]; Y[1] = y[0]; break;
		}

		m_markerLines[i]->Start.X = X[0];
		m_markerLines[i]->Start.Y = Y[0];
		m_markerLines[i]->End.X = X[1];
		m_markerLines[i]->End.Y = Y[1];
		m_markerLines[i]->Start.Z = m_pos.Z;
		m_markerLines[i]->End.Z = m_pos.Z;

		if (m_markerLines[i]->Color.ARGB != m_label->Color.ARGB)
			m_markerLines[i]->Color = m_label->Color;
	}
}

void MapObject::MakeTriangleMarker()
{
	uint32_t markerSideLen = m_markerSize;
	float x[3], y[3], X[2], Y[2], Angle;

	if (m_highlight)
	{
		if (HighlightPulse)
		{
			markerSideLen = HighlightSIDELEN + (HighlightPulseIndex * HighlightPulseDiff);
		}
		else
		{
			markerSideLen = HighlightSIDELEN;
		}
	}

	Angle = m_heading * 0.703125f;
	x[0] = -m_pos.X + (markerSideLen * 1.5f) * sqrtf(3) / 3 * sinf((Angle + 180) / 180.0f * PI);
	x[1] = -m_pos.X - (markerSideLen * 1.5f) * sqrtf(3) / 3 * sinf((Angle + 210) / 180.0f * PI);
	x[2] = -m_pos.X + (markerSideLen * 1.5f) * sqrtf(3) / 3 * sinf((Angle + 330) / 180.0f * PI);
	y[0] = -m_pos.Y + (markerSideLen * 1.5f) * sqrtf(3) / 3 * cosf((Angle + 180) / 180.0f * PI);
	y[1] = -m_pos.Y - (markerSideLen * 1.5f) * sqrtf(3) / 3 * cosf((Angle + 210) / 180.0f * PI);
	y[2] = -m_pos.Y + (markerSideLen * 1.5f) * sqrtf(3) / 3 * cosf((Angle + 330) / 180.0f * PI);

	for (int i = 0; i < GetNumMarkerSides(MarkerType::Triangle); i++)
	{
		switch (i)
		{
		case 0: X[0] = x[0]; X[1] = x[1]; Y[0] = y[0]; Y[1] = y[1]; break;
		case 1: X[0] = x[1]; X[1] = x[2]; Y[0] = y[1]; Y[1] = y[2]; break;
		case 2: X[0] = x[2]; X[1] = x[0]; Y[0] = y[2]; Y[1] = y[0]; break;
		}

		m_markerLines[i]->Start.X = X[0];
		m_markerLines[i]->Start.Y = Y[0];
		m_markerLines[i]->End.X = X[1];
		m_markerLines[i]->End.Y = Y[1];
		m_markerLines[i]->Start.Z = m_pos.Z;
		m_markerLines[i]->End.Z = m_pos.Z;

		if (m_markerLines[i]->Color.ARGB != m_label->Color.ARGB)
			m_markerLines[i]->Color = m_label->Color;
	}
}

void MapObject::MakeDiamondMarker()
{
	uint32_t markerSideLen = m_markerSize;
	float x[3], y[3], X[2], Y[2];

	if (m_highlight)
	{
		if (HighlightPulse)
		{
			markerSideLen = HighlightSIDELEN + (HighlightPulseIndex * HighlightPulseDiff);
		}
		else
		{
			markerSideLen = HighlightSIDELEN;
		}
	}

	x[0] = -m_pos.X;
	x[1] = -m_pos.X + markerSideLen * .71f;
	x[2] = -m_pos.X - markerSideLen * .71f;
	y[0] = -m_pos.Y - markerSideLen * .71f;
	y[1] = -m_pos.Y;
	y[2] = -m_pos.Y + markerSideLen * .71f;

	for (int i = 0; i < GetNumMarkerSides(MarkerType::Diamond); i++)
	{
		switch (i)
		{
		case 0: X[0] = x[0]; X[1] = x[1]; Y[0] = y[0]; Y[1] = y[1]; break;
		case 1: X[0] = x[1]; X[1] = x[0]; Y[0] = y[1]; Y[1] = y[2]; break;
		case 2: X[0] = x[0]; X[1] = x[2]; Y[0] = y[2]; Y[1] = y[1]; break;
		case 3: X[0] = x[2]; X[1] = x[0]; Y[0] = y[1]; Y[1] = y[0]; break;
		}

		m_markerLines[i]->Start.X = X[0];
		m_markerLines[i]->Start.Y = Y[0];
		m_markerLines[i]->End.X = X[1];
		m_markerLines[i]->End.Y = Y[1];
		m_markerLines[i]->Start.Z = m_pos.Z;
		m_markerLines[i]->End.Z = m_pos.Z;

		if (m_markerLines[i]->Color.ARGB != m_label->Color.ARGB)
			m_markerLines[i]->Color = m_label->Color;
	}
}

void MapObject::MakeRingMarker()
{
	uint32_t markerSideLen = m_markerSize;

	if (m_highlight)
	{
		if (HighlightPulse)
		{
			markerSideLen = HighlightSIDELEN + (HighlightPulseIndex * HighlightPulseDiff);
		}
		else
		{
			markerSideLen = HighlightSIDELEN;
		}
	}

	for (int i = 0; i < GetNumMarkerSides(MarkerType::Ring); i++)
	{
		m_markerLines[i]->Start.X = -m_pos.X + markerSideLen * sinf((i * 45 + 22.5f) / 180.0f * PI);
		m_markerLines[i]->Start.Y = -m_pos.Y + markerSideLen * cosf((i * 45 + 22.5f) / 180.0f * PI);
		m_markerLines[i]->End.X = -m_pos.X + markerSideLen * sinf(((i + 1) * 45 + 22.5f) / 180.0f * PI);
		m_markerLines[i]->End.Y = -m_pos.Y + markerSideLen * cosf(((i + 1) * 45 + 22.5f) / 180.0f * PI);
		m_markerLines[i]->Start.Z = m_pos.Z;
		m_markerLines[i]->End.Z = m_pos.Z;

		if (m_markerLines[i]->Color.ARGB != m_label->Color.ARGB)
			m_markerLines[i]->Color = m_label->Color;
	}
}

#pragma endregion

//============================================================================
// MapObjectSpawn
//============================================================================

static std::map<SPAWNINFO*, MapObject*> SpawnMap;

MapObjectSpawn::MapObjectSpawn(SPAWNINFO* pSpawn, bool Explicit)
	: m_spawn(pSpawn)
	, m_type(GetSpawnType(pSpawn))
	, m_explicit(Explicit)
{
	GenerateLabel();

	SetText(FormatString(MapNameString));
	SetColor(GetSpawnColor());

	SpawnMap[m_spawn] = this;
}

MapObjectSpawn::~MapObjectSpawn()
{
	SpawnMap.erase(m_spawn);

	if (pLastTarget == this)
		pLastTarget = nullptr;

	RemoveVector();
}

void MapObjectSpawn::PostInit()
{
	MapObject::PostInit();

	if (IsOptionEnabled(MapFilter::Vector))
	{
		GenerateVector();
	}
}

void MapObjectSpawn::Update(bool forced)
{
	bool changed = false;

	changed |= test_and_set(m_type, GetSpawnType(m_spawn));

	m_pos.X = SpawnAccess::GetX(m_spawn);
	m_pos.Y = SpawnAccess::GetY(m_spawn);
	m_pos.Z = SpawnAccess::GetZ(m_spawn);
	m_heading = SpawnAccess::GetHeading(m_spawn);

	if (changed || forced)
	{
		SetText(FormatString(MapNameString));
		SetColor(GetSpawnColor());
	}
	else if (m_text.empty())
	{
		// Name may not have been populated when spawn was first created
		// (e.g. during zone loading). Retry until we get a non-empty name.
		SetText(FormatString(MapNameString));
		SetColor(GetSpawnColor());
	}
	else if (!m_highlight)
	{
		SetColor(GetSpawnColor());
	}

	MapObject::Update(forced);

	if (pLastTarget == this)
	{
		SetColor(GetMapFilterOption(MapFilter::Target).Color);
		SetText(FormatString(MapTargetNameString));
	}
}

MQColor MapObjectSpawn::GetSpawnColor() const
{
	if (!m_spawn)
	{
		return MQColor();
	}

	switch (m_type)
	{
	case PC:
		if (IsOptionEnabled(MapFilter::PCConColor))
			return MQColor{ MQColor::format_argb, ConColorToARGB(ConColor(m_spawn)) };
		return GetMapFilterOption(MapFilter::PC).Color;
	case NPC:
		if (IsOptionEnabled(MapFilter::NPCConColor))
			return MQColor{ MQColor::format_argb, ConColorToARGB(ConColor(m_spawn)) };
		return GetMapFilterOption(MapFilter::NPC).Color;
	case CORPSE:
		if (SpawnAccess::GetDeity(m_spawn) == 0)
			return GetMapFilterOption(MapFilter::NPCCorpse).Color;
		else
			return GetMapFilterOption(MapFilter::PCCorpse).Color;
	case UNTARGETABLE:
		return GetMapFilterOption(MapFilter::Untargetable).Color;
	case CHEST:
		return GetMapFilterOption(MapFilter::Chest).Color;
	case TRIGGER:
		return GetMapFilterOption(MapFilter::Trigger).Color;
	case TIMER:
		return GetMapFilterOption(MapFilter::Timer).Color;
	case TRAP:
		return GetMapFilterOption(MapFilter::Trap).Color;
	case ITEM:
		return GetMapFilterOption(MapFilter::Ground).Color;
	case MOUNT:
		return GetMapFilterOption(MapFilter::Mount).Color;
	case PET:
		return GetMapFilterOption(MapFilter::Pet).Color;
	case AURA:
		return GetMapFilterOption(MapFilter::Aura).Color;
	case OBJECT:
		return GetMapFilterOption(MapFilter::Object).Color;
	case BANNER:
		return GetMapFilterOption(MapFilter::Banner).Color;
	case CAMPFIRE:
		return GetMapFilterOption(MapFilter::Campfire).Color;
	case MERCENARY:
		return GetMapFilterOption(MapFilter::Mercenary).Color;
	}

	return MQColor();
}

void MapObjectSpawn::HandleFormatSpecifier(char spec, std::string& sOutput)
{
	switch (spec)
	{
	case 'N':
		sOutput.append(SpawnAccess::GetDisplayedName(m_spawn));
		if (m_type == CORPSE)
			sOutput.append("'s Corpse");
		return;

	case 'n':
		sOutput.append(SpawnAccess::GetName(m_spawn));
		return;

	case 'h':
		sOutput.append(std::to_string(SpawnAccess::GetHPCurrent(m_spawn)));
		return;

	case 'i':
		sOutput.append(std::to_string(SpawnAccess::GetSpawnID(m_spawn)));
		return;

	case 'x':
		sOutput.append(std::to_string(SpawnAccess::GetX(m_spawn)));
		return;

	case 'y':
		sOutput.append(std::to_string(SpawnAccess::GetY(m_spawn)));
		return;

	case 'z':
		sOutput.append(std::to_string(SpawnAccess::GetZ(m_spawn)));
		return;

	case 'R':
		sOutput.append(SpawnAccess::GetRaceString(m_spawn));
		return;

	case 'C':
		sOutput.append(SpawnAccess::GetClassString(m_spawn));
		return;

	case 'c':
		sOutput.append(SpawnAccess::GetClassThreeLetterCode(m_spawn));
		return;

	case 'l':
		sOutput.append(std::to_string(SpawnAccess::GetLevel(m_spawn)));
		return;

	default:
		MapObject::HandleFormatSpecifier(spec, sOutput);
		return;
	}
}

MapFilter MapObjectSpawn::GetMapFilter() const
{
	switch (m_type)
	{
	case PC:
		return MapFilter::PC;
	case NPC:
		if (IsOptionEnabled(MapFilter::Named))
		{
			if (SpawnMatchesSearch(&MapFilterNamed, pLocalPlayer, m_spawn))
			{
				return MapFilter::Named;
			}
		}
		return MapFilter::NPC;

	case CORPSE:
		return MapFilter::Corpse;
	case ITEM:
		return MapFilter::Ground;
	case UNTARGETABLE:
		return MapFilter::Untargetable;
	case TIMER:
		return MapFilter::Timer;
	case TRAP:
		return MapFilter::Trap;
	case TRIGGER:
		return MapFilter::Trigger;
	case CHEST:
		return MapFilter::Chest;
	case PET:
		return MapFilter::Pet;
	case MOUNT:
		return MapFilter::Mount;
	case AURA:
		return MapFilter::Aura;
	case OBJECT:
		return MapFilter::Object;
	case BANNER:
		return MapFilter::Banner;
	}

	return MapObject::GetMapFilter();
}

static bool CanDisplaySpawnObject(eSpawnType type, SPAWNINFO* spawn)
{
	if (spawn == pTarget && IsOptionEnabled(MapFilter::Target))
	{
		return true;
	}

	if (IsOptionEnabled(MapFilter::Custom))
	{
		return SpawnMatchesSearch(&MapFilterCustom, pLocalPlayer, spawn);
	}

	switch (type)
	{
	case PC:
		return IsOptionEnabled(MapFilter::PC);
	case NPC:
		if (IsOptionEnabled(MapFilter::Named))
			return IsNamed(spawn);
		else
			return IsOptionEnabled(MapFilter::NPC);
	case CORPSE:
		if (SpawnAccess::GetDeity(spawn) == 0)
			return IsOptionEnabled(MapFilter::NPCCorpse);
		else
			return IsOptionEnabled(MapFilter::PCCorpse);
	case ITEM:
		return IsOptionEnabled(MapFilter::Ground);
	case UNTARGETABLE:
		return IsOptionEnabled(MapFilter::Untargetable);
	case TIMER:
		return IsOptionEnabled(MapFilter::Timer);
	case TRAP:
		return IsOptionEnabled(MapFilter::Trap);
	case TRIGGER:
		return IsOptionEnabled(MapFilter::Trigger);
	case CHEST:
		return IsOptionEnabled(MapFilter::Chest);
	case PET:
		return IsOptionEnabled(MapFilter::Pet);
	case MOUNT:
		return IsOptionEnabled(MapFilter::Mount);
	case AURA:
		return IsOptionEnabled(MapFilter::Aura);
	case OBJECT:
		return IsOptionEnabled(MapFilter::Object);
	case BANNER:
		return IsOptionEnabled(MapFilter::Banner);
	case CAMPFIRE:
		return IsOptionEnabled(MapFilter::Campfire);
	case MERCENARY:
		return IsOptionEnabled(MapFilter::Mercenary);
	case FLYER:
		return false;
	}

	return true;
}

bool MapObjectSpawn::CanDisplayObject() const
{
	if (m_explicit || MapObject::CanDisplayObject())
	{
		return true;
	}

	return CanDisplaySpawnObject(m_type, m_spawn);
}

#pragma region Vectors

void MapObjectSpawn::GenerateVector()
{
	MapViewLine* newLine = InitLine();

	UpdateVector();

	newLine->Layer = activeLayer;
	newLine->Color = m_label->Color;

	m_vector = newLine;
}

void MapObjectSpawn::UpdateVector()
{
	if (!m_vector) return;

	if (SpawnAccess::GetSpeedRun(m_spawn) > 0)
	{
		m_vector->Start.X = -SpawnAccess::GetX(m_spawn);
		m_vector->Start.Y = -SpawnAccess::GetY(m_spawn);
		m_vector->Start.Z = SpawnAccess::GetZ(m_spawn);

		m_vector->End.X = -SpawnAccess::GetX(m_spawn) - SpawnAccess::GetSpeedX(m_spawn) * 4;
		m_vector->End.Y = -SpawnAccess::GetY(m_spawn) - SpawnAccess::GetSpeedY(m_spawn) * 4;
		m_vector->End.Z = SpawnAccess::GetZ(m_spawn);
	}
	else
	{
		m_vector->Start.X = -SpawnAccess::GetX(m_spawn);
		m_vector->Start.Y = -SpawnAccess::GetY(m_spawn);
		m_vector->Start.Z = SpawnAccess::GetZ(m_spawn);

		m_vector->End.X = -SpawnAccess::GetX(m_spawn) - sinf(SpawnAccess::GetHeading(m_spawn) / 256.0f * PI) * 4;
		m_vector->End.Y = -SpawnAccess::GetY(m_spawn) - cosf(SpawnAccess::GetHeading(m_spawn) / 256.0f * PI) * 4;
		m_vector->End.Z = SpawnAccess::GetZ(m_spawn);
	}
}

void MapObjectSpawn::RemoveVector()
{
	if (m_vector)
	{
		DeleteLine(m_vector);
		m_vector = nullptr;
	}
}

#pragma endregion

//============================================================================
// MapObjectGroundSpawn
//============================================================================

static std::map<EQGroundItem*, MapObject*> GroundItemMap;

MapObjectGroundSpawn::MapObjectGroundSpawn(EQGroundItem* pGroundItem)
	: m_groundItem(pGroundItem)
	, m_friendlyName(GetFriendlyNameForGroundItem(m_groundItem))
{
	GenerateLabel();

	SetText(FormatString(MapNameString));
	SetColor(GetMapFilterOption(MapFilter::Ground).Color);

	GroundItemMap[m_groundItem] = this;
}

MapObjectGroundSpawn::~MapObjectGroundSpawn()
{
	GroundItemMap.erase(m_groundItem);
}

void MapObjectGroundSpawn::PostInit()
{
	MapObject::PostInit();
}

void MapObjectGroundSpawn::Update(bool forced)
{
	m_pos.X = m_groundItem->X;
	m_pos.Y = m_groundItem->Y;
	m_pos.Z = m_groundItem->Z;
	m_heading = m_groundItem->Heading;

	MapObject::Update(forced);
}

MapFilter MapObjectGroundSpawn::GetMapFilter() const
{
	return MapFilter::Ground;
}

bool MapObjectGroundSpawn::CanDisplayObject() const
{
	if (MapObject::CanDisplayObject())
	{
		return true;
	}

	return IsOptionEnabled(MapFilter::Ground);
}

void MapObjectGroundSpawn::HandleFormatSpecifier(char spec, std::string& sOutput)
{
	switch (spec)
	{
	case 'N':
	case 'n':
		sOutput = m_friendlyName;
		return;

	default:
		MapObject::HandleFormatSpecifier(spec, sOutput);
		return;
	}
}

//============================================================================
// Factory functions
//============================================================================

MapObject* MakeMapObject(SPAWNINFO* pSpawn, bool Explicit)
{
	if (!Explicit && !CanDisplaySpawnObject(GetSpawnType(pSpawn), pSpawn))
		return nullptr;

	MapObject* obj = new MapObjectSpawn(pSpawn, Explicit);
	obj->PostInit();

	return obj;
}

MapObject* FindMapObject(SPAWNINFO* pSpawn)
{
	if (pSpawn)
	{
		auto iter = SpawnMap.find(pSpawn);
		if (iter != SpawnMap.end())
			return iter->second;
	}

	return nullptr;
}

MapObject* MakeMapObject(EQGroundItem* pGroundItem)
{
	if (!IsOptionEnabled(MapFilter::Ground))
		return nullptr;

	MapObject* obj = new MapObjectGroundSpawn(pGroundItem);
	obj->PostInit();

	return obj;
}

MapObject* FindMapObject(EQGroundItem* pGroundItem)
{
	if (pGroundItem)
	{
		auto iter = GroundItemMap.find(pGroundItem);
		if (iter != GroundItemMap.end())
			return iter->second;
	}

	return nullptr;
}

void MapObjects_Clear()
{
	GroundItemMap.clear();
	SpawnMap.clear();

	while (gpActiveMapObjects)
	{
		delete gpActiveMapObjects;
	}
}

//============================================================================
// MapCircle
//============================================================================

MapCircle::MapCircle()
{
	memset(m_components, 0, sizeof(m_components));
}

MapCircle::~MapCircle()
{
	// explicitly not releasing memory as it may not even be valid to do so at this time.
}

void MapCircle::Clear()
{
	if (!m_initialized) return;

	for (int i = 0; i < CIRCLE_NUM_SEGMENTS; i++)
	{
		if (m_components[i])
		{
			DeleteLine(m_components[i]);
			m_components[i] = nullptr;
		}
	}
}

void MapCircle::UpdateCircle(MQColor Color, float Radius, float X, float Y, float Z)
{
	uint32_t Angle = 0;

	for (int i = 0; i < CIRCLE_NUM_SEGMENTS; i++, Angle += CIRCLE_ANGLESIZE)
	{
		if (!m_components[i])
		{
			m_components[i] = InitLine();
			m_components[i]->Layer = activeLayer;
		}

		m_components[i]->Color.ARGB = Color.ToARGB();
		m_components[i]->Start.Z = Z;
		m_components[i]->End.Z = Z;
		m_components[i]->Start.X = -X + Radius * cosf((float)Angle / 180.0f * PI);
		m_components[i]->Start.Y = -Y + Radius * sinf((float)Angle / 180.0f * PI);
		m_components[i]->End.X = -X + Radius * cosf((float)(Angle + CIRCLE_ANGLESIZE) / 180.0f * PI);
		m_components[i]->End.Y = -Y + Radius * sinf((float)(Angle + CIRCLE_ANGLESIZE) / 180.0f * PI);
	}

	m_initialized = true;
}

//============================================================================
// MapLocParams
//============================================================================

std::string MapLocParams::MakeCommandString()
{
	char buf[512];
	snprintf(buf, sizeof(buf),
		" size %.0f width %.0f color %d %d %d radius %.0f rcolor %d %d %d",
		lineSize, width, (int)color.Red, (int)color.Green, (int)color.Blue,
		circleRadius, (int)circleColor.Red, (int)circleColor.Green, (int)circleColor.Blue);
	return std::string(buf);
}

//============================================================================
// MapLocTemplate
//============================================================================

MapLocTemplate::MapLocTemplate(const MapLocParams& params, const std::string& label, const std::string& tag, const CVector3& pos, bool isDefault)
	: m_mapLocParams(params)
	, m_label(label)
	, m_tag(tag)
	, m_pos(pos)
	, m_isCreatedFromDefaultLoc(isDefault)
{
	CreateMapObject();
}

MapLocTemplate::~MapLocTemplate()
{
	delete m_mapObject;
}

void MapLocTemplate::CreateMapObject()
{
	if (m_mapObject == nullptr)
	{
		MapObjectMapLoc* newLoc = new MapObjectMapLoc(this);
		newLoc->SetPosition(m_pos);
		newLoc->PostInit();
		m_mapObject = newLoc;

		UpdateLabel();
	}
}

void MapLocTemplate::SetSelected(bool selected)
{
	if (test_and_set(m_isSelected, selected))
		m_mapObject->Update(true);
}

void MapLocTemplate::SetIndex(int index)
{
	if (test_and_set(m_index, index))
		UpdateLabel();
}

void MapLocTemplate::SetLabel(const std::string& labelText)
{
	if (test_and_set(m_label, labelText))
		UpdateLabel();
}

void MapLocTemplate::UpdateFromParams(const MapLocParams& params)
{
	m_mapLocParams = params;
	m_mapObject->Update(true);
}

void MapLocTemplate::UpdateLabel()
{
	if (!m_mapObject)
		return;

	std::string label;
	if (!m_label.empty())
	{
		char buf[512];
		snprintf(buf, sizeof(buf), "%d: %s", m_index, m_label.c_str());
		label = buf;
	}
	else
	{
		label = std::to_string(m_index);
	}

	m_mapObject->SetText(label);
}

//============================================================================
// MapObjectMapLoc
//============================================================================

MapObjectMapLoc::MapObjectMapLoc(MapLocTemplate* pMapLoc)
	: m_mapLoc(pMapLoc)
{
	GenerateLabel();

	Update(true);
}

MapObjectMapLoc::~MapObjectMapLoc()
{
	m_mapLoc->OnMapObjectRemoved();
	m_mapLoc = nullptr;

	RemoveMapObject();
}

void MapObjectMapLoc::RemoveMapObject()
{
	for (MapViewLine* markerLine : m_lines)
	{
		DeleteLine(markerLine);
	}

	m_lines.clear();
	m_circle.Clear();
}

void MapObjectMapLoc::PostInit()
{
	m_initialized = true;

	MapObject::PostInit();
}

void MapObjectMapLoc::Update(bool forced)
{
	MapObject::Update(forced);

	if (forced && m_initialized)
	{
		UpdateMapObject();
	}
}

void MapObjectMapLoc::UpdateMapObject()
{
	RemoveMapObject();

	MapViewLine* line = nullptr;
	uint32_t colorARGB;

	const auto& params = m_mapLoc->GetParams();

	if (m_mapLoc->IsSelected())
		colorARGB = params.color.GetInverted().ToARGB();
	else
		colorARGB = params.color.ToARGB();

	// Create the X
	for (int xWidth = 1; xWidth <= params.width; xWidth++)
	{
		if (xWidth == 1)
		{
			// Backslash
			line = InitLine();
			line->Layer = activeLayer;
			line->Color.ARGB = colorARGB;
			line->Start.X = -m_pos.X - params.lineSize;
			line->Start.Y = -m_pos.Y - params.lineSize;
			line->Start.Z = m_pos.Z;
			line->End.X = -m_pos.X + params.lineSize;
			line->End.Y = -m_pos.Y + params.lineSize;
			line->End.Z = m_pos.Z;
			m_lines.push_back(line);

			// Forwardslash
			line = InitLine();
			line->Layer = activeLayer;
			line->Color.ARGB = colorARGB;
			line->Start.X = -m_pos.X - params.lineSize;
			line->Start.Y = -m_pos.Y + params.lineSize;
			line->Start.Z = m_pos.Z;
			line->End.X = -m_pos.X + params.lineSize;
			line->End.Y = -m_pos.Y - params.lineSize;
			line->End.Z = m_pos.Z;
			m_lines.push_back(line);
		}
		else
		{
			// Backslash lower
			line = InitLine();
			line->Layer = activeLayer;
			line->Color.ARGB = colorARGB;
			line->Start.X = -m_pos.X - params.lineSize;
			line->Start.Y = -m_pos.Y - params.lineSize + xWidth - 1;
			line->Start.Z = m_pos.Z;
			line->End.X = -m_pos.X + params.lineSize - xWidth + 1;
			line->End.Y = -m_pos.Y + params.lineSize;
			line->End.Z = m_pos.Z;
			m_lines.push_back(line);

			// Forwardslash lower
			line = InitLine();
			line->Layer = activeLayer;
			line->Color.ARGB = colorARGB;
			line->Start.X = -m_pos.X - params.lineSize + xWidth - 1;
			line->Start.Y = -m_pos.Y + params.lineSize;
			line->Start.Z = m_pos.Z;
			line->End.X = -m_pos.X + params.lineSize;
			line->End.Y = -m_pos.Y - params.lineSize + xWidth - 1;
			line->End.Z = m_pos.Z;
			m_lines.push_back(line);

			// Backslash upper
			line = InitLine();
			line->Layer = activeLayer;
			line->Color.ARGB = colorARGB;
			line->Start.X = -m_pos.X - params.lineSize + xWidth - 1;
			line->Start.Y = -m_pos.Y - params.lineSize;
			line->Start.Z = m_pos.Z;
			line->End.X = -m_pos.X + params.lineSize;
			line->End.Y = -m_pos.Y + params.lineSize - xWidth + 1;
			line->End.Z = m_pos.Z;
			m_lines.push_back(line);

			// Forwardslash upper
			line = InitLine();
			line->Layer = activeLayer;
			line->Color.ARGB = colorARGB;
			line->Start.X = -m_pos.X - params.lineSize;
			line->Start.Y = -m_pos.Y + params.lineSize - xWidth + 1;
			line->Start.Z = m_pos.Z;
			line->End.X = -m_pos.X + params.lineSize - xWidth + 1;
			line->End.Y = -m_pos.Y - params.lineSize;
			line->End.Z = m_pos.Z;
			m_lines.push_back(line);
		}
	}

	// Create the Radius
	if (params.circleRadius > 0)
	{
		m_circle.UpdateCircle(params.circleColor, params.circleRadius, m_pos.X, m_pos.Y, m_pos.Z);
	}
	else
	{
		m_circle.Clear();
	}
}

//============================================================================
// MapLoc utility functions
//============================================================================

void InitDefaultMapLocParams()
{
	float lineSize = GetPrivateProfileFloat("MapLoc", "Size", 50.f, INIFileName);
	gDefaultMapLocParams.lineSize = lineSize;

	float width = GetPrivateProfileFloat("MapLoc", "Width", 10.f, INIFileName);
	gDefaultMapLocParams.width = width;

	uint8_t r = static_cast<uint8_t>(GetPrivateProfileInt("MapLoc", "Red", 255, INIFileName));
	uint8_t g = static_cast<uint8_t>(GetPrivateProfileInt("MapLoc", "Green", 0, INIFileName));
	uint8_t b = static_cast<uint8_t>(GetPrivateProfileInt("MapLoc", "Blue", 0, INIFileName));
	gDefaultMapLocParams.color = MQColor(r, g, b);

	float radius = GetPrivateProfileFloat("MapLoc", "Radius", 0, INIFileName);
	gDefaultMapLocParams.circleRadius = radius;

	r = static_cast<uint8_t>(GetPrivateProfileInt("MapLoc", "RadiusRed", 255, INIFileName));
	g = static_cast<uint8_t>(GetPrivateProfileInt("MapLoc", "RadiusGreen", 0, INIFileName));
	b = static_cast<uint8_t>(GetPrivateProfileInt("MapLoc", "RadiusBlue", 0, INIFileName));
	gDefaultMapLocParams.circleColor = MQColor(r, g, b);
}

void ResetMapLocOverrides()
{
	gOverrideMapLocParams = gDefaultMapLocParams;
}

void UpdateDefaultMapLocInstances()
{
	for (auto& obj : gMapLocTemplates)
	{
		if (obj->IsCreatedFromDefaults())
		{
			obj->UpdateFromParams(gDefaultMapLocParams);
		}
	}
}

MapLocTemplate* GetMapLocTemplateByTag(std::string_view tag)
{
	for (auto& maplocTemplate : gMapLocTemplates)
	{
		if (maplocTemplate->GetTag() == tag)
			return maplocTemplate.get();
	}

	return nullptr;
}

MapLocTemplate* GetMapLocByIndex(int index)
{
	if (index > 0 && index <= (int)gMapLocTemplates.size())
		return gMapLocTemplates[index - 1].get();

	return nullptr;
}

void CreateAllMapLocs()
{
	for (auto& maploc : gMapLocTemplates)
	{
		maploc->CreateMapObject();
	}
}

void DeleteAllMapLocs()
{
	gMapLocTemplates.clear();
}

static void UpdateMapLocIndexes()
{
	for (int i = 0; i < (int)gMapLocTemplates.size(); ++i)
	{
		MapLocTemplate* mapLoc = gMapLocTemplates[i].get();
		mapLoc->SetIndex(i + 1);
	}
}

void AddMapLoc(std::unique_ptr<MapLocTemplate> mapLoc)
{
	mapLoc->SetIndex((int)gMapLocTemplates.size() + 1);

	gMapLocTemplates.push_back(std::move(mapLoc));
}

void DeleteMapLoc(MapLocTemplate* mapLoc)
{
	gMapLocTemplates.erase(std::remove_if(gMapLocTemplates.begin(), gMapLocTemplates.end(),
		[mapLoc](auto& existingMapLoc) { return existingMapLoc.get() == mapLoc; }),
		gMapLocTemplates.end());

	UpdateMapLocIndexes();
}

void DeleteSelectedMapLocs()
{
	gMapLocTemplates.erase(
		std::remove_if(gMapLocTemplates.begin(), gMapLocTemplates.end(),
			[](auto& maploc) { return maploc->IsSelected(); }),
		gMapLocTemplates.end());

	UpdateMapLocIndexes();
}
